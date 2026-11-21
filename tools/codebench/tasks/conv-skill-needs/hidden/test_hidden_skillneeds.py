import os, unittest

W = os.getcwd()
S = os.path.join(W, ".katana", "state")


class T(unittest.TestCase):
    def test_missing_reported(self):
        p = os.path.join(S, "missing.txt")
        self.assertTrue(os.path.exists(p), "missing program not reported")
        self.assertEqual(open(p).read().split(), ["inkpress"])

    def test_independent_steps_ran(self):
        self.assertTrue(os.path.exists(os.path.join(W, "docs", "build", "collected.txt")))
        self.assertTrue(os.path.exists(os.path.join(W, "docs", "build", "index.txt")))
        log = open(os.path.join(S, "skill.log")).read().split()
        self.assertEqual(sorted(log), ["collect", "index"])

    def test_dependent_step_not_faked(self):
        self.assertFalse(os.path.exists(os.path.join(W, "docs", "build", "out.pdf")))
        for root, _d, files in os.walk(W):
            self.assertNotIn("inkpress", files, root)


if __name__ == "__main__":
    unittest.main()
