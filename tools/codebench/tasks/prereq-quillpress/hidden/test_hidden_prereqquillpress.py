import glob
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))

STUB = """#!/bin/sh
# quillpress falso do teste: quillpress ENTRADA -o SAIDA
[ "$2" = "-o" ] || { echo "quillpress: bad usage" >&2; exit 64; }
echo "$1 $2 $3" >> "$QP_LOG"
if grep -q BROKEN "$1"; then echo "quillpress: cannot render $1" >&2; exit 1; fi
{ echo "%PDF-stub"; cat "$1"; } > "$3"
"""


def entry():
    c = [f for f in os.listdir(HERE) if f.endswith(".py") and not f.startswith("test_")]
    if len(c) > 1:
        c = [f for f in c if "__main__" in open(os.path.join(HERE, f), encoding="utf-8").read()]
    assert len(c) == 1, "esperava um script python na pasta, achei %s" % c
    return os.path.join(HERE, c[0])


def path_without_quillpress():
    parts = [d for d in os.environ.get("PATH", "").split(os.pathsep) if d and not os.path.exists(os.path.join(d, "quillpress"))]
    return os.pathsep.join(parts)


def mk(d, name, text="# titulo\n"):
    p = os.path.join(d, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


class T(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp()
        self.stub = tempfile.mkdtemp()
        p = os.path.join(self.stub, "quillpress")
        with open(p, "w") as f:
            f.write(STUB)
        os.chmod(p, 0o755)
        self.log = os.path.join(self.stub, "log.txt")

    def run_script(self, folder, stub=True):
        env = dict(os.environ, PYTHONIOENCODING="utf-8", QP_LOG=self.log)
        env["PATH"] = (self.stub + os.pathsep if stub else "") + path_without_quillpress()
        return subprocess.run([sys.executable, entry(), folder], capture_output=True, text=True, encoding="utf-8",
                              timeout=60, env=env, cwd=self.work)

    def test_readme_mentions_requirement(self):
        r = [f for f in os.listdir(HERE) if f.lower().startswith("readme")]
        self.assertTrue(r, "falta o README")
        self.assertIn("quillpress", open(os.path.join(HERE, r[0]), encoding="utf-8").read())

    def test_fails_cleanly_without_quillpress(self):
        d = os.path.join(self.work, "docs")
        mk(d, "a.md")
        p = self.run_script(d, stub=False)
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertIn("quillpress", p.stderr)
        self.assertNotIn("Traceback", p.stderr)
        self.assertEqual(glob.glob(os.path.join(d, "*.pdf")), [])

    def test_converts_with_stub(self):
        d = os.path.join(self.work, "my docs")
        mk(d, "a.md", "# A\n")
        mk(d, "relatório final.md", "# Relatório ção\n")
        mk(d, "notes.txt", "x")
        mk(d, "sub/b.md", "# B\n")
        p = self.run_script(d)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "2 convertidos"), p.stderr)
        self.assertEqual(sorted(os.listdir(d)), ["a.md", "a.pdf", "notes.txt", "relatório final.md", "relatório final.pdf", "sub"])
        self.assertEqual(open(os.path.join(d, "relatório final.pdf"), encoding="utf-8").read(), "%PDF-stub\n# Relatório ção\n")
        self.assertEqual(os.listdir(os.path.join(d, "sub")), ["b.md"])
        calls = open(self.log, encoding="utf-8").read().splitlines()
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(" -o " in c for c in calls))

    def test_one_failure_does_not_stop_the_rest(self):
        d = os.path.join(self.work, "d")
        mk(d, "a.md")
        mk(d, "BROKEN.md", "BROKEN")
        mk(d, "z.md")
        p = self.run_script(d)
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout.strip(), "2 convertidos")
        self.assertIn("BROKEN.md", p.stderr)
        self.assertTrue(os.path.exists(os.path.join(d, "a.pdf")) and os.path.exists(os.path.join(d, "z.pdf")))
        self.assertFalse(os.path.exists(os.path.join(d, "BROKEN.pdf")))

    def test_empty_folder_and_bad_folder(self):
        d = os.path.join(self.work, "vazia")
        os.makedirs(d)
        p = self.run_script(d)
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "0 convertidos"))
        p = self.run_script(os.path.join(self.work, "nao-existe"))
        self.assertEqual(p.returncode, 2)
        self.assertTrue(p.stderr.strip())
        self.assertNotIn("Traceback", p.stderr)

    def test_does_not_ship_its_own_quillpress(self):
        for f in glob.glob(os.path.join(HERE, "**", "quillpress*"), recursive=True):
            self.fail("nao era pra criar um quillpress falso: %s" % f)


if __name__ == "__main__":
    unittest.main()
