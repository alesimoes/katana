import json
from bench_lib import http_get


def setup(work):
    d = json.loads(http_get("https://api.bcb.gov.br/dados/serie/bcdata.sgs.432/dados/ultimos/1?formato=json"))
    return dict(vars={"truth": d[-1]["valor"]})
