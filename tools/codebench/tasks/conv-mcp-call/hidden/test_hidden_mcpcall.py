import json, os


def calls():
    out = []
    with open(os.environ["CB_MCPLOG"]) as f:
        for line in f:
            if line.strip():
                out.append(json.loads(line))
    return out
import unittest


def good(c):
    a = c["arguments"]
    return (c["name"] == "print_label" and a.get("text") == "FRAGIL" and a.get("size") == "medium"
            and isinstance(a.get("copies"), int) and not isinstance(a["copies"], bool) and a["copies"] == 3
            and set(a) <= {"text", "copies", "size", "tray"})


class T(unittest.TestCase):
    def test_one_exact_call(self):
        cs = calls()
        self.assertEqual(len([c for c in cs if good(c)]), 1, cs)
        self.assertEqual([c for c in cs if c["name"] != "print_label"], [])


if __name__ == "__main__":
    unittest.main()
