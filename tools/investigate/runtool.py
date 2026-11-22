#!/usr/bin/env python3
"""Executa ferramentas REAIS do katana (sem modelo) sobre a copia de uma pergunta e grava o traco.

Uso (QID = id em questions.json; o traco vai para $INVEST_TRACES/QID.jsonl, padrao ./traces):
  runtool.py QID hyp "texto"            registra a hipotese (antes de uma consulta)
  runtool.py QID call Grep '{"pattern": "x", "output_mode": "content", "-n": true, "-C": 2}'
  runtool.py QID call Read '{"file_path": "bridge/server.mjs", "offset": 100, "limit": 40}'
  runtool.py QID call Glob '{"pattern": "**/*.mjs"}'
  runtool.py QID learned "texto"        registra o que se aprendeu (depois da consulta)
  runtool.py QID answer "texto final"   registra a resposta final (ou answer @arquivo)
  runtool.py QID reset                  apaga o traco desta pergunta
  runtool.py QID show                   imprime a pergunta e o resumo do traco
Recusas (nada e executado nem gravado): chamada identica a uma anterior; Read sem limit ou com limit > 80.
O resultado impresso e o texto exato que a ferramenta do motor devolve.
"""
import json, os, subprocess, sys
from common import KATANA, question, worktree, check_args

TOOLS = {"Read", "Grep", "Glob", "Find"}      # Find entra quando o motor tiver a ferramenta


def trace_path(qid):
    d = os.environ.get("INVEST_TRACES", os.path.join(os.getcwd(), "traces"))
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, qid + ".jsonl")


def load(p):
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def add(p, ev):
    with open(p, "a", encoding="utf-8") as f:
        f.write(json.dumps(ev, ensure_ascii=False) + "\n")


def norm_args(args, wt):
    """Caminhos relativos de Read viram absolutos (como o modelo escreve); o resto fica como veio."""
    a = dict(args)
    for k in ("file_path", "path"):
        if k in a and isinstance(a[k], str) and not os.path.isabs(a[k]):
            a[k] = os.path.normpath(os.path.join(wt, a[k]))
    return a


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    qid, cmd, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    q = question(qid)
    tp = trace_path(qid)
    ev = load(tp)
    if cmd == "reset":
        if os.path.exists(tp):
            os.remove(tp)
        return
    if cmd == "show":
        print(q["question"])
        for e in ev:
            if e["t"] == "call":
                print("call", e["name"], json.dumps(e["args"], ensure_ascii=False)[:200])
            else:
                print(e["t"], e.get("text", "")[:200])
        return
    if cmd in ("hyp", "learned", "answer"):
        text = " ".join(rest)
        if text.startswith("@"):
            text = open(text[1:], encoding="utf-8").read().strip()
        if not text:
            sys.exit("texto vazio")
        add(tp, {"t": cmd, "text": text})
        print("ok")
        return
    if cmd != "call" or len(rest) != 2:
        sys.exit(__doc__)
    name, raw = rest
    if name not in TOOLS:
        sys.exit("ferramenta fora do conjunto do investigador: " + name)
    wt = worktree(q)
    try:
        args = norm_args(json.loads(raw), wt)
    except json.JSONDecodeError as e:
        sys.exit("json invalido: %s" % e)
    for e in ev:
        if e["t"] == "call" and e["name"] == name and e["args"] == args:
            sys.exit("RECUSADO: chamada identica a uma anterior; use o que ja aprendeu ou mude a consulta")
    bad = check_args(name, args)
    if bad:
        sys.exit("RECUSADO: " + bad)
    env = dict(os.environ, KATANA_LOG=os.devnull)
    r = subprocess.run([KATANA, "tool", name, json.dumps(args, ensure_ascii=False)], cwd=wt, env=env,
                       capture_output=True, text=True, timeout=120)
    res = r.stdout
    if res.endswith("\n"):
        res = res[:-1]
    add(tp, {"t": "call", "name": name, "args": args, "result": res})
    print(res)


if __name__ == "__main__":
    main()
