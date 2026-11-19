import unittest
from search import binary_search
class T(unittest.TestCase):
    def test_all_found(self):
        for n in range(0, 12):
            a = list(range(0, n * 3, 3))
            for i, v in enumerate(a):
                self.assertEqual(binary_search(a, v), i)
    def test_missing(self):
        self.assertEqual(binary_search([1, 3, 5], 4), -1)
        self.assertEqual(binary_search([], 1), -1)
        self.assertEqual(binary_search([1, 3, 5], 6), -1)
        self.assertEqual(binary_search([1, 3, 5], 0), -1)
if __name__ == "__main__":
    unittest.main()
