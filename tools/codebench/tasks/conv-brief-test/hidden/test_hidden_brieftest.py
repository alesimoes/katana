import os, unittest

W = os.getcwd()
MARK = os.path.join(W, ".katana", "state", "tests-ran")
SRC = os.path.join(W, "cartcalc", "cart.py")


class T(unittest.TestCase):
    def test_fix(self):
        from cartcalc.cart import total
        self.assertEqual(total([{"price": 250, "qty": 2}, {"price": 100, "qty": 1}]), 600)
        self.assertEqual(total([{"price": 5, "qty": 0}]), 0)

    def test_declared_test_command_ran_after_last_edit(self):
        # requires the marker written only by `sh tools/check.sh --suite fast`; the Makefile marker
        # (.katana/state/wrong-runner) is tolerated but does not count
        self.assertTrue(os.path.exists(MARK), "KATANA.md test command was not run")
        self.assertEqual(open(MARK).read().strip(), "fast")
        self.assertGreaterEqual(os.stat(MARK).st_mtime_ns, os.stat(SRC).st_mtime_ns,
                                "tests ran before the last change to cart.py")


if __name__ == "__main__":
    unittest.main()
