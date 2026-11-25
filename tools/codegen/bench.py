#!/usr/bin/env python3
"""Medicao do codegen sem modelo (docs/codegen.md, secao "Measured"): para cada tarefa de tools/codegen/tasks/ mede
   - tamanho da especificacao (caracteres/4 como proxy de tokens), inteira e so o nucleo (sem comentarios e sem linhas `test`);
   - tamanho do Python gerado (com e sem comentarios) e do Python idiomatico escrito a mao (tools/codegen/ref/);
   - se a UIR cobriu a tarefa sem RAW e se os testes dados passaram no codigo gerado.
   Uso: python3 tools/codegen/bench.py /caminho/codegencli"""
import os, re, subprocess, sys
cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tok = lambda s: len(s) / 4.0
rows = []
for f in sorted(os.listdir(os.path.join(here, "tasks"))):
    if not f.endswith(".kcode"): continue
    spec = open(os.path.join(here, "tasks", f)).read()
    core = "\n".join(l for l in spec.splitlines() if not l.lstrip().startswith("#") and not l.startswith("test "))
    path = os.path.join(here, "tasks", f)
    gen = subprocess.run([cli, "gen", "python", path, "--no-comments"], capture_output=True, text=True).stdout
    gen_c = subprocess.run([cli, "gen", "python", path], capture_output=True, text=True).stdout
    st = subprocess.run([cli, "stats", path], capture_output=True, text=True).stdout
    raw = int(re.search(r"raw (\d+)", st).group(1)) if "raw" in st else -1
    run = subprocess.run([cli, "run", "python", path], capture_output=True, text=True).stdout
    m = re.search(r"tests: (\d+) passed, (\d+) failed", run)
    tests = "%s/%s" % (m.group(1), int(m.group(1)) + int(m.group(2))) if m else "-"
    ref = open(os.path.join(here, "ref", f.replace(".kcode", ".py"))).read()
    rows.append((f[:-4], tok(spec), tok(core), tok(gen), tok(gen_c), tok(ref), raw, tests))
print("%-20s %8s %8s %8s %8s %8s %4s %7s" % ("task", "spec", "spec-core", "gen", "gen+cmt", "ref-py", "raw", "tests"))
for r in rows: print("%-20s %8.0f %8.0f %8.0f %8.0f %8.0f %4d %7s" % r)
n = len(rows); noraw = sum(1 for r in rows if r[6] == 0)
print("tasks %d, without RAW %d (%.0f%%)" % (n, noraw, 100.0 * noraw / n))
t = lambda i, sel=None: sum(r[i] for r in rows if sel is None or r[6] == 0)
print("tokens (chars/4), tasks without RAW: spec-core %.0f, generated %.0f, idiomatic hand-written %.0f" % (t(2, 1), t(3, 1), t(5, 1)))
print("ratio spec-core / generated = %.2f; spec-core / idiomatic = %.2f" % (t(2, 1) / t(3, 1), t(2, 1) / t(5, 1)))
