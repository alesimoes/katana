import os, unittest

W = os.getcwd()


class T(unittest.TestCase):
    def test_right_skill_only(self):
        p = os.path.join(W, ".katana", "state", "skill.log")
        self.assertTrue(os.path.exists(p), "no step ran")
        self.assertEqual(open(p).read().split(), ["files-tar", "files-verify"])

    def test_archive(self):
        self.assertTrue(os.path.exists(os.path.join(W, "backups", "uploads.tar")))


if __name__ == "__main__":
    unittest.main()
