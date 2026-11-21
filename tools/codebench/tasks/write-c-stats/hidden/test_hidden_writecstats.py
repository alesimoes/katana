import glob
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def snapshot(d):
    out = set()
    for root, _dirs, files in os.walk(d):
        for f in files:
            out.add(os.path.relpath(os.path.join(root, f), d))
    return out


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        for f in os.listdir(HERE):
            if f.startswith("test_") or f == "__pycache__":
                continue
            s = os.path.join(HERE, f)
            (shutil.copytree if os.path.isdir(s) else shutil.copy2)(s, os.path.join(cls.tmp, f))
        cls.before = snapshot(cls.tmp)
        cls.make = subprocess.run(["make"], cwd=cls.tmp, capture_output=True, text=True, timeout=120)
        cls.built = sorted(f for f in snapshot(cls.tmp) - cls.before if os.access(os.path.join(cls.tmp, f), os.X_OK)
                           and not f.endswith((".o", ".a", ".so")))

    def exe(self):
        self.assertEqual(self.make.returncode, 0, self.make.stderr)
        self.assertEqual(len(self.built), 1, "esperava um executavel novo depois do make: %s" % self.built)
        return os.path.join(self.tmp, self.built[0])

    def run_prog(self, text):
        return subprocess.run([self.exe()], input=text, capture_output=True, text=True, timeout=30)

    def test_basic(self):
        p = self.run_prog("1\n2\n3\n4\n5\n")
        self.assertEqual(p.returncode, 0)
        self.assertEqual(p.stdout.splitlines(), ["n=5", "soma=15.00", "media=3.00", "min=1.00", "max=5.00", "mediana=3.00"])

    def test_even_negative_decimal_blank_lines(self):
        p = self.run_prog("\n-1.5\n  4  \n\n10\r\n0.3\n\n")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.splitlines(), ["n=4", "soma=12.80", "media=3.20", "min=-1.50", "max=10.00", "mediana=2.15"])

    def test_no_final_newline_and_unsorted(self):
        p = self.run_prog("9\n1\n5")
        self.assertEqual(p.stdout.splitlines()[-1], "mediana=5.00")
        self.assertIn("max=9.00", p.stdout)

    def test_empty(self):
        for text in ("", "\n\n  \n"):
            p = self.run_prog(text)
            self.assertEqual((p.returncode, p.stdout.strip()), (1, "sem dados"))

    def test_invalid_line(self):
        p = self.run_prog("1\n\nabc\n3\n")
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stderr.strip(), "linha 3 invalida")
        p = self.run_prog("1\n12abc\n")
        self.assertEqual((p.returncode, p.stderr.strip()), (2, "linha 2 invalida"))

    def test_big_input(self):
        n = 100000
        p = self.run_prog("".join("%d\n" % i for i in range(n, 0, -1)))
        self.assertEqual(p.returncode, 0)
        self.assertEqual(p.stdout.splitlines()[:2], ["n=100000", "soma=5000050000.00"])
        self.assertIn("mediana=50000.50", p.stdout)

    def test_no_warnings(self):
        for c in glob.glob(os.path.join(self.tmp, "**", "*.c"), recursive=True):
            p = subprocess.run(["gcc", "-Wall", "-Wextra", "-fsyntax-only", c], capture_output=True, text=True)
            self.assertEqual(p.stderr.strip(), "", c)

    def test_clean(self):
        exe = self.exe()
        p = subprocess.run(["make", "clean"], cwd=self.tmp, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(os.path.exists(exe))
        subprocess.run(["make"], cwd=self.tmp, capture_output=True, text=True, timeout=120)   # as outras provas precisam do binario


if __name__ == "__main__":
    unittest.main()
