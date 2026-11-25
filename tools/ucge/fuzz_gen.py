"""Gerador de programas CSP validos para o teste diferencial do otimizador (docs/ucge.md, "UIR-G").
Gera especificacoes aleatorias respeitando as regras do validador (tipos, escopo sem sombra, retorno em todos os caminhos, lacos limitados),
com muita coisa para o otimizador mexer (constantes, ramos mortos, copias, variaveis nao usadas, funcoes pequenas) e muita coisa que NAO pode
mudar (divisao por zero, indices fora, erros, efeitos de print, inteiros grandes, NaN, -0.0, strings com acento)."""
import random

SCALARS = ["int", "float", "bool", "str"]
PARAM_TYPES = ["int", "int", "float", "bool", "str", "list[int]", "list[str]"]
RET_TYPES = ["int", "int", "float", "bool", "str", "list[int]"]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0
        self.funcs = []          # (name, [ptypes], rtype), as ja definidas (chamaveis)
        self.consts = []         # (name, type) constantes de modulo
        self.out = []
        self.has_box = False
        self.scopes = []         # lista de listas de dicts {name, ty, mut, iter}
        self.loop = 0
        self.indent = 0
        self.cur_ret = "int"

    # ---------- utilidades ----------
    def name(self, p="v"):
        self.n += 1
        return f"{p}{self.n}"

    def emit(self, s):
        self.out.append("  " * self.indent + s)

    def visible(self, ty=None, mut=False):
        res = []
        for sc in self.scopes:
            for v in sc:
                if (ty is None or v["ty"] == ty) and (not mut or (v["mut"] and not v["iter"])):
                    res.append(v)
        return res

    def decl(self, name, ty, mut=True):
        self.scopes[-1].append({"name": name, "ty": ty, "mut": mut, "iter": False})

    def chance(self, p):
        return self.r.random() < p

    def pick(self, xs):
        return self.r.choice(xs)

    # ---------- literais ----------
    def lit(self, ty):
        r = self.r
        if ty == "int":
            return str(self.pick([0, 0, 1, 1, 2, 3, 5, 7, 10, -1, -3, 100, 2 ** 31, 10 ** 12, -(10 ** 9), 9007199254740993, 4611686018427387904, 123456789012345678]))
        if ty == "float":
            return self.pick(["0.0", "1.0", "1.5", "-2.5", "0.1", "3.25", "1e10", "-0.0", "2.0", "100.0", "0.5", "7.0", "1e308"])
        if ty == "bool":
            return self.pick(["true", "false"])
        if ty == "str":
            return self.pick(['""', '"a"', '"ab"', '"Hello"', '" pad "', '"x,y,z"', '"café"', '"AbC"', '"abc abc"', '"1"', '"12"'])
        if ty == "list[int]":
            return self.pick(["[]", "[1, 2, 3]", "[5, -1, 5]", "[0]", "[10, 20]", "[3, 1, 2]"]) if False else "[" + ", ".join(self.lit("int") for _ in range(r.randint(0, 4))) + "]"
        if ty == "list[str]":
            return "[" + ", ".join(self.lit("str") for _ in range(r.randint(0, 3))) + "]"
        raise ValueError(ty)

    # ---------- expressoes ----------
    def const_expr(self, ty, d=2):
        """so literais: o SCCP e a dobra tem que resolver"""
        r = self.r
        if d <= 0 or self.chance(0.25):
            return self.lit(ty) if ty in ("int", "float", "bool", "str") else self.lit(ty)
        if ty == "int":
            c = r.randint(0, 7)
            a, b = self.const_expr("int", d - 1), self.const_expr("int", d - 1)
            if c == 0: return f"({a} + {b})"
            if c == 1: return f"({a} - {b})"
            if c == 2: return f"({a} * {b})"
            if c == 3: return f"({a} // {self.pick(['2', '3', '-2', '7', '0', '1'])})"
            if c == 4: return f"({a} % {self.pick(['2', '3', '-5', '0', '1'])})"
            if c == 5: return f"max({a}, {b})"
            if c == 6: return f"abs({a})"
            return f"len({self.const_expr('str', d - 1)})"
        if ty == "float":
            c = r.randint(0, 4)
            a, b = self.const_expr("float", d - 1), self.const_expr("float", d - 1)
            if c == 0: return f"({a} + {b})"
            if c == 1: return f"({a} * {b})"
            if c == 2: return f"({a} / {self.pick(['2.0', '0.0', '4.0', '3.0'])})"
            if c == 3: return f"({self.const_expr('int', d - 1)} / {self.pick(['2', '3', '0', '7'])})"
            return f"sqrt({self.pick(['4.0', '2.0', '9', '0.0', '1e10'])})"
        if ty == "bool":
            c = r.randint(0, 5)
            if c == 0: return f"({self.const_expr('int', d - 1)} {self.pick(['<', '<=', '==', '!=', '>', '>='])} {self.const_expr('int', d - 1)})"
            if c == 1: return f"({self.const_expr('str', d - 1)} {self.pick(['==', '!=', '<'])} {self.const_expr('str', d - 1)})"
            if c == 2: return f"(not {self.const_expr('bool', d - 1)})"
            if c == 3: return f"({self.const_expr('bool', d - 1)} and {self.const_expr('bool', d - 1)})"
            if c == 4: return f"({self.const_expr('bool', d - 1)} or {self.const_expr('bool', d - 1)})"
            return f"startswith({self.const_expr('str', d - 1)}, {self.lit('str')})"
        if ty == "str":
            c = r.randint(0, 5)
            if c == 0: return f"({self.const_expr('str', d - 1)} + {self.const_expr('str', d - 1)})"
            if c == 1: return f"upper({self.const_expr('str', d - 1)})"
            if c == 2: return f"trim({self.const_expr('str', d - 1)})"
            if c == 3: return f"str({self.const_expr('int', d - 1)})"
            if c == 4: return f"slice({self.const_expr('str', d - 1)}, {self.pick(['0', '1', '-1', '2'])}, {self.pick(['2', '3', '-1', '9'])})"
            return f"replace({self.const_expr('str', d - 1)}, {self.pick(['\"a\"', '\"b\"', '\"c\"'])}, {self.lit('str')})"
        return self.lit(ty)

    def call_expr(self, ty, d):
        cands = [f for f in self.funcs if f[2] == ty]
        if not cands:
            return None
        name, pts, _ = self.pick(cands)
        args = ", ".join(self.expr(t, d - 1) for t in pts)
        return f"{name}({args})"

    def expr(self, ty, d=3):
        r = self.r
        vs = self.visible(ty)
        if self.chance(0.12) and ty in SCALARS + ["list[int]", "list[str]"]:
            return self.const_expr(ty, 2)
        if d <= 0 or self.chance(0.22):
            if vs and self.chance(0.7):
                return self.pick(vs)["name"]
            if ty in ("int", "float", "bool", "str"):
                cs = [c for c in self.consts if c[1] == ty]
                if cs and self.chance(0.3):
                    return self.pick(cs)[0]
            return self.lit(ty)
        if ty == "int":
            c = r.randint(0, 17)
            a, b = self.expr("int", d - 1), self.expr("int", d - 1)
            if c == 0: return f"({a} + {b})"
            if c == 1: return f"({a} - {b})"
            if c == 2: return f"({a} * {b})"
            if c == 3: return f"({a} // {self.divisor(d)})"
            if c == 4: return f"({a} % {self.divisor(d)})"
            if c == 5: return f"(-{a})"
            if c == 6: return f"abs({a})"
            if c == 7: return f"min({a}, {b})"
            if c == 8: return f"max({a}, {b})"
            if c == 9: return f"len({self.expr(self.pick(['str', 'list[int]', 'list[str]']), d - 1)})"
            if c == 10: return f"({self.expr('bool', d - 1)} ? {a} : {b})"
            if c == 11:
                x = self.call_expr("int", d)
                return x if x else f"({a} + 0)"
            if c == 12: return f"{self.expr('list[int]', d - 1)}[{self.index(d)}]"
            if c == 13: return f"indexof({self.expr('str', d - 1)}, {self.pick(['\"a\"', '\"b\"', '\"l\"', '\"\"'])})"
            if c == 14: return f"sum({self.expr('list[int]', d - 1)})"
            if c == 15: return f"int({self.pick([self.expr('float', d - 1), self.expr('str', d - 1), self.expr('bool', d - 1), self.expr('int', d - 1)])})"
            if c == 16: return f"floor({self.expr('float', d - 1)})"
            return f"({a} * 1)" if self.chance(0.5) else f"({a} + 0)"
        if ty == "float":
            c = r.randint(0, 11)
            a, b = self.expr("float", d - 1), self.expr("float", d - 1)
            if c == 0: return f"({a} + {b})"
            if c == 1: return f"({a} - {b})"
            if c == 2: return f"({a} * {b})"
            if c == 3: return f"({a} / {b})"
            if c == 4: return f"({self.expr('int', d - 1)} / {self.expr('int', d - 1)})"
            if c == 5: return f"float({self.expr('int', d - 1)})"
            if c == 6: return f"sqrt(abs({a}))"
            if c == 7: return f"({a} + {self.expr('int', d - 1)})"
            if c == 8: return f"({self.expr('bool', d - 1)} ? {a} : {b})"
            if c == 9: return f"abs({a})"
            if c == 10:
                x = self.call_expr("float", d)
                return x if x else f"({a} * 1)"
            return f"({a} * 1.0)" if self.chance(0.5) else f"(-{a})"
        if ty == "bool":
            c = r.randint(0, 13)
            if c == 0: return f"({self.expr('int', d - 1)} {self.pick(['<', '<=', '==', '!=', '>', '>='])} {self.expr('int', d - 1)})"
            if c == 1: return f"({self.expr('float', d - 1)} {self.pick(['<', '<=', '==', '!=', '>', '>='])} {self.expr('float', d - 1)})"
            if c == 2: return f"({self.expr('str', d - 1)} {self.pick(['==', '!=', '<', '>='])} {self.expr('str', d - 1)})"
            if c == 3: return f"(not {self.expr('bool', d - 1)})"
            if c == 4: return f"({self.expr('bool', d - 1)} and {self.expr('bool', d - 1)})"
            if c == 5: return f"({self.expr('bool', d - 1)} or {self.expr('bool', d - 1)})"
            if c == 6: return f"({self.expr('int', d - 1)} in {self.expr('list[int]', d - 1)})"
            if c == 7: return f"contains({self.expr('str', d - 1)}, {self.expr('str', d - 1)})"
            if c == 8: return f"startswith({self.expr('str', d - 1)}, {self.expr('str', d - 1)})"
            if c == 9:
                x = self.call_expr("bool", d)
                return x if x else f"({self.expr('int', d - 1)} > 0)"
            if c == 10:
                v = self.visible("int")
                if v:
                    n = self.pick(v)["name"]
                    return f"({n} {self.pick(['==', '<=', '>='])} {n})"
            if c == 11: return f"({self.expr('bool', d - 1)} ? {self.expr('bool', d - 1)} : {self.expr('bool', d - 1)})"
            if c == 12: return f"({self.expr('float', d - 1)} != {self.expr('float', d - 1)})"
            return f"endswith({self.expr('str', d - 1)}, {self.expr('str', d - 1)})"
        if ty == "str":
            c = r.randint(0, 11)
            a, b = self.expr("str", d - 1), self.expr("str", d - 1)
            if c == 0: return f"({a} + {b})"
            if c == 1: return f"upper({a})"
            if c == 2: return f"lower({a})"
            if c == 3: return f"trim({a})"
            if c == 4: return f"str({self.expr('int', d - 1)})"
            if c == 5: return f"slice({a}, {self.expr('int', d - 1)}, {self.expr('int', d - 1)})"
            if c == 6: return f"replace({a}, {self.pick(['\"a\"', '\"l\"', '\"bc\"'])}, {b})"
            if c == 7: return f"join({self.expr('list[str]', d - 1)}, {self.pick(['\", \"', '\"-\"', '\"\"'])})"
            if c == 8: return f"({self.expr('bool', d - 1)} ? {a} : {b})"
            if c == 9:
                x = self.call_expr("str", d)
                return x if x else f"({a} + \"\")"
            if c == 10: return f"{a}[{self.index(d)}]"
            return f"reverse({a})"
        if ty == "list[int]":
            c = r.randint(0, 5)
            if c == 0: return f"[{self.expr('int', d - 1)}, {self.expr('int', d - 1)}]"
            if c == 1: return f"sort({self.expr('list[int]', d - 1)})"
            if c == 2: return f"slice({self.expr('list[int]', d - 1)}, {self.expr('int', d - 1)}, {self.expr('int', d - 1)})"
            if c == 3: return f"reverse({self.expr('list[int]', d - 1)})"
            if c == 4:
                x = self.call_expr("list[int]", d)
                if x: return x
            return self.lit("list[int]")
        if ty == "list[str]":
            c = r.randint(0, 3)
            if c == 0: return f"split({self.expr('str', d - 1)}, {self.pick(['\",\"', '\" \"', '\"a\"'])})"
            if c == 1: return f"words({self.expr('str', d - 1)})"
            return self.lit("list[str]")
        raise ValueError(ty)

    def divisor(self, d):
        c = self.r.randint(0, 5)
        if c == 0: return self.pick(["2", "3", "-2", "7", "1"])
        if c == 1: return "0"
        if c == 2: return self.expr("int", d - 1)
        if c == 3: return f"({self.expr('int', d - 1)} + 1)"
        return self.expr("int", d - 1)

    def index(self, d):
        c = self.r.randint(0, 3)
        if c == 0: return self.pick(["0", "1", "2", "-1", "-2", "5"])
        return self.expr("int", d - 1)

    # ---------- comandos ----------
    def block(self, n_lo, n_hi, depth, scope_open=True):
        if scope_open: self.scopes.append([])
        self.indent += 1
        k = self.r.randint(n_lo, n_hi)
        for _ in range(k):
            self.stmt(depth)
        if k == 0: self.emit(f"print({self.expr('int', 1)})")
        self.indent -= 1
        if scope_open: self.scopes.pop()

    def stmt(self, depth):
        r = self.r
        c = r.randint(0, 30)
        if c <= 4:                                   # let
            ty = self.pick(SCALARS + ["list[int]", "list[str]"])
            nm = self.name("v")
            e = self.expr(ty, 2)
            if ty.startswith("list") and e.startswith("[]"):
                self.emit(f"let {nm}: {ty} = {e}")
            elif self.chance(0.3) or ty.startswith("list") and e == "[]":
                self.emit(f"let {nm}: {ty} = {e}")
            else:
                self.emit(f"let {nm} = {e}")
            self.decl(nm, ty)
        elif c <= 7:                                 # atribuicao e atribuicao composta
            ty = self.pick(SCALARS)
            vs = self.visible(ty, mut=True)
            if not vs: return self.stmt(depth)
            v = self.pick(vs)["name"]
            e = self.expr(ty, 2)
            if ty in ("int", "float") and self.chance(0.4):
                op = self.pick(["+=", "-=", "*="])
                if ty == "int":
                    e = self.expr("int", 2)
                self.emit(f"{v} {op} {e}")
            elif ty == "str" and self.chance(0.3):
                self.emit(f"{v} += {e}")
            else:
                self.emit(f"{v} = {e}")
        elif c <= 11 and depth > 0:                  # if / elif / else
            self.emit(f"if {self.expr('bool', 2)}")
            self.block(1, 3, depth - 1)
            if self.chance(0.4):
                self.emit(f"elif {self.expr('bool', 2)}")
                self.block(1, 2, depth - 1)
            if self.chance(0.5):
                self.emit("else")
                self.block(1, 3, depth - 1)
        elif c == 12 and depth > 0:                  # if com condicao constante
            self.emit(f"if {self.const_expr('bool', 2)}")
            self.block(1, 2, depth - 1)
            if self.chance(0.6):
                self.emit("else")
                self.block(1, 2, depth - 1)
        elif c <= 14 and depth > 0:                  # while limitado
            k = self.name("k")
            self.emit(f"let {k} = 0")
            self.decl(k, "int", mut=False)
            lim = self.pick([2, 3, 4])
            cond = f"{k} < {lim}" if self.chance(0.6) else f"{k} < {lim} and {self.expr('bool', 1)}"
            self.emit(f"while {cond}")
            self.loop += 1
            self.scopes.append([])
            self.indent += 1
            self.emit(f"{k} += 1")
            for _ in range(self.r.randint(1, 3)): self.stmt(depth - 1)
            self.indent -= 1
            self.scopes.pop()
            self.loop -= 1
        elif c <= 16 and depth > 0:                  # for faixa
            i = self.name("i")
            lo, hi = self.pick(["0", "0", "1", "2"]), self.pick(["3", "4", "5", "0", "2"])
            step = self.pick(["", "", " step 2", " step -1"]) if True else ""
            if "-1" in step: lo, hi = "4", "0"
            self.emit(f"for {i} in {lo}..{hi}{step}")
            self.loop += 1
            self.scopes.append([{"name": i, "ty": "int", "mut": False, "iter": False}])
            self.indent += 1
            for _ in range(self.r.randint(1, 3)): self.stmt(depth - 1)
            self.indent -= 1
            self.scopes.pop()
            self.loop -= 1
        elif c <= 18 and depth > 0:                  # for each (lista ou texto)
            if self.chance(0.6):
                vs = self.visible("list[int]")
                coll = self.pick(vs) if vs else None
                src = coll["name"] if coll else self.lit("list[int]")
                x = self.name("x")
                self.emit(f"for {x} in {src}")
                if coll: coll["iter"] = True
                self.loop += 1
                self.scopes.append([{"name": x, "ty": "int", "mut": False, "iter": False}])
                self.indent += 1
                for _ in range(self.r.randint(1, 3)): self.stmt(depth - 1)
                self.indent -= 1
                self.scopes.pop(); self.loop -= 1
                if coll: coll["iter"] = False
            else:
                x = self.name("ch")
                self.emit(f"for {x} in {self.expr('str', 1)}")
                self.loop += 1
                self.scopes.append([{"name": x, "ty": "str", "mut": False, "iter": False}])
                self.indent += 1
                for _ in range(self.r.randint(1, 2)): self.stmt(depth - 1)
                self.indent -= 1
                self.scopes.pop(); self.loop -= 1
        elif c <= 21 and depth > 0:                  # try / catch / finally
            self.emit("try")
            self.block(1, 3, depth - 1)
            has_c = self.chance(0.8)
            if has_c:
                e = self.name("e")
                self.emit(f"catch {e}")
                self.scopes.append([{"name": e, "ty": "Error", "mut": False, "iter": False}])
                self.indent += 1
                self.emit(f"print(errmsg({e}))")
                for _ in range(self.r.randint(0, 2)): self.stmt(depth - 1)
                self.indent -= 1
                self.scopes.pop()
            if not has_c or self.chance(0.4):
                self.emit("finally")
                self.block(1, 2, depth - 1)
        elif c == 22:                                # throw sob condicao
            self.emit(f"if {self.expr('bool', 2)}")
            self.indent += 1
            self.emit(f"throw {self.expr('str', 1)}")
            self.indent -= 1
        elif c == 23:                                # print
            ty = self.pick(SCALARS + ["list[int]"])
            self.emit(f"print({self.expr(ty, 2)})")
        elif c == 24:                                # push / pop / sort sobre lista nao iterada
            vs = self.visible("list[int]", mut=True)
            if not vs: return self.stmt(depth)
            v = self.pick(vs)["name"]
            k = self.r.randint(0, 3)
            if k == 0: self.emit(f"push({v}, {self.expr('int', 2)})")
            elif k == 1: self.emit(f"{v}[{self.index(2)}] = {self.expr('int', 2)}")
            elif k == 2: self.emit(f"{v}[{self.index(2)}] += {self.expr('int', 1)}")
            else: self.emit(f"let {self.name('p')} = pop({v})")
            if k == 3:
                self.scopes[-1].append({"name": self.out[-1].split()[1], "ty": "int", "mut": True, "iter": False})
        elif c == 25 and self.loop and self.chance(0.7):     # break / continue sob condicao
            self.emit(f"if {self.expr('bool', 1)}")
            self.indent += 1
            self.emit(self.pick(["break", "continue"]))
            self.indent -= 1
        elif c == 26 and depth > 0:                  # return antecipado
            self.emit(f"if {self.expr('bool', 2)}")
            self.indent += 1
            self.emit(f"return {self.expr(self.cur_ret, 2)}")
            self.indent -= 1
        elif c == 27:                                # variavel morta / atribuicao morta / copia
            nm = self.name("d")
            ty = self.pick(SCALARS)
            self.emit(f"let {nm} = {self.expr(ty, 2)}")
            self.decl(nm, ty)
            if self.chance(0.5):
                self.emit(f"{nm} = {self.expr(ty, 1)}")
            if self.chance(0.6):
                cp = self.name("c")
                self.emit(f"let {cp} = {nm}")
                self.decl(cp, ty)
        elif c == 28 and self.has_box:               # classe
            bs = self.visible("Box")
            if not bs or self.chance(0.3):
                nm = self.name("b")
                self.emit(f"let {nm} = Box({self.expr('int', 2)})")
                self.decl(nm, "Box")
            else:
                b = self.pick(bs)["name"]
                k = self.r.randint(0, 3)
                if k == 0: self.emit(f"{b}.add({self.expr('int', 2)})")
                elif k == 1: self.emit(f"{b}.val = {self.expr('int', 2)}")
                elif k == 2: self.emit(f"{b}.val += {self.expr('int', 1)}")
                else: self.emit(f"print({b}.risky({self.expr('int', 1)}))")
        else:                                        # chamada como comando, ou uma constante
            cs = [f for f in self.funcs]
            if cs and self.chance(0.6):
                name, pts, _ = self.pick(cs)
                self.emit(f"{name}({', '.join(self.expr(t, 1) for t in pts)})")
            else:
                nm = self.name("k")
                ty = self.pick(SCALARS)
                self.emit(f"let {nm} = {self.const_expr(ty, 2)}")
                self.decl(nm, ty)

    # ---------- funcoes e programa ----------
    def function(self, public=True):
        name = self.name("f")
        n = self.r.randint(1, 3)
        pts = [self.pick(PARAM_TYPES) for _ in range(n)]
        ret = self.pick(RET_TYPES)
        params = [(self.name("p"), t) for t in pts]
        self.cur_ret = ret
        self.scopes = [[{"name": p, "ty": t, "mut": t in SCALARS + ["list[int]"], "iter": False} for p, t in params]]
        self.indent = 0
        self.out.append(f"fn {name}({', '.join(p + ': ' + t for p, t in params)}) -> {ret}")
        self.indent = 1
        for _ in range(self.r.randint(2, 7)):
            self.stmt(2)
        if self.chance(0.3):                         # temporario antes do retorno
            t = self.name("t")
            self.emit(f"let {t} = {self.expr(ret, 2)}")
            self.emit(f"return {t}")
        else:
            self.emit(f"return {self.expr(ret, 2)}")
        self.indent = 0
        self.funcs.append((name, pts, ret))
        return name, pts, ret

    def tiny(self):
        """funcao pequena de uma linha, boa para expandir em linha"""
        name = self.name("f")
        t = self.pick(["int", "int", "float", "bool", "str"])
        pn = [self.name("p") for _ in range(self.r.randint(1, 2))]
        pts = [t if i == 0 else self.pick(["int", t]) for i in range(len(pn))]
        self.scopes = [[{"name": p, "ty": ty, "mut": False, "iter": False} for p, ty in zip(pn, pts)]]
        self.cur_ret = t
        self.out.append(f"fn {name}({', '.join(p + ': ' + ty for p, ty in zip(pn, pts))}) -> {t}")
        self.out.append(f"  return {self.expr(t, 2)}")
        self.funcs.append((name, pts, t))

    def program(self):
        r = self.r
        # constantes de modulo
        for _ in range(r.randint(0, 2)):
            ty = self.pick(["int", "int", "str", "bool", "float"])
            nm = f"K{self.n}"
            self.n += 1
            self.out.append(f"const {nm} = {self.lit(ty)}")
            self.consts.append((nm, ty))
        if self.chance(0.3):
            self.has_box = True
            self.out += ["class Box", "  field val: int = 0", "  fn init(v: int)", "    self.val = v", "  fn add(n: int) -> int", "    self.val += n", "    return self.val",
                         "  fn get() -> int", "    return self.val", "  fn risky(n: int) -> int", "    return self.val // n"]
        if self.chance(0.25):                        # recursao
            self.out += ["fn fact(n: int) -> int", "  if n <= 1", "    return 1", "  if n > 25", "    throw \"too big\"", "  return n * fact(n - 1)"]
            self.funcs.append(("fact", ["int"], "int"))
        if self.chance(0.15):
            self.out += ["fn isEven(n: int) -> bool", "  if n == 0", "    return true", "  if n < 0 or n > 30", "    throw \"range\"", "  return isOdd(n - 1)",
                         "fn isOdd(n: int) -> bool", "  if n == 0", "    return false", "  if n < 0 or n > 30", "    throw \"range\"", "  return isEven(n - 1)"]
            self.funcs.append(("isEven", ["int"], "bool")); self.funcs.append(("isOdd", ["int"], "bool"))
        for _ in range(r.randint(0, 2)):
            self.tiny()
        pub = []
        for _ in range(r.randint(2, 4)):
            self.has_box = self.has_box
            pub.append(self.function())
        public = [f for f in self.funcs]
        return "\n".join(self.out) + "\n", public


def gen_program(seed):
    g = Gen(seed)
    return g.program()
