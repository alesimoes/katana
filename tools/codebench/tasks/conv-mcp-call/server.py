#!/usr/bin/env python3
"""labelprn: a label printer (stdio MCP server, benchmark fixture). Logs every tools/call to the file given with --log."""
import json, sys

LOG = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else None


def log_call(name, arguments):
    if LOG:
        with open(LOG, "a") as f:
            f.write(json.dumps({"name": name, "arguments": arguments}) + "\n")


def text(s, err=False):
    r = {"content": [{"type": "text", "text": s}]}
    if err:
        r["isError"] = True
    return r


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)

SERVER_NAME = "labelprn"
TOOLS = [
    {"name": "print_label", "description": "Queue labels on the shared label printer.",
     "inputSchema": {"type": "object", "properties": {
         "text": {"type": "string", "description": "text printed on the label"},
         "copies": {"type": "integer", "minimum": 1, "maximum": 50, "description": "how many labels"},
         "size": {"type": "string", "enum": ["small", "medium", "large"], "description": "label stock size"},
         "tray": {"type": "integer", "minimum": 1, "maximum": 3, "description": "paper tray, default 1"}},
         "required": ["text", "copies", "size"]}},
]


def print_label(a):
    if not isinstance(a.get("text"), str) or not a["text"]:
        return text("text must be a non-empty string", True)
    if not is_int(a.get("copies")) or not 1 <= a["copies"] <= 50:
        return text("copies must be an integer from 1 to 50", True)
    if a.get("size") not in ("small", "medium", "large"):
        return text("size must be one of small, medium, large", True)
    return text("queued %d label(s) size=%s" % (a["copies"], a["size"]))


HANDLERS = {"print_label": print_label}


def handle(msg):
    m, mid = msg.get("method"), msg.get("id")
    if mid is None:
        return None
    if m == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
               "serverInfo": {"name": SERVER_NAME, "version": "1.0"}}
    elif m == "ping":
        res = {}
    elif m == "tools/list":
        res = {"tools": TOOLS}
    elif m == "tools/call":
        p = msg.get("params", {})
        name, args = p.get("name"), p.get("arguments") or {}
        if name not in HANDLERS:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool"}}
        log_call(name, args)
        res = HANDLERS[name](args)
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "unknown method"}}
    return {"jsonrpc": "2.0", "id": mid, "result": res}


if __name__ == "__main__":
    for line in sys.stdin:
        line = line.strip()
        if line:
            r = handle(json.loads(line))
            if r is not None:
                sys.stdout.write(json.dumps(r) + "\n")
                sys.stdout.flush()
