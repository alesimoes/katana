import http.client
import json
import os
import socket
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def entry():
    c = [f for f in os.listdir(HERE) if f.endswith(".py") and not f.startswith("test_")]
    if len(c) > 1:
        c = [f for f in c if "__main__" in open(os.path.join(HERE, f), encoding="utf-8").read()]
    assert len(c) == 1, "esperava um servidor python na pasta, achei %s" % c
    return os.path.join(HERE, c[0])


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.proc = subprocess.Popen([sys.executable, entry(), str(cls.port)], cwd=HERE, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("servidor nao subiu")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)

    def call(self, method, path, body=None, raw=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        data = raw if raw is not None else (json.dumps(body).encode("utf-8") if body is not None else None)
        c.request(method, path, body=data, headers={"Content-Type": "application/json"} if data is not None else {})
        r = c.getresponse()
        payload = r.read()
        ctype = r.getheader("Content-Type") or ""
        c.close()
        return r.status, ctype, payload

    def jcall(self, *a, **k):
        st, ctype, payload = self.call(*a, **k)
        self.assertIn("application/json", ctype)
        return st, json.loads(payload.decode("utf-8"))

    def test_flow(self):
        # as provas compartilham o servidor: um teste so, em ordem
        self.assertEqual(self.jcall("GET", "/notes"), (200, []))
        self.assertEqual(self.jcall("POST", "/notes", {"text": "primeira"}), (201, {"id": 1, "text": "primeira"}))
        self.assertEqual(self.jcall("POST", "/notes", {"text": "ação coração 🚀", "extra": 1}),
                         (201, {"id": 2, "text": "ação coração 🚀"}))
        self.assertEqual(self.jcall("POST", "/notes", {"text": "terceira"})[1]["id"], 3)
        st, lst = self.jcall("GET", "/notes")
        self.assertEqual((st, [n["id"] for n in lst], lst[1]["text"]), (200, [1, 2, 3], "ação coração 🚀"))
        self.assertEqual(self.jcall("GET", "/notes/2"), (200, {"id": 2, "text": "ação coração 🚀"}))
        st, ctype, payload = self.call("DELETE", "/notes/2")
        self.assertEqual((st, payload), (204, b""))
        self.assertEqual(self.jcall("GET", "/notes/2")[0], 404)
        self.assertEqual(self.jcall("DELETE", "/notes/2")[0], 404)
        self.assertEqual([n["id"] for n in self.jcall("GET", "/notes")[1]], [1, 3])
        self.assertEqual(self.jcall("POST", "/notes", {"text": "depois"})[1]["id"], 4)   # id nao reaproveitado

    def test_errors(self):
        for path in ("/notes/999", "/notes/abc", "/notes/-1"):
            st, obj = self.jcall("GET", path)
            self.assertEqual(st, 404, path)
            self.assertIn("error", obj)
        for kw in ({"raw": b"{not json"}, {"raw": b""}, {"body": {}}, {"body": {"text": ""}}, {"body": {"text": "   "}},
                    {"body": {"text": 5}}, {"body": {"text": None}}, {"body": [1, 2]}, {"body": "texto"}):
            st, obj = self.jcall("POST", "/notes", **kw)
            self.assertEqual(st, 400, kw)
            self.assertIn("error", obj)
        for path in ("/", "/nope", "/notes/1/extra"):
            self.assertEqual(self.jcall("GET", path)[0], 404, path)
        for method, path in (("PUT", "/notes"), ("DELETE", "/notes"), ("POST", "/notes/1"), ("PUT", "/notes/1")):
            self.assertEqual(self.call(method, path, body={"text": "x"})[0], 405, (method, path))


if __name__ == "__main__":
    unittest.main()
