import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def script():
    c = [f for f in os.listdir(HERE) if f.endswith(".sh")]
    assert len(c) == 1, "esperava um script .sh na pasta, achei %s" % c
    return os.path.join(HERE, c[0])


def mk(root, rel, content=""):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write(content)


def tree(root):
    out = {}
    for d, _dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = open(p).read()
    return out


class T(unittest.TestCase):
    def run_sh(self, *args, cwd=None):
        return subprocess.run(["sh", script(), *args], capture_output=True, text=True, timeout=30, cwd=cwd)

    def test_basic_sort(self):
        d = tempfile.mkdtemp()
        mk(d, "foto.JPG", "1")
        mk(d, "nota.txt", "2")
        mk(d, "Makefile", "3")
        mk(d, "relatorio final.PDF", "4")
        mk(d, ".bashrc", "5")
        mk(d, "sub/inside.txt", "6")
        p = self.run_sh(d)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "4 arquivos movidos"), p.stderr)
        self.assertEqual(tree(d), {"jpg/foto.JPG": "1", "txt/nota.txt": "2", "sem-extensao/Makefile": "3",
                                   "pdf/relatorio final.PDF": "4", ".bashrc": "5", "sub/inside.txt": "6"})

    def test_no_overwrite(self):
        d = tempfile.mkdtemp()
        mk(d, "pdf/a.pdf", "old")
        mk(d, "pdf/a-1.pdf", "old1")
        mk(d, "a.pdf", "new1")
        mk(d, "README", "newr")
        mk(d, "sem-extensao/README", "oldr")
        p = self.run_sh(d)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), "2 arquivos movidos")
        self.assertEqual(tree(d), {"pdf/a.pdf": "old", "pdf/a-1.pdf": "old1", "pdf/a-2.pdf": "new1",
                                   "sem-extensao/README": "oldr", "sem-extensao/README-1": "newr"})

    def test_same_name_other_case_is_kept(self):
        d = tempfile.mkdtemp()
        mk(d, "a.pdf", "1")
        mk(d, "a.PDF", "2")
        p = self.run_sh(d)
        self.assertEqual(p.stdout.strip(), "2 arquivos movidos", p.stderr)
        self.assertEqual(sorted(tree(d).values()), ["1", "2"])

    def test_spaces_in_folder_and_relative_path(self):
        base = tempfile.mkdtemp()
        d = os.path.join(base, "minha pasta")
        mk(d, "a b.txt", "x")
        p = self.run_sh("minha pasta/", cwd=base)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "1 arquivo movido"), p.stderr)
        self.assertEqual(tree(d), {"txt/a b.txt": "x"})

    def test_empty_folder(self):
        d = tempfile.mkdtemp()
        p = self.run_sh(d)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "nenhum arquivo"))
        self.assertEqual(os.listdir(d), [])

    def test_errors(self):
        p = self.run_sh()
        self.assertEqual(p.returncode, 1)
        self.assertTrue(p.stderr.strip())
        p = self.run_sh("/nonexistent/dir")
        self.assertEqual(p.returncode, 1)
        self.assertTrue(p.stderr.strip())

    def test_posix_only(self):
        src = open(script()).read()
        first = src.splitlines()[0] if src else ""
        self.assertNotIn("bash", first)
        self.assertNotIn("[[", src)


if __name__ == "__main__":
    unittest.main()
