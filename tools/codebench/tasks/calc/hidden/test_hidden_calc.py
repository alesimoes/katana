import unittest
from calc import evaluate
class T(unittest.TestCase):
    def test_values(self):
        cases = [("1 + 2 * 3", 7.0), ("(1 + 2) * 3", 9.0), ("2^3^2", 512.0), ("-2^2", -4.0),
                 ("10 / 4", 2.5), ("  3.5*2 ", 7.0), ("-(2 + 3)", -5.0), ("2 * -3", -6.0),
                 ("1 - 2 - 3", -4.0), ("100 / 10 / 5", 2.0), ("+4", 4.0), (".5 + .5", 1.0),
                 ("2 ^ -1", 0.5)]
        for e, r in cases:
            self.assertAlmostEqual(evaluate(e), r, msg=e)
            self.assertIsInstance(evaluate(e), float)
    def test_zero(self):
        with self.assertRaises(ZeroDivisionError):
            evaluate("1 / (2 - 2)")
    def test_malformed(self):
        for e in ("", "   ", "(1 + 2", "1 + 2)", "1 + * 2", "1 +", "* 3", "2 $ 3", "1 2", "()"):
            with self.assertRaises(ValueError, msg=e):
                evaluate(e)
if __name__ == "__main__":
    unittest.main()
