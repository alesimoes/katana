import unittest
from pager import paginate
class T(unittest.TestCase):
    def test_pages(self):
        xs = list(range(10))
        self.assertEqual(paginate(xs, 1, 3), [0, 1, 2])
        self.assertEqual(paginate(xs, 2, 3), [3, 4, 5])
        self.assertEqual(paginate(xs, 4, 3), [9])
        self.assertEqual(paginate(xs, 5, 3), [])
    def test_empty(self):
        self.assertEqual(paginate([], 1, 5), [])
if __name__ == "__main__":
    unittest.main()
