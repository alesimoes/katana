python3 - <<'PYEOF'
import json, subprocess, sys

spec = next(iter(json.load(open(".katana/mcp.json"))["mcpServers"].values()))
p = subprocess.Popen([spec["command"]] + spec["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
_id = [0]


def send(method, params=None, notify=False):
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        msg["params"] = params
    if not notify:
        _id[0] += 1
        msg["id"] = _id[0]
    p.stdin.write(json.dumps(msg) + "\n")
    p.stdin.flush()
    if not notify:
        return json.loads(p.stdout.readline())["result"]


def call(name, arguments):
    r = send("tools/call", {"name": name, "arguments": arguments})
    if r.get("isError"):
        sys.exit("tool error: " + r["content"][0]["text"])
    return r["content"][0]["text"]


send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "solve", "version": "1"}})
send("notifications/initialized", notify=True)
print(call("schedule_irrigation", {"zone": "B", "start": "06:30", "duration_min": 20, "days": ["mon", "wed", "fri"],
    "mode": "drip", "valve": {"id": "v3", "pressure_kpa": 180}, "moisture_skip_above": 65, "dry_run": True}))
p.stdin.close()
p.wait()
PYEOF
