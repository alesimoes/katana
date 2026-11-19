import unittest
from matrix import rotate
class T(unittest.TestCase):
    def test_square(self):
        self.assertEqual(rotate([[1, 2], [3, 4]]), [[3, 1], [4, 2]])
    def test_rect(self):
        self.assertEqual(rotate([[1, 2, 3], [4, 5, 6]]), [[4, 1], [5, 2], [6, 3]])
    def test_new_object(self):
        m = [[1]]
        self.assertIsNot(rotate(m), m)
if __name__ == "__main__":
    unittest.main()
