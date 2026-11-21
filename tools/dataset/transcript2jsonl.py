#!/usr/bin/env python3
"""Transcricoes de um agente de programacao -> conversas no formato de mensagens, para o `katana import`.

Entrada: arquivos .jsonl (ou pastas, lidas com as subpastas) em que cada linha e um registro
{"type": "user"|"assistant", "message": {"role", "content"}, ...}. O conteudo e um texto ou uma lista de blocos:
"text", "thinking", "tool_use" {id, name, input} e "tool_result" {tool_use_id, content}. Os outros tipos de registro
sao ignorados. As ferramentas precisam ter os nomes e os argumentos do agente do katana (o segundo conjunto, src/tools/).

Saida: uma linha por episodio, {"messages": [...], "source": arquivo, "episode": n}. Um episodio comeca num pedido do
usuario (ou num resumo de contexto) e vai ate o pedido seguinte; o treino corta tudo em janelas de poucos tokens, entao
um episodio por pedido nao perde nada e cada exemplo comeca do prompt de sistema. `--whole` deixa um arquivo = uma
conversa.

    python3 tools/dataset/transcript2jsonl.py PASTA -o conversas.jsonl
    ./katana agentsys | sed '$d' > sys.txt
    ./katana import modelo.gguf conversas.jsonl treino.jsonl --system file:sys.txt --only-known-tools

O que sai e o que fica:
- avisos automaticos dentro das mensagens (<system-reminder>..</system-reminder>) sao tirados; registros marcados como
  meta, mensagens de erro da interface e ecos de comandos locais sao ignorados;
- uma fala cujas chamadas ficaram sem resultado (interrompida) encerra o episodio e nao entra, a nao ser que a chamada
  seja a que encerra o turno (o relatorio final de um sub-agente);
- resultado de ferramenta maior que --max-result caracteres fica com o comeco e o fim (o agente tambem corta);
- imagens num resultado viram "[image]";
- --replace 'REGEX=TEXTO' (pode repetir, vale na ordem dada) troca o que casar em todos os textos: pedidos, falas,
  argumentos das chamadas e resultados. Serve para caminhos e nomes da maquina de origem, que assim continuam coerentes
  entre a chamada e o resultado;
- --cut-matching REGEX encerra o episodio antes da primeira fala do modelo (texto, raciocinio ou argumentos de chamada)
  que casa com a expressao: nomes e caminhos que o modelo nao deve aprender a escrever, por exemplo. O que veio antes
  fica; um episodio que casa logo na primeira fala some.
"""
import argparse, collections, json, os, re, sys

REMINDER = re.compile(r"<system-reminder>.*?</system-reminder>\s*", re.S)
# mensagens "do usuario" que a interface escreve sozinha: nao sao pedidos
LOCAL = ("<local-command-", "<command-name>", "<command-message>", "<bash-input>", "<bash-stdout>", "<bash-stderr>")
INTERRUPT = "[Request interrupted by user"
NOTICE = ("<task-notification>", "[SYSTEM NOTIFICATION")     # evento de tarefa em segundo plano: continua o episodio


def subst(v, reps):
    """aplica as trocas em todos os textos de um valor JSON (chaves de objetos ficam como estao)"""
    if isinstance(v, str):
        for r, t in reps:
            v = r.sub(t, v)
        return v
    if isinstance(v, list):
        return [subst(x, reps) for x in v]
    if isinstance(v, dict):
        return {k: (x if k in ("role", "tool_call_id", "id", "type", "name") else subst(x, reps)) for k, x in v.items()}
    return v


def files_of(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for d, _, fs in os.walk(p):
                out += [os.path.join(d, f) for f in fs if f.endswith(".jsonl")]
        else:
            out.append(p)
    return sorted(out)


def clip(s, cap):
    if cap <= 0 or len(s) <= cap:
        return s
    a, b = cap * 2 // 3, cap // 3
    return "%s\n[... %d characters omitted ...]\n%s" % (s[:a], len(s) - a - b, s[-b:])


def result_text(content, cap):
    if isinstance(content, str):
        return clip(content, cap)
    parts = []
    for b in content or []:
        t = b.get("type")
        if t == "text":
            parts.append(b.get("text", ""))
        elif t == "image":
            parts.append("[image]")
        elif t == "tool_reference":
            parts.append(b.get("tool_name", ""))
    return clip("\n".join(parts), cap)


def user_text(content):
    if isinstance(content, str):
        return content
    return "\n\n".join(b.get("text", "") for b in content if b.get("type") == "text")


class Episode:
    def __init__(self):
        self.msgs = []          # mensagens ja no formato de saida
        self.by_id = {}         # id da resposta -> mensagem do modelo (os blocos chegam em registros separados)
        self.answered = set()   # ids de chamadas com resultado
        self.ends = set()       # ids de chamadas que encerram o turno
        self.cut = False        # interrompido: nada mais entra

    def close(self):
        """Mensagens validas do episodio: para na primeira fala com chamada sem resultado; termina numa fala do modelo."""
        out = []
        for m in self.msgs:
            if m["role"] == "assistant":
                ids = [c["id"] for c in m.get("tool_calls", [])]
                miss = [i for i in ids if i not in self.answered]
                if miss:
                    if all(i in self.ends for i in miss):
                        out.append(m)
                    break
                if not m["content"] and not ids:
                    continue                       # so raciocinio vazio: nada a aprender
            out.append(m)
        while out and out[-1]["role"] != "assistant":
            out.pop()
        return out if any(m["role"] == "assistant" for m in out) else []


def convert(path, args, seen, stats):
    eps, ep = [], None
    try:
        f = open(path, encoding="utf-8", errors="replace")
    except OSError as e:
        print("nao abriu %s: %s" % (path, e), file=sys.stderr)
        return eps

    def flush():
        nonlocal ep
        if ep is not None:
            eps.append(ep)
        ep = None

    for line in f:
        try:
            o = json.loads(line)
        except ValueError:
            continue
        t = o.get("type")
        if t not in ("user", "assistant") or not isinstance(o.get("message"), dict):
            continue
        u = o.get("uuid")
        if u:
            if u in seen:                          # sessao retomada ou copiada: o mesmo registro em outro arquivo
                continue
            seen.add(u)
        msg = o["message"]
        content = msg.get("content")
        if t == "assistant":
            if o.get("isApiErrorMessage") or msg.get("model") == "<synthetic>" or ep is None or ep.cut:
                continue
            mid = msg.get("id") or u
            m = ep.by_id.get(mid)
            if m is None:
                m = {"role": "assistant", "content": ""}
                ep.by_id[mid] = m
                ep.msgs.append(m)
            for b in content if isinstance(content, list) else [{"type": "text", "text": content or ""}]:
                bt = b.get("type")
                if bt == "text" and b.get("text", "").strip():
                    m["content"] = (m["content"] + "\n\n" if m["content"] else "") + b["text"].strip()
                elif bt == "thinking" and b.get("thinking", "").strip():
                    m["reasoning_content"] = (m.get("reasoning_content", "") + "\n\n" if m.get("reasoning_content") else "") + b["thinking"].strip()
                elif bt == "tool_use":
                    m.setdefault("tool_calls", []).append({"id": b.get("id", ""), "type": "function",
                                                           "function": {"name": b.get("name", ""), "arguments": b.get("input") or {}}})
                    if o.get("toolEndsTurn"):
                        ep.ends.add(b.get("id", ""))
            continue
        # registro do usuario: resultados de ferramenta, ou texto
        if isinstance(content, list) and any(b.get("type") == "tool_result" for b in content):
            if ep is None or ep.cut:
                continue
            for b in content:
                if b.get("type") != "tool_result":
                    continue
                ep.answered.add(b.get("tool_use_id", ""))
                ep.msgs.append({"role": "tool", "tool_call_id": b.get("tool_use_id", ""), "content": result_text(b.get("content"), args.max_result)})
            continue
        text = user_text(content)
        if INTERRUPT in text[:80]:
            if ep is not None:
                ep.cut = True
            continue
        if o.get("isMeta") or text.lstrip().startswith(LOCAL):
            continue
        text = REMINDER.sub("", text).strip()
        if not text:
            continue
        notice = text.startswith(NOTICE)
        if ep is not None and not ep.cut and (notice or args.whole) and not o.get("isCompactSummary"):
            ep.msgs.append({"role": "user", "content": text})
            continue
        if notice:
            continue                               # aviso sem episodio aberto
        flush()
        ep = Episode()
        ep.msgs.append({"role": "user", "content": text})
    flush()
    f.close()
    return eps


def main():
    ap = argparse.ArgumentParser(description="transcricoes de um agente de programacao -> conversas para o katana import")
    ap.add_argument("paths", nargs="+", help="arquivos .jsonl ou pastas")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--whole", action="store_true", help="um arquivo = uma conversa (so os resumos de contexto dividem)")
    ap.add_argument("--max-result", type=int, default=6000, help="caracteres por resultado de ferramenta (0 = sem corte)")
    ap.add_argument("--min-calls", type=int, default=0, help="descarta episodios com menos chamadas de ferramenta que isto")
    ap.add_argument("--replace", action="append", default=[], metavar="REGEX=TEXTO", help="troca em todos os textos; pode repetir")
    ap.add_argument("--cut-matching", default="", help="expressao regular: o episodio acaba antes da primeira fala do modelo que casar")
    args = ap.parse_args()
    reps = []
    for r in args.replace:
        a, sep, b = r.partition("=")
        if not sep or not a:
            ap.error("--replace espera REGEX=TEXTO: %s" % r)
        reps.append((re.compile(a), b))
    cut = re.compile(args.cut_matching, re.I) if args.cut_matching else None
    stats = {"tools": collections.Counter()}
    seen = set()
    nfiles = nep = nout = nmsg = ncall = ncut = ndrop = nshort = 0
    with open(args.out, "w", encoding="utf-8") as out:
        for path in files_of(args.paths):
            nfiles += 1
            for i, ep in enumerate(convert(path, args, seen, stats)):
                nep += 1
                msgs = ep.close()
                if reps:
                    msgs = subst(msgs, reps)
                if cut:
                    for j, m in enumerate(msgs):
                        if m["role"] == "assistant" and (cut.search(m["content"]) or cut.search(m.get("reasoning_content", "")) or
                                                         cut.search(json.dumps([c["function"] for c in m.get("tool_calls", [])], ensure_ascii=False))):
                            msgs = msgs[:j]
                            while msgs and msgs[-1]["role"] != "assistant":
                                msgs.pop()
                            ncut += 1
                            ndrop += not msgs
                            break
                if not msgs:
                    continue
                calls = sum(len(m.get("tool_calls", [])) for m in msgs)
                if calls < args.min_calls:
                    nshort += 1
                    continue
                out.write(json.dumps({"messages": msgs, "source": os.path.basename(path), "episode": i}, ensure_ascii=False) + "\n")
                nout += 1
                nmsg += sum(m["role"] == "assistant" for m in msgs)
                ncall += calls
                for m in msgs:
                    for c in m.get("tool_calls", []):
                        stats["tools"][c["function"]["name"]] += 1
    print("transcricoes: %d arquivos, %d episodios, %d gravados em %s (%d falas do modelo, %d chamadas de ferramenta)%s%s"
          % (nfiles, nep, nout, args.out, nmsg, ncall,
             ", %d encurtados por --cut-matching (%d inteiros)" % (ncut, ndrop) if cut else "", ", %d por --min-calls" % nshort if args.min_calls else ""))
    if stats["tools"]:
        print("ferramentas: " + ", ".join("%s %d" % kv for kv in stats["tools"].most_common()))
    return 0 if nout else 1


if __name__ == "__main__":
    sys.exit(main())
