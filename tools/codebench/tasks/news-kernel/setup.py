import json
from bench_lib import http_get


def setup(work):
    d = json.loads(http_get("https://www.kernel.org/releases.json"))
    return dict(vars={"truth": d["latest_stable"]["version"]})
