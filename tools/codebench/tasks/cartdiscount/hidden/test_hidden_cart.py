import unittest
from shop.cart import total
class T(unittest.TestCase):
    def test_discount_once(self):
        self.assertEqual(total([("a", 100.0, 1), ("b", 25.0, 2)], 10), 135.0)
    def test_no_discount(self):
        self.assertEqual(total([("a", 19.99, 3)]), 59.97)
    def test_empty(self):
        self.assertEqual(total([], 15), 0.0)
    def test_rounding(self):
        self.assertEqual(total([("a", 10.0, 1)], 33), 6.7)
if __name__ == "__main__":
    unittest.main()
