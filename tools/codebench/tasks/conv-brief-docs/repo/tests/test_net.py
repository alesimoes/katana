import unittest
from pricing.tax import net


class T(unittest.TestCase):
    def test_net(self):
        self.assertEqual(net(1000, 1000), 900)
