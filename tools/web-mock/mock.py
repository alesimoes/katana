#!/usr/bin/env python3
"""Stand-in for `katana serve --agent`, to look at the web page without a model.

It extracts the page (SRV_PAGE) straight from src/app/server.c and imitates /v1/agent/sessions with canned
server-sent events (tool_call, tool_output, tool_result, todo, question, permission, text, tokens, done).

  python3 tools/web-mock/mock.py [port] [--chat-only] [--key SECRET] [--fast]

--chat-only: /v1/agent/* answers 404 like a server without --agent (the page falls back to plain chat).
--key: /v1/* answers 401 without "Authorization: Bearer SECRET" (the page then asks for the key).
?send=text makes the page send that text on load (for screenshots). Message text picks the scenario: "pergunta" asks a question, "permiss" asks permission, "erro" fails a tool,
"sair" ends the agent, anything else runs a short tool sequence.
"""
import json, re, sys, time, threading
_sleep = time.sleep
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src/app/server.c"

def page():
    s = SRC.read_text()
    a = s.index("static const char SRV_PAGE[] =\n") + len("static const char SRV_PAGE[] =\n")
    b = s.index('";\n', a) + 1
    out = []
    for l in re.findall(r'^"(.*)"$', s[a:b], re.M):
        out.append(bytes(l, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8"))
    return "".join(out).encode()

ARGS = sys.argv[1:]
CHAT_ONLY = "--chat-only" in ARGS
KEY = ARGS[ARGS.index("--key") + 1] if "--key" in ARGS else ""
FAST = "--fast" in ARGS   # no pauses: lets a headless browser with a virtual clock see the whole stream
if FAST: time.sleep = lambda t: _sleep(0.05 if t < 5 else t)
PORT = next((int(a) for a in ARGS if a.isdigit()), 8099)
S = {"id": None, "state": "ready", "waiting": False, "answer": None, "stop": False}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj, ctype="application/json"):
        b = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try: return json.loads(self.rfile.read(n) or b"{}")
        except Exception: return {}
    def sess(self):
        return {"id": S["id"], "object": "agent.session", "state": S["state"], "cwd": "/home/dono/projeto",
                "tokens": {"used": 5120, "context": 131072}, "question": None, "todo": []}
    def ev(self, name, data):
        self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(data))).encode()); self.wfile.flush()
    def auth(self):
        if KEY and self.headers.get("Authorization") != "Bearer " + KEY:
            self.send(401, {"error": {"message": "missing or invalid API key (Authorization: Bearer ...)"}}); return False
        return True
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            h = page()
            q = re.search(r"send=([^&]*)", self.path)   # ?send=texto: the page sends it by itself (for screenshots)
            if q:
                from urllib.parse import unquote
                js = "setTimeout(function(){document.getElementById('in').value=%s;document.getElementById('f').requestSubmit()},400)" % json.dumps(unquote(q.group(1)))
                h = h.replace(b"</script>", ("\n" + js + "\n</script>").encode(), 1)
            return self.send(200, h, "text/html; charset=utf-8")
        if p == "/v1/models": return self.send(200, {"data": [{"id": "mock-model"}]}) if self.auth() else None
        if p.startswith("/v1/agent/"):
            if not self.auth(): return
            if CHAT_ONLY: return self.send(404, {"error": {"message": "agent mode off"}})
            return self.send(200, {"object": "list", "data": [self.sess()] if S["id"] else []})
        self.send(404, {"error": "nao encontrado"})
    def do_DELETE(self):
        if not self.auth(): return
        S["id"] = None; self.send(200, {"ok": True})
    def do_POST(self):
        p = self.path.split("?")[0]; b = self.body()
        if p == "/api/chat":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
            for t in ["Ola, ", "isto e ", "o chat simples."]:
                self.wfile.write(("data: %s\n\n" % json.dumps({"token": t})).encode()); self.wfile.flush(); time.sleep(0.1)
            self.wfile.write(("data: %s\n\n" % json.dumps({"done": True, "tok_s": 9.9, "tokens": 3, "ctx": 10, "ctx_max": 100, "cached": 0, "prompt_tokens": 5})).encode()); return
        if not self.auth(): return
        if CHAT_ONLY: return self.send(404, {"error": {"message": "agent mode off"}})
        if p == "/v1/agent/sessions":
            time.sleep(2)  # "carregando o modelo"
            S.update(id=b.get("session_id") or "mock1", state="ready"); return self.send(200, self.sess())
        if p.endswith("/answer"):
            if not S["waiting"]: return self.send(409, {"error": {"message": "no question is waiting"}})
            S["answer"] = b.get("answer"); S["waiting"] = False; return self.send(200, {"ok": True})
        if p.endswith("/interrupt"): S["stop"] = True; return self.send(200, {"ok": True})
        if p.endswith("/messages"): return self.run(b.get("content", ""))
        self.send(404, {"error": "?"})
    def run(self, msg):
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        e = self.ev; S["state"] = "busy"
        e("tokens", {"used": 98000, "context": 131072})
        e("mode", {"mode": "plan"}); e("status", {"marker": "PREP", "args": "1200"}); time.sleep(0.5)
        e("todo", {"items": ["[ ] Ler o projeto", "[ ] Escrever notes.txt", "[ ] Conferir o resultado"]})
        e("text", {"text": "Vou olhar a pasta primeiro.\n"})
        e("tool_call", {"name": "Bash", "arguments": "command: ls -la src | head", "description": ""}); time.sleep(0.4)
        e("tool_output", {"text": "total 8\n"}); time.sleep(0.4); e("tool_output", {"text": "drwxr-xr-x app\n"}); time.sleep(0.4)
        e("tool_result", {"name": "Bash", "output": "total 8\ndrwxr-xr-x app\ndrwxr-xr-x engine\n"})
        e("tool_call", {"name": "Read", "arguments": "file_path: /home/dono/projeto/README.md", "description": ""}); time.sleep(0.3)
        e("tool_result", {"name": "Read", "output": "# Projeto\nolá <script>alert(1)</script>\n"})
        e("todo", {"items": ["[x] Ler o projeto", "[ ] Escrever notes.txt", "[ ] Conferir o resultado"]})
        if "erro" in msg:
            e("tool_call", {"name": "Write", "arguments": "file_path: /root/x", "description": "Gravar em /root"}); time.sleep(0.3)
            e("tool_result", {"name": "Write", "output": "error: permission denied"})
        if "pergunta" in msg or "permiss" in msg:
            S["waiting"] = True; S["answer"] = None
            if "permiss" in msg: e("permission", {"kind": "run", "what": "executar rm -rf build", "detail": "cwd /home/dono/projeto"})
            else: e("question", {"question": "Qual formato prefere?", "options": ["Markdown", "Texto puro"]})
            for _ in range(120):
                if S["answer"] is not None: break
                time.sleep(0.25)
            e("text", {"text": "Resposta recebida: **%s**.\n" % S["answer"]})
        if "parado" in msg:   # stays running (a command with no end) to look at the live state
            e("tool_call", {"name": "Bash", "arguments": "command: sleep 600", "description": ""}); e("tool_output", {"text": "esperando...\n"}); time.sleep(300); return
        if "sair" in msg:
            e("exit", {"exit_code": 1}); S["id"] = None; return
        e("notify", {"title": "katana", "message": "terminei os passos"})
        e("file", {"mode": "local", "path": "/home/dono/projeto/notes.txt", "caption": "as tarefas"})
        e("text", {"text": "Pronto. Gravei `notes.txt`:\n```\n- tarefa 1\n- tarefa 2\n```\nUse **com cuidado** <b>html</b>.\n"})
        e("tokens", {"used": 112000, "context": 131072})
        S["state"] = "ready"; e("done", {})

if __name__ == "__main__":
    print("mock em http://127.0.0.1:%d/" % PORT)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
