import os
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def git(*a):
    return subprocess.run(["git"] + list(a), cwd=HERE, capture_output=True, text=True).stdout.strip()


class T(unittest.TestCase):
    def test_culprit(self):
        with open(os.path.join(HERE, "culprit.txt")) as f:
            self.assertEqual(f.read().strip(), os.environ["CB_BAD"])

    def test_history_and_tree_untouched(self):
        self.assertEqual(git("rev-parse", "HEAD"), os.environ["CB_HEAD"])
        self.assertEqual(git("rev-list", "--count", "HEAD"), "20")
        self.assertEqual(git("diff", "HEAD", "--stat"), "")      # arquivos rastreados iguais ao HEAD


if __name__ == "__main__":
    unittest.main()
