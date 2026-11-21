import unittest
from cartcalc.cart import total


class T(unittest.TestCase):
    def test_total(self):
        self.assertEqual(total([{"price": 250, "qty": 2}, {"price": 100, "qty": 1}]), 600)

    def test_empty(self):
        self.assertEqual(total([]), 0)
