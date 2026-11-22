#!/usr/bin/env python3
"""Stand-in for the `claude` program in print mode, to test the teacher through the logged-in session without it.

The agent calls: claude -p --system-prompt-file F --tools '' --json-schema S --output-format stream-json --verbose
--include-partial-messages --model M ...
with the conversation on stdin. This script checks the options, the system prompt and the conversation, appends what
it received to $MOCK_CLAUDE_LOG, and prints the result object the real program prints. Scripted teacher: write a
file, read it back, final answer."""
import json, os, sys

a = sys.argv[1:]
log = os.environ.get("MOCK_CLAUDE_LOG")


def fail(why):
    if log:
        open(log, "a").write(json.dumps({"ok": False, "why": why}) + "\n")
    print(json.dumps({"type": "result", "is_error": True, "result": why}))
    sys.exit(1)


def opt(name):
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else None


if "-p" not in a or opt("--output-format") != "stream-json" or "--include-partial-messages" not in a or opt("--tools") != "" or "--no-session-persistence" not in a:
    fail("options: %s" % a)
schema = json.loads(opt("--json-schema") or "null")
if not schema or set(schema.get("required", [])) != {"text", "tool_calls"}:
    fail("schema")
system = open(opt("--system-prompt-file")).read()
if os.environ.get("MOCK_DUMP_SYSTEM"):
    open(os.environ["MOCK_DUMP_SYSTEM"], "w").write(system)
CL = os.environ.get("MOCK_TOOLS") == "2"       # o agente declara o segundo conjunto de ferramentas (KATANA_TOOLS=2)
MCP = os.environ.get("MOCK_MCP") == "1"           # katana -p --mcp-config ... --allowed-tools 'fake__ping,Read': so estas ferramentas aparecem
if MCP:
    if '"name": "fake__ping"' not in system or '"name": "Write"' in system or '"name": "Bash"' in system or '"name": "ToolSearch"' in system:
        fail("system prompt: allowed tools not filtered (fake__ping %s, Write %s, Bash %s, ToolSearch %s)" % tuple('"name": "%s"' % n in system for n in ("fake__ping", "Write", "Bash", "ToolSearch")))
elif "<tool_call>" in system or ('"name": "Write"' if CL else '"name": "write_file"') not in system or '"input_schema"' not in system:
    fail("system prompt")
if CL and not MCP and ('"name": "write_file"' in system or '"file_path"' not in system or '"name": "Bash"' not in system or '"name": "ToolSearch"' not in system):
    fail("system prompt: old tool names or missing new ones")
prompt = sys.stdin.read()
try:
    ms = json.loads(prompt[prompt.index("\n[") + 1:prompt.rindex("]") + 1])
except Exception as e:
    fail("conversation is not JSON: %s" % e)
if not ms or ms[0]["role"] != "user" or ms[-1]["role"] != "user" or any(i and m["role"] == ms[i - 1]["role"] for i, m in enumerate(ms)):
    fail("roles")
last_user_text = max(i for i, m in enumerate(ms) if m["role"] == "user" and any(x["type"] == "text" for x in m["content"]))
step = sum(1 for m in ms[last_user_text + 1:] if m["role"] == "assistant")
if MCP:
    # servidor MCP de teste: chama fake__ping, tenta o Bash (fora da lista: recusado) e responde com o que viu
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    if len(res) == 0:
        out = {"text": "Chamo o servidor.", "tool_calls": [{"name": "fake__ping", "arguments": {"host": "alvo"}}]}
    elif len(res) == 1:
        out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "echo nao"}}]}
    else:
        good = "pong alvo" in res[0] and "not allowed" in res[1]
        out = {"text": "MCP ok: pong alvo; Bash recusado." if good else "FALHA: %s" % res, "tool_calls": []}
elif os.environ.get("MOCK_SILENT") == "1":
    # nunca responde em palavras: so chama ferramentas (headless com --max-turns termina sem resposta final)
    out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "echo calado"}}]}
elif os.environ.get("MOCK_INSIST") == "1":
    # professor teimoso: repete a mesma chamada que falha ate o agente mandar encerrar e relatar
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    if res and "used its tool rounds" in res[-1]:
        good = len(res) == 4 and "times now" in res[1] and "refused" in res[2] and "NOT run again" in res[3]
        out = {"text": "Insistencia contida." if good else "FALHA: %s" % res, "tool_calls": []}
    elif len(res) > 8:
        out = {"text": "FALHA: o agente nao mandou parar", "tool_calls": []}
    else:
        out = {"text": "", "tool_calls": [{"name": "Read", "arguments": {"file_path": "nao_existe.txt"}}]}
elif os.environ.get("MOCK_BLOCKED") == "1":
    # pre-requisito de fora: o programa ./fakesdk falha sempre por falta de algo (menos --help); na 3a falha em volta
    # dele o resultado tem de trazer a nota que nomeia o programa e manda seguir com o codigo
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    calls = [{"name": "Bash", "arguments": {"command": c}} for c in
             ["./fakesdk build", "cd . && ./fakesdk install /Applications/Sdk.app", "./fakesdk --help", "./fakesdk auth login"]]
    if len(res) < 4:
        out = {"text": "", "tool_calls": [calls[len(res)]]}
    elif len(res) == 4:
        good = all("failed around the same thing" not in r for r in res[:3]) and "3 calls in this request failed around the same thing: the program fakesdk" in res[3] \
            and "write the code and files anyway" in res[3]
        out = {"text": "Falta o SDK; sigo com o codigo." if good else "FALHA: %s" % res,
               "tool_calls": [{"name": "Write", "arguments": {"file_path": "app.txt", "content": "codigo\n"}}] if good else []}
    else:
        out = {"text": "Bloqueio relatado e codigo escrito.", "tool_calls": []}
elif os.environ.get("MOCK_ROUNDS") in ("1", "2"):
    # limite de rodadas com KATANA_ROUNDS=4: com progresso (um arquivo gravado por rodada) o pedido passa das 4 rodadas;
    # sem progresso (so leituras) a 3a rodada ja leva a nota de encerrar
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    prog = os.environ["MOCK_ROUNDS"] == "1"
    if any("used its tool rounds" in r for r in res):
        good = not prog and len(res) == 3
        out = {"text": "Limite relatado." if good else "FALHA: nota com %d resultados" % len(res), "tool_calls": []}
    elif prog and len(res) >= 10:
        out = {"text": "Dez arquivos gravados sem parar.", "tool_calls": []}
    elif len(res) > 12:
        out = {"text": "FALHA: o limite nao veio", "tool_calls": []}
    elif prog:
        out = {"text": "", "tool_calls": [{"name": "Write", "arguments": {"file_path": "f%d.txt" % len(res), "content": "x\n"}}]}
    else:
        out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "echo leitura %d" % len(res)}}]}
elif os.environ.get("MOCK_PLAN") in ("1", "2"):
    # planejar antes de executar: sem lista, a 3a e a 6a chamada levam a nota; com lista, a 5a chamada traz o passo atual
    # e o TodoWrite que conclui um passo nomeia o proximo
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    ut = " ".join(x.get("text", "") for x in ms[last_user_text]["content"] if x["type"] == "text")
    def todo(a, b):
        return {"name": "TodoWrite", "arguments": {"todos": [{"content": "passo a", "status": a, "activeForm": "a"},
                {"content": "passo b", "status": b, "activeForm": "b"}, {"content": "passo c", "status": "pending", "activeForm": "c"}]}}
    echo = {"name": "Bash", "arguments": {"command": "echo %d" % len(res), "description": "Mostra um numero"}}
    if "unfinished steps" in ut:
        out = {"text": "Fim.", "tool_calls": []}
    elif os.environ["MOCK_PLAN"] == "1":
        if len(res) < 6:
            out = {"text": "", "tool_calls": [echo]}
        else:
            good = [i for i, r in enumerate(res) if "plan first" in r] == [2] and [i for i, r in enumerate(res) if "still no plan" in r] == [5]
            out = {"text": "Plano cobrado." if good else "FALHA: %s" % res, "tool_calls": []}
    else:
        if len(res) == 0:
            out = {"text": "", "tool_calls": [todo("in_progress", "pending")]}
        elif len(res) < 5:
            out = {"text": "", "tool_calls": [echo]}
        elif len(res) == 5:
            out = {"text": "", "tool_calls": [todo("completed", "in_progress")]}
        else:
            good = "Now step 1 of 3: passo a" in res[0] and "[plan: step 1 of 3 in progress: passo a;" in res[4] and "Now step 2 of 3: passo b" in res[5] \
                and not any("plan first" in r for r in res) and not any("[plan:" in r for r in res[1:4])
            out = {"text": "Plano seguido." if good else "FALHA: %s" % res, "tool_calls": []}
elif os.environ.get("MOCK_GOLD") == "1":
    # conversa curta para conferir o texto renderizado contra o template do modelo: Write; Bash e Grep juntos; resposta
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    if step == 0 and not res:
        out = {"text": "", "tool_calls": [{"name": "Write", "arguments": {"file_path": "g.txt", "content": "um\ndois \"tres\"\n"}}]}
    elif len(res) == 1:
        out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "cat g.txt", "timeout": 20000}},
                                           {"name": "Grep", "arguments": {"pattern": "dois", "path": "g.txt", "output_mode": "content"}}]}
    else:
        out = {"text": "Pronto: g.txt conferido.", "tool_calls": []}
elif os.environ.get("MOCK_DELEGATE") == "1":
    # subagente: chama Agent; o resultado tem de ser a resposta do subagente (ou a falta dela), nunca "nao disponivel"
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    res = [r if isinstance(r, str) else json.dumps(r) for r in res]
    if not res:
        out = {"text": "", "tool_calls": [{"name": "Agent", "arguments": {"description": "find the notes", "prompt": "Find the file notes.txt and report its first line."}}]}
    else:
        good = ("[sub-agent answer" in res[0] or "the sub-agent did not reach an answer" in res[0]) and "not available" not in res[0]
        out = {"text": "Subagente ok." if good else "FALHA: %s" % res[0][:300], "tool_calls": []}
elif os.environ.get("MOCK_LONG") == "1":
    # pedido longo: resultados grandes ate a compactacao automatica; depois a resposta final
    res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
    if len(res) < 6:
        out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "seq %d %d" % (len(res) * 1000, len(res) * 1000 + 250)}}]}
    else:
        out = {"text": "Longo pronto.", "tool_calls": []}
elif os.environ.get("MOCK_REDIRECT") == "1":
    # o usuario muda o plano no meio: o lembrete da lista e a mensagem nova dizem que a mensagem nova vem primeiro
    ut = " ".join(x.get("text", "") for x in ms[last_user_text]["content"] if x["type"] == "text")
    if "your todo list still has unfinished steps" in ut:
        out = {"text": "Lembrete ok." if "newest message comes first" in ut and "TodoWrite" in ut else "FALHA lembrete: %s" % ut[:300], "tool_calls": []}
    elif "esqueca a lista" in ut:
        good = "todo list from before still has 2 unfinished steps" in ut and "comes first" in ut
        out = {"text": "Plano novo." if good else "FALHA pedido novo: %s" % ut[:300], "tool_calls": []}
    elif step == 0:
        out = {"text": "", "tool_calls": [{"name": "TodoWrite", "arguments": {"todos": [
            {"content": "passo um", "status": "in_progress", "activeForm": "Fazendo um"},
            {"content": "passo dois", "status": "pending", "activeForm": "Fazendo dois"}]}}]}
    else:
        out = {"text": "Parei.", "tool_calls": []}
elif CL:
    if step == 0:
        out = {"text": "Gravo o arquivo.", "tool_calls": [{"name": "Write", "arguments": {"file_path": "nota2.txt", "content": "linha 1\nlinha 2\n"}}]}
    elif step == 1:
        out = {"text": "", "tool_calls": [{"name": "Edit", "arguments": {"file_path": "nota2.txt", "old_string": "linha 2", "new_string": "linha dois"}}]}
    elif step == 2:
        out = {"text": "", "tool_calls": [{"name": "Bash", "arguments": {"command": "cat nota2.txt && cd / && pwd", "timeout": 20000}},
                                           {"name": "Grep", "arguments": {"pattern": "linha d", "path": "nota2.txt", "output_mode": "content"}}]}
    else:
        res = [x.get("content") for m in ms for x in m["content"] if x["type"] == "tool_result"]
        res = [r if isinstance(r, str) else json.dumps(r) for r in res]
        good = len(res) == 4 and "linha dois" in res[2] and "Shell cwd was reset" in res[2] and "linha dois" in res[3] and "updated" in res[1]
        out = {"text": "Pronto: ferramentas novas conferidas." if good else "FALHA nos resultados: %s" % res, "tool_calls": []}
elif step == 0:
    out = {"text": "Vou gravar o arquivo: ação \"um\"\ne 😀 dois.", "tool_calls": [{"name": "write_file", "arguments": {"path": "nota.txt", "content": "linha 1\nação \"citada\" <ok>\n"}}]}
elif step == 1:
    out = {"text": "", "tool_calls": [{"name": "run_command", "arguments": {"command": "cat nota.txt", "timeout_s": 20}}]}
else:
    out = {"text": "Pronto: nota.txt gravado e conferido.", "tool_calls": []}
if log:
    open(log, "a").write(json.dumps({"ok": True, "step": step, "model": opt("--model"), "messages": len(ms)}) + "\n")
# como o programa de verdade: o objeto da resposta chega em pedacos (cortados no meio de escapes e de acentos)
sys.stdout.write(json.dumps({"type": "system", "subtype": "init"}) + "\n")
for enc in ([json.dumps(out)] if step % 2 == 0 else [json.dumps(out, ensure_ascii=False)]):
    for i in range(0, len(enc), 5):
        sys.stdout.write(json.dumps({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0,
                                     "delta": {"type": "input_json_delta", "partial_json": enc[i:i + 5]}}}, separators=(",", ":"), ensure_ascii=(i % 2 == 0)) + "\n")
        sys.stdout.flush()
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": json.dumps(out), "structured_output": out,
                  "usage": {"input_tokens": 10, "cache_read_input_tokens": 5, "cache_creation_input_tokens": 0, "output_tokens": 7}}))
