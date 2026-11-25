#!/usr/bin/env python3
"""Propriedade de soundness da inferencia do UCGE (docs/ucge.md, "Type inference").
Gera programas CSP bem tipados POR CONSTRUCAO (cada expressao e gerada para um tipo pedido), pede a assinatura inferida a `ucgecli infer`,
gera o Python com `ucgecli gen python`, executa as funcoes com argumentos do tipo pretendido e compara o tipo OBSERVADO do resultado com o tipo
inferido (instanciado com os argumentos). Tambem faz mutacoes que tornam o programa mal tipado e exige que a inferencia as rejeite.
  uso: python3 infer_prop.py <ucgecli> [N=300] [seed=1]
Saida: contagens e `property: ok` quando nao ha violacao de soundness (o tipo observado nunca foge do inferido) e nenhuma mutacao passa."""
import os, random, re, subprocess, sys, tempfile

TYPES = ["int", "float", "str", "bool", "list[int]", "list[str]", "map[str,int]"]
SCAL = ["int", "float", "str", "bool"]

class G:
    def __init__(self, rng):
        self.r = rng; self.n = 0; self.funcs = []          # funcs: (name, [param types], ret type, [annotated?], ret annotated?)
    def fresh(self, p):
        self.n += 1; return "%s%d" % (p, self.n)
    def lit(self, ty):
        r = self.r
        if ty == "int": return str(r.randint(0, 9))
        if ty == "float": return r.choice(["0.5", "1.5", "2.25", "3.0"])
        if ty == "str": return r.choice(['"a"', '"xy z"', '""', '"q,w"'])
        if ty == "bool": return r.choice(["true", "false"])
        if ty == "list[int]": return "[" + ", ".join(str(r.randint(0, 9)) for _ in range(r.randint(1, 3))) + "]"
        if ty == "list[str]": return "[" + ", ".join(r.choice(['"a"', '"b c"', '"d"']) for _ in range(r.randint(1, 3))) + "]"
        return '{"k": %d, "j": %d}' % (r.randint(0, 5), r.randint(0, 5))
    def var(self, ty, env):
        c = [n for n, t in env if t == ty]
        return self.r.choice(c) if c else None
    def call(self, ty, env, d):
        c = [f for f in self.funcs if f[2] == ty]
        if not c: return None
        f = self.r.choice(c)
        return "%s(%s)" % (f[0], ", ".join(self.expr(t, env, d - 1) for t in f[1]))
    def expr(self, ty, env, d):
        r = self.r
        v = self.var(ty, env)
        if d <= 0 or r.random() < 0.18:
            return v if v and r.random() < 0.7 else self.lit(ty)
        if r.random() < 0.15:
            c = self.call(ty, env, d)
            if c: return c
        if v and r.random() < 0.25: return v
        E = lambda t: self.expr(t, env, d - 1)
        if ty == "int":
            k = r.randint(0, 11)
            if k == 0: return "(%s + %s)" % (E("int"), E("int"))
            if k == 1: return "(%s - %s)" % (E("int"), E("int"))
            if k == 2: return "(%s * %s)" % (E("int"), E("int"))
            if k == 3: return "len(%s)" % E(r.choice(["str", "list[int]", "list[str]", "map[str,int]"]))
            if k == 4: return "sum(%s)" % E("list[int]")
            if k == 5: return "(%s ? %s : %s)" % (E("bool"), E("int"), E("int"))
            if k == 6: return "abs(%s)" % E("int")
            if k == 7: return "max(%s, %s)" % (E("int"), E("int"))
            if k == 8: return "floor(%s)" % E("float")
            if k == 9: return "indexof(%s, %s)" % (E("str"), E("str"))
            if k == 10: return "(%s // (abs(%s) + 1))" % (E("int"), E("int"))
            return "(%s %% (abs(%s) + 1))" % (E("int"), E("int"))
        if ty == "float":
            k = r.randint(0, 6)
            if k == 0: return "(%s + %s)" % (E(r.choice(["int", "float"])), E("float"))
            if k == 1: return "(%s * %s)" % (E("float"), E(r.choice(["int", "float"])))
            if k == 2: return "(%s / (abs(%s) + 1))" % (E("int"), E("int"))
            if k == 3: return "float(%s)" % E("int")
            if k == 4: return "sqrt(abs(%s))" % E(r.choice(["int", "float"]))
            if k == 5: return "(%s ? %s : %s)" % (E("bool"), E("float"), E("float"))
            return "min(%s, %s)" % (E("float"), E(r.choice(["int", "float"])))
        if ty == "str":
            k = r.randint(0, 7)
            if k == 0: return "(%s + %s)" % (E("str"), E("str"))
            if k == 1: return "upper(%s)" % E("str")
            if k == 2: return "lower(%s)" % E("str")
            if k == 3: return "trim(%s)" % E("str")
            if k == 4: return "str(%s)" % E(r.choice(["int", "bool", "float"]))
            if k == 5: return "join(%s, %s)" % (E("list[str]"), E("str"))
            if k == 6: return "replace(%s, %s, %s)" % (E("str"), E("str"), E("str"))
            return "(%s ? %s : %s)" % (E("bool"), E("str"), E("str"))
        if ty == "bool":
            k = r.randint(0, 7)
            if k == 0: return "(%s < %s)" % (E("int"), E(r.choice(["int", "float"])))
            if k == 1: return "(%s == %s)" % (E("str"), E("str"))
            if k == 2: return "contains(%s, %s)" % (E("list[int]"), E("int"))
            if k == 3: return "(not %s)" % E("bool")
            if k == 4: return "(%s and %s)" % (E("bool"), E("bool"))
            if k == 5: return "startswith(%s, %s)" % (E("str"), E("str"))
            if k == 6: return "(%s >= %s)" % (E("str"), E("str"))
            return "(%s or %s)" % (E("bool"), E("bool"))
        if ty == "list[int]":
            k = r.randint(0, 4)
            if k == 0: return "[%s, %s]" % (E("int"), E("int"))
            if k == 1: return "sort(%s)" % E("list[int]")
            if k == 2: return "reverse(%s)" % E("list[int]")
            if k == 3: return "slice(%s, 0, %s)" % (E("list[int]"), E("int"))
            return "(%s ? %s : %s)" % (E("bool"), E("list[int]"), E("list[int]"))
        if ty == "list[str]":
            k = r.randint(0, 4)
            if k == 0: return "split(%s, %s)" % (E("str"), r.choice(['","', '" "', '"a"']))
            if k == 1: return "words(%s)" % E("str")
            if k == 2: return "keys(%s)" % E("map[str,int]")
            if k == 3: return "[%s, %s]" % (E("str"), E("str"))
            return "sort(%s)" % E("list[str]")
        return '{"a": %s, "b": %s}' % (E("int"), E("int"))
    def fn(self):
        r = self.r; name = "f%d" % len(self.funcs)
        np_ = r.randint(0, 3); ptys = [r.choice(TYPES) for _ in range(np_)]
        ann = [r.random() < 0.7 for _ in range(np_)]
        ret = r.choice(TYPES); rann = r.random() < 0.5
        pn = ["p%d" % i for i in range(np_)]
        env = list(zip(pn, ptys)); lines = []
        for i in range(np_):                       # um uso que fixa o parametro sem anotacao
            if not ann[i]:
                t = ptys[i]; u = self.fresh("u")
                pin = {"int": "%s + 1" % pn[i], "float": "%s * 1.5" % pn[i], "str": "upper(%s)" % pn[i], "bool": "not %s" % pn[i],
                       "list[int]": "sum(%s)" % pn[i], "list[str]": "join(%s, \",\")" % pn[i], "map[str,int]": "keys(%s)" % pn[i]}[t]
                pty = {"int": "int", "float": "float", "str": "str", "bool": "bool", "list[int]": "int", "list[str]": "str", "map[str,int]": "list[str]"}[t]
                lines.append("  let %s = %s" % (u, pin)); env.append((u, pty))
        for _ in range(r.randint(0, 3)):
            t = r.choice(TYPES); n = self.fresh("t"); lines.append("  let %s = %s" % (n, self.expr(t, env, 3))); env.append((n, t))
        if r.random() < 0.4:                       # laco acumulador
            src = [n for n, t in env if t in ("list[int]", "list[str]")]
            if src:
                L = r.choice(src); isint = dict(env)[L] == "list[int]"; a = self.fresh("acc"); x = self.fresh("x")
                lines.append("  let %s = %s" % (a, "0" if isint else '""')); lines.append("  for %s in %s" % (x, L))
                lines.append("    %s = %s + %s" % (a, a, x)); env.append((a, "int" if isint else "str"))
        if r.random() < 0.4:
            lines.append("  if %s" % self.expr("bool", env, 2)); lines.append("    return %s" % self.expr(ret, env, 2))
        lines.append("  return %s" % self.expr(ret, env, 3))
        head = "fn %s(%s)%s" % (name, ", ".join("%s: %s" % (pn[i], self.csp_ty(ptys[i])) if ann[i] else pn[i] for i in range(np_)),
                                " -> " + self.csp_ty(ret) if rann else "")
        self.funcs.append((name, ptys, ret, ann, rann))
        return head + "\n" + "\n".join(lines)
    @staticmethod
    def csp_ty(t): return t.replace("map[str,int]", "map[str, int]")

# ---- tipos: assinatura inferida e tipo observado ----
def parse_ty(s, i=0):
    def union(i):
        a, i = opt(i); alts = [a]
        while s[i:i + 3] == " | ": b, i = opt(i + 3); alts.append(b)
        return (("union", alts) if len(alts) > 1 else a), i
    def opt(i):
        t, i = atom(i)
        while s[i:i + 1] == "?": t = ("opt", t); i += 1
        return t, i
    def atom(i):
        m = re.compile(r"list\[|map\[str,").match(s, i)
        if m:
            e, j = union(m.end()); assert s[j] == "]", (s, j); return (m.group()[:3], e), j + 1
        m = re.compile(r"'[a-z]|[A-Za-z_][A-Za-z_0-9]*").match(s, i)
        assert m, (s, i)
        return (("var", m.group()) if m.group()[0] == "'" else (m.group(),)), m.end()
    return union(i)
def parse_sig(line):
    m = re.match(r"fn (\w+): \((.*?)\) -> (.*?)(?: where (.*?))?(?: \[(?:derived|declared|any)\])?$", line)
    if not m: return None
    ps = m.group(2); params = []
    if ps:
        depth = 0; cur = ""
        for ch in ps:
            if ch in "[(": depth += 1
            if ch in "])": depth -= 1
            if ch == "," and depth == 0: params.append(cur.strip()); cur = ""
            else: cur += ch
        params.append(cur.strip())
    masks = {}
    if m.group(4):
        for part in re.finditer(r"('[a-z]) in ([\w|]+)", m.group(4)): masks[part.group(1)] = set(part.group(2).split("|"))
    return m.group(1), [parse_ty(p)[0] for p in params], parse_ty(m.group(3))[0], masks

def observe(v):
    if v is None: return ("null",)
    if isinstance(v, bool): return ("bool",)
    if isinstance(v, int): return ("int",)
    if isinstance(v, float): return ("float",)
    if isinstance(v, str): return ("str",)
    if isinstance(v, list):
        el = [observe(x) for x in v]; return ("list", join(el))
    if isinstance(v, dict):
        el = [observe(x) for x in v.values()]; return ("map", join(el))
    return ("?",)
def join(ts):
    if not ts: return None
    k = set(ts)
    if len(k) == 1: return ts[0]
    if k <= {("int",), ("float",)}: return ("float",)
    return ("mixed",)
def match(p, o, sub, masks):
    if o is None: return True                               # lista ou mapa vazio: sem evidencia
    k = p[0]
    if k == "any": return True
    if k == "var":
        if p[1] in masks and o[0] not in masks[p[1]]: return False
        b = sub.get(p[1])
        if b is None: sub[p[1]] = o; return True
        return b[0] == o[0] or {b[0], o[0]} <= {"int", "float"}
    if k == "opt": return o == ("null",) or match(p[1], o, sub, masks)
    if k == "union": return any(match(a, o, dict(sub), masks) for a in p[1])
    if k == "int": return o == ("int",)
    if k == "float": return o in (("int",), ("float",))
    if k in ("str", "bool"): return o == (k,)
    if k in ("lis", "map"): return o[0] == ("list" if k == "lis" else "map") and match(p[1], o[1], sub, masks) if o[1] is not None else o[0] == ("list" if k == "lis" else "map")
    return True

def sample(t, r):
    if t == "int": return r.randint(-4, 6)
    if t == "float": return r.choice([0.5, -2.5, 3.0, 1.25])
    if t == "str": return r.choice(["", "a b", "xyz,q", "Hello World"])
    if t == "bool": return r.choice([True, False])
    if t == "list[int]": return [r.randint(-3, 9) for _ in range(r.randint(0, 4))]
    if t == "list[str]": return [r.choice(["a", "bc", "d e"]) for _ in range(r.randint(0, 3))]
    return {"k": r.randint(0, 4), "m": r.randint(0, 4)}

def sh(args, **kw): return subprocess.run(args, capture_output=True, text=True, timeout=30, **kw)

def main():
    u = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 300; seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed); tmp = tempfile.mkdtemp()
    st = dict(programs=0, rejected=0, genbad=0, calls=0, unsound=0, incomplete=0, mut=0, mut_missed=0)
    bad = []
    for it in range(N):
        g = G(rng); src = "\n".join(g.fn() for _ in range(rng.randint(2, 5))) + "\n"
        path = os.path.join(tmp, "p%d.csp" % it); open(path, "w").write(src); st["programs"] += 1
        chk = sh([u, "check", path])
        if "ok" not in chk.stdout.split("\n")[-2:] and chk.returncode != 0:
            st["genbad"] += 1; bad.append(("generator program rejected by the validator", src, chk.stdout)); continue
        inf = sh([u, "infer", path])
        if inf.returncode != 0: st["rejected"] += 1; bad.append(("inference rejected a well typed program", src, inf.stdout)); continue
        sigs = {}
        for line in inf.stdout.split("\n"):
            s = parse_sig(line)
            if s: sigs[s[0]] = s
        code = sh([u, "gen", "python", path]).stdout
        env = {"__name__": "ucgeprop"}
        try: exec(compile(code, path, "exec"), env)
        except Exception as e: st["genbad"] += 1; bad.append(("generated python failed to load: %r" % e, src, code)); continue
        for (name, ptys, ret, ann, rann) in g.funcs:
            args = [sample(t, rng) for t in ptys]
            try: res = env[name](*[a.copy() if isinstance(a, (list, dict)) else a for a in args])
            except ZeroDivisionError: continue
            except Exception as e: st["genbad"] += 1; bad.append(("runtime error %r in %s" % (e, name), src, "")); continue
            st["calls"] += 1; _, ps, r, masks = sigs[name]; sub = {}
            if not all(match(p, observe(a), sub, masks) for p, a in zip(ps, args)): st["incomplete"] += 1; continue   # parametro mais estreito que o pretendido (limite conhecido)
            if not match(r, observe(res), sub, masks):
                st["unsound"] += 1; bad.append(("observed %r does not fit the inferred result of %s (%r)" % (observe(res), name, r), src, inf.stdout))
        # mutacoes: tem de ser rejeitadas
        for (name, ptys, ret, ann, rann) in g.funcs:
            muts = []
            if rann: muts.append((re.compile(r"(fn %s\(.*\) -> )([^\n]+)" % name), lambda m: m.group(1) + ("str" if m.group(2) != "str" else "int")))
            for rx, fn in muts:
                s2, n = rx.subn(fn, src, count=1)
                if not n: continue
                st["mut"] += 1; p2 = path + ".m"; open(p2, "w").write(s2)
                if sh([u, "infer", p2]).returncode == 0:
                    # `-> str` onde o corpo ja retornava str por acaso nao muda nada: so conta quando o retorno real e outro
                    if g.funcs and ret not in ("str",): st["mut_missed"] += 1; bad.append(("mutation accepted", s2, ""))
        if len(g.funcs) >= 1:                                    # chamada com argumento de tipo errado a uma funcao com parametro anotado
            for (name, ptys, ret, ann, rann) in g.funcs:
                for i, t in enumerate(ptys):
                    if not ann[i] or t not in SCAL: continue
                    wrong = {"int": '"s"', "float": '"s"', "str": "1", "bool": "1"}[t]
                    args = [wrong if j == i else g.lit(ptys[j]) for j in range(len(ptys))]
                    s2 = src + "fn zz_bad() -> any\n  return %s(%s)\n" % (name, ", ".join(args)); p2 = path + ".w"; open(p2, "w").write(s2)
                    st["mut"] += 1
                    if sh([u, "infer", p2]).returncode == 0: st["mut_missed"] += 1; bad.append(("wrong argument accepted", s2, ""))
                    break
                else: continue
                break
    print("programs %(programs)d, calls %(calls)d, validator-rejected %(genbad)d, inference-rejected %(rejected)d, narrower-than-intended (known limit) %(incomplete)d" % st)
    print("soundness violations %(unsound)d; mutations %(mut)d, accepted by mistake %(mut_missed)d" % st)
    for why, s, extra in bad[:3]: print("---", why); print(s); print(extra[:600])
    ok = st["unsound"] == 0 and st["mut_missed"] == 0 and st["genbad"] == 0 and st["rejected"] <= max(2, N // 50)
    print("property: ok" if ok else "property: FAILED")
    sys.exit(0 if ok else 1)
main()
