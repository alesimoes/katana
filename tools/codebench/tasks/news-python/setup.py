import json
from bench_lib import http_get


def setup(work):
    d = json.loads(http_get("https://endoflife.date/api/python.json"))
    return dict(vars={"truth": d[0]["latest"]})
