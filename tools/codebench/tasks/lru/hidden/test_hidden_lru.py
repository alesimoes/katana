import unittest
from lru import LRUCache
class T(unittest.TestCase):
    def test_get_refreshes(self):
        c = LRUCache(2)
        c.put("a", 1); c.put("b", 2)
        self.assertEqual(c.get("a"), 1)
        c.put("c", 3)
        self.assertIsNone(c.get("b"))
        self.assertEqual(c.get("a"), 1)
        self.assertEqual(c.get("c"), 3)
    def test_put_refreshes(self):
        c = LRUCache(2)
        c.put("a", 1); c.put("b", 2); c.put("a", 10); c.put("c", 3)
        self.assertIsNone(c.get("b"))
        self.assertEqual(c.get("a"), 10)
    def test_missing(self):
        self.assertIsNone(LRUCache(1).get("x"))
if __name__ == "__main__":
    unittest.main()
