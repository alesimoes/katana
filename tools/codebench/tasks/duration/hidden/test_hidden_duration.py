import unittest
from duration import parse_duration
class T(unittest.TestCase):
    def test_ok(self):
        self.assertEqual(parse_duration("1h30m"), 5400)
        self.assertEqual(parse_duration("45s"), 45)
        self.assertEqual(parse_duration("2d"), 172800)
        self.assertEqual(parse_duration("1h 30m"), 5400)
        self.assertEqual(parse_duration("90"), 90)
        self.assertEqual(parse_duration("1d2h3m4s"), 86400 + 7200 + 180 + 4)
    def test_bad(self):
        for s in ("", "5x", "h", "abc"):
            with self.assertRaises(ValueError):
                parse_duration(s)
if __name__ == "__main__":
    unittest.main()
