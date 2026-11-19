import unittest
from wc import count_words
class T(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(count_words("Hello, hello! World."), {"hello": 2, "world": 1})
    def test_apostrophe(self):
        self.assertEqual(count_words("It's fine. IT'S \"fine\"!"), {"it's": 2, "fine": 2})
    def test_only_punct(self):
        self.assertEqual(count_words("... ! ? hi"), {"hi": 1})
    def test_empty(self):
        self.assertEqual(count_words(""), {})
if __name__ == "__main__":
    unittest.main()
