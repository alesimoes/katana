import json, os


def calls():
    out = []
    with open(os.environ["CB_MCPLOG"]) as f:
        for line in f:
            if line.strip():
                out.append(json.loads(line))
    return out
import unittest


class T(unittest.TestCase):
    def test_chained_close(self):
        want = [t["id"] for t in json.load(open(os.environ["CB_MCPLOG"] + ".ids"))
                if t["title"] == "timeout no export CSV"][0]
        cs = calls()
        names = [c["name"] for c in cs]
        self.assertIn("list_tickets", names, "the id was guessed, never listed")
        closes = [c for c in cs if c["name"] == "close_ticket"]
        ok = [c for c in closes if c["arguments"].get("ticket_id") == want and c["arguments"].get("resolution") == "fixed"]
        self.assertEqual(len(ok), 1, cs)
        self.assertLess(names.index("list_tickets"), names.index("close_ticket"))
        self.assertEqual([c for c in closes if c["arguments"].get("ticket_id") != want
                          and c["arguments"].get("ticket_id") in [t["id"] for t in json.load(open(os.environ["CB_MCPLOG"] + ".ids"))]], [])


if __name__ == "__main__":
    unittest.main()
