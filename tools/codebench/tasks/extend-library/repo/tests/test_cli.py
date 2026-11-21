import io
import os
import tempfile
import unittest

from lending import cli


class CliTest(unittest.TestCase):
    def setUp(self):
        os.environ["LENDING_DB"] = os.path.join(tempfile.mkdtemp(), "db.json")

    def tearDown(self):
        os.environ.pop("LENDING_DB", None)

    def run_cli(self, *argv):
        out = io.StringIO()
        code = cli.run(list(argv), out)
        return code, out.getvalue()

    def test_flow(self):
        self.assertEqual(self.run_cli("add-book", "Dom Casmurro", "Machado", "--copies", "2"), (0, "added book 1\n"))
        self.assertEqual(self.run_cli("add-member", "Ana"), (0, "added member 1\n"))
        self.assertEqual(self.run_cli("borrow", "1", "1", "--date", "2026-01-10"), (0, "loan 1 created\n"))
        self.assertEqual(self.run_cli("books"), (0, "book 1: Dom Casmurro by Machado (1 of 2 available)\n"))
        self.assertEqual(self.run_cli("loans"), (0, "loan 1: Dom Casmurro -> Ana, borrowed 2026-01-10, open\n"))
        self.assertEqual(self.run_cli("members"), (0, "member 1: Ana (1 open)\n"))
        self.assertEqual(self.run_cli("return", "1", "--date", "2026-01-11"), (0, "loan 1 returned\n"))
        self.assertEqual(self.run_cli("loans"), (0, ""))

    def test_error_exit_code(self):
        self.assertEqual(self.run_cli("return", "7", "--date", "2026-01-11")[0], 1)
