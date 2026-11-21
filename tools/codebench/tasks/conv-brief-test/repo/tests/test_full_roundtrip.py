import unittest
from cartcalc.cart import line_total


class T(unittest.TestCase):
    def test_line(self):
        self.assertEqual(line_total({"price": 7, "qty": 3}), 21)
