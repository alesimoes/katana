#!/usr/bin/env python3
# Servidor LSP falso para tools/tooltest/symedit.sh: publica um erro por linha que contem BAD_TOKEN (e um aviso por TODO_WARN).
import sys, json

def read():
    h = {}
    while True:
        l = sys.stdin.buffer.readline()
        if not l: return None
        l = l.strip()
        if not l: break
        k, v = l.decode().split(":", 1); h[k.lower()] = v.strip()
    return json.loads(sys.stdin.buffer.read(int(h["content-length"])))

def send(m):
    b = json.dumps(m).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); sys.stdout.buffer.flush()

def diag(uri, text):
    ds = []
    for i, l in enumerate(text.split("\n")):
        c = l.find("BAD_TOKEN")
        if c >= 0: ds.append({"range": {"start": {"line": i, "character": c}, "end": {"line": i, "character": c + 9}}, "severity": 1, "message": "undefined name BAD_TOKEN"})
        c = l.find("TODO_WARN")
        if c >= 0: ds.append({"range": {"start": {"line": i, "character": c}, "end": {"line": i, "character": c + 9}}, "severity": 2, "message": "todo marker"})
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {"uri": uri, "diagnostics": ds}})

while True:
    m = read()
    if m is None: break
    meth = m.get("method")
    if meth == "initialize": send({"jsonrpc": "2.0", "id": m["id"], "result": {"capabilities": {}}})
    elif meth == "textDocument/didOpen": diag(m["params"]["textDocument"]["uri"], m["params"]["textDocument"]["text"])
    elif meth == "textDocument/didChange": diag(m["params"]["textDocument"]["uri"], m["params"]["contentChanges"][0]["text"])
    elif meth == "shutdown": send({"jsonrpc": "2.0", "id": m["id"], "result": None})
    elif meth == "exit": break
