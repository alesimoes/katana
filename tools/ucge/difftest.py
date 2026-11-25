#!/usr/bin/env python3
"""Teste diferencial do otimizador do UCGE (docs/ucge.md, "UIR-G"): gera programas CSP aleatorios validos (fuzz_gen.py), gera o Python antes e
depois da otimizacao e executa os dois com as mesmas entradas aleatorias (casos de borda: zero, negativos, inteiros enormes, NaN, infinito, -0.0,
textos vazios e com acento, listas vazias). Exige saidas identicas: valor devolvido, erro lancado (tipo e texto), texto impresso e listas alteradas.
Uso: python3 tools/ucge/difftest.py <ucgecli> [programas=300] [semente=1] [-k]      (-k mantem os arquivos dos casos que falham em /tmp/ucge_diff)"""
import warnings
warnings.filterwarnings("ignore")
import sys, os, io, math, random, subprocess, tempfile, contextlib, copy, ast, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fuzz_gen

INT_POOL = [0, 1, -1, 2, 3, -3, 7, 10, 100, -100, 2 ** 31, -2 ** 31, 2 ** 53 + 1, 2 ** 62, 2 ** 63 - 1, -2 ** 63, 2 ** 64 + 5, -2 ** 70, 10 ** 30]
FLOAT_POOL = [0.0, -0.0, 1.0, 1.5, -2.5, 0.1, 1e10, 1e308, -1e308, float("inf"), float("-inf"), float("nan"), 3.0, 5e-324]
STR_POOL = ["", "a", "ab", "Hello World", "  pad  ", "café", "a,b,c", "x" * 12, "AbC", "1", "12", "abc abc"]
LIST_INT_POOL = [[], [1], [3, 1, 2], [0, 0], [-5, 5, 5], [10, 20, 30, 40], [2 ** 40, -1]]
LIST_STR_POOL = [[], ["a"], ["b", "a"], ["x", "", "y"]]


def arg(r, ty):
    if ty == "int": return r.choice(INT_POOL)
    if ty == "float": return r.choice(FLOAT_POOL)
    if ty == "bool": return r.choice([True, False])
    if ty == "str": return r.choice(STR_POOL)
    if ty == "list[int]": return list(r.choice(LIST_INT_POOL))
    if ty == "list[str]": return list(r.choice(LIST_STR_POOL))
    raise ValueError(ty)


def norm(v):
    """repr com NaN igual a NaN (o repr ja distingue -0.0 de 0.0)"""
    if isinstance(v, float) and math.isnan(v): return "nan"
    if isinstance(v, (list, tuple)): return "[" + ", ".join(norm(x) for x in v) + "]"
    if isinstance(v, dict): return "{" + ", ".join(f"{norm(k)}: {norm(x)}" for k, x in v.items()) + "}"
    return repr(v) + ":" + type(v).__name__


def run_call(fn, args):
    buf = io.StringIO()
    a2 = copy.deepcopy(args)
    try:
        with contextlib.redirect_stdout(buf):
            res = ("ret", norm(fn(*a2)))
    except RecursionError:
        res = ("exc", "RecursionError", "")
    except BaseException as e:
        res = ("exc", type(e).__name__, str(e))
    return (res, buf.getvalue(), norm(a2))


def load(src):
    ns = {"__name__": "ucge_gen"}
    exec(compile(src, "<gen>", "exec"), ns)
    return ns


def sh(cmd, inp=None):
    p = subprocess.run(cmd, capture_output=True, text=True, input=inp)
    return p.returncode, p.stdout, p.stderr


def main():
    cli = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 300
    seed0 = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("-") else 1
    keep = "-k" in sys.argv
    tmp = tempfile.mkdtemp(prefix="ucge_diff_")
    stat = dict(gen=0, invalid=0, tested=0, calls=0, fail=0, changed=0, exc_calls=0, nonempty_out=0, rounds=0, folded=0, dead=0, copies=0, branches=0, unreach=0, temps=0, algebra=0, inlined=0, big=0)
    fails = []
    seed = seed0
    while stat["tested"] < n and stat["gen"] < n * 4:
        seed += 1
        stat["gen"] += 1
        spec, funcs = fuzz_gen.gen_program(seed)
        path = os.path.join(tmp, f"p{seed}.csp")
        open(path, "w").write(spec)
        rc, out, err = sh([cli, "check", path])
        if rc != 0:
            stat["invalid"] += 1
            if stat["invalid"] <= 3: print(f"[generator] seed {seed} rejected by the checker:\n{out}{err}")
            continue
        rc, orig, err = sh([cli, "gen", "python", path, "--no-comments"])
        if rc != 0:
            stat["invalid"] += 1
            if stat["invalid"] <= 3: print(f"[generator] seed {seed} not generated:\n{orig}{err}")
            continue
        variants = {}
        for tag, extra in (("opt", []), ("all", ["--all"])):
            rc, o2, e2 = sh([cli, "optgen", "python", path, "--no-comments", "--recheck"] + extra)
            if rc != 0 and "RECHECK FAILED" in o2 and all(("without `return`" in l or "RECHECK" in l) for l in o2.splitlines()):
                # o validador exige um `return` sintatico no fim; depois de provar que o fim nao e alcancavel o otimizador o remove (o Python gerado nao muda)
                rc, o2, e2 = sh([cli, "optgen", "python", path, "--no-comments"] + extra)
            if rc != 0:
                fails.append((seed, tag, "optgen/recheck failed:\n" + o2 + e2)); stat["fail"] += 1; variants = None; break
            try:
                ast.parse(o2)
            except SyntaxError as ex:
                fails.append((seed, tag, f"optimized code does not parse: {ex}")); stat["fail"] += 1; variants = None; break
            variants[tag] = o2
            if tag == "all":
                import re
                m = re.search(r"optimizer: (\d+) round.*?folded (\d+).*?propagated (\d+).*?(\d+) copy.*?(\d+) algebraic, (\d+) dead.*?(\d+) unreachable.*?(\d+) branch.*?(\d+) loop.*?(\d+) temporar.*?(\d+) call", e2)
                if m:
                    g = [int(x) for x in m.groups()]
                    stat["rounds"] += g[0]; stat["folded"] += g[1] + g[2]; stat["copies"] += g[3]; stat["algebra"] += g[4]; stat["dead"] += g[5]; stat["unreach"] += g[6]; stat["branches"] += g[7]; stat["temps"] += g[9]; stat["inlined"] += g[10]
                    if g[1] + g[2] + g[3] + g[4] + g[5] + g[6] + g[7] + g[8] + g[9] + g[10] > 0: stat["changed"] += 1
        if variants is None:
            continue
        stat["tested"] += 1
        r = random.Random(seed * 7919)
        try:
            base = load(orig)
            ns = {t: load(v) for t, v in variants.items()}
        except Exception as ex:
            fails.append((seed, "load", f"{type(ex).__name__}: {ex}")); stat["fail"] += 1
            continue
        bad = None
        for name, pts, rt in funcs:
            if name not in base: continue
            combos = [[arg(r, t) for t in pts] for _ in range(14)]
            for args in combos:
                ref = run_call(base[name], args)
                stat["calls"] += 1
                if ref[0][0] == "exc": stat["exc_calls"] += 1
                if ref[1]: stat["nonempty_out"] += 1
                if any(isinstance(a, int) and abs(a) > 2 ** 63 for a in args): stat["big"] += 1
                for tag, nsv in ns.items():
                    got = run_call(nsv[name], args)
                    if got != ref:
                        bad = (name, args, tag, ref, got)
                        break
                if bad: break
            if bad: break
        if bad:
            stat["fail"] += 1
            fails.append((seed, bad[2], f"{bad[0]}{bad[1]!r}\n  original : {bad[3]}\n  optimized: {bad[4]}"))
            if keep:
                os.makedirs("/tmp/ucge_diff", exist_ok=True)
                shutil.copy(path, f"/tmp/ucge_diff/p{seed}.csp")
    print(f"programs generated {stat['gen']}, rejected by the checker {stat['invalid']}, tested {stat['tested']}, calls compared {stat['calls']} "
          f"(raising {stat['exc_calls']}, printing {stat['nonempty_out']}, big int arguments {stat['big']})")
    print(f"optimizer changed {stat['changed']} programs; rounds {stat['rounds']}, folded+propagated {stat['folded']}, copies {stat['copies']}, algebraic {stat['algebra']}, dead {stat['dead']}, "
          f"unreachable {stat['unreach']}, branches {stat['branches']}, temporaries {stat['temps']}, inlined {stat['inlined']}")
    for seed, tag, msg in fails[:8]:
        print(f"FAIL seed {seed} [{tag}]: {msg}")
    print("DIFF OK" if not fails and stat["tested"] >= n else f"DIFF FAILED ({len(fails)} failures, tested {stat['tested']}/{n})")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if not fails and stat["tested"] >= n else 1


if __name__ == "__main__":
    sys.exit(main())
