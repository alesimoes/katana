import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = ['10.0.0.22 44', '10.0.1.37 33', '10.0.2.61 33']


class T(unittest.TestCase):
    def test_report(self):
        with open(os.path.join(HERE, "report.txt")) as f:
            lines = [" ".join(l.split()) for l in f.read().strip().split("\n")]
        self.assertEqual(lines, EXPECTED)


if __name__ == "__main__":
    unittest.main()
