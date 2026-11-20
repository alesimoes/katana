#!/usr/bin/env python3
"""Stand-in for the Anthropic Messages API, to test the agent's teacher mode without a key or network.

Usage: mock_anthropic.py PORT LOGFILE
It checks every request the way the real API would reject it (headers, alternating roles, every tool_use answered by
a tool_result in the next message, no empty content, thinking blocks returned unchanged) and answers 400 with the
reason when a rule is broken. The scripted teacher: writes a file, runs a command to read it back, then gives the
final answer. Every request body is appended to LOGFILE (one JSON per line)."""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT, LOG = int(sys.argv[1]), sys.argv[2]
SIG = "sig-mock-0001"


def check(h, b):
    if h.get("x-api-key") != "sk-mock-key":
        return 401, "bad x-api-key"
    if h.get("anthropic-version") != "2026-10-21":
        return 400, "bad anthropic-version"
    for k in ("temperature", "top_p", "top_k"):
        if k in b:
            return 400, "%s is not accepted" % k
    if b.get("fallbacks") is not None and "server-side-fallback-2026-07-01" not in h.get("anthropic-beta", ""):
        return 400, "fallbacks without its beta header"
    if not isinstance(b.get("system"), str) or "<tool_call>" in b["system"]:
        return 400, "system prompt still teaches the student's call syntax"
    names = [t.get("name") for t in b.get("tools", [])]
    if "write_file" not in names or "run_command" not in names or "todo" not in names or any("input_schema" not in t for t in b["tools"]):
        return 400, "tools: %s" % names
    ms = b.get("messages", [])
    if not ms or ms[0]["role"] != "user" or ms[-1]["role"] != "user":
        return 400, "first and last message must be user"
    pend = []
    for i, m in enumerate(ms):
        if i and m["role"] == ms[i - 1]["role"]:
            return 400, "roles do not alternate at %d" % i
        c = m["content"]
        if not isinstance(c, list) or not c:
            return 400, "empty content at %d" % i
        if m["role"] == "user":
            got = [x["tool_use_id"] for x in c if x["type"] == "tool_result"]
            if got != pend:
                return 400, "tool_result ids %s, expected %s at %d" % (got, pend, i)
            pend = []
        else:
            if pend:
                return 400, "tool_use without tool_result before %d" % i
            for x in c:
                if x["type"] == "text" and not x["text"].strip():
                    return 400, "empty text block at %d" % i
                if x["type"] == "thinking" and x.get("signature") != SIG:
                    return 400, "thinking block changed at %d" % i
            pend = [x["id"] for x in c if x["type"] == "tool_use"]
    return 200, ""


def answer(b):
    ms = b["messages"]
    done = sum(1 for m in ms if m["role"] == "user" and any(x["type"] == "tool_result" for x in m["content"]))
    # so as chamadas feitas depois do ultimo pedido do usuario contam (a conversa pode ter turnos anteriores)
    last_user_text = max(i for i, m in enumerate(ms) if m["role"] == "user" and any(x["type"] == "text" for x in m["content"]))
    step = sum(1 for m in ms[last_user_text + 1:] if m["role"] == "assistant")
    if step == 0:
        content = [{"type": "thinking", "thinking": "The user wants a file. I will write it and read it back.", "signature": SIG},
                   {"type": "text", "text": "Vou gravar o arquivo."},
                   {"type": "tool_use", "id": "toolu_w%d" % done, "name": "write_file",
                    "input": {"path": "nota.txt", "content": "linha 1\nação \"citada\" <ok>\n"}}]
        stop = "tool_use"
    elif step == 1:
        content = [{"type": "thinking", "thinking": "", "signature": SIG},
                   {"type": "tool_use", "id": "toolu_r%d" % done, "name": "run_command", "input": {"command": "cat nota.txt", "timeout_s": 20}},
                   {"type": "tool_use", "id": "toolu_l%d" % done, "name": "list_dir", "input": {"path": "."}}]
        stop = "tool_use"
    else:
        content = [{"type": "text", "text": "Pronto: nota.txt gravado e conferido."}]
        stop = "end_turn"
    return {"id": "msg_mock", "type": "message", "role": "assistant", "model": b["model"], "content": content, "stop_reason": stop,
            "usage": {"input_tokens": 100, "output_tokens": 20, "cache_read_input_tokens": 50 * step, "cache_creation_input_tokens": 0}}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", 0)))
        try:
            b = json.loads(raw)
        except Exception as e:
            b = None
            code, why = 400, "invalid JSON: %s" % e
        if b is not None:
            code, why = (404, "not found") if self.path != "/v1/messages" else check({k.lower(): v for k, v in self.headers.items()}, b)
        out = answer(b) if code == 200 else {"type": "error", "error": {"type": "invalid_request_error", "message": why}}
        open(LOG, "a").write(json.dumps({"status": code, "why": why, "request": b}) + "\n")
        data = json.dumps(out).encode()
        self.send_response(code); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(data))); self.end_headers()
        self.wfile.write(data)


HTTPServer(("127.0.0.1", PORT), H).serve_forever()
