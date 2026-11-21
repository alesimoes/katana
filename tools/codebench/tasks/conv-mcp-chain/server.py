#!/usr/bin/env python3
"""tracker: a ticket tracker (stdio MCP server, benchmark fixture). Logs every tools/call to the file given with --log."""
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

SERVER_NAME = "tracker"
IDS = (LOG + ".ids") if LOG else None
TOOLS = [
    {"name": "list_tickets", "description": "List tickets with their ids.",
     "inputSchema": {"type": "object", "properties": {
         "status": {"type": "string", "enum": ["open", "closed"], "description": "filter, default all"}}}},
    {"name": "close_ticket", "description": "Close one ticket by its id.",
     "inputSchema": {"type": "object", "properties": {
         "ticket_id": {"type": "string", "description": "id from list_tickets"},
         "resolution": {"type": "string", "enum": ["fixed", "duplicate", "wontfix"]}},
         "required": ["ticket_id", "resolution"]}},
]


def load():
    with open(IDS) as f:
        return json.load(f)


def list_tickets(a):
    rows = [t for t in load() if a.get("status") in (None, t["status"])]
    return text("\n".join("%s | %s | %s" % (t["id"], t["status"], t["title"]) for t in rows) or "no tickets")


def close_ticket(a):
    ts = load()
    for t in ts:
        if t["id"] == a.get("ticket_id"):
            if a.get("resolution") not in ("fixed", "duplicate", "wontfix"):
                return text("resolution must be one of fixed, duplicate, wontfix", True)
            t["status"] = "closed"
            with open(IDS, "w") as f:
                json.dump(ts, f)
            return text("closed %s as %s" % (t["id"], a["resolution"]))
    return text("no such ticket id", True)


HANDLERS = {"list_tickets": list_tickets, "close_ticket": close_ticket}


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
