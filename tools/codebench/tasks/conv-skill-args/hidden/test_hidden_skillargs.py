import os, unittest

W = os.getcwd()


class T(unittest.TestCase):
    def test_log(self):
        p = os.path.join(W, ".katana", "state", "skill.log")
        self.assertTrue(os.path.exists(p), "no step ran")
        self.assertEqual(open(p).read().splitlines(),
                         ["pack version=3.14.2", "push version=3.14.2 channel=nightly", "status channel=nightly"])

    def test_pack(self):
        self.assertTrue(os.path.exists(os.path.join(W, "dist", "app-3.14.2.pack")))


if __name__ == "__main__":
    unittest.main()
