#!/usr/bin/env python3
"""Teste do katana serve de ponta a ponta (rode com o modelo minusculo de tools/tinymodel).

  python3 tools/tinymodel/make_tiny_gguf.py /tmp/tiny.gguf --no-eos
  ./katana serve /tmp/tiny.gguf 8091 &
  python3 tools/servertest/test_server.py http://127.0.0.1:8091 [CHAVE]
"""
import json, urllib.request, threading, time, socket, sys
B = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8091"
KEY = sys.argv[2] if len(sys.argv) > 2 else None
def post(path, body, key=KEY, raw=False):
    h = {"Content-Type": "application/json"}
    if key: h["Authorization"] = "Bearer " + key
    r = urllib.request.Request(B + path, data=json.dumps(body).encode(), headers=h)
    try:
        with urllib.request.urlopen(r, timeout=60) as f: return f.status, f.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()
ok = 0; bad = 0
def check(c, msg):
    global ok, bad
    if c: ok += 1
    else: bad += 1; print("FAIL", msg)
m = [{"role": "user", "content": "hello"}]
st, b = post("/v1/chat/completions", {"messages": m, "max_tokens": 20, "temperature": 0})
full = json.loads(b)["choices"][0]["message"]["content"]
check(st == 200 and json.loads(b)["choices"][0]["finish_reason"] == "length", "base")
# stop: pick a substring from the middle of the deterministic answer
stop = next(ch for i, ch in enumerate(full) if i > 2 and 33 <= ord(ch) < 127)
st, b = post("/v1/chat/completions", {"messages": m, "max_tokens": 20, "temperature": 0, "stop": [stop]})
r = json.loads(b)["choices"][0]
check(r["finish_reason"] == "stop" and r["message"]["content"] == full[:full.index(stop)], "stop %r -> %r (full %r)" % (stop, r["message"]["content"], full))
# streaming gives the same text as non-streaming
r = urllib.request.Request(B + "/v1/chat/completions", data=json.dumps({"messages": m, "max_tokens": 20, "temperature": 0, "stream": True}).encode(),
                           headers=dict({"Content-Type": "application/json"}, **({"Authorization": "Bearer " + KEY} if KEY else {})))
txt = ""; done = False
with urllib.request.urlopen(r, timeout=60) as f:
    for line in f:
        line = line.decode().strip()
        if not line.startswith("data: "): continue
        d = line[6:]
        if d == "[DONE]": done = True; break
        j = json.loads(d)
        for c in j["choices"]: txt += c["delta"].get("content", "")
check(done and txt == full, "stream text %r vs %r" % (txt, full))
# errors
st, b = post("/v1/chat/completions", {"messages": [{"role": "user", "content": "a"}], "tools": [{"type": "function", "function": {"name": "f"}}]})
check(st == 400 and "tools" in json.loads(b)["error"]["message"], "tools err %s %s" % (st, b))
st, b = post("/v1/chat/completions", {"nope": 1})
check(st == 400, "missing messages %s" % st)
st, b = post("/v1/chat/completions", {"messages": m, "response_format": {"type": "json_object"}, "max_tokens": 40, "temperature": 0})
try: content = json.loads(b)["choices"][0]["message"]["content"]; js_ok = True
except Exception: js_ok = False
check(st == 200 and js_ok, "json mode %s %s" % (st, b[:200]))
# legacy /api/chat still works
st, b = post("/api/chat", {"messages": m, "max_tokens": 5, "temperature": 0})
check(st == 200 and '"done":true' in b, "/api/chat %s %s" % (st, b[:200]))
# concurrency: 4 parallel requests all succeed; a client that disconnects mid-stream does not block the queue
res = []
def one(i):
    st, b = post("/v1/chat/completions", {"messages": [{"role": "user", "content": "q%d" % i}], "max_tokens": 200, "temperature": 0.8})
    res.append(st)
s = socket.create_connection(("127.0.0.1", int(B.rsplit(":", 1)[1])))
body = json.dumps({"messages": m, "max_tokens": 400, "stream": True}).encode()
hdr = "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
if KEY: hdr += "Authorization: Bearer %s\r\n" % KEY
s.sendall(hdr.encode() + b"\r\n" + body); s.recv(100); s.close()
ts = [threading.Thread(target=one, args=(i,)) for i in range(4)]
t0 = time.time(); [t.start() for t in ts]; [t.join() for t in ts]
check(res == [200] * 4, "parallel %s" % res)
print("ok=%d fail=%d (parallel took %.1fs)" % (ok, bad, time.time() - t0))
