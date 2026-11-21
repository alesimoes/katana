import os, unittest

W = os.getcwd()


class T(unittest.TestCase):
    def test_steps_in_order(self):
        p = os.path.join(W, ".katana", "state", "skill.log")
        self.assertTrue(os.path.exists(p), "no step ran")
        self.assertEqual(open(p).read().split(), ["check", "bump", "notes", "seal"])

    def test_done_state(self):
        self.assertEqual(open(os.path.join(W, "VERSION")).read().strip(), "1.5.0")
        self.assertTrue(os.path.exists(os.path.join(W, "RELEASE_NOTES.md")))
        self.assertTrue(os.path.exists(os.path.join(W, "dist", "READY")))


if __name__ == "__main__":
    unittest.main()
