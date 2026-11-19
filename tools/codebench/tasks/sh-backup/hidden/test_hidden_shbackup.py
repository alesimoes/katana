import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def run(*args):
    p = subprocess.run(["bash", os.path.join(HERE, "backup.sh")] + list(args), capture_output=True, text=True, timeout=30)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.src = os.path.join(self.d, "my notes")
        os.makedirs(os.path.join(self.src, "sub"))
        for n in ("a.txt", "b c.txt", "d.md"):
            with open(os.path.join(self.src, n), "w") as f:
                f.write(n)
        with open(os.path.join(self.src, "sub", "deep.txt"), "w") as f:
            f.write("deep")

    def test_copy_with_spaces(self):
        dst = os.path.join(self.d, "out dir", "x")
        rc, out, _ = run(self.src, dst)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "copied 2 files")
        self.assertEqual(sorted(os.listdir(dst)), ["a.txt", "b c.txt"])
        with open(os.path.join(dst, "b c.txt")) as f:
            self.assertEqual(f.read(), "b c.txt")

    def test_none(self):
        empty = os.path.join(self.d, "empty")
        os.makedirs(empty)
        dst = os.path.join(self.d, "o2")
        rc, out, _ = run(empty, dst)
        self.assertEqual((rc, out), (0, "copied 0 files"))
        self.assertTrue(os.path.isdir(dst))
        self.assertEqual(os.listdir(dst), [])

    def test_errors(self):
        rc, out, err = run(self.src)
        self.assertEqual(rc, 2)
        self.assertTrue(err)
        rc, out, err = run(os.path.join(self.d, "missing"), os.path.join(self.d, "o3"))
        self.assertEqual(rc, 2)
        self.assertTrue(err)
        self.assertNotIn("copied", out)


if __name__ == "__main__":
    unittest.main()
