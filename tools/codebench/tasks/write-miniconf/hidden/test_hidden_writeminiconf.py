import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import miniconf
from miniconf import parse, dumps, ParseError


SAMPLE = """# example
name = demo app   # trailing comment
debug=true
ratio = -0.25
workers = 4

[server.main]
host = example.org
port = 8080
motd = "hello # not a comment, \\"quoted\\" \\\\ ok"
tags = [1, "a b", true, plain]

[empty-one]
"""


class T(unittest.TestCase):
    def test_parse_sample(self):
        d = parse(SAMPLE)
        self.assertEqual(d["name"], "demo app")
        self.assertIs(d["debug"], True)
        self.assertEqual((d["ratio"], d["workers"]), (-0.25, 4))
        self.assertIsInstance(d["workers"], int)
        s = d["server.main"]
        self.assertEqual(s["host"], "example.org")
        self.assertEqual(s["port"], 8080)
        self.assertEqual(s["motd"], 'hello # not a comment, "quoted" \\ ok')
        self.assertEqual(s["tags"], [1, "a b", True, "plain"])
        self.assertEqual(d["empty-one"], {})
        self.assertEqual(list(d), ["name", "debug", "ratio", "workers", "server.main", "empty-one"])

    def test_parse_misc(self):
        self.assertEqual(parse(""), {})
        self.assertEqual(parse("# only\n\n   \n"), {})
        self.assertEqual(parse("a = []\nb = [ ]\nc=[\"x, y\", 2]"), {"a": [], "b": [], "c": ["x, y", 2]})
        self.assertEqual(parse("s = \"café ção \U0001f680\"\r\nt = \"tab\\there\\nnew\""),
                         {"s": "café ção \U0001f680", "t": "tab\there\nnew"})
        self.assertEqual(parse("x = true\ny = false\nz = trueish"), {"x": True, "y": False, "z": "trueish"})
        self.assertEqual(parse("[a]\nk = 1\n[b]\nk = 2"), {"a": {"k": 1}, "b": {"k": 2}})
        self.assertEqual(parse("  key   =   spaced out value  "), {"key": "spaced out value"})
        self.assertEqual(parse('e = "" # empty string'), {"e": ""})

    def test_errors_have_line_numbers(self):
        cases = [
            ("a = 1\nnot an entry", 2),
            ("\n\n[bad name]", 3),
            ("[a]\n[a]", 2),
            ("k = 1\nk = 2", 2),
            ("[s]\nk = 1\nk = 3", 3),
            ('# c\nk = "open', 2),
            ('k = "a" b', 1),
            ("k = [1, 2", 1),
            ("k = [[1], 2]", 1),
            ("k =", 1),
            ("bad key = 1", 1),
            ("k = [1,, 2]", 1),
        ]
        for text, n in cases:
            with self.assertRaises(ParseError, msg=text) as cm:
                parse(text)
            self.assertTrue(str(cm.exception).startswith("line %d:" % n), (text, str(cm.exception)))
        self.assertTrue(issubclass(ParseError, ValueError))

    def test_dumps_exact(self):
        d = {"name": "x", "port": 80, "ratio": 1.5, "on": True, "tags": [1, "a\"b", False], "db": {"user": "a\\b",
             "n": 0}, "log": {"text": "l1\nl2\t!"}}
        self.assertEqual(dumps(d), 'name = "x"\nport = 80\nratio = 1.5\non = true\ntags = [1, "a\\"b", false]\n\n'
                                   '[db]\nuser = "a\\\\b"\nn = 0\n\n[log]\ntext = "l1\\nl2\\t!"\n')
        self.assertEqual(dumps({}), "")
        self.assertEqual(dumps({"s": {}}), "[s]\n")
        self.assertEqual(dumps({"a": 1}), "a = 1\n")
        # a secao vem depois das entradas de topo mesmo quando aparece antes no dict
        self.assertEqual(dumps({"s": {"k": 1}, "top": 2}), 'top = 2\n\n[s]\nk = 1\n')

    def test_roundtrip(self):
        for d in (
            {}, {"s": {}}, parse(SAMPLE),
            {"e": "", "sp": "  padded ", "num": "12", "b": "true", "br": "[x]", "hash": "a # b", "q": '"', "u": "ção \U0001f680",
             "f": -0.0, "big": 10 ** 30, "l": ["", "a,b", 1.5, None.__class__.__name__], "nl": ["x\ny"]},
            {"a": {"b": 1}, "c": {"d": [1, 2]}},
        ):
            self.assertEqual(parse(dumps(d)), d, d)

    def test_dumps_rejects(self):
        for bad in ({"a": {"b": {"c": 1}}}, {"bad key": 1}, {"s": {"bad=key": 1}}, {"a": [[1]]}, {"a": [{"x": 1}]},
                    {"a": None}, {"a": 1e20}, {"a": float("nan")}, {"a": float("inf")}, {"a": (1, 2)}, {"": 1},
                    {"a": b"x"}, {1: 2}):
            with self.assertRaises(ValueError, msg=repr(bad)):
                dumps(bad)

    def test_module_is_stdlib_only(self):
        src = open(os.path.join(HERE, "miniconf.py"), encoding="utf-8").read()
        for line in src.splitlines():
            if line.startswith(("import ", "from ")):
                self.assertIn(line.split()[1].split(".")[0], sys.stdlib_module_names, line)


if __name__ == "__main__":
    unittest.main()
