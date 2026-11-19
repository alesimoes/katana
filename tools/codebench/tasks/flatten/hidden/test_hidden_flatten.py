import unittest
from flatten import flatten
class T(unittest.TestCase):
    def test_strings(self):
        self.assertEqual(flatten([1, [2, ["ab", 3]], (4, 5)]), [1, 2, "ab", 3, 4, 5])
    def test_bytes_and_empty(self):
        self.assertEqual(flatten([b"xy", [[]], []]), [b"xy"])
    def test_deep(self):
        self.assertEqual(flatten([[[[[1]]]], 2]), [1, 2])
if __name__ == "__main__":
    unittest.main()
