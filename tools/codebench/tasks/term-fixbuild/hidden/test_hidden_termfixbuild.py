import os
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def sh(*cmd):
    return subprocess.run(cmd, cwd=HERE, capture_output=True, text=True, timeout=120)


class T(unittest.TestCase):
    def test_clean_build_and_run(self):
        sh("make", "clean")
        p = sh("make")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertTrue(os.path.exists(os.path.join(HERE, "stats")))
        p = sh("./stats", "data/input.txt")
        self.assertEqual((p.returncode, p.stdout.strip()), (0, "n=6 mean=18.000 stddev=12.315"))

    def test_rules_kept(self):
        with open(os.path.join(HERE, "Makefile")) as f:
            mk = f.read()
        self.assertIn("-Wall", mk)
        self.assertIn("-Werror", mk)

    def test_data_untouched(self):
        with open(os.path.join(HERE, "data", "input.txt")) as f:
            self.assertEqual(f.read(), "4\n8\n15\n16\n23\n42\n")


if __name__ == "__main__":
    unittest.main()
