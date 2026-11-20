import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class T(unittest.TestCase):
    def test_answer(self):
        with open(os.path.join(HERE, "answer.txt")) as f:
            lines = [l.strip() for l in f.read().strip().split("\n")]
        self.assertEqual(lines, ["4.2.7", "590bfa5159ec6f4f334e425ffec5b2037ded52c665e968632c4ab4d40a5aa0bb"])


if __name__ == "__main__":
    unittest.main()
