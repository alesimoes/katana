#!/usr/bin/env python3
"""Teste de propriedade/diferencial do UCGE (docs/ucge.md, "Backends JS e TS"): gera programas CSP aleatorios VALIDOS (tipados pela IR: int, float, str, bool,
   list[int], list[str], map[str,int], funcoes com default, uma classe, laco, try/catch/finally) e compara a saida de Python x JavaScript x TypeScript (jsdiff.py).
   Operacoes que podem levantar erro (divisao, indice, pop, int(), sqrt...) ficam em try/catch e imprimem errmsg, entao o TEXTO dos erros tambem e comparado.
   Uso: jsfuzz.py <ucgecli> [N=200] [semente=1] [diretorio-de-falhas]      (UCGE_TSC=... inclui TypeScript)"""
import os, random, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jsdiff

SPOOL = ["", " ", "a", "abc", "Hello World", "  pad  ", "añb", "世界", "🙂x", "ß", "ÀÉ", "x,y,,z", "tab\\there", "a b  c", " nb", "zzz", "a🙂b"]
SEPS = [",", " ", "a", "ab", "世"]
PAT = ["[a-z]+", "^[A-Za-z ]*$", "\\\\d", "a.c", "^$"]
Q = lambda s: '"' + s + '"'

class G:
    def __init__(self, rnd):
        self.r = rnd; self.n = 0; self.scopes = [[]]; self.lines = []; self.ind = 0; self.risky = False; self.loops = 0; self.fns = []; self.cls = False
    # ---- escopo ----
    def new(self, prefix="v"): self.n += 1; return "%s%d" % (prefix, self.n)
    def declare(self, name, t, mut=True): self.scopes[-1].append((name, t, mut))
    def vars(self, t, mut=False): return [n for sc in self.scopes for (n, ty, m) in sc if ty == t and (m or not mut)]
    def emit(self, s): self.lines.append("  " * self.ind + s)
    # ---- expressoes: devolvem (texto, limite) ----
    def lit_int(self): v = self.r.randint(-20, 50); return (str(v), abs(v))
    def ex_int(self, d=0):
        r = self.r; c = r.random()
        vs = self.vars("int")
        if d >= 3 or c < 0.18: return self.lit_int() if (not vs or r.random() < 0.4) else (r.choice(vs), 100003)
        if c < 0.30: a, ba = self.ex_int(d + 1); b, bb = self.ex_int(d + 1); return ("(%s + %s)" % (a, b), ba + bb)
        if c < 0.38: a, ba = self.ex_int(d + 1); b, bb = self.ex_int(d + 1); return ("(%s - %s)" % (a, b), ba + bb)
        if c < 0.46:
            a, ba = self.ex_int(d + 1); b, bb = self.ex_int(d + 1)
            if ba * bb > 1e14: return ("(%s + %s)" % (a, b), ba + bb)
            return ("(%s * %s)" % (a, b), ba * bb)
        if c < 0.54:  # // e % por literais (nunca zero) ou por expressao arriscada
            a, ba = self.ex_int(d + 1)
            op = r.choice(["//", "%"])
            if r.random() < 0.7:
                k = r.choice([2, 3, 5, 7, -2, -3, 10, 1, -1, 16]); return ("(%s %s %s)" % (a, op, ("(%d)" % k) if k < 0 else k), ba)
            b, bb = self.ex_int(d + 1); self.risky = True; return ("(%s %s %s)" % (a, op, b), ba)
        if c < 0.60: a, ba = self.ex_int(d + 1); return ("abs(%s)" % a, ba)
        if c < 0.66: a, ba = self.ex_int(d + 1); b, bb = self.ex_int(d + 1); return ("%s(%s, %s)" % (r.choice(["min", "max"]), a, b), max(ba, bb))
        if c < 0.70: return ("len(%s)" % self.ex_str(d + 1)[0], 100)
        if c < 0.74: return ("len(%s)" % self.ex_list(r.choice(["list[int]", "list[str]"]), d + 1)[0], 100)
        if c < 0.77: return ("indexof(%s, %s)" % (self.ex_str(d + 1)[0], Q(r.choice(SEPS))), 100)
        if c < 0.81:
            self.risky = True; f = self.ex_float(d + 1)[0]; return ("int(%s)" % f, 1e6) if False else ("int(min(1000000.0, max(-1000000.0, %s)))" % f, 1e6)
        if c < 0.84: self.risky = True; return ("int(%s)" % r.choice([Q("12"), Q(" -7 "), Q("x1"), Q("3.5"), Q(""), Q("1_0"), self.ex_str(d + 1)[0]]), 1e9) if False else ("int(%s)" % r.choice([Q("12"), Q(" -7 "), Q("x1"), Q("3.5"), Q(""), Q("1_0")]), 1e3)
        if c < 0.88:
            self.risky = True; l = self.ex_list("list[int]", d + 1)[0]; i = self.ex_int(d + 1)[0]; return ("%s[%s]" % (l, i), 100003) if l[0] != "[" else ("(%s)[%s]" % (l, i), 100003)
        if c < 0.91: return ("sum(%s)" % self.ex_list("list[int]", d + 1)[0], 1e9)
        if c < 0.94:
            m = self.vars("map[str,int]")
            if m: self.risky = True; return ("%s[%s]" % (r.choice(m), Q(r.choice(["a", "b", "zz", "k1"]))), 1e5)
        if c < 0.97 and self.fns:
            fn = r.choice(self.fns); a = self.ex_int(d + 1)[0]; self.risky = True if fn[2] else self.risky
            return ("%s(%s)" % (fn[0], a), 100003) if fn[1] and r.random() < 0.5 else ("%s(%s, %s)" % (fn[0], a, self.ex_int(d + 1)[0]), 100003)
        if c < 0.99: return ("floor(%s)" % ("min(1000000.0, max(-1000000.0, %s))" % self.ex_float(d + 1)[0]), 1e6)
        a, ba = self.ex_int(d + 1); return ("(%s ? %s : %s)" % (self.ex_bool(d + 1), a, self.ex_int(d + 1)[0]), ba + 100)
    def ex_float(self, d=0):
        r = self.r; c = r.random(); vs = self.vars("float")
        if d >= 3 or c < 0.2:
            if vs and r.random() < 0.5: return (r.choice(vs), 1000.0)
            v = r.choice([0.0, 1.5, -2.25, 0.1, 3.0, 100.0, 0.5, 1e-3, 2.5, -0.75, 1e3]); return (repr(v) if v >= 0 else "(%r)" % v, abs(v))
        if c < 0.35: a, ba = self.ex_float(d + 1); b, bb = self.ex_float(d + 1); return ("(%s + %s)" % (a, b), ba + bb)
        if c < 0.45: a, ba = self.ex_float(d + 1); b, bb = self.ex_float(d + 1); return ("(%s - %s)" % (a, b), ba + bb)
        if c < 0.55:
            a, ba = self.ex_float(d + 1); b, bb = self.ex_float(d + 1)
            if ba * bb > 1e8: return ("(%s + %s)" % (a, b), ba + bb)
            return ("(%s * %s)" % (a, b), ba * bb)
        if c < 0.65:
            a, ba = self.ex_float(d + 1)
            if r.random() < 0.6: k = r.choice([2.0, 4.0, 3.0, 0.5, 10.0, -8.0]); return ("(%s / %s)" % (a, ("(%r)" % k) if k < 0 else repr(k)), ba * 10)
            self.risky = True; b, bb = self.ex_float(d + 1); return ("(%s / %s)" % (a, b), 1e12)
        if c < 0.72: a, ba = self.ex_int(d + 1); b, bb = self.ex_int(d + 1); self.risky = True; return ("(%s / %s)" % (a, b), ba)
        if c < 0.78: return ("float(%s)" % self.ex_int(d + 1)[0], 1e5)
        if c < 0.84: self.risky = True; a, ba = self.ex_float(d + 1); return ("sqrt(%s)" % a, 1e6)
        if c < 0.88: self.risky = True; a, ba = self.ex_float(d + 1); return ("pow(%s, %s)" % (a, r.choice(["2.0", "3.0", "0.5", "(-1.0)", "2"])), 1e9) if ba < 100 else ("abs(%s)" % a, ba)
        if c < 0.92: a, ba = self.ex_float(d + 1); return ("abs(%s)" % a, ba)
        if c < 0.95: a, ba = self.ex_float(d + 1); b, bb = self.ex_float(d + 1); return ("%s(%s, %s)" % (r.choice(["min", "max"]), a, b), max(ba, bb))
        if c < 0.97: self.risky = True; return ("float(%s)" % r.choice([Q("2.5"), Q("1e3"), Q("abc"), Q(" 7 "), Q("inf")]), 1e3) if False else ("float(%s)" % r.choice([Q("2.5"), Q("1e3"), Q("abc"), Q(" 7 ")]), 1e3)
        a, ba = self.ex_float(d + 1); return ("(%s ? %s : %s)" % (self.ex_bool(d + 1), a, self.ex_float(d + 1)[0]), ba + 1)
    def ex_str(self, d=0):
        r = self.r; c = r.random(); vs = self.vars("str")
        if d >= 3 or c < 0.2:
            if vs and r.random() < 0.5: return (r.choice(vs), 0)
            return (Q(r.choice(SPOOL)), 0)
        if c < 0.34: return ("(%s + %s)" % (self.ex_str(d + 1)[0], self.ex_str(d + 1)[0]), 0)
        if c < 0.44: return ("str(%s)" % self.ex_int(d + 1)[0], 0)
        if c < 0.50: return ("str(%s)" % self.ex_float(d + 1)[0], 0)
        if c < 0.54: return ("str(%s)" % self.ex_bool(d + 1), 0)
        if c < 0.58: return ("str(%s)" % self.ex_list(r.choice(["list[int]", "list[str]"]), d + 1)[0], 0)
        if c < 0.64: return ("%s(%s)" % (r.choice(["upper", "lower", "trim"]), self.ex_str(d + 1)[0]), 0)
        if c < 0.72: return ("slice(%s, %s%s)" % (self.ex_str(d + 1)[0], self.ex_int(d + 1)[0], (", " + self.ex_int(d + 1)[0]) if r.random() < 0.6 else ""), 0)
        if c < 0.78: return ("replace(%s, %s, %s)" % (self.ex_str(d + 1)[0], Q(r.choice(["a", "", "b", "世", " "])), Q(r.choice(["X", "", "$&", "ab"]))), 0)
        if c < 0.83: return ("join(%s, %s)" % (self.ex_list("list[str]", d + 1)[0], Q(r.choice(SEPS + ["", "-"]))), 0)
        if c < 0.87: return ("reverse(%s)" % self.ex_str(d + 1)[0], 0)
        if c < 0.92: self.risky = True; return ("%s[%s]" % (self.ex_str_atom(d + 1), self.ex_int(d + 1)[0]), 0)
        if c < 0.96: self.risky = True; l = self.ex_list("list[str]", d + 1)[0]; return ("(%s)[%s]" % (l, self.ex_int(d + 1)[0]), 0)
        return ("(%s ? %s : %s)" % (self.ex_bool(d + 1), self.ex_str(d + 1)[0], self.ex_str(d + 1)[0]), 0)
    def ex_str_atom(self, d):
        vs = self.vars("str")
        return self.r.choice(vs) if vs and self.r.random() < 0.6 else Q(self.r.choice(SPOOL))
    def ex_bool(self, d=0):
        r = self.r; c = r.random(); vs = self.vars("bool")
        if d >= 3 or c < 0.1:
            if vs and r.random() < 0.6: return r.choice(vs)
            return r.choice(["true", "false"])
        if c < 0.25: return "(%s %s %s)" % (self.ex_int(d + 1)[0], r.choice(["==", "!=", "<", "<=", ">", ">="]), self.ex_int(d + 1)[0])
        if c < 0.35: return "(%s %s %s)" % (self.ex_float(d + 1)[0], r.choice(["==", "!=", "<", "<=", ">", ">="]), self.ex_float(d + 1)[0])
        if c < 0.47: return "(%s %s %s)" % (self.ex_str(d + 1)[0], r.choice(["==", "!=", "<", "<=", ">", ">="]), self.ex_str(d + 1)[0])
        if c < 0.55: return "%s(%s, %s)" % (r.choice(["contains", "startswith", "endswith"]), self.ex_str(d + 1)[0], Q(r.choice(["a", "", "ab", "世", "bc", " "])))
        if c < 0.60: return "(%s in %s)" % (self.ex_int(d + 1)[0], self.ex_list("list[int]", d + 1)[0])
        if c < 0.64: return "(%s in %s)" % (self.ex_str(d + 1)[0], self.ex_list("list[str]", d + 1)[0])
        if c < 0.68: return "(%s in %s)" % (Q(r.choice(["a", "b", "q"])), self.ex_map(d + 1))
        if c < 0.72: return "(%s %s %s)" % (self.ex_list("list[int]", d + 1)[0], r.choice(["==", "!="]), self.ex_list("list[int]", d + 1)[0])
        if c < 0.75: return "(%s %s %s)" % (self.ex_list("list[str]", d + 1)[0], r.choice(["==", "!="]), self.ex_list("list[str]", d + 1)[0])
        if c < 0.78: return "matches(%s, %s)" % (self.ex_str(d + 1)[0], Q(r.choice(PAT)))
        if c < 0.88: return "(%s %s %s)" % (self.ex_bool(d + 1), r.choice(["and", "or"]), self.ex_bool(d + 1))
        if c < 0.93: return "(not %s)" % self.ex_bool(d + 1)
        if c < 0.96: return "(%s == %s)" % (self.ex_bool(d + 1), self.ex_bool(d + 1))
        return "(len(%s) %s %d)" % (self.ex_list(r.choice(["list[int]", "list[str]"]), d + 1)[0], r.choice([">", "==", "<"]), r.randint(0, 3))
    def ex_list(self, t, d=0):
        r = self.r; c = r.random(); vs = self.vars(t)
        elem = (lambda dd: self.ex_int(dd)[0]) if t == "list[int]" else (lambda dd: self.ex_str(dd)[0])
        if d >= 3 or c < 0.25:
            if vs and r.random() < 0.6: return (r.choice(vs), 0)
            return ("[%s]" % ", ".join(elem(d + 1) for _ in range(r.randint(0, 4))), 0)
        if c < 0.40: return ("sort(%s)" % self.ex_list(t, d + 1)[0], 0)
        if c < 0.52: return ("reverse(%s)" % self.ex_list(t, d + 1)[0], 0)
        if c < 0.64: return ("slice(%s, %s%s)" % (self.ex_list(t, d + 1)[0], self.ex_int(d + 1)[0], (", " + self.ex_int(d + 1)[0]) if r.random() < 0.6 else ""), 0)
        if t == "list[str]":
            if c < 0.76: self.risky = True; return ("split(%s, %s)" % (self.ex_str(d + 1)[0], Q(r.choice(SEPS + [""]))), 0)
            if c < 0.86: return ("words(%s)" % self.ex_str(d + 1)[0], 0)
            if c < 0.93: return ("keys(%s)" % self.ex_map(d + 1), 0)
        return ("[%s]" % ", ".join(elem(d + 1) for _ in range(r.randint(0, 4))), 0)
    def ex_map(self, d=0):
        vs = self.vars("map[str,int]")
        if vs and self.r.random() < 0.7: return self.r.choice(vs)
        ks = self.r.sample(["a", "b", "10", "2", "zz", "世"], self.r.randint(0, 4))
        return "{%s}" % ", ".join('%s: %s' % (Q(k), self.ex_int(d + 1)[0]) for k in ks)
    def ex(self, t, d=0):
        return {"int": self.ex_int, "float": self.ex_float, "str": self.ex_str, "bool": lambda d: (self.ex_bool(d), 0),
                "list[int]": lambda d: self.ex_list("list[int]", d), "list[str]": lambda d: self.ex_list("list[str]", d), "map[str,int]": lambda d: (self.ex_map(d), 0)}[t](d)[0]
    # ---- instrucoes ----
    def guarded(self, stmts_fn):
        """emite as linhas de stmts_fn; se o texto gerado e arriscado, embrulha em try/catch"""
        self.risky = False; mark = len(self.lines); stmts_fn()
        if self.risky:
            body = self.lines[mark:]; del self.lines[mark:]
            e = self.new("e")
            self.emit("try"); self.ind += 1; self.lines.extend("  " + l for l in body); self.ind -= 1
            self.emit("catch %s" % e); self.emit('  print("err: " + errmsg(%s))' % e)
    def safe(self, fn, fallback):
        """expressao que nao levanta erro (para condicoes e limites de laco): tenta de novo ate nao ser arriscada"""
        for _ in range(12):
            self.risky = False; e = fn()
            if not self.risky: return e
        self.risky = False; return fallback
    def reduce_int(self, t): return "(%s) %% 100003" % t
    def stmt(self, d):
        r = self.r; c = r.random()
        if c < 0.20: self.st_let()
        elif c < 0.34: self.st_print()
        elif c < 0.46: self.st_assign()
        elif c < 0.54 and d < 2: self.st_if(d)
        elif c < 0.60 and d < 2: self.st_for(d)
        elif c < 0.64 and d < 2: self.st_each(d)
        elif c < 0.67 and d < 2: self.st_while(d)
        elif c < 0.73 and d < 2: self.st_try(d)
        elif c < 0.80: self.st_list()
        elif c < 0.85: self.st_map()
        elif c < 0.90 and self.cls: self.st_box()
        elif c < 0.93 and self.loops and d > 0: self.emit("if %s" % self.safe(lambda: self.ex_bool(2), "true")); self.emit("  " + r.choice(["break", "continue"]))
        else: self.st_print()
    def block(self, d, n):
        self.scopes.append([]); self.ind += 1; k0 = len(self.lines)
        for _ in range(n): self.stmt(d + 1)
        if len(self.lines) == k0: self.emit("print(\"empty\")")
        self.ind -= 1; self.scopes.pop()
    def st_let(self):
        t = self.r.choice(["int", "int", "float", "str", "bool", "list[int]", "list[str]", "map[str,int]"]); name = self.new()
        self.risky = False; e = self.ex(t); risky = self.risky
        if t == "int": e = self.reduce_int(e)
        if t == "float": e = "max(-100000.0, min(100000.0, %s))" % e
        ann = {"list[int]": ": list[int]", "list[str]": ": list[str]", "map[str,int]": ": map[str,int]"}.get(t, "") if self.r.random() < 0.5 or e.startswith("[") or e.startswith("{") else ""
        if not risky: self.emit("let %s%s = %s" % (name, ann, e))
        else:
            init = {"int": "0", "float": "0.0", "str": '""', "bool": "false", "list[int]": "[]", "list[str]": "[]", "map[str,int]": "{}"}[t]
            ann = ann or {"list[int]": ": list[int]", "list[str]": ": list[str]", "map[str,int]": ": map[str,int]"}.get(t, "")
            self.emit("let %s%s = %s" % (name, ann, init)); en = self.new("e")
            self.emit("try"); self.emit("  %s = %s" % (name, e)); self.emit("catch %s" % en); self.emit('  print("err: " + errmsg(%s))' % en)
        self.declare(name, t)
    def st_print(self):
        def f():
            t = self.r.choice(["int", "float", "str", "bool", "list[int]", "list[str]", "map[str,int]", "int", "str"]); e = self.ex(t)
            self.emit("print(%s)" % (e if t in ("int", "float", "str") else "str(%s)" % e))
        self.guarded(f)
    def st_assign(self):
        r = self.r; cands = [(t, v) for t in ("int", "float", "str", "bool", "list[int]", "list[str]") for v in self.vars(t, True)]
        if not cands: return self.st_let()
        t, v = r.choice(cands)
        def f():
            e = self.ex(t)
            if t == "int":
                op = r.choice(["=", "=", "+=", "-=", "*=", "%="])
                if op == "=": self.emit("%s = %s" % (v, self.reduce_int(e)))
                elif op == "*=": self.emit("%s = (%s * %s) %% 100003" % (v, v, self.ex_int(2)[0]) if False else "%s = %s" % (v, self.reduce_int("(%s %% 1000) * (%s %% 1000)" % (v, self.ex_int(2)[0]))))
                elif op == "%=": self.risky = True; self.emit("%s %%= %s" % (v, e))
                else: self.emit("%s %s %s" % (v, op, "(%s) %% 1000" % e)); self.emit("%s = %s %% 100003" % (v, v))
            elif t == "float":
                op = r.choice(["=", "+=", "-=", "/="])
                if op == "=": self.emit("%s = max(-100000.0, min(100000.0, %s))" % (v, e))
                elif op == "/=": self.risky = True; self.emit("%s /= %s" % (v, e))
                else: self.emit("%s %s max(-1000.0, min(1000.0, %s))" % (v, op, e))
            elif t == "str":
                self.emit("%s %s %s" % (v, r.choice(["=", "+="]), e)); self.emit("%s = slice(%s, 0, 40)" % (v, v))
            else: self.emit("%s = %s" % (v, e))
        self.guarded(f)
    def st_if(self, d):
        r = self.r
        self.emit("if %s" % self.safe(lambda: self.ex_bool(1), "true")); self.block(d, r.randint(1, 3))
        for _ in range(r.randint(0, 2)): self.emit("elif %s" % self.safe(lambda: self.ex_bool(1), "false")); self.block(d, r.randint(1, 2))
        if r.random() < 0.6: self.emit("else"); self.block(d, r.randint(1, 2))
    def st_for(self, d):
        r = self.r; i = self.new("i"); a = r.randint(-2, 3); b = r.choice([str(r.randint(0, 6)), "len(%s)" % self.ex_str(2)[0] if False else "min(6, len(%s))" % self.safe(lambda: self.ex_str(2)[0], Q("abc"))])
        step = r.choice(["", "", " step 2", " step -1", " step 3"])
        if "-" in step: a, b = r.randint(2, 6), str(r.randint(-2, 1))
        self.emit("for %s in %d..%s%s" % (i, a, b, step)); self.scopes.append([]); self.declare(i, "int", False); self.loops += 1; self.block(d, r.randint(1, 3)); self.loops -= 1; self.scopes.pop()
    def st_each(self, d):
        r = self.r; x = self.new("x"); k = r.choice(["list[int]", "list[str]", "str", "map"])
        self.scopes.append([]); self.loops += 1
        if k == "map":
            kk = self.new("k"); vv = self.new("w"); self.emit("for %s, %s in %s" % (kk, vv, self.safe(lambda: self.ex_map(2), '{"a": 1}'))); self.declare(kk, "str", False); self.declare(vv, "int", False)
            self.block(d, r.randint(1, 3))
        else:
            self.emit("for %s in %s" % (x, self.safe(lambda: self.ex_list(k, 2)[0] if k != "str" else self.ex_str(2)[0], "[1, 2]" if k == "list[int]" else '"ab"' if k == "str" else '["p", "q"]'))); self.declare(x, "int" if k == "list[int]" else "str", False)
            self.block(d, r.randint(1, 3))
        self.loops -= 1; self.scopes.pop()
    def st_while(self, d):
        w = self.new("w"); self.emit("let %s = 0" % w); self.emit("while %s < %d" % (w, self.r.randint(1, 4))); self.declare(w, "int", False)
        self.scopes.append([]); self.loops += 1; self.ind += 1; self.emit("%s += 1" % w); self.ind -= 1; self.block(d, self.r.randint(1, 2)); self.loops -= 1; self.scopes.pop()
    def st_try(self, d):
        r = self.r; e = self.new("e")
        self.emit("try"); self.scopes.append([]); self.ind += 1
        for _ in range(r.randint(1, 2)): self.stmt(d + 1)
        if r.random() < 0.6: self.emit("if %s" % self.safe(lambda: self.ex_bool(2), "true")); self.emit('  throw "boom " + %s' % self.safe(lambda: self.ex_str(2)[0], Q("x")))
        self.ind -= 1; self.scopes.pop()
        self.emit("catch %s" % e); self.emit('  print("caught " + errmsg(%s))' % e)
        if r.random() < 0.3: self.emit("finally"); self.emit('  print("fin")')
    def st_list(self):
        r = self.r; ls = self.vars("list[int]", True) + self.vars("list[str]", True)
        if not ls: return self.st_let()
        v = r.choice(ls); t = "list[int]" if v in self.vars("list[int]", True) else "list[str]"; el = "int" if t == "list[int]" else "str"
        def f():
            c = r.random()
            if c < 0.4: self.emit("push(%s, %s)" % (v, self.ex(el)))
            elif c < 0.55: self.risky = True; self.emit("print(%s(%s))" % ("str" if el == "int" else "", "pop(%s)" % v) if el == "int" else "print(pop(%s))" % v)
            elif c < 0.75: self.risky = True; self.emit("%s[%s] = %s" % (v, self.ex_int(2)[0], self.ex(el)))
            elif c < 0.85 and el == "int": self.risky = True; self.emit("%s[%s] += %s" % (v, self.ex_int(2)[0], "(%s) %% 1000" % self.ex_int(2)[0]))
            else: self.emit("%s = %s" % (v, self.ex(t)))
        self.guarded(f)
    def st_map(self):
        r = self.r; ms = self.vars("map[str,int]", True)
        if not ms: return self.st_let()
        v = r.choice(ms)
        def f():
            c = r.random(); k = Q(r.choice(["a", "b", "10", "2", "zz", "世", "k1"]))
            if c < 0.5: self.emit("%s[%s] = %s" % (v, k, self.reduce_int(self.ex_int(2)[0])))
            elif c < 0.8: self.risky = True; self.emit("%s[%s] += 1" % (v, k))
            else: self.emit("print(str(contains(%s, %s)))" % (v, k))
        self.guarded(f)
    def st_box(self):
        r = self.r; c = r.random()
        def f():
            if c < 0.5: self.emit("print(bx.add(%s))" % self.reduce_int(self.ex_int(2)[0]))
            elif c < 0.8: self.risky = True; self.emit("print(bx.at(%s))" % self.ex_int(2)[0])
            else: self.emit("print(bx.describe())")
        self.guarded(f)

def program(seed):
    r = random.Random(seed); g = G(r); out = []
    # funcoes auxiliares (int -> int), sem recursao: so chamam as anteriores
    for i in range(r.randint(0, 2)):
        name = "h%d" % i; dflt = r.choice(["", " = 3", " = -1"]); g.lines = []; g.ind = 1; g.scopes = [[]]; g.declare("a", "int", False); g.declare("b", "int", False)
        g.risky = False
        for _ in range(r.randint(1, 4)): g.stmt(1)
        risky = any("try" in l for l in g.lines)
        body = g.lines
        ret = g.reduce_int(g.safe(lambda: g.ex_int(1)[0], "1"))
        out.append("fn %s(a: int, b: int%s) -> int" % (name, dflt)); out.extend(body); out.append("  return %s" % ret)
        g.fns.append((name, dflt != "", True))
    if r.random() < 0.7:
        g.cls = True
        out += ["class Box", "  field items: list[int] = []", "  field total: int = 0", "  field label: str = \"bx\"", "  fn add(v: int) -> int", "    push(self.items, v)", "    self.total += v", "    return self.total",
                "  fn at(i: int) -> int", "    return self.items[i]", "  fn describe() -> str", "    return self.label + \":\" + str(self.total) + \":\" + str(len(self.items))"]
    g.lines = []; g.ind = 1; g.scopes = [[]]; g.n += 0
    if g.cls: g.emit("let bx = Box()"); g.declare("bx", "box", False)
    for _ in range(r.randint(10, 22)): g.stmt(0)
    out.append("fn main() -> int"); out.extend(g.lines); out.append("  return 0"); out.append("test main() == 0")
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    cli = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 200; seed0 = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    faildir = sys.argv[4] if len(sys.argv) > 4 else None; tsc = os.environ.get("UCGE_TSC")
    bad = invalid = 0; lines = 0
    with tempfile.TemporaryDirectory() as d:
        for s in range(seed0, seed0 + n):
            p = os.path.join(d, "f%d.csp" % s); text = program(s); open(p, "w", encoding="utf8").write(text)
            import subprocess
            chk = subprocess.run([cli, "check", p], capture_output=True, text=True)
            if chk.stdout.strip() != "ok":
                invalid += 1; print("INVALIDO seed %d: %s" % (s, chk.stdout.strip().splitlines()[0] if chk.stdout.strip() else chk.stderr[:100]))
                if faildir: os.makedirs(faildir, exist_ok=True); open(os.path.join(faildir, "invalid%d.csp" % s), "w", encoding="utf8").write(text)
                continue
            ok, msg = jsdiff.compare(cli, p, tsc)
            if ok: lines += int(msg.split()[0])
            else:
                bad += 1; print("FAIL seed %d: %s" % (s, msg))
                if faildir: os.makedirs(faildir, exist_ok=True); open(os.path.join(faildir, "fail%d.csp" % s), "w", encoding="utf8").write(text)
    print("programas %d, divergentes %d, invalidos (gerador) %d, linhas de saida comparadas %d, alvos: python js%s" % (n, bad, invalid, lines, " ts" if tsc else ""))
    sys.exit(1 if bad or invalid else 0)
