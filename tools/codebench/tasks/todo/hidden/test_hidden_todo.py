import json, os, tempfile, unittest
import todo
class T(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.p = os.path.join(self.dir, "t.json")
    def test_flow(self):
        self.assertEqual(todo.list_items(self.p), [])
        a = todo.add(self.p, "milk"); b = todo.add(self.p, "eggs")
        self.assertEqual((a, b), (1, 2))
        todo.done(self.p, a)
        self.assertEqual(todo.list_items(self.p), [{"id": 1, "text": "milk", "done": True}, {"id": 2, "text": "eggs", "done": False}])
        self.assertEqual([i["id"] for i in todo.list_items(self.p, only_open=True)], [2])
        json.load(open(self.p))
    def test_ids_never_repeat(self):
        todo.add(self.p, "a"); todo.add(self.p, "b")
        todo.remove(self.p, 2)
        self.assertEqual(todo.add(self.p, "c"), 3)
    def test_errors(self):
        with self.assertRaises(KeyError):
            todo.done(self.p, 9)
        with self.assertRaises(KeyError):
            todo.remove(self.p, 9)
        for t in ("", "   "):
            with self.assertRaises(ValueError):
                todo.add(self.p, t)
    def test_empty_file(self):
        open(self.p, "w").close()
        self.assertEqual(todo.list_items(self.p), [])
        self.assertEqual(todo.add(self.p, "x"), 1)
if __name__ == "__main__":
    unittest.main()
