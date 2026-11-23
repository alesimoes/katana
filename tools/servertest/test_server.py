#!/usr/bin/env python3
"""Teste do katana serve de ponta a ponta (rode com o modelo minusculo de tools/tinymodel).

Um servidor ja de pe (testes basicos: texto, stop, stream, JSON, fila):
  python3 tools/tinymodel/make_tiny_gguf.py /tmp/tiny.gguf --no-eos
  ./katana serve /tmp/tiny.gguf 8091 &
  python3 tools/servertest/test_server.py http://127.0.0.1:8091 [CHAVE]

Tudo (sobe e derruba os servidores sozinho: basicos, ferramentas em cada formato, raciocinio do Gemma 4, agente):
  python3 tools/servertest/test_server.py --all ./katana

Um modelo aleatorio nao chama ferramentas: os testes de ferramentas usam KATANA_TEST_RAW_OUTPUT (o texto cru que o
"modelo" escreve; o prompt e montado igual). O agente pela rede usa o professor de mentira (tools/traintest/mock_teacher_prog.py).
"""
import json, os, urllib.request, threading, time, socket, subprocess, sys, tempfile, shutil

ok = 0; bad = 0
def check(c, msg):
    global ok, bad
    if c: ok += 1
    else: bad += 1; print("FAIL", msg)

def post(B, path, body, key=None, method="POST", timeout=120):
    h = {"Content-Type": "application/json"}
    if key: h["Authorization"] = "Bearer " + key
    data = json.dumps(body).encode() if body is not None else (b"" if method == "POST" else None)
    r = urllib.request.Request(B + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r, timeout=timeout) as f: return f.status, f.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()

def sse(B, path, body, key=None, timeout=300):
    """eventos de um stream: lista de (evento, dados)"""
    h = {"Content-Type": "application/json"}
    if key: h["Authorization"] = "Bearer " + key
    r = urllib.request.Request(B + path, data=json.dumps(body).encode(), headers=h)
    out = []; ev = "message"
    with urllib.request.urlopen(r, timeout=timeout) as f:
        for line in f:
            line = line.decode().rstrip("\n")
            if line.startswith("event: "): ev = line[7:]
            elif line.startswith("data: "):
                d = line[6:]
                out.append((ev, d if d == "[DONE]" else json.loads(d))); ev = "message"
    return out

def basic(B, KEY):
    m = [{"role": "user", "content": "hello"}]
    st, b = post(B, "/v1/chat/completions", {"messages": m, "max_tokens": 20, "temperature": 0}, KEY)
    full = json.loads(b)["choices"][0]["message"]["content"]
    check(st == 200 and json.loads(b)["choices"][0]["finish_reason"] == "length", "base")
    # stop: pick a substring from the middle of the deterministic answer
    stop = next(ch for i, ch in enumerate(full) if i > 2 and 33 <= ord(ch) < 127)
    st, b = post(B, "/v1/chat/completions", {"messages": m, "max_tokens": 20, "temperature": 0, "stop": [stop]}, KEY)
    r = json.loads(b)["choices"][0]
    check(r["finish_reason"] == "stop" and r["message"]["content"] == full[:full.index(stop)], "stop %r -> %r (full %r)" % (stop, r["message"]["content"], full))
    # streaming gives the same text as non-streaming
    ev = sse(B, "/v1/chat/completions", {"messages": m, "max_tokens": 20, "temperature": 0, "stream": True}, KEY)
    txt = "".join(c["delta"].get("content", "") for e, d in ev if d != "[DONE]" for c in d["choices"])
    check(ev[-1][1] == "[DONE]" and txt == full, "stream text %r vs %r" % (txt, full))
    # errors
    st, b = post(B, "/v1/chat/completions", {"messages": [{"role": "user", "content": "a"}], "tools": [{"type": "function"}]}, KEY)
    check(st == 400 and "tools" in json.loads(b)["error"]["message"], "malformed tools err %s %s" % (st, b))
    st, b = post(B, "/v1/chat/completions", {"nope": 1}, KEY)
    check(st == 400, "missing messages %s" % st)
    st, b = post(B, "/v1/chat/completions", {"messages": m, "response_format": {"type": "json_object"}, "max_tokens": 40, "temperature": 0}, KEY)
    try: content = json.loads(b)["choices"][0]["message"]["content"]; js_ok = True
    except Exception: js_ok = False
    check(st == 200 and js_ok, "json mode %s %s" % (st, b[:200]))
    # legacy /api/chat still works
    st, b = post(B, "/api/chat", {"messages": m, "max_tokens": 5, "temperature": 0}, KEY)
    check(st == 200 and '"done":true' in b, "/api/chat %s %s" % (st, b[:200]))
    # tools on a real (random) model: declared, rendered and answered (no call expected from random weights)
    st, b = post(B, "/v1/chat/completions", {"messages": m, "max_tokens": 10, "temperature": 0, "tools": [TOOL]}, KEY)
    check(st == 200 and json.loads(b)["choices"][0]["finish_reason"] in ("length", "stop", "tool_calls"), "tools on random model %s %s" % (st, b[:200]))
    # concurrency: 4 parallel requests all succeed; a client that disconnects mid-stream does not block the queue
    res = []
    def one(i):
        st, b = post(B, "/v1/chat/completions", {"messages": [{"role": "user", "content": "q%d" % i}], "max_tokens": 200, "temperature": 0.8}, KEY)
        res.append(st)
    s = socket.create_connection(("127.0.0.1", int(B.rsplit(":", 1)[1])))
    body = json.dumps({"messages": m, "max_tokens": 400, "stream": True}).encode()
    hdr = "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
    if KEY: hdr += "Authorization: Bearer %s\r\n" % KEY
    s.sendall(hdr.encode() + b"\r\n" + body); s.recv(100); s.close()
    ts = [threading.Thread(target=one, args=(i,)) for i in range(4)]
    t0 = time.time(); [t.start() for t in ts]; [t.join() for t in ts]
    check(res == [200] * 4, "parallel %s" % res)
    print("basic: parallel took %.1fs" % (time.time() - t0))

TOOL = {"type": "function", "function": {"name": "get_weather", "description": "Weather now", "parameters": {"type": "object",
        "properties": {"city": {"type": "string"}, "days": {"type": "integer"}}, "required": ["city"]}}}
HIST = [{"role": "user", "content": "weather in Rio?"},
        {"role": "assistant", "content": None, "tool_calls": [{"id": "call_1", "type": "function", "function": {"name": "get_weather", "arguments": "{\"city\": \"Rio\"}"}}]},
        {"role": "tool", "tool_call_id": "call_1", "content": "sunny, 30 C"},
        {"role": "user", "content": "and in 3 days?"}]
RAW = {   # texto cru do "modelo" em cada formato: texto antes, uma chamada com um inteiro que o XML/GLM escrevem como texto
    "chatml-xml": "I'll check.\n\n<tool_call>\n<function=get_weather>\n<parameter=city>\nRio\n</parameter>\n<parameter=days>\n3\n</parameter>\n</function>\n</tool_call>",
    "chatml": "I'll check.\n\n<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"city\": \"Rio\", \"days\": \"3\"}}\n</tool_call>",
    "gemma4": "<|channel>thought\nneed the weather<channel|>I'll check.<|tool_call>call:get_weather{city:<|\"|>Rio<|\"|>,days:3}<tool_call|>",
    "glm": "need the weather</think>I'll check.\n<tool_call>get_weather<arg_key>city</arg_key><arg_value>Rio</arg_value><arg_key>days</arg_key><arg_value>3</arg_value></tool_call>",
}

def tools_fmt(B, fmt):
    req = {"messages": HIST, "tools": [TOOL], "temperature": 0, "max_tokens": 50}
    st, b = post(B, "/v1/chat/completions", req)
    try:
        ch = json.loads(b)["choices"][0]; msg = ch["message"]; tc = msg["tool_calls"]
        good = (ch["finish_reason"] == "tool_calls" and msg["content"] == "I'll check." and len(tc) == 1 and tc[0]["type"] == "function"
                and tc[0]["id"].startswith("call_") and tc[0]["function"]["name"] == "get_weather"
                and json.loads(tc[0]["function"]["arguments"]) == {"city": "Rio", "days": 3})
        if fmt in ("gemma4", "glm"): good = good and msg.get("reasoning_content") == "need the weather"
    except Exception as e: good = False; b += " (%r)" % e
    check(st == 200 and good, "%s non-stream tool call: %s %s" % (fmt, st, b[:600]))
    # streaming: content deltas, then tool_calls deltas (index, id, name, argument fragments), finish tool_calls
    ev = sse(B, "/v1/chat/completions", dict(req, stream=True))
    content = ""; reason = ""; calls = {}; finish = None; leaked = False
    for e, d in ev:
        if d == "[DONE]": continue
        for c in d["choices"]:
            dl = c["delta"]
            content += dl.get("content") or ""; reason += dl.get("reasoning_content") or ""
            if "tool_call" in (dl.get("content") or ""): leaked = True
            for t in dl.get("tool_calls", []):
                x = calls.setdefault(t["index"], {"id": None, "name": "", "args": ""})
                if t.get("id"): x["id"] = t["id"]
                f = t.get("function", {})
                x["name"] += f.get("name") or ""; x["args"] += f.get("arguments") or ""
            if c.get("finish_reason"): finish = c["finish_reason"]
    good = (ev[-1][1] == "[DONE]" and finish == "tool_calls" and content.strip() == "I'll check." and not leaked and list(calls) == [0]
            and calls[0]["id"] and calls[0]["name"] == "get_weather" and json.loads(calls[0]["args"]) == {"city": "Rio", "days": 3})
    if fmt in ("gemma4", "glm"): good = good and reason == "need the weather"
    check(good, "%s stream tool call: content=%r reason=%r calls=%r finish=%r" % (fmt, content, reason, calls, finish))
    # tool_choice required and a named function are accepted; none: no tool_calls come back
    for tc_ in ("required", {"type": "function", "function": {"name": "get_weather"}}):
        st, b = post(B, "/v1/chat/completions", dict(req, tool_choice=tc_))
        check(st == 200, "%s tool_choice %r: %s %s" % (fmt, tc_, st, b[:300]))
    st, b = post(B, "/v1/chat/completions", dict(req, tool_choice="none"))
    check(st == 200 and "tool_calls" not in json.loads(b)["choices"][0]["message"], "%s tool_choice none: %s" % (fmt, b[:300]))
    # validation errors
    for bad_req, what in ((dict(req, messages=HIST + [{"role": "tool", "tool_call_id": "nope", "content": "x"}]), "tool_call_id"),
                          (dict(req, messages=[{"role": "wizard", "content": "x"}]), "role"),
                          (dict(req, tools=[{"type": "function", "function": {"description": "no name"}}]), "tools")):
        st, b = post(B, "/v1/chat/completions", bad_req)
        check(st == 400 and what in json.loads(b)["error"]["message"], "%s 400 %s: %s %s" % (fmt, what, st, b[:200]))
    if fmt == "gemma4":   # raciocinio do Gemma 4 sem ferramentas: o canal vai para reasoning_content
        st, b = post(B, "/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}], "max_tokens": 50})
        msg = json.loads(b)["choices"][0]["message"]
        check(msg.get("reasoning_content") == "need the weather" and msg["content"].startswith("I'll check.") and "<|channel>" not in msg["content"], "gemma4 reasoning split: %s" % b[:300])
        ev = sse(B, "/v1/chat/completions", {"messages": [{"role": "user", "content": "hi"}], "max_tokens": 50, "stream": True})
        r = "".join(c["delta"].get("reasoning_content") or "" for e, d in ev if d != "[DONE]" for c in d["choices"])
        t = "".join(c["delta"].get("content") or "" for e, d in ev if d != "[DONE]" for c in d["choices"])
        check(r == "need the weather" and t.startswith("I'll check."), "gemma4 stream reasoning split: %r %r" % (r, t))

def start(cmd, env, port):
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(600):
        try: socket.create_connection(("127.0.0.1", port), timeout=1).close(); return p
        except OSError: time.sleep(0.1)
        if p.poll() is not None: break
    raise SystemExit("server did not start: %s" % p.stderr.read().decode()[-2000:])

def stop(p):
    p.terminate()
    try: p.wait(10)
    except subprocess.TimeoutExpired: p.kill()

def agent(K, D, port):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    model = os.path.join(D, "agent.gguf")
    subprocess.check_call([sys.executable, os.path.join(root, "tools/tinymodel/make_tiny_gguf.py"), model, "--template", "chatml-xml"], stdout=subprocess.DEVNULL)
    home = os.path.join(D, "home"); work = os.path.join(D, "work"); os.makedirs(home, exist_ok=True); os.makedirs(work, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("KATANA_") and not k.endswith("_API_KEY")}   # professor so pelo programa de mentira
    env.update(HOME=home, KATANA_CPU="1", KATANA_TEACHER="1", 
               KATANA_TEACHER_CMD="python3 " + os.path.join(root, "tools/traintest/mock_teacher_prog.py"))
    # fora do loopback sem chave: recusa subir
    r = subprocess.run([K, "serve", model, str(port), "--agent"], env=dict(env, KATANA_HOST="0.0.0.0"), capture_output=True, timeout=30)
    check(r.returncode != 0 and b"KATANA_API_KEY" in r.stderr, "agent mode on the network without a key must refuse: %r" % r.stderr[-300:])
    p = start([K, "serve", model, str(port), "--agent"], env, port)
    B = "http://127.0.0.1:%d" % port
    try:
        st, b = post(B, "/v1/chat/completions", {"messages": [{"role": "user", "content": "x"}]})
        check(st == 503 and "agent mode" in b, "chat completions in agent mode: %s %s" % (st, b[:200]))
        st, b = post(B, "/v1/agent/sessions", {"cwd": work, "session_id": "t1"}, timeout=600)
        s = json.loads(b) if st == 200 else {}
        check(st == 200 and s.get("id") == "t1" and s.get("state") == "ready" and s.get("cwd") == os.path.realpath(work), "create: %s %s" % (st, b[:300]))
        st, b = post(B, "/v1/agent/sessions", {"cwd": work})
        check(st == 409, "second session must be 409: %s" % st)
        st, b = post(B, "/v1/agent/sessions/t1/interrupt", None)
        check(st == 409, "interrupt while idle: %s" % st)
        st, b = post(B, "/v1/agent/sessions/t1/answer", {"answer": "x"})
        check(st == 409, "answer with no question: %s" % st)
        st, b = post(B, "/v1/agent/sessions/t1/messages", {"content": "/exit"})
        check(st == 400, "slash command refused: %s" % st)
        ev = sse(B, "/v1/agent/sessions/t1/messages", {"content": "grave e confira\nnota2.txt"}, timeout=600)
        kinds = [e for e, d in ev]
        names = [d["name"] for e, d in ev if e == "tool_call"]
        text = "".join(d["text"] for e, d in ev if e == "text")
        results = [d for e, d in ev if e == "tool_result"]
        good = (kinds and kinds[-1] == "done" and names[:2] == ["Write", "Edit"] and "Bash" in names and len(results) >= 3
                and kinds.index("tool_call") < kinds.index("tool_result") and "Pronto: ferramentas novas conferidas." in text and "\x1b" not in text)
        check(good, "agent events: kinds=%s names=%s text=%r" % (kinds, names, text[-300:]))
        try: content = open(os.path.join(work, "nota2.txt")).read()
        except OSError: content = None
        check(content == "linha 1\nlinha dois\n", "file written through the API: %r" % content)
        st, b = post(B, "/v1/agent/sessions/t1", None, method="GET")
        check(st == 200 and json.loads(b)["state"] == "ready" and json.loads(b)["session_file"].endswith("t1.ksess"), "state: %s %s" % (st, b[:300]))
        st, b = post(B, "/v1/agent/sessions/zz", None, method="GET")
        check(st == 404, "unknown session: %s" % st)
        st, b = post(B, "/v1/agent/sessions/t1", None, method="DELETE")
        check(st == 200, "delete: %s %s" % (st, b))
        st, b = post(B, "/v1/agent/sessions", None, method="GET")
        check(st == 200 and json.loads(b)["data"] == [], "list after delete: %s" % b)
    finally:
        stop(p)

def run_all(K):
    K = os.path.abspath(K)
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    D = tempfile.mkdtemp(prefix="katana-servertest-")
    port = 18291
    try:
        base = os.path.join(D, "tiny.gguf")
        subprocess.check_call([sys.executable, os.path.join(root, "tools/tinymodel/make_tiny_gguf.py"), base, "--no-eos"], stdout=subprocess.DEVNULL)
        env = {k: v for k, v in os.environ.items() if not k.startswith("KATANA_")}
        env["KATANA_CPU"] = "1"; env["KATANA_CTX"] = "4096"   # o bloco de ferramentas nao cabe nos 512 do modelo minusculo
        p = start([K, "serve", base, str(port)], env, port)
        try: basic("http://127.0.0.1:%d" % port, None)
        finally: stop(p)
        print("basic: ok=%d fail=%d" % (ok, bad))
        for fmt in ("chatml-xml", "chatml", "gemma4", "glm"):
            port += 1
            mp = os.path.join(D, fmt + ".gguf")
            subprocess.check_call([sys.executable, os.path.join(root, "tools/tinymodel/make_tiny_gguf.py"), mp, "--template", fmt], stdout=subprocess.DEVNULL)
            e = dict(env, KATANA_TEST_RAW_OUTPUT=RAW[fmt])
            if fmt == "gemma4": e["KATANA_THINK"] = "1"
            p = start([K, "serve", mp, str(port)], e, port)
            try: tools_fmt("http://127.0.0.1:%d" % port, fmt)
            finally: stop(p)
            print("tools %s: ok=%d fail=%d" % (fmt, ok, bad))
        agent(K, D, port + 1)
        print("agent: ok=%d fail=%d" % (ok, bad))
    finally:
        shutil.rmtree(D, ignore_errors=True)

if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--all":
        run_all(sys.argv[2])
    else:
        basic(sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8091", sys.argv[2] if len(sys.argv) > 2 else None)
    print("ok=%d fail=%d" % (ok, bad))
    sys.exit(1 if bad else 0)
