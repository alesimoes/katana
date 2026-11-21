import os
import socket
import subprocess
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def entry():
    c = [f for f in os.listdir(HERE) if f.endswith(".py") and not f.startswith("test_")]
    if len(c) > 1:
        c = [f for f in c if "__main__" in open(os.path.join(HERE, f), encoding="utf-8").read()]
    assert len(c) == 1, "esperava um script python na pasta, achei %s" % c
    return os.path.join(HERE, c[0])


class FakePingd:
    """servidor tcp de teste: guarda as linhas recebidas e responde com reply (None = nao responde)"""

    def __init__(self, reply="OK"):
        self.reply, self.lines, self.conns = reply, [], 0
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(5)
        self.port = self.sock.getsockname()[1]
        self.stop = False
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.conns += 1
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        buf = b""
        c.settimeout(10)
        try:
            while b"\n" not in buf:
                d = c.recv(1024)
                if not d:
                    break
                buf += d
            self.lines.append(buf.decode("utf-8"))
            if self.reply is not None:
                c.sendall((self.reply + "\n").encode("utf-8"))
                c.close()
            else:
                time.sleep(8)
        except OSError:
            pass

    def close(self):
        self.stop = True
        self.sock.close()


class T(unittest.TestCase):
    def run_script(self, *args, port=None, timeout=60):
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PINGD_ADDR="127.0.0.1:%d" % port)
        return subprocess.run([sys.executable, entry(), *args], capture_output=True, text=True, encoding="utf-8",
                              timeout=timeout, env=env)

    def test_readme(self):
        r = [f for f in os.listdir(HERE) if f.lower().startswith("readme")]
        self.assertTrue(r, "falta o README")
        self.assertIn("pingd", open(os.path.join(HERE, r[0]), encoding="utf-8").read())

    def test_sends_metric(self):
        s = FakePingd("OK")
        self.addCleanup(s.close)
        p = self.run_script("web.latency_ms", "-0.75", port=s.port)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "enviado web.latency_ms -0.75"), p.stderr)
        self.assertEqual(s.lines, ["web.latency_ms -0.75\n"])

    def test_default_address_is_9125(self):
        src = open(entry(), encoding="utf-8").read()
        self.assertIn("9125", src)

    def test_err_reply(self):
        s = FakePingd("ERR unknown metric")
        self.addCleanup(s.close)
        p = self.run_script("x", "1", port=s.port)
        self.assertEqual(p.returncode, 1)
        self.assertIn("unknown metric", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertNotIn("enviado", p.stdout)

    def test_service_not_running(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        p = self.run_script("x", "1", port=port)
        self.assertEqual(p.returncode, 1)
        self.assertIn("pingd", p.stderr)
        self.assertIn("127.0.0.1:%d" % port, p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_no_reply_times_out(self):
        s = FakePingd(None)
        self.addCleanup(s.close)
        t0 = time.time()
        p = self.run_script("x", "1", port=s.port, timeout=30)
        self.assertEqual(p.returncode, 1)
        self.assertLess(time.time() - t0, 7)
        self.assertIn("pingd", p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_usage_errors_do_not_connect(self):
        s = FakePingd("OK")
        self.addCleanup(s.close)
        for args in (("x",), (), ("a b", "1"), ("a", "abc"), ("a", "1e3"), ("a", ""), ("a", "1."), ("a", "1", "2"),
                     ("a", "--5")):
            p = self.run_script(*args, port=s.port)
            self.assertEqual(p.returncode, 2, args)
            self.assertTrue(p.stderr.strip(), args)
            self.assertNotIn("Traceback", p.stderr)
        self.assertEqual(s.conns, 0)
        self.assertEqual(s.lines, [])


if __name__ == "__main__":
    unittest.main()
