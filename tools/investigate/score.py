#!/usr/bin/env python3
"""Mede uma execucao de `katana -p`: score.py QID LOG ERR OUT SEGUNDOS  ->  uma linha de colunas separadas por tab.

Colunas: id, chamadas, tokens_entrada, maior_contexto, repetidas, veredito, arquivos_citados, palavras, segundos
- chamadas: linhas do registro (KATANA_LOG) desta execucao
- tokens_entrada: soma de 'entrada: N tokens' do stderr (todas as rodadas)
- maior_contexto: maior 'contexto N de M'
- repetidas: chamadas com ferramenta e argumentos iguais a uma anterior
- veredito: PASSOU (cita >= min_files arquivos do gabarito e as palavras-chave), FALHOU, SEM RESPOSTA
"""
import os, re, sys
from common import question


def read(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def metrics(log, err):
    calls, seen, rep = 0, set(), 0
    for ln in log.splitlines():
        f = ln.split("\t")
        if len(f) < 3 or not re.match(r"\d{4}-\d\d-\d\d ", f[0]):
            continue
        calls += 1
        key = (f[1], f[2])
        if key in seen:
            rep += 1
        seen.add(key)
    tin = sum(int(m) for m in re.findall(r"entrada: (\d+) tokens", err))
    ctx = max([int(m) for m in re.findall(r"contexto (\d+) de", err)] or [0])
    return calls, tin, ctx, rep


def verdict(q, out):
    ans = out.strip()
    if not ans:
        return "SEM RESPOSTA", 0, 0
    low = ans.lower()
    files = sum(1 for f in q["answer_files"] if f.lower() in low)
    kws = q.get("keywords", [])
    kwn = sum(1 for k in kws if re.search(k, ans, re.I))
    need_k = q.get("min_keywords", len(kws))
    ok = files >= q.get("min_files", 1) and kwn >= need_k
    return ("PASSOU" if ok else "FALHOU"), files, kwn


def main():
    qid, log, err, out, secs = sys.argv[1:6]
    q = question(qid)
    calls, tin, ctx, rep = metrics(read(log), read(err))
    v, nf, nk = verdict(q, read(out))
    print("\t".join(str(x) for x in (qid, calls, tin, ctx, rep, v, "%d/%d" % (nf, len(q["answer_files"])),
                                     "%d/%d" % (nk, len(q.get("keywords", []))), secs)))


if __name__ == "__main__":
    main()
