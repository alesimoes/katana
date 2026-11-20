import os
from bench_lib import start_site

HERE = os.path.dirname(os.path.abspath(__file__))


def setup(work):
    port, stop = start_site(os.path.join(HERE, "site"))
    return dict(vars={"url": "http://127.0.0.1:%d" % port}, stop=stop)
