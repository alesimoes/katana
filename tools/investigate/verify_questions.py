#!/usr/bin/env python3
# Verifica o gabarito das perguntas sem precisar das copias completas (usa so git show/diff nos repositorios de origem).
# Uso: verify_questions.py [--file F.json] [--split train|val] [id ...]
# Por pergunta: (1) cada arquivo do gabarito existe no BASE; (2) as palavras-chave casam em ao menos 1 arquivo do
# gabarito NO BASE (todas, ou min_keywords); (3) a pergunta nao contem o nome de arquivo do gabarito; (4) o worktree do
# base pode ser criado (teste sem checkout, removido em seguida); alem disso confere fix_commit (pai == base, e o commit
# altera os arquivos do gabarito) e avisa se a pergunta ja casa as palavras-chave da causa. Sai com 1 se algo falhar.
import argparse, json, os, re, subprocess, sys, tempfile, shutil

LEGACY = {"grey-gkatana", "grey-vpn-loop", "sonora-bounce-loop"}
HERE = os.path.dirname(os.path.abspath(__file__))


def git(repo, *a):
    return subprocess.run(["git", "-C", repo] + list(a), capture_output=True, text=True, errors="replace")


def check(q):
    errs, warns = [], []
    repo, base = q["repo"], q["base"]
    r = git(repo, "rev-parse", "--verify", base + "^{commit}")
    if r.returncode:
        return ["base inexistente"], warns
    full = r.stdout.strip()
    texts = {}
    for f in q["answer_files"]:                                   # (1)
        r = git(repo, "show", "%s:%s" % (full, f))
        if r.returncode:
            errs.append("arquivo ausente no base: " + f)
        else:
            texts[f] = r.stdout
    kws = q.get("keywords", [])                                   # (2)
    if not kws:
        errs.append("sem keywords")
    need = q.get("min_keywords", len(kws))
    hit = [k for k in kws if any(re.search(k, t, re.I) for t in texts.values())]
    if len(hit) < need:
        errs.append("keywords nao casam no base: " + ", ".join(k for k in kws if k not in hit))
    low = q["question"].lower()                                   # (3)
    for f in q["answer_files"]:
        b = os.path.basename(f).lower()
        if b in low:
            errs.append("pergunta cita arquivo do gabarito: " + b)
        stem = os.path.splitext(b)[0]
        if len(stem) > 5 and re.search(r"\b" + re.escape(stem) + r"\b", low):
            warns.append("pergunta contem o nome base de " + b)
    for k in kws:
        if re.search(k, q["question"], re.I):
            warns.append("pergunta ja casa a keyword " + k)
    fx = q.get("fix_commit")                                      # auditoria
    if fx:
        r = git(repo, "rev-parse", fx + "^")
        if r.stdout.strip() != full:
            errs.append("pai de fix_commit != base")
        names = set(git(repo, "diff", "--name-only", fx + "^", fx).stdout.split())
        miss = [f for f in q["answer_files"] if f not in names]
        if miss:
            errs.append("gabarito fora do fix_commit: " + ",".join(miss))
    elif q.get("split") and not q["id"].startswith("grey-gkatana"):
        warns.append("sem fix_commit")
    if not errs:                                                  # (4)
        tmp = tempfile.mkdtemp(prefix="vq-", dir=os.environ.get("INVEST_TMP", "/mnt/dados"))
        d = os.path.join(tmp, "wt")
        r = git(repo, "worktree", "add", "--detach", "--no-checkout", d, full)
        if r.returncode:
            errs.append("worktree nao criou: " + r.stderr.strip()[:120])
        else:
            git(repo, "worktree", "remove", "--force", d)
        shutil.rmtree(tmp, ignore_errors=True)
    return errs, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=os.path.join(HERE, "questions.json"))
    ap.add_argument("--split")
    ap.add_argument("ids", nargs="*")
    a = ap.parse_args()
    qs = json.load(open(a.file, encoding="utf-8"))
    bad = 0
    for q in qs:
        if a.ids and q["id"] not in a.ids:
            continue
        if a.split and q.get("split") != a.split:
            continue
        e, w = check(q)
        if q["id"] in LEGACY and e:                     # as 12 perguntas originais ficam como estao
            w, e = ["legado, nao reprovado: " + "; ".join(e)] + w, []
        print(("FALHOU " if e else "ok     ") + q["id"], "; ".join(e + ["(aviso) " + x for x in w]))
        bad += bool(e)
    print("%d com falha" % bad)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
