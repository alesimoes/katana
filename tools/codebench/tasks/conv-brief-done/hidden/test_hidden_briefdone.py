import hashlib, os, sys, unittest

W = os.getcwd()
sys.path.insert(0, os.path.join(W, "src"))


def digest(root):
    h = hashlib.sha256()
    paths = []
    for d, dirs, files in os.walk(os.path.join(root, "src")):
        dirs[:] = [x for x in dirs if x != "__pycache__"]
        paths += [os.path.join(d, f) for f in files if f.endswith(".py")]
    for p in sorted(paths):
        h.update(os.path.relpath(p, root).encode() + b"\0" + open(p, "rb").read() + b"\0")
    return h.hexdigest()


class T(unittest.TestCase):
    def test_median(self):
        from statlib.stats import median
        self.assertEqual(median([3, 1, 2]), 2)
        self.assertEqual(median([4, 1, 3, 2]), 2.5)
        self.assertEqual(median([5]), 5)
        with self.assertRaises(ValueError):
            median([])

    def test_stamp_matches_final_state(self):
        p = os.path.join(W, ".katana", "state", "verified")
        self.assertTrue(os.path.exists(p), "the done command was never run")
        self.assertEqual(open(p).read().strip(), digest(W), "src/ changed after the done command ran")


if __name__ == "__main__":
    unittest.main()
