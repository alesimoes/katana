#!/usr/bin/env python3
"""Stdio MCP server for tools/tooltest/extend.sh. Covers: paginated tools/list, structuredContent, isError, image and
resource_link blocks, a list_changed notification, prompts, a server that dies and one that is slow, instructions."""
import json, sys, time

mode = sys.argv[1] if len(sys.argv) > 1 else "main"
extra = False


def send(o):
    sys.stdout.write(json.dumps(o) + "\n")
    sys.stdout.flush()


T1 = [
    {"name": "echo", "description": "Echo the text back\nsecond line", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "sum", "description": "Add two numbers and return the sum as structured data", "inputSchema": {"type": "object", "properties": {
        "mode": {"type": "string", "enum": ["fast", "slow"]}, "b": {"type": "number"}, "a": {"type": "number"}, "tags": {"type": "array", "items": {"type": "string"}}, "flag": {"type": "boolean"}},
        "required": ["a", "b"]}},
]
T2 = [
    {"name": "fail", "description": "Always reports an error result", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "pic", "description": "Returns an image and a resource link", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "add_tool", "description": "Adds new_tool and announces the change", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "die", "description": "Exits without answering", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "slow", "description": "Answers after 5 seconds", "inputSchema": {"type": "object", "properties": {}}},
]
NEW = {"name": "new_tool", "description": "Added at run time", "inputSchema": {"type": "object", "properties": {}}}

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    m = json.loads(line)
    meth, i = m.get("method"), m.get("id")
    p = m.get("params") or {}
    if i is None:
        continue
    if meth == "initialize":
        caps = {"tools": {"listChanged": True}, "prompts": {}}
        res = {"protocolVersion": "2025-06-18", "capabilities": caps, "serverInfo": {"name": "ext-" + mode, "version": "1.0"}}
        if mode == "main":
            res["instructions"] = "Test server with arithmetic and echo tools"
        send({"jsonrpc": "2.0", "id": i, "result": res})
    elif meth == "tools/list":
        if p.get("cursor") == "p2":
            tl = T2 + ([NEW] if extra else [])
            send({"jsonrpc": "2.0", "id": i, "result": {"tools": tl}})
        else:
            send({"jsonrpc": "2.0", "id": i, "result": {"tools": T1, "nextCursor": "p2"}})
    elif meth == "tools/call":
        n, a = p["name"], p.get("arguments") or {}
        if n == "echo":
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": a.get("text", "")}]}})
        elif n == "sum":
            s = a["a"] + a["b"]
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "sum computed"}], "structuredContent": {"sum": s, "args": a}}})
        elif n == "fail":
            send({"jsonrpc": "2.0", "id": i, "result": {"isError": True, "content": [{"type": "text", "text": "boom happened"}]}})
        elif n == "pic":
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [
                {"type": "text", "text": "here"}, {"type": "image", "mimeType": "image/png", "data": "aGVsbG8gd29ybGQ="},
                {"type": "resource_link", "uri": "file:///x.txt", "name": "x"}]}})
        elif n == "add_tool":
            extra = True
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "added"}]}})
        elif n == "die":
            sys.exit(0)
        elif n == "slow":
            time.sleep(5)
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "late"}]}})
        elif n == "new_tool":
            send({"jsonrpc": "2.0", "id": i, "result": {"content": [{"type": "text", "text": "new ok"}]}})
        else:
            send({"jsonrpc": "2.0", "id": i, "error": {"code": -32602, "message": "unknown tool " + n}})
    elif meth == "prompts/list":
        send({"jsonrpc": "2.0", "id": i, "result": {"prompts": [{"name": "review", "description": "Review a file", "arguments": [
            {"name": "file", "required": True}, {"name": "focus", "required": False}]}]}})
    elif meth == "prompts/get":
        a = p.get("arguments") or {}
        send({"jsonrpc": "2.0", "id": i, "result": {"messages": [{"role": "user", "content": {"type": "text", "text": "Please review %s focusing on %s" % (a.get("file"), a.get("focus", "anything"))}}]}})
    else:
        send({"jsonrpc": "2.0", "id": i, "error": {"code": -32601, "message": "Method not found"}})
