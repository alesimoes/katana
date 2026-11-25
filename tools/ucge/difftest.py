#!/usr/bin/env python3
"""Teste diferencial do backend C do UCGE: gera programas CSP validos e pequenos ao acaso, gera Python e C a partir do mesmo
programa, roda os dois e compara a saida inteira (as linhas impressas pelo programa e o resultado dos `test`).
Uso: python3 tools/ucge/difftest.py UCGECLI [-n 200] [--seed 1] [--san] [--keep DIR] [--only SEED]
  UCGECLI = o binario de tools/ucge/ucgecli.c.  --san compila o C com ASan e UBSan (e sai com erro se qualquer um acusar).
Cada programa usa: aritmetica inteira com piso, listas, mapas, strings, floats, loops, try/catch/finally, break/continue,
funcoes e classes. O gerador mantem a validade pelo tipo (nao ha rejeicao por tentativa): um programa recusado pelo validador
e contado como "invalid" e e um defeito do gerador."""
import random, subprocess, sys, os, tempfile, argparse, shutil, concurrent.futures as cf

class G:
    def __init__(s, seed):
        s.r = random.Random(seed); s.n = 0; s.lines = []; s.ind = 0
        s.scopes = [[]]            # (nome, tipo, mutavel)
        s.loops = 0; s.frozen = set(); s.funcs = []; s.fn_ret = None; s.depth = 0
    # ---- ambiente ----
    def new(s, p="v"): s.n += 1; return "%s%d" % (p, s.n)
    def push(s): s.scopes.append([])
    def pop(s): s.scopes.pop()
    def declare(s, name, ty): s.scopes[-1].append((name, ty))
    def vars(s, ty, writable=False):
        out = [n for sc in s.scopes for (n, t) in sc if t == ty]
        if writable: out = [n for n in out if n not in s.frozen]
        return out
    def emit(s, t): s.lines.append("  " * s.ind + t)
    # ---- expressoes ----
    def lit_int(s): return str(s.r.choice([0, 1, 2, 3, 5, 7, 10, 13, 100, -1, -4, -9, 25]))
    def lit_str(s): return '"%s"' % s.r.choice(["", "a", "ab", "abc", "hello", "x y", " pad ", "A,B,C", "the cat", "Zed", "1", "42", "k"])
    def e_int(s, d=0):
        r = s.r; vs = s.vars("int"); ch = r.random()
        if d > 2 or ch < 0.18: return r.choice(vs) if vs and r.random() < 0.65 else s.lit_int()
        k = r.randrange(16)
        a = lambda: s.e_int(d + 1)
        if k == 0: return "(%s + %s)" % (a(), a())
        if k == 1: return "(%s - %s)" % (a(), a())
        if k == 2: return "((%s * %s) %% 10007)" % (a(), a())
        if k == 3: return "(%s // %s)" % (a(), s.nonzero(d))
        if k == 4: return "(%s %% %s)" % (a(), s.nonzero(d))
        if k == 5: return "abs(%s)" % a()
        if k == 6: return "min(%s, %s)" % (a(), a())
        if k == 7: return "max(%s, %s)" % (a(), a())
        if k == 8: return "len(%s)" % s.e_str(d + 1)
        if k == 9: return "len(%s)" % s.e_list(d + 1)
        if k == 10: return "(%s ? %s : %s)" % (s.e_bool(d + 1), a(), a())
        if k == 11: return "sum(%s)" % s.e_list(d + 1)
        if k == 12: return "indexof(%s, %s)" % (s.e_str(d + 1), s.lit_str())
        if k == 13: return "int(str(%s))" % a()
        if k == 14: return "floor(%s)" % s.e_float(d + 1)
        if k == 15 and s.funcs: return s.call_int(d)
        return a()
    def nonzero(s, d):
        r = s.r
        if r.random() < 0.6: return str(r.choice([1, 2, 3, 4, 7, -2, -3, -5]))
        return "(abs(%s) + 1)" % s.e_int(d + 1)
    def call_int(s, d):
        name, nargs = s.r.choice(s.funcs)
        return "%s(%s)" % (name, ", ".join(s.e_int(d + 1) for _ in range(nargs)))
    def e_float(s, d=0):
        r = s.r; vs = s.vars("float")
        if d > 2 or r.random() < 0.3: return r.choice(vs) if vs and r.random() < 0.6 else r.choice(["0.5", "1.25", "2.0", "-3.5", "10.0", "0.1"])
        k = r.randrange(7)
        a = lambda: s.e_float(d + 1)
        if k == 0: return "(%s + %s)" % (a(), a())
        if k == 1: return "(%s - %s)" % (a(), a())
        if k == 2: return "(%s * %s)" % (a(), a())
        if k == 3: return "(%s / %s)" % (s.e_int(d + 1), s.nonzero(d))
        if k == 4: return "float(%s)" % s.e_int(d + 1)
        if k == 5: return "abs(%s)" % a()
        return "(%s * 0.5)" % a()
    def e_bool(s, d=0):
        r = s.r; vs = s.vars("bool")
        if d > 2 or r.random() < 0.15: return r.choice(vs) if vs and r.random() < 0.5 else r.choice(["true", "false"])
        k = r.randrange(11)
        a = lambda: s.e_int(d + 1)
        if k == 0: return "(%s %s %s)" % (a(), r.choice(["<", "<=", ">", ">=", "==", "!="]), a())
        if k == 1: return "(%s and %s)" % (s.e_bool(d + 1), s.e_bool(d + 1))
        if k == 2: return "(%s or %s)" % (s.e_bool(d + 1), s.e_bool(d + 1))
        if k == 3: return "(not %s)" % s.e_bool(d + 1)
        if k == 4: return "(%s %s %s)" % (s.e_str(d + 1), r.choice(["==", "!=", "<", ">="]), s.e_str(d + 1))
        if k == 5: return "contains(%s, %s)" % (s.e_str(d + 1), s.lit_str())
        if k == 6: return "(%s in %s)" % (a(), s.e_list(d + 1))
        if k == 7: return "startswith(%s, %s)" % (s.e_str(d + 1), s.lit_str())
        if k == 8: return "endswith(%s, %s)" % (s.e_str(d + 1), s.lit_str())
        if k == 9: return "(%s < %s)" % (s.e_float(d + 1), s.e_float(d + 1))
        if k == 10 and s.vars("map"): return "contains(%s, %s)" % (r.choice(s.vars("map")), s.lit_str())
        return "(%s > %s)" % (a(), a())
    def e_str(s, d=0):
        r = s.r; vs = s.vars("str")
        if d > 2 or r.random() < 0.25: return r.choice(vs) if vs and r.random() < 0.6 else s.lit_str()
        k = r.randrange(11)
        if k == 0: return "(%s + %s)" % (s.e_str(d + 1), s.e_str(d + 1))
        if k == 1: return "upper(%s)" % s.e_str(d + 1)
        if k == 2: return "lower(%s)" % s.e_str(d + 1)
        if k == 3: return "trim(%s)" % s.e_str(d + 1)
        if k == 4: return "str(%s)" % s.e_int(d + 1)
        if k == 5: return "slice(%s, %s, %s)" % (s.e_str(d + 1), r.choice(["0", "1", "-2", "2"]), r.choice(["1", "3", "-1", "9"]))
        if k == 6: return "replace(%s, %s, %s)" % (s.e_str(d + 1), r.choice(['"a"', '" "', '"l"', '""']), s.lit_str())
        if k == 7: return 'join(split(%s, %s), %s)' % (s.e_str(d + 1), r.choice(['","', '" "', '"a"']), r.choice(['"-"', '""', '"+"']))
        if k == 8: return "reverse(%s)" % s.e_str(d + 1)
        if k == 9: return "str(%s)" % s.e_float(d + 1)
        return "(%s ? %s : %s)" % (s.e_bool(d + 1), s.e_str(d + 1), s.e_str(d + 1))
    def e_list(s, d=0):
        r = s.r; vs = s.vars("list")
        if d > 2 or r.random() < 0.3:
            if vs and r.random() < 0.6: return r.choice(vs)
            return "[%s]" % ", ".join(s.e_int(3) for _ in range(r.randrange(0, 5)))
        k = r.randrange(6)
        if k == 0: return "sort(%s)" % s.e_list(d + 1)
        if k == 1: return "reverse(%s)" % s.e_list(d + 1)
        if k == 2: return "slice(%s, %s, %s)" % (s.e_list(d + 1), r.choice(["0", "1", "-2"]), r.choice(["2", "-1", "7"]))
        if k == 3: return "[%s, %s]" % (s.e_int(d + 1), s.e_int(d + 1))
        if k == 4 and s.funcs_list: return "%s(%s)" % (s.funcs_list[0], s.e_list(d + 1))
        return "[%s]" % ", ".join(s.e_int(3) for _ in range(r.randrange(0, 4)))
    funcs_list = []
    # ---- instrucoes ----
    def block(s, n, ctx=""):
        s.ind += 1; s.push()
        if not n: n = 1
        for _ in range(n): s.stmt(ctx)
        s.pop(); s.ind -= 1
    def pr(s):
        r = s.r; k = r.randrange(7)
        e = [lambda: "print(%s)" % s.e_int(), lambda: "print(%s)" % s.e_str(), lambda: "print(%s)" % s.e_bool(), lambda: "print(%s)" % s.e_list(),
             lambda: "print(%s)" % s.e_float(), lambda: "print(str(%s) + \"|\" + %s)" % (s.e_int(), s.e_str()),
             lambda: ("print(%s)" % r.choice(s.vars("map"))) if s.vars("map") else "print(1)"][k]
        s.emit(e())
    def stmt(s, ctx=""):
        r = s.r; s.depth += 1; d = s.depth
        ch = r.random()
        if d > 3: ch = r.choice([0.05, 0.1, 0.4, 0.5])
        try:
            if ch < 0.12:
                n = s.new(); ty = r.choice(["int", "int", "int", "str", "bool", "list", "float", "map"])
                if ty == "int": s.emit("let %s = %s" % (n, s.e_int()))
                elif ty == "str": s.emit("let %s = %s" % (n, s.e_str()))
                elif ty == "bool": s.emit("let %s = %s" % (n, s.e_bool()))
                elif ty == "float": s.emit("let %s = %s" % (n, s.e_float()))
                elif ty == "list": s.emit("let %s: list[int] = %s" % (n, s.e_list()))
                else: s.emit('let %s = {"a": %s, "b": %s}' % (n, s.e_int(2), s.e_int(2)))
                s.declare(n, ty)
            elif ch < 0.24:
                vs = s.vars("int", True)
                if vs: s.emit("%s %s %s" % (r.choice(vs), r.choice(["=", "+=", "-=", "*="]) if False else r.choice(["=", "+=", "-="]), s.e_int()))
                else: s.pr()
            elif ch < 0.28:
                vs = s.vars("str", True)
                if vs: s.emit("%s %s %s" % (r.choice(vs), r.choice(["=", "+="]), s.e_str()))
                else: s.pr()
            elif ch < 0.31:
                vs = s.vars("float", True)
                if vs: s.emit("%s %s %s" % (r.choice(vs), r.choice(["=", "+=", "*="]), s.e_float()))
                else: s.pr()
            elif ch < 0.40:
                s.pr()
            elif ch < 0.48:
                vs = s.vars("list", True)
                if vs:
                    v = r.choice(vs); k = r.randrange(4)
                    if k == 0 or k == 1: s.emit("push(%s, %s)" % (v, s.e_int()))
                    elif k == 2: s.emit("if len(%s) > 0" % v); s.ind += 1; s.emit("print(pop(%s))" % v); s.ind -= 1
                    else:
                        s.emit("if len(%s) > 1" % v); s.ind += 1; s.emit("%s[%s] = %s" % (v, "0" if r.random() < 0.5 else "len(%s) - 1" % v, s.e_int())); s.ind -= 1
                else: s.pr()
            elif ch < 0.56:
                s.emit("if %s" % s.e_bool()); s.block(r.randrange(1, 3), ctx)
                for _ in range(r.randrange(0, 2)): s.emit("elif %s" % s.e_bool()); s.block(r.randrange(1, 3), ctx)
                if r.random() < 0.6: s.emit("else"); s.block(r.randrange(1, 3), ctx)
            elif ch < 0.64:
                w = s.new("w"); k = r.randrange(0, 6)
                s.emit("let %s = 0" % w); s.declare(w, "int"); s.frozen.add(w)
                s.emit("while %s < %d" % (w, k)); s.ind += 1; s.emit("%s += 1" % w); s.ind -= 1
                s.loops += 1; s.block(r.randrange(1, 3), "loop"); s.loops -= 1
            elif ch < 0.72:
                i = s.new("i"); lo = r.randrange(-1, 3); hi = r.randrange(0, 6)
                st = r.choice(["", "", " step 2", " step -1"])
                if "-1" in st: s.emit("for %s in %d..%d%s" % (i, hi, lo, st))
                else: s.emit("for %s in %d..%d%s" % (i, lo, hi, st))
                s.push(); s.declare(i, "int"); s.frozen.add(i); s.loops += 1; s.block(r.randrange(1, 3), "loop"); s.loops -= 1; s.pop()
            elif ch < 0.78:
                vs = s.vars("list"); sv = s.vars("str"); mv = s.vars("map")
                pick = r.choice([k for k, a in (("l", vs), ("s", sv), ("m", mv)) if a] or ["x"])
                if pick == "x": s.pr()
                elif pick == "l":
                    v = r.choice(vs); x = s.new("x"); s.emit("for %s in %s" % (x, v)); s.push(); s.declare(x, "int"); s.frozen.add(x); s.frozen.add(v)
                    s.loops += 1; s.block(r.randrange(1, 3), "loop"); s.loops -= 1; s.frozen.discard(v); s.pop()
                elif pick == "s":
                    v = r.choice(sv); x = s.new("c"); s.emit("for %s in %s" % (x, v)); s.push(); s.declare(x, "str"); s.frozen.add(x)
                    s.loops += 1; s.block(r.randrange(1, 3), "loop"); s.loops -= 1; s.pop()
                else:
                    v = r.choice(mv); k = s.new("k"); x = s.new("x"); s.emit("for %s, %s in %s" % (k, x, v)); s.push(); s.declare(k, "str"); s.declare(x, "int"); s.frozen.add(x)
                    s.loops += 1; s.block(r.randrange(1, 3), "loop"); s.loops -= 1; s.pop()
            elif ch < 0.82:
                mv = s.vars("map", True)
                if mv:
                    v = r.choice(mv); k = s.lit_str()
                    if r.random() < 0.5: s.emit("%s[%s] = %s" % (v, k, s.e_int()))
                    else:
                        s.emit("if contains(%s, %s)" % (v, k)); s.ind += 1; s.emit("%s[%s] += %s" % (v, k, s.e_int())); s.ind -= 1
                    s.emit("print(keys(%s))" % v)
                else: s.pr()
            elif ch < 0.92:
                s.try_stmt(ctx)
            elif ch < 0.95 and ctx == "loop":
                s.emit("if %s" % s.e_bool()); s.ind += 1; s.emit(r.choice(["break", "continue"])); s.ind -= 1
            elif ch < 0.97 and s.fn_ret == "int":
                s.emit("if %s" % s.e_bool()); s.ind += 1; s.emit("return %s" % s.e_int()); s.ind -= 1
            else: s.pr()
        finally: s.depth -= 1
    def try_stmt(s, ctx):
        r = s.r; has_c = r.random() < 0.8; risky = has_c and r.random() < 0.6
        s.emit("try"); s.ind += 1; s.push()
        n = r.randrange(1, 3)
        for _ in range(n): s.stmt(ctx)
        if risky:
            k = r.randrange(4)
            if k == 0: s.emit("print(%s // %s)" % (s.e_int(2), s.e_int(2)))
            elif k == 1: s.emit("print(%s %% %s)" % (s.e_int(2), s.e_int(2)))
            elif k == 2: s.emit("print(%s[%s])" % (s.e_list(2), s.e_int(2)))
            else: s.emit('print(int("x%s"))' % r.choice(["1", "y"]))
        else:
            s.emit("if %s" % s.e_bool()); s.ind += 1; s.emit('throw "boom%d"' % r.randrange(100)); s.ind -= 1
        s.pop(); s.ind -= 1
        has_f = (not has_c) or r.random() < 0.4
        if has_c:
            e = s.new("e"); s.emit("catch %s" % e); s.ind += 1; s.push()
            s.emit('print("caught " + %s)' % (("errmsg(%s)" % e) if not risky else '"x"'))
            if r.random() < 0.5: s.stmt(ctx)
            s.pop(); s.ind -= 1
        if has_f:
            s.emit("finally"); s.ind += 1; s.push(); s.emit('print("fin")'); s.pop(); s.ind -= 1
    # ---- programa ----
    def gen(s):
        r = s.r; out = []
        # funcoes auxiliares
        nf = r.randrange(0, 3)
        for k in range(nf):
            name = "f%d" % k; a, b = s.new("p"), s.new("p")
            s.lines = []; s.ind = 0; s.scopes = [[]]; s.fn_ret = "int"
            s.emit("fn %s(%s: int, %s: int) -> int" % (name, a, b)); s.ind += 1; s.push(); s.declare(a, "int"); s.declare(b, "int"); s.frozen |= {a, b}
            for _ in range(r.randrange(1, 4)): s.stmt()
            s.emit("return %s" % s.e_int()); s.pop(); s.ind -= 1
            out.append("\n".join(s.lines)); s.funcs.append((name, 2)); s.fn_ret = None
        if r.random() < 0.5:
            s.lines = []; s.ind = 0; s.scopes = [[]]
            s.emit("fn lf(xs: list[int]) -> list[int]"); s.ind += 1
            s.emit("let out: list[int] = []"); s.emit("for x in xs"); s.ind += 1; s.emit("push(out, x * 2 + 1)"); s.ind -= 1
            s.emit("return out"); s.ind -= 1
            out.append("\n".join(s.lines)); s.funcs_list = ["lf"]
        if r.random() < 0.4:
            out.append("class Acc\n  field total: int = 0\n  field log: list[int] = []\n  fn add(x: int) -> int\n    self.total += x\n    push(self.log, x)\n    return self.total\n  fn last() -> int\n    if len(self.log) == 0\n      throw \"empty\"\n    return self.log[len(self.log) - 1]")
            s.use_class = True
        else: s.use_class = False
        s.lines = []; s.ind = 0; s.scopes = [[]]; s.fn_ret = "int"
        s.emit("fn run() -> int"); s.ind += 1; s.push()
        for _ in range(r.randrange(4, 10)): s.stmt()
        if s.use_class:
            s.emit("let acc = Acc()"); s.emit("acc.add(%s)" % s.e_int(2)); s.emit("print(acc.add(%s))" % s.e_int(2))
            s.emit("try"); s.emit("  print(acc.last())"); s.emit("catch e"); s.emit('  print("none")')
        s.emit("return %s" % s.e_int()); s.pop(); s.ind -= 1
        out.append("\n".join(s.lines))
        out.append("test run() >= -1000000000")
        return "\n".join(out) + "\n"

def one(args):
    seed, ucgecli, san, keep = args
    spec = G(seed).gen()
    d = tempfile.mkdtemp(prefix="ucgediff")
    try:
        sp = os.path.join(d, "p.csp"); open(sp, "w").write(spec)
        def sh(cmd, t=30):
            try: return subprocess.run(cmd, capture_output=True, text=True, timeout=t, cwd=d)
            except subprocess.TimeoutExpired: return None
        chk = sh([ucgecli, "check", sp])
        if chk is None or "ok" not in chk.stdout: return (seed, "invalid", spec + "\n" + (chk.stdout if chk else "timeout"))
        gp = sh([ucgecli, "gen", "python", sp, "--tests"]); gc = sh([ucgecli, "gen", "c", sp, "--tests"])
        if gp is None or gp.returncode or gc is None or gc.returncode: return (seed, "gen-fail", spec + "\n" + (gc.stdout if gc else "") + (gp.stdout if gp else ""))
        open(os.path.join(d, "p.py"), "w").write(gp.stdout); open(os.path.join(d, "p.c"), "w").write(gc.stdout)
        flags = ["gcc", "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-O1", "-g", "p.c", "-o", "p", "-lm"] + (["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if san else [])
        cc = sh(flags, 90)
        if cc is None or cc.returncode or cc.stderr.strip(): return (seed, "cc-fail", spec + "\n" + (cc.stderr[:1500] if cc else "timeout"))
        py = sh(["python3", "-I", "p.py"], 20); c = sh(["./p"], 20)
        if py is None: return (seed, "py-timeout", spec)
        if c is None: return (seed, "c-timeout", spec)
        # o runtime sanitizer escreve em stderr; o programa em stdout
        if san and ("runtime error" in c.stderr or "AddressSanitizer" in c.stderr or "LeakSanitizer" in c.stderr): return (seed, "sanitizer", spec + "\n" + c.stderr[:1500])
        import re
        norm = lambda t: re.sub(r"(test \d+: FAIL raised).*", r"\1", t)          # o texto do erro de execucao e do idioma
        if norm(py.stdout) != norm(c.stdout) or (c.returncode != 0) or (py.returncode != 0):
            return (seed, "diff", spec + "\n--- python\n" + py.stdout[-1500:] + py.stderr[-300:] + "\n--- c\n" + c.stdout[-1500:] + c.stderr[-300:])
        return (seed, "ok", len(py.stdout.splitlines()))
    finally:
        if keep: shutil.copytree(d, os.path.join(keep, "p%d" % seed), dirs_exist_ok=True)
        shutil.rmtree(d, ignore_errors=True)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("ucgecli"); ap.add_argument("-n", type=int, default=200); ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--san", action="store_true"); ap.add_argument("--keep"); ap.add_argument("--only", type=int); ap.add_argument("-j", type=int, default=4)
    a = ap.parse_args()
    seeds = [a.only] if a.only is not None else list(range(a.seed, a.seed + a.n))
    if a.keep: os.makedirs(a.keep, exist_ok=True)
    res = {}; lines = 0; bad = []
    with cf.ThreadPoolExecutor(a.j) as ex:
        for seed, st, info in ex.map(one, [(x, os.path.abspath(a.ucgecli), a.san, a.keep) for x in seeds]):
            res[st] = res.get(st, 0) + 1
            if st == "ok": lines += info
            else: bad.append((seed, st, info))
    for seed, st, info in bad[:6]: print("=== seed %d: %s\n%s" % (seed, st, info))
    print("programs %d: %s; output lines compared: %d" % (len(seeds), ", ".join("%s %d" % kv for kv in sorted(res.items())), lines))
    sys.exit(0 if list(res) == ["ok"] else 1)
