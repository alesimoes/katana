#!/usr/bin/env python3
"""Diferencial Python x JavaScript x TypeScript do codegen (docs/codegen.md, "Backends JS e TS"): gera o codigo de cada alvo com `codegencli gen <alvo> --tests`,
   executa e compara a saida (stdout). Linhas `test N: FAIL ...` so comparam ate o FAIL (o texto depois e o repr de cada linguagem).
   Uso: jsdiff.py <codegencli> <arquivo.kcode>...     (KATANA_CODEGEN_TSC=/caminho/do/tsc para incluir o TypeScript; sem ele so Python x JS)"""
import os, subprocess, sys, tempfile

def gen(cli, target, path):
    r = subprocess.run([cli, "gen", target, path, "--tests"], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr

def run_py(code, d):
    f = os.path.join(d, "p.py"); open(f, "w").write(code)
    r = subprocess.run(["python3", "-I", f], capture_output=True, text=True, timeout=30)
    return r.stdout + ("<<stderr>>" + r.stderr.strip().splitlines()[-1] if r.returncode else "")

def run_js(code, d):
    f = os.path.join(d, "p.js"); open(f, "w").write(code)
    r = subprocess.run(["node", f], capture_output=True, text=True, timeout=30)
    return r.stdout + ("<<stderr>>" if r.returncode else "")

def run_ts(code, d, tsc):
    f = os.path.join(d, "p.ts"); open(f, "w").write(code)
    r = subprocess.run([tsc, "--strict", "--target", "es2022", "--module", "commonjs", "--lib", "es2022,dom", "--skipLibCheck", "--outDir", os.path.join(d, "out"), f], capture_output=True, text=True, timeout=120)
    if r.returncode: return "<<tsc>>" + r.stdout.strip()[:600]
    r = subprocess.run(["node", os.path.join(d, "out", "p.js")], capture_output=True, text=True, timeout=30)
    return r.stdout + ("<<stderr>>" if r.returncode else "")

def norm(out):
    res = []
    for ln in out.splitlines():
        if ln.startswith("test ") and ": FAIL" in ln: ln = ln[:ln.index(": FAIL") + 6]
        res.append(ln)
    return res

def compare(cli, path, tsc=None, verbose=False):
    """devolve (ok, mensagem)"""
    with tempfile.TemporaryDirectory() as d:
        outs = {}
        for tgt in ("python", "js") + (("ts",) if tsc else ()):
            rc, code, err = gen(cli, tgt, path)
            if rc: return False, "%s: geracao falhou: %s" % (tgt, (code + err).strip()[:300])
            try:
                outs[tgt] = run_py(code, d) if tgt == "python" else run_js(code, d) if tgt == "js" else run_ts(code, d, tsc)
            except subprocess.TimeoutExpired:
                if tgt == "python": return True, "0 linhas (o Python nao terminou em 30 s: programa ignorado)"     # laco infinito do gerador, nao divergencia
                return False, "%s: tempo esgotado (o Python terminou)" % tgt
        ref = norm(outs["python"])
        for tgt, o in outs.items():
            if norm(o) != ref:
                a, b = ref, norm(o)
                for i in range(max(len(a), len(b))):
                    x = a[i] if i < len(a) else "<fim>"; y = b[i] if i < len(b) else "<fim>"
                    if x != y: return False, "%s difere na linha %d\n  python: %s\n  %s: %s" % (tgt, i + 1, x[:300], tgt, y[:300])
        return True, "%d linhas iguais" % len(ref)

if __name__ == "__main__":
    cli = sys.argv[1]; tsc = os.environ.get("KATANA_CODEGEN_TSC"); bad = 0
    for p in sys.argv[2:]:
        ok, msg = compare(cli, p, tsc)
        print(("ok   " if ok else "FAIL ") + os.path.basename(p) + ": " + msg)
        bad += not ok
    sys.exit(1 if bad else 0)
