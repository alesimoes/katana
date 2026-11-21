import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))


def entry():
    c = [f for f in os.listdir(HERE) if f.endswith(".py") and not f.startswith("test_")]
    if len(c) > 1:
        c = [f for f in c if "__main__" in open(os.path.join(HERE, f), encoding="utf-8").read()]
    assert len(c) == 1, "expected one python script in the folder, found %s" % c
    return os.path.join(HERE, c[0])


class Fake(http.server.BaseHTTPRequestHandler):
    status = 201
    seen = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        Fake.seen.append((self.path, self.headers.get("Authorization"), self.headers.get("Content-Type"), body))
        out = json.dumps({"id": "f_123"} if Fake.status == 201 else {"error": "x"}).encode()
        self.send_response(Fake.status)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        cls.url = "http://127.0.0.1:%d" % cls.srv.server_address[1]
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def setUp(self):
        Fake.status = 201
        Fake.seen = []
        self.home = tempfile.mkdtemp()
        self.work = tempfile.mkdtemp()

    def creds(self, text='{"token": "tok123"}'):
        os.makedirs(os.path.join(self.home, ".datavault"))
        with open(os.path.join(self.home, ".datavault", "credentials"), "w") as f:
            f.write(text)

    def run_script(self, *args, url=None):
        env = dict(os.environ, HOME=self.home, DATAVAULT_URL=url or self.url, PYTHONIOENCODING="utf-8")
        env.pop("USERPROFILE", None)
        return subprocess.run([sys.executable, entry(), *args], capture_output=True, text=True, encoding="utf-8",
                               timeout=60, env=env, cwd=self.work)

    def mkfile(self, name="relatório 1.bin", data=b"\x00\xffbinary\r\n\xc3(data"):
        p = os.path.join(self.work, name)
        with open(p, "wb") as f:
            f.write(data)
        return p, data

    def test_readme(self):
        r = [f for f in os.listdir(HERE) if f.lower().startswith("readme")]
        self.assertTrue(r, "missing README")
        text = open(os.path.join(HERE, r[0]), encoding="utf-8").read().lower()
        self.assertIn("credentials", text)
        self.assertIn("datavault", text)

    def test_missing_credentials(self):
        p, _ = self.mkfile()
        r = self.run_script(p)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("credentials", r.stderr)
        self.assertNotIn("Traceback", r.stderr)
        self.assertEqual(Fake.seen, [])

    def test_bad_credentials_file(self):
        p, _ = self.mkfile()
        for text in ("not json", "{}", '{"token": ""}', "[1]"):
            self.home = tempfile.mkdtemp()
            self.creds(text)
            r = self.run_script(p)
            self.assertEqual(r.returncode, 1, text)
            self.assertNotIn("Traceback", r.stderr, text)
        self.assertEqual(Fake.seen, [])

    def test_upload(self):
        self.creds()
        p, data = self.mkfile()
        r = self.run_script(p)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "f_123"), r.stderr)
        self.assertEqual(len(Fake.seen), 1)
        path, auth, ctype, body = Fake.seen[0]
        u = urllib.parse.urlsplit(path)
        self.assertEqual(u.path, "/v1/files")
        self.assertEqual(urllib.parse.parse_qs(u.query), {"name": ["relatório 1.bin"]})
        self.assertEqual((auth, ctype, body), ("Bearer tok123", "application/octet-stream", data))

    def test_rejected_token_and_server_error(self):
        self.creds()
        p, _ = self.mkfile()
        Fake.status = 401
        r = self.run_script(p)
        self.assertEqual(r.returncode, 1)
        self.assertIn("token", r.stderr.lower())
        self.assertNotIn("Traceback", r.stderr)
        Fake.status = 500
        r = self.run_script(p)
        self.assertEqual(r.returncode, 1)
        self.assertTrue(r.stderr.strip())
        self.assertNotIn("Traceback", r.stderr)

    def test_server_down(self):
        self.creds()
        p, _ = self.mkfile()
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        r = self.run_script(p, url="http://127.0.0.1:%d" % port)
        self.assertEqual(r.returncode, 1)
        self.assertTrue(r.stderr.strip())
        self.assertNotIn("Traceback", r.stderr)

    def test_usage_errors(self):
        self.creds()
        for args in ((), ("/nonexistent/file",), ("a", "b")):
            r = self.run_script(*args)
            self.assertEqual(r.returncode, 2, args)
            self.assertNotIn("Traceback", r.stderr)
        self.assertEqual(Fake.seen, [])


if __name__ == "__main__":
    unittest.main()
