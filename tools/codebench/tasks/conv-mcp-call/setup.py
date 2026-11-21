import json, os, shutil, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def setup(work):
    d = tempfile.mkdtemp(prefix="cb_mcp_")
    log = os.path.join(d, "calls.log")
    os.makedirs(os.path.join(work, ".katana"), exist_ok=True)
    cfg = {"mcpServers": {"labelprn": {"command": "python3", "args": [os.path.join(HERE, "server.py"), "--log", log]}}}
    with open(os.path.join(work, ".katana", "mcp.json"), "w") as f:
        json.dump(cfg, f, indent=2)
    return dict(vars={"mcplog": log}, stop=lambda: shutil.rmtree(d, ignore_errors=True))
