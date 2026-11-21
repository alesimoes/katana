import json, os, shutil, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def setup(work):
    d = tempfile.mkdtemp(prefix="cb_mcp_")
    log = os.path.join(d, "calls.log")
    import random
    titles = ["timeout no export CSV", "timeout no export PDF", "login quebra no Safari", "typo na tela de ajuda",
              "export CSV sem cabecalho"]
    tickets = [{"id": "tk_%08x" % random.getrandbits(32), "title": x, "status": "open"} for x in titles]
    with open(log + ".ids", "w") as f:
        json.dump(tickets, f)
    os.makedirs(os.path.join(work, ".katana"), exist_ok=True)
    cfg = {"mcpServers": {"tracker": {"command": "python3", "args": [os.path.join(HERE, "server.py"), "--log", log]}}}
    with open(os.path.join(work, ".katana", "mcp.json"), "w") as f:
        json.dump(cfg, f, indent=2)
    return dict(vars={"mcplog": log}, stop=lambda: shutil.rmtree(d, ignore_errors=True))
