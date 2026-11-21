import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def entry():
    c = [f for f in os.listdir(HERE) if f.endswith(".py") and not f.startswith("test_")]
    if len(c) > 1:
        c = [f for f in c if "__main__" in open(os.path.join(HERE, f), encoding="utf-8").read()]
    assert len(c) == 1, "esperava um programa python na pasta, achei %s" % c
    return os.path.join(HERE, c[0])


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.db = os.path.join(self.tmp, "t.json")

    def run_cli(self, *args, db=True):
        env = dict(os.environ, PYTHONIOENCODING="utf-8")
        env.pop("TAREFAS_ARQUIVO", None)
        if db:
            env["TAREFAS_ARQUIVO"] = self.db
        return subprocess.run([sys.executable, entry(), *args], cwd=self.tmp, env=env, capture_output=True,
                              text=True, encoding="utf-8", timeout=20)

    def test_empty(self):
        p = self.run_cli("list")
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "nenhuma tarefa"))

    def test_flow(self):
        for t in ("comprar pão", "pagar luz", "ligar pra mãe"):
            self.assertEqual(self.run_cli("add", *t.split()).returncode, 0)
        self.assertEqual(self.run_cli("done", "2").returncode, 0)
        p = self.run_cli("list")
        self.assertEqual(p.stdout.splitlines(), ["1 [ ] comprar pão", "2 [x] pagar luz", "3 [ ] ligar pra mãe"])
        self.assertEqual(self.run_cli("rm", "1").returncode, 0)
        p = self.run_cli("list")
        self.assertEqual(p.stdout.splitlines(), ["1 [x] pagar luz", "2 [ ] ligar pra mãe"])
        self.assertTrue(os.path.exists(self.db))

    def test_persists_between_runs_and_unicode(self):
        self.run_cli("add", "caça-níqueis ção")
        self.assertIn("caça-níqueis ção", self.run_cli("list").stdout)
        json.load(open(self.db, encoding="utf-8"))

    def test_default_file_in_cwd(self):
        self.run_cli("add", "x", db=False)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "tarefas.json")))

    def test_bad_number(self):
        self.run_cli("add", "a")
        for args in (("done", "5"), ("rm", "0"), ("done", "abc"), ("rm", "-1")):
            p = self.run_cli(*args)
            self.assertEqual(p.returncode, 1, args)
            self.assertTrue(p.stderr.strip(), args)
            self.assertNotIn("Traceback", p.stderr)
        self.assertEqual(self.run_cli("list").stdout.strip(), "1 [ ] a")

    def test_usage_errors(self):
        for args in ((), ("foo",), ("add",)):
            p = self.run_cli(*args)
            self.assertEqual(p.returncode, 2, args)
            self.assertTrue(p.stderr.strip())
            self.assertNotIn("Traceback", p.stderr)


if __name__ == "__main__":
    unittest.main()
