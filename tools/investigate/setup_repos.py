#!/usr/bin/env python3
# Cria (so o que falta) a copia de cada pergunta: git worktree --detach no commit base. Nao altera os repositorios alem
# do registro do worktree. Uso: setup_repos.py [--split train|val|all] [--base-dir DIR] [--du] [id ...]
# (INVEST_REPOS tambem muda a pasta das copias; o padrao e /mnt/dados/investigate-repos). Imprime "id pasta" por pergunta.
import argparse, os, subprocess, sys
import common

ap = argparse.ArgumentParser()
ap.add_argument("--split", default="all", choices=["train", "val", "all"])
ap.add_argument("--base-dir", help="pasta das copias (padrao: $INVEST_REPOS ou /mnt/dados/investigate-repos)")
ap.add_argument("--du", action="store_true", help="no fim, mostra o espaco usado (du -sh)")
ap.add_argument("ids", nargs="*")
a = ap.parse_args()
if a.base_dir:
    common.REPOS = os.path.abspath(a.base_dir)

made = 0
for q in common.questions(a.split):
    if a.ids and q["id"] not in a.ids:
        continue
    d = os.path.join(common.REPOS, q["id"])
    had = os.path.isdir(d) or (q.get("worktree_hint") and os.path.isdir(q["worktree_hint"]))
    print(q["id"], common.worktree(q), flush=True)
    made += 0 if had else 1
print("criadas agora: %d" % made, file=sys.stderr)
if a.du and os.path.isdir(common.REPOS):
    print(subprocess.run(["du", "-sh", common.REPOS], capture_output=True, text=True).stdout.strip(), file=sys.stderr)
