"""Scripted stand-in teacher program for the katana teacher protocol (docs/teacher-protocol.md).

The agent runs this program with one JSON request line on stdin. The script checks the request (tool declarations, roles,
the conversation), appends what it received to $MOCK_TEACHER_LOG, and answers with a done event. Scenarios are picked
by environment variables (MOCK_TOOLS, MOCK_MCP, MOCK_PLAN, ...); the default is: write a file, read it back, final answer."""
import json, os, sys

log = os.environ.get("MOCK_TEACHER_LOG")


def fail(why):
    if log:
        open(log, "a").write(json.dumps({"ok": False, "why": why}) + "\n")
    print(json.dumps({"event": "error", "message": why}))
    sys.exit(1)


try:
    req = json.loads(sys.stdin.readline())
except Exception as e:
    fail("request is not JSON: %s" % e)
if req.get("protocol") != 1 or set(req) < {"system", "tools", "messages", "options"}:
    fail("request fields: %s" % sorted(req))
system = req["system"]
names = [t["name"] for t in req["tools"]]
if any("parameters" not in t for t in req["tools"]):
    fail("tool without parameters")
if os.environ.get("MOCK_DUMP_SYSTEM"):
    open(os.environ["MOCK_DUMP_SYSTEM"], "w").write(system)
CL = os.environ.get("MOCK_TOOLS") == "2"       # o agente declara o segundo conjunto de ferramentas (KATANA_TOOLS=2)
MCP = os.environ.get("MOCK_MCP") == "1"           # katana -p --mcp-config ... --allowed-tools 'fake__ping,Read': so estas ferramentas aparecem
if MCP:
    if "fake__ping" not in names or "Write" in names or "Bash" in names or "ToolSearch" in names:
        fail("tools: allowed tools not filtered (fake__ping %s, Write %s, Bash %s, ToolSearch %s)" % tuple(n in names for n in ("fake__ping", "Write", "Bash", "ToolSearch")))
elif "<tool_call>" in system or ("Write" if CL else "write_file") not in names:
    fail("system prompt or tools")
if CL and not MCP and ("write_file" in names or "file_path" not in json.dumps(req["tools"]) or "Bash" not in names or "ToolSearch" not in names):
    fail("tools: old tool names or missing new ones")
ms = req["messages"]
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
            good = "Now step 1 of 3: passo a" in res[0] and "Now step 2 of 3: passo b" in res[5] \
                and not any("plan first" in r for r in res) and not any("[plan:" in r for r in res)      # o lembrete "[plan: step ..." foi cortado
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
    open(log, "a").write(json.dumps({"ok": True, "step": step, "model": req["options"].get("model"), "messages": len(ms)}) + "\n")
# como um programa de verdade: texto em pedacos (cortados no meio de acentos), depois o evento done
if out["text"]:
    for i in range(0, len(out["text"]), 5):
        print(json.dumps({"event": "text", "text": out["text"][i:i + 5]}, ensure_ascii=(i % 2 == 0)), flush=True)
done = {"event": "done", "text": out["text"], "tool_calls": [dict(c, id="m%d_%d" % (len(ms), i)) for i, c in enumerate(out["tool_calls"])],
        "stop": "end_turn", "usage": {"input": 15, "output": 7, "cached": 5}}
print(json.dumps(done, ensure_ascii=(step % 2 == 0)))
