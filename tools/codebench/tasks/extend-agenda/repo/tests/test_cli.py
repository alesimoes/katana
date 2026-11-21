import io
import os
import tempfile
import unittest

from agenda import cli


class CliTest(unittest.TestCase):
    def setUp(self):
        os.environ["AGENDA_DB"] = os.path.join(tempfile.mkdtemp(), "a.json")
        self.addCleanup(os.environ.pop, "AGENDA_DB", None)

    def run_cli(self, *argv):
        out = io.StringIO()
        return cli.run(list(argv), out), out.getvalue()

    def test_flow(self):
        self.assertEqual(self.run_cli("add", "Dentist", "2026-03-02", "14:00", "45"), (0, "added event 1\n"))
        self.assertEqual(self.run_cli("add", "Standup", "2026-03-02", "09:30", "15"), (0, "added event 2\n"))
        self.assertEqual(self.run_cli("day", "2026-03-02"),
                         (0, "2026-03-02\n  09:30-09:45 Standup (id 2)\n  14:00-14:45 Dentist (id 1)\n"))
        self.assertEqual(self.run_cli("day", "2026-03-09"), (0, "2026-03-09: nothing\n"))
        self.assertEqual(self.run_cli("range", "2026-03-01", "2026-03-31")[1].splitlines()[0], "2026-03-02")
        self.assertEqual(self.run_cli("conflicts", "2026-03-02"), (0, "no conflicts\n"))
        self.assertEqual(self.run_cli("remove", "1"), (0, "removed event 1\n"))

    def test_errors(self):
        self.assertEqual(self.run_cli("day", "nope")[0], 1)
        self.assertEqual(self.run_cli("remove", "3")[0], 1)
