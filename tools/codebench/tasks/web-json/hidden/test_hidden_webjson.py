import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class T(unittest.TestCase):
    def test_answer(self):
        with open(os.path.join(HERE, "answer.txt")) as f:
            self.assertEqual(f.read().strip(), "799")


if __name__ == "__main__":
    unittest.main()
