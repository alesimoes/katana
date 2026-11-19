import contextlib
import io
import json
import os
import tempfile
import unittest

from kv.store import Store
from kv.cli import main


def run(argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(argv)
    return rc, buf.getvalue().strip().split("\n") if buf.getvalue().strip() else []


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.p = os.path.join(self.d, "db.log")

    def lines(self):
        with open(self.p) as f:
            return [json.loads(l) for l in f if l.strip()]

    def test_basic(self):
        s = Store(self.p)
        self.assertEqual(len(s), 0)
        self.assertEqual(s.keys(), [])
        s.set("b", 1)
        s.set("a", {"x": [1, 2]})
        self.assertEqual(s.get("a"), {"x": [1, 2]})
        self.assertEqual(s.get("zz"), None)
        self.assertEqual(s.get("zz", 7), 7)
        self.assertEqual(s.keys(), ["a", "b"])
        self.assertEqual(len(s), 2)

    def test_log_format(self):
        s = Store(self.p)
        s.set("k", "v")
        s.set("k", "w")
        self.assertTrue(s.delete("k"))
        self.assertFalse(s.delete("k"))
        self.assertEqual(self.lines(), [{"op": "set", "key": "k", "value": "v"}, {"op": "set", "key": "k", "value": "w"},
                                        {"op": "del", "key": "k"}])

    def test_persistence(self):
        s = Store(self.p)
        s.set("a", 1)
        s.set("b", None)
        s.delete("a")
        t = Store(self.p)
        self.assertEqual(t.keys(), ["b"])
        self.assertIsNone(t.get("b", "dflt"))
        self.assertEqual(len(t), 1)

    def test_compact(self):
        s = Store(self.p)
        for i in range(5):
            s.set("z", i)
        s.set("a", "x")
        s.set("gone", 1)
        s.delete("gone")
        self.assertEqual(s.compact(), 6)
        self.assertEqual(self.lines(), [{"op": "set", "key": "a", "value": "x"}, {"op": "set", "key": "z", "value": 4}])
        self.assertEqual(s.compact(), 0)
        s.set("m", True)
        self.assertEqual(Store(self.p).keys(), ["a", "m", "z"])
        self.assertEqual(Store(self.p).compact(), 0)

    def test_bad_keys(self):
        s = Store(self.p)
        for bad in ("", 3, None):
            with self.assertRaises(ValueError):
                s.set(bad, 1)
            with self.assertRaises(ValueError):
                s.get(bad)
            with self.assertRaises(ValueError):
                s.delete(bad)

    def test_cli(self):
        self.assertEqual(run([self.p, "set", "name", '"Ana"']), (0, ["OK"]))
        self.assertEqual(run([self.p, "set", "n", "42"]), (0, ["OK"]))
        self.assertEqual(run([self.p, "get", "name"]), (0, ['"Ana"']))
        self.assertEqual(run([self.p, "get", "n"]), (0, ["42"]))
        self.assertEqual(run([self.p, "get", "nope"]), (1, ["NOT FOUND"]))
        self.assertEqual(run([self.p, "keys"]), (0, ["n", "name"]))
        self.assertEqual(run([self.p, "del", "n"]), (0, ["DELETED"]))
        self.assertEqual(run([self.p, "del", "n"]), (1, ["NOT FOUND"]))
        self.assertEqual(run([self.p, "compact"]), (0, ["removed 2"]))
        self.assertEqual(Store(self.p).keys(), ["name"])

    def test_cli_null_value(self):
        self.assertEqual(run([self.p, "set", "x", "null"]), (0, ["OK"]))
        self.assertEqual(run([self.p, "get", "x"]), (0, ["null"]))

    def test_cli_errors(self):
        for argv in ([self.p, "set", "k", "{bad"], [self.p, "frob"], [self.p, "get"], [self.p, "set", "k"],
                     [self.p, "keys", "extra"], [self.p]):
            rc, out = run(argv)
            self.assertEqual(rc, 2, argv)
            self.assertEqual(len(out), 1, argv)
            self.assertTrue(out[0].startswith("ERROR"), argv)


if __name__ == "__main__":
    unittest.main()
