import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class T(unittest.TestCase):
    def test_answer(self):
        with open(os.path.join(HERE, "answer.txt")) as f:
            lines = [l.strip() for l in f.read().strip().split("\n")]
        self.assertEqual(len(lines), 2)
        self.assertEqual(os.path.normpath(lines[0]), os.path.normpath("services/media/staging/extra.conf"))
        self.assertEqual(lines[1], "media-transcoder")

    def test_untouched(self):
        with open(os.path.join(HERE, "services", "media", "staging", "extra.conf")) as f:
            self.assertIn("PORT = 8443", f.read())
        n = sum(len(fs) for _, _, fs in os.walk(os.path.join(HERE, "services")))
        self.assertEqual(n, 48)


if __name__ == "__main__":
    unittest.main()
