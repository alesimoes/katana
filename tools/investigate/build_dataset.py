#!/usr/bin/env python3
"""Converte tracos de investigacao (runtool.py) em conversas de treino do katana (uma linha {"messages": [...]}).

  build_dataset.py [--model Q.gguf] [--out pilot.jsonl] [--max-tokens 12000] [--report] traco.jsonl ...

Cada traco (QID.jsonl) vira: user (a pergunta), depois, para cada chamada, um assistant com o texto curto
"<aprendi da chamada anterior> <hipotese desta>" e a tool_call, o resultado como mensagem tool, e por fim o assistant
com a resposta. O prompt de sistema fica de fora (katana import --system file:F poe o do agente).
Validacao (conversa que falha nao entra, o motivo vai para o erro padrao):
  - a resposta cita o gabarito (mesmo criterio de score.py)
  - nenhuma chamada repetida; toda chamada tem hipotese antes (<= 140 caracteres) e aprendi depois (<= 140)
  - soma de tokens (pedido, textos, chamadas, resultados, resposta) <= --max-tokens (tokenizador do katana com --model,
    senao caracteres / 3.3)
  - Read com offset e limit <= 80; argumentos validos contra o esquema curto da ferramenta
O caminho da copia no resultado e nos argumentos vira /work/<repo> (nome do repositorio).
"""
import argparse, json, os, re, subprocess, sys
from common import KATANA, question, worktree, check_args, MAX_TEXT
from score import verdict

def count_tokens(text, model):
    if model:
        r = subprocess.run([KATANA, "tokenize", model, "-"], input=text, capture_output=True, text=True)
        m = re.match(r"(\d+) tokens", r.stdout)
        if m:
            return int(m.group(1))
    return int(len(text) / 3.3) + 1


def strip_label(text, labels):
    return re.sub(r"^\s*(%s)\s*:\s*" % labels, "", text, flags=re.I).strip()


def build(trace_path, model, max_tokens):
    qid = os.path.splitext(os.path.basename(trace_path))[0]
    q = question(qid)
    with open(trace_path, encoding="utf-8") as f:
        ev = [json.loads(l) for l in f if l.strip()]
    wt = worktree(q)
    alias = "/work/" + os.path.basename(q["repo"].rstrip("/"))

    def fix(s):
        return s.replace(wt, alias)

    # agrupa: hyp, call, learned
    steps, cur, answer = [], None, None
    for e in ev:
        if e["t"] == "hyp":
            cur = {"hyp": strip_label(e["text"], "hip[oó]tese"), "call": None, "learned": None}
        elif e["t"] == "call":
            if cur is None or cur["call"] is not None:
                return None, "chamada sem hipotese antes"
            cur["call"] = e
            steps.append(cur)
        elif e["t"] == "learned":
            if not steps or steps[-1]["learned"] is not None:
                return None, "'aprendi' sem chamada antes"
            steps[-1]["learned"] = strip_label(e["text"], "aprendi")
        elif e["t"] == "answer":
            answer = e["text"].strip()
    if not steps:
        return None, "traco sem chamadas"
    if answer is None:
        return None, "traco sem resposta final"
    seen = set()
    for i, s in enumerate(steps):
        c = s["call"]
        key = (c["name"], json.dumps(c["args"], sort_keys=True))
        if key in seen:
            return None, "chamada repetida (%d)" % (i + 1)
        seen.add(key)
        bad = check_args(c["name"], c["args"])
        if bad:
            return None, "chamada %d: %s" % (i + 1, bad)
        if not s["hyp"] or len(s["hyp"]) > MAX_TEXT:
            return None, "hipotese %d vazia ou com mais de %d caracteres (%d)" % (i + 1, MAX_TEXT, len(s["hyp"]))
        if not s["learned"] or len(s["learned"]) > MAX_TEXT:
            return None, "aprendi %d vazio ou com mais de %d caracteres (%d)" % (i + 1, MAX_TEXT, len(s["learned"] or ""))
    v, nf, nk = verdict(q, answer)
    if v != "PASSOU":
        return None, "resposta nao cita o gabarito (%s, arquivos %d, palavras %d)" % (v, nf, nk)
    if not re.search(r"\.[A-Za-z0-9]+:\d+", answer):
        return None, "resposta sem arquivo:linha"

    msgs = [{"role": "user", "content": q["question"]}]
    prev = None
    for i, s in enumerate(steps, 1):
        c = s["call"]
        text = ((prev + " ") if prev else "") + s["hyp"]
        tid = "call_%d" % i
        msgs.append({"role": "assistant", "content": text, "tool_calls": [{"id": tid, "type": "function", "function": {
            "name": c["name"], "arguments": json.dumps(json.loads(fix(json.dumps(c["args"], ensure_ascii=False))), ensure_ascii=False)}}]})
        msgs.append({"role": "tool", "tool_call_id": tid, "content": fix(c["result"])})
        prev = s["learned"]
    msgs.append({"role": "assistant", "content": prev + "\n\n" + fix(answer)})
    blob = "\n".join(m["content"] + "".join(t["function"]["arguments"] for t in m.get("tool_calls", [])) for m in msgs)
    ntok = count_tokens(blob, model)
    if ntok > max_tokens:
        return None, "soma de tokens %d > %d" % (ntok, max_tokens)
    return {"messages": msgs, "source": "investigate", "id": qid}, (len(steps), ntok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--model")
    ap.add_argument("--out", default="pilot.jsonl")
    ap.add_argument("--max-tokens", type=int, default=12000)
    a = ap.parse_args()
    ok, bad, toks, calls = 0, 0, [], []
    with open(a.out, "w", encoding="utf-8") as fo:
        for t in a.traces:
            row, info = build(t, a.model, a.max_tokens)
            if row is None:
                bad += 1
                print("REPROVADO %s: %s" % (os.path.basename(t), info), file=sys.stderr)
                continue
            fo.write(json.dumps(row, ensure_ascii=False) + "\n")
            ok += 1
            calls.append(info[0]); toks.append(info[1])
            print("ok %s: %d chamadas, %d tokens" % (os.path.basename(t), info[0], info[1]))
    print("passaram %d de %d; tokens medios %.0f; chamadas medias %.1f" % (
        ok, ok + bad, sum(toks) / max(1, len(toks)), sum(calls) / max(1, len(calls))))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
