import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class T(unittest.TestCase):
    def test_answer(self):
        with open(os.path.join(HERE, "answer.txt")) as f:
            lines = [l.strip() for l in f.read().strip().split("\n")]
        self.assertEqual(lines, ["238", "14"])

    def test_untouched(self):
        self.assertEqual(sorted(os.listdir(os.path.join(HERE, "logs"))),
                         ["app.log", "app.log.1.gz", "app.log.2.gz", "app.log.3.gz", "audit.log"])
        self.assertEqual(os.path.getsize(os.path.join(HERE, "logs", "audit.log")), 2560)


if __name__ == "__main__":
    unittest.main()
