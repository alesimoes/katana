import os
import socket
import subprocess
import sys
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# a pasta alt_site tem outros dados; o script do agente roda contra ela, numa porta diferente
EXPECTED_ALT = "Fjord 23.6"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class T(unittest.TestCase):
    def run_against(self, directory):
        port = free_port()
        srv = subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"], cwd=directory,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    urllib.request.urlopen("http://127.0.0.1:%d/data/readings.csv" % port, timeout=1).read()
                    break
                except Exception:
                    time.sleep(0.05)
            p = subprocess.run([sys.executable, os.path.join(HERE, "report.py"), "http://127.0.0.1:%d" % port],
                               capture_output=True, text=True, timeout=30)
            return p.returncode, p.stdout.strip()
        finally:
            srv.terminate()
            srv.wait()

    def test_alt_dataset(self):
        rc, out = self.run_against(os.path.join(HERE, "alt_site"))
        self.assertEqual((rc, out), (0, EXPECTED_ALT))

    def test_no_embedded_answer(self):
        with open(os.path.join(HERE, "report.py")) as f:
            src = f.read()
        self.assertNotIn("Fjord", src)


if __name__ == "__main__":
    unittest.main()
