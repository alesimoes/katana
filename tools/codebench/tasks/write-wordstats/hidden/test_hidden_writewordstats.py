import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


class T(unittest.TestCase):
    def test_three_modules_exist(self):
        for m in ("tokenizer", "counter", "report"):
            self.assertTrue(os.path.exists(os.path.join(HERE, "wordstats", m + ".py")), m)

    def test_tokenize(self):
        from wordstats.tokenizer import tokenize
        self.assertEqual(tokenize("Olá, mundo! OLÁ"), ["olá", "mundo", "olá"])
        self.assertEqual(tokenize("Don't stop -- state-of-the-art"), ["don't", "stop", "state-of-the-art"])
        self.assertEqual(tokenize("'hello' and -dash- and a_b"), ["hello", "and", "dash", "and", "a", "b"])
        self.assertEqual(tokenize("3D printing, 3d"), ["3d", "printing", "3d"])
        self.assertEqual(tokenize("ação   coração\n\tçedilha"), ["ação", "coração", "çedilha"])
        self.assertEqual(tokenize(""), [])
        self.assertEqual(tokenize("... --- !!!"), [])

    def test_count_top(self):
        from wordstats.counter import count, top
        t = "b a c b a d b"
        self.assertEqual(count(t), {"a": 2, "b": 3, "c": 1, "d": 1})
        self.assertEqual(top(t), [("b", 3), ("a", 2), ("c", 1)])
        self.assertEqual(top(t, 1), [("b", 3)])
        self.assertEqual(top(t, 10), [("b", 3), ("a", 2), ("c", 1), ("d", 1)])
        self.assertEqual(top("", 3), [])
        self.assertEqual(top("x y", 0), [])

    def test_render_and_exports(self):
        import wordstats
        for name in ("tokenize", "count", "top", "render"):
            self.assertTrue(callable(getattr(wordstats, name)), name)
        self.assertEqual(wordstats.render("b a c b a d b"), "b: 3\na: 2\nc: 1")
        self.assertEqual(wordstats.render("b a b", 1), "b: 2")
        self.assertEqual(wordstats.render(""), "(no words)")
        self.assertEqual(wordstats.render("?!"), "(no words)")

    def run_main(self, *args):
        return subprocess.run([sys.executable, "-m", "wordstats", *args], cwd=HERE, capture_output=True,
                              text=True, encoding="utf-8", timeout=20,
                              env=dict(os.environ, PYTHONIOENCODING="utf-8"))

    def test_cli(self):
        d = tempfile.mkdtemp()
        f = os.path.join(d, "t.txt")
        with open(f, "w", encoding="utf-8") as fh:
            fh.write("coração coração Coração ação ação x\n")
        p = self.run_main(f, "-n", "2")
        self.assertEqual((p.returncode, p.stdout), (0, "coração: 3\nação: 2\n"))
        p = self.run_main(f)
        self.assertEqual(p.stdout.splitlines(), ["coração: 3", "ação: 2", "x: 1"])

    def test_cli_errors(self):
        p = self.run_main("/nonexistent/file.txt")
        self.assertEqual(p.returncode, 1)
        self.assertTrue(p.stderr.strip())
        self.assertNotIn("Traceback", p.stderr)
        p = self.run_main(os.path.join(HERE, "README.md"), "-n", "abc")
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main()
