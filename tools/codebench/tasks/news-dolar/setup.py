import json
from bench_lib import http_get


def setup(work):
    try:
        v = json.loads(http_get("https://open.er-api.com/v6/latest/USD"))["rates"]["BRL"]
    except Exception:
        v = json.loads(http_get("https://api.frankfurter.dev/v1/latest?base=USD&symbols=BRL"))["rates"]["BRL"]
    return dict(vars={"truth": "%.4f" % float(v)})
