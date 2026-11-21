#!/usr/bin/env python3
"""greenhouse: an irrigation controller (stdio MCP server, benchmark fixture). Logs every tools/call to the file given with --log."""
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

SERVER_NAME = "greenhouse"
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
TOOLS = [
    {"name": "schedule_irrigation", "description": "Create a recurring irrigation schedule for one zone.",
     "inputSchema": {"type": "object", "properties": {
         "zone": {"type": "string", "description": "zone letter, e.g. A"},
         "start": {"type": "string", "description": "start time, HH:MM 24h"},
         "duration_min": {"type": "integer", "minimum": 1, "maximum": 240},
         "days": {"type": "array", "items": {"type": "string", "enum": DAYS}, "minItems": 1},
         "mode": {"type": "string", "enum": ["drip", "spray", "mist"]},
         "valve": {"type": "object", "description": "valve settings", "properties": {
             "id": {"type": "string", "description": "valve id, e.g. v1"},
             "pressure_kpa": {"type": "number", "description": "line pressure in kPa"}},
             "required": ["id", "pressure_kpa"]},
         "moisture_skip_above": {"type": "number", "description": "skip the run above this soil moisture percent"},
         "dry_run": {"type": "boolean", "description": "validate only, do not arm the schedule"},
         "note": {"type": "string"}},
         "required": ["zone", "start", "duration_min", "days", "mode", "valve"]}},
]


def schedule(a):
    v = a.get("valve")
    if not isinstance(a.get("zone"), str) or not isinstance(a.get("start"), str) or not is_int(a.get("duration_min")):
        return text("zone, start (string) and duration_min (integer) are required", True)
    if not isinstance(a.get("days"), list) or not a["days"] or any(d not in DAYS for d in a["days"]):
        return text("days must be a non-empty array of " + ",".join(DAYS), True)
    if a.get("mode") not in ("drip", "spray", "mist"):
        return text("mode must be drip, spray or mist", True)
    if not isinstance(v, dict) or not isinstance(v.get("id"), str) or not is_num(v.get("pressure_kpa")):
        return text("valve must be an object with id (string) and pressure_kpa (number)", True)
    return text("%s schedule for zone %s accepted" % ("dry-run" if a.get("dry_run") else "armed", a["zone"]))


HANDLERS = {"schedule_irrigation": schedule}


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
