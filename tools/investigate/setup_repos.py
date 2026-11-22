#!/usr/bin/env python3
# Cria (se faltar) a copia de cada pergunta: git worktree --detach no commit base. Nao altera os repositorios alem
# do registro do worktree. Uso: setup_repos.py [id ...]   (INVEST_REPOS muda a pasta das copias)
import sys
from common import questions, worktree

ids = sys.argv[1:]
for q in questions():
    if ids and q["id"] not in ids:
        continue
    print(q["id"], worktree(q))
