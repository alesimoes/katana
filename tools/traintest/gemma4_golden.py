#!/usr/bin/env python3
"""Golden checks of the text the katana agent renders for Gemma 4, against the model's own chat template.

The template is read from the metadata of a real Gemma 4 GGUF (only the key/value header is read: no weights, no
inference) and rendered with jinja2 on the same messages. Nothing of the template is stored in the repository.

  gemma4_golden.py sys  GGUF JSON_SYS G4_SYS THINK          system turn: text + declarations of every tool
  gemma4_golden.py conv GGUF JSON_SYS G4_SYS THINK DUMP     scripted conversation (MOCK_GOLD) as the cache holds it
  gemma4_golden.py compact GGUF JSON_SYS CDUMP              live compaction request after a tool result

JSON_SYS is `katana agentsys json` (tools as JSON lines inside <tools>), G4_SYS is `katana agentsys gemma4` (or the
agent's own system text), DUMP is KATANA_CTX_DUMP, CDUMP is KATANA_COMPACT_DUMP (cache text, then the request). Exit 0 = equal, 1 = differs,
3 = skipped (no GGUF or no jinja2).

Known, deliberate difference (normalized before comparing, and checked separately): katana keeps every model turn as
it was generated, so a model turn starts with the empty thought channel <|channel>thought\\n<channel|> when thinking
is off (the template writes it only in the generation prompt and drops it from earlier turns)."""
import json, re, struct, sys


def template_of(path):
    try:
        f = open(path, "rb")
    except OSError:
        return None
    if f.read(4) != b"GGUF":
        return None
    SZ = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}

    def r(fmt):
        return struct.unpack("<" + fmt, f.read(struct.calcsize("<" + fmt)))

    def rs():
        (n,) = r("Q")
        return f.read(n).decode("utf-8", "replace")
    r("I")
    _, nkv = r("QQ")
    for _ in range(nkv):
        k = rs()
        (t,) = r("I")
        if t == 8:
            v = rs()
            if k == "tokenizer.chat_template":
                return v
        elif t == 9:
            at, n = r("IQ")
            if at == 8:
                for _ in range(n):
                    rs()
            else:
                f.seek(struct.calcsize("<" + SZ[at]) * n, 1)
        else:
            r(SZ[t])
    return None


def tools_of(json_sys):
    a = json_sys.index("<tools>\n") + 8
    b = json_sys.index("</tools>", a)
    return [json.loads(l) for l in json_sys[a:b].splitlines() if l.strip()]


def render(tpl, messages, tools, think, gen):
    import jinja2
    env = jinja2.Environment(trim_blocks=True, lstrip_blocks=True, extensions=["jinja2.ext.loopcontrols"])
    env.globals["raise_exception"] = lambda m: (_ for _ in ()).throw(Exception(m))
    return env.from_string(tpl).render(messages=messages, tools=tools, enable_thinking=bool(think),
                                       add_generation_prompt=gen, bos_token="")


def diff(a, b):
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    return "first difference at byte %d:\n  template: %r\n  katana:   %r" % (i, a[max(0, i - 80):i + 120], b[max(0, i - 80):i + 120])


EMPTY = "<|channel>thought\n<channel|>"
Q = '<|"|>'


def sort_args(a):
    """k:v,k:v (valores simples: texto entre <|"|> ou numero) -> as mesmas chaves em ordem alfabetica"""
    parts, i = [], 0
    while i < len(a):
        j = a.index(":", i)
        k = a[i:j]
        j += 1
        if a.startswith(Q, j):
            e = a.index(Q, j + len(Q)) + len(Q)
        else:
            e = a.find(",", j)
            e = len(a) if e < 0 else e
        parts.append((k, a[i:e]))
        i = e + 1 if e < len(a) and a[e] == "," else e
    return ",".join(p for _, p in sorted(parts))


def main():
    mode, gguf = sys.argv[1], sys.argv[2]
    tpl = template_of(gguf)
    try:
        import jinja2  # noqa: F401
    except ImportError:
        tpl = None
    if not tpl:
        print("skip (no Gemma 4 GGUF or no jinja2)")
        return 3
    jsys = open(sys.argv[3]).read()
    if mode == "compact":       # o sistema vem do proprio texto do cache
        first = open(sys.argv[4], errors="replace").read().split("\n=====\n")[0]
        dump, cdump = first.split("\n=====req\n")
        a = len("<|turn>system\n") + (len("<|think|>\n") if dump.startswith("<|turn>system\n<|think|>\n") else 0)
        gsys = dump[a:dump.index("<turn|>\n<|turn>user\n")]
    else:
        gsys = open(sys.argv[4]).read()
    m = re.search(r"\n\[\d+ bytes\]\n?$", gsys)          # saida do katana agentsys
    if m:
        gsys = gsys[:m.start()]
    tools = tools_of(jsys)
    k = gsys.find("<|tool>")
    content = gsys[:k] if k >= 0 else gsys
    if mode == "sys":
        think = sys.argv[5] == "1"
        want = render(tpl, [{"role": "system", "content": content}], tools, think, False)
        got = "<|turn>system\n" + ("<|think|>\n" if think else "") + gsys + "<turn|>\n"
        if want == got:
            print("system turn equal to the template (%d tools, %d bytes)" % (len(tools), len(got)))
            return 0
        print(diff(want, got))
        return 1
    if mode == "conv":
        think = sys.argv[5] == "1"
        dump = open(sys.argv[6], errors="replace").read()
        bodies = re.findall(r'<\|tool_response>response:(\w+)\{value:<\|"\|>(.*?)<\|"\|>\}<tool_response\|>', dump, re.S)
        if len(bodies) != 3:
            print("expected 3 tool responses in the dump, found %d" % len(bodies))
            return 1
        msgs = [{"role": "system", "content": content}, {"role": "user", "content": "grave e confira g.txt"},
                {"role": "assistant", "content": "", "tool_calls": [{"id": "1", "function": {"name": "Write", "arguments": {"file_path": "g.txt", "content": "um\ndois \"tres\"\n"}}}]},
                {"role": "tool", "tool_call_id": "1", "content": bodies[0][1]},
                {"role": "assistant", "content": "", "tool_calls": [{"id": "2", "function": {"name": "Bash", "arguments": {"command": "cat g.txt", "timeout": 20000}}},
                                                                   {"id": "3", "function": {"name": "Grep", "arguments": {"pattern": "dois", "path": "g.txt", "output_mode": "content"}}}]},
                {"role": "tool", "tool_call_id": "2", "content": bodies[1][1]},
                {"role": "tool", "tool_call_id": "3", "content": bodies[2][1]},
                {"role": "assistant", "content": "Pronto: g.txt conferido."},
                {"role": "user", "content": "obrigado"}, {"role": "assistant", "content": "Pronto: g.txt conferido."}]
        want = render(tpl, msgs, tools, think, False)
        # katana: o historico termina no fim da ultima fala, sem o fecho do turno (o fim de turno nao entra no cache)
        got = dump + "<turn|>\n"
        bad = []
        if not think:
            # cada turno do modelo comeca pelo canal de pensamento vazio (o prompt de geracao do template)
            n_turns = got.count("<|turn>model\n")
            if got.count("<|turn>model\n" + EMPTY) != n_turns:
                bad.append("a model turn without the empty thought channel")
            got = got.replace("<|turn>model\n" + EMPTY, "<|turn>model\n")
            want_t = want.replace("<|turn>model\n" + EMPTY, "<|turn>model\n")
        else:
            # raciocinio ligado: o template so escreve o pensamento quando a mensagem o traz (aqui nenhuma); o katana
            # guarda o canal como foi gerado (vazio, do professor de mentira). Cada canal aberto tem de fechar
            if got.count("<|channel>thought\n") != got.count("<channel|>"):
                bad.append("an open thought channel")
            got = re.sub(r"<\|channel>thought\n.*?<channel\|>", "", got, flags=re.S)
            want_t = want
        # o professor de mentira escreve os argumentos na ordem em que vieram; o template os ordena (o modelo escreve
        # a propria ordem): compara com as chaves ordenadas
        got = re.sub(r"<\|tool_call>call:(\w+)\{(.*?)\}<tool_call\|>", lambda mm: "<|tool_call>call:%s{%s}<tool_call|>" % (mm.group(1), sort_args(mm.group(2))), got, flags=re.S)
        if got != want_t:
            bad.append(diff(want_t, got))
        if bad:
            print("\n".join(bad))
            return 1
        print("conversation equal to the template (%d bytes; thinking %s; tool calls, results, a note, two user turns)" % (len(got), "on" if think else "off"))
        return 0
    if mode == "compact":
        # o pedido de resumo ao vivo: o resultado pendente, o turno do modelo fecha, o pedido num turno do usuario e o
        # prompt de geracao sem raciocinio
        m = re.match(r'(.*<tool_response\|>)<turn\|>\n<\|turn>user\n(\[katana: context compaction\].*?)<turn\|>\n<\|turn>model\n<\|channel>thought\n<channel\|>$', cdump, re.S)
        if not m:
            print("compaction request not in the expected shape: %r" % cdump[-400:])
            return 1
        resp = re.findall(r'<\|tool_response>response:(\w+)\{value:<\|"\|>(.*?)<\|"\|>\}<tool_response\|>', m.group(1), re.S)
        calls = re.findall(r'<\|tool_call>call:(\w+)\{command:<\|"\|>(.*?)<\|"\|>\}<tool_call\|>', dump, re.S)
        if not resp or not calls:
            print("no tool call or response to rebuild the messages")
            return 1
        # mensagens: ate a ultima chamada do dump, o resultado pendente e o pedido de resumo
        msgs = [{"role": "system", "content": content}, {"role": "user", "content": re.search(r"<\|turn>user\n(.*?)<turn\|>", dump, re.S).group(1)}]
        prev = re.findall(r'<\|tool_response>response:(\w+)\{value:<\|"\|>(.*?)<\|"\|>\}<tool_response\|>', dump, re.S)
        for i, (nm, cmd) in enumerate(calls):
            msgs.append({"role": "assistant", "content": "", "tool_calls": [{"id": str(i), "function": {"name": nm, "arguments": {"command": cmd}}}]})
            body = prev[i][1] if i < len(prev) else resp[0][1]
            msgs.append({"role": "tool", "tool_call_id": str(i), "content": body})
        msgs.append({"role": "user", "content": m.group(2)})
        want = render(tpl, msgs, tools, False, True).replace("<|turn>model\n" + EMPTY, "<|turn>model\n")
        got = (dump + cdump).replace("<|turn>model\n" + EMPTY, "<|turn>model\n")
        if want != got:
            print(diff(want, got))
            return 1
        print("live compaction request equal to the template (%d bytes)" % len(got))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
