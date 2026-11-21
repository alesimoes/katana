import unittest


class T(unittest.TestCase):
    def test_add_tax(self):
        from pricing.tax import add_tax
        for cents, want in ((1000, 1073), (200, 215), (3000, 3218), (0, 0), (10000, 10725), (1234, 1323)):
            got = add_tax(cents)
            self.assertIsInstance(got, int)
            self.assertEqual(got, want, cents)

    def test_net_still_works(self):
        from pricing.tax import net
        self.assertEqual(net(1000, 1000), 900)


if __name__ == "__main__":
    unittest.main()
