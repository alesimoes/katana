import unittest
from roman import to_roman
class T(unittest.TestCase):
    def test_values(self):
        for n, r in [(1, "I"), (4, "IV"), (9, "IX"), (40, "XL"), (58, "LVIII"), (90, "XC"),
                     (400, "CD"), (1994, "MCMXCIV"), (3999, "MMMCMXCIX")]:
            self.assertEqual(to_roman(n), r)
    def test_range(self):
        for n in (0, -5, 4000):
            with self.assertRaises(ValueError):
                to_roman(n)
if __name__ == "__main__":
    unittest.main()
