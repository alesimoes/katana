import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from agenda import cli
from agenda.calendar import Calendar
from agenda.errors import AgendaError
from agenda.events import Event
from agenda.storage import Storage


def ev(day, repeat=None, until=None):
    return Event(1, "x", day, "09:00", 30, repeat, until)


class Recurrence(unittest.TestCase):
    def occ(self, *a):
        from agenda.recurrence import occurrences
        return occurrences(*a)

    def test_plain(self):
        self.assertEqual(self.occ(ev("2026-03-02"), "2026-03-01", "2026-03-31"), ["2026-03-02"])
        self.assertEqual(self.occ(ev("2026-03-02"), "2026-03-02", "2026-03-02"), ["2026-03-02"])
        self.assertEqual(self.occ(ev("2026-03-02"), "2026-03-03", "2026-03-31"), [])
        self.assertEqual(self.occ(ev("2026-03-02"), "2026-02-01", "2026-03-01"), [])

    def test_daily(self):
        self.assertEqual(self.occ(ev("2026-03-02", "daily"), "2026-03-01", "2026-03-04"),
                         ["2026-03-02", "2026-03-03", "2026-03-04"])
        self.assertEqual(self.occ(ev("2026-03-02", "daily", "2026-03-03"), "2026-03-01", "2026-03-10"),
                         ["2026-03-02", "2026-03-03"])
        self.assertEqual(self.occ(ev("2026-03-02", "daily"), "2026-02-27", "2026-03-01"), [])
        self.assertEqual(self.occ(ev("2026-12-30", "daily"), "2026-12-31", "2027-01-01"), ["2026-12-31", "2027-01-01"])

    def test_weekly(self):
        e = ev("2026-03-02", "weekly")
        self.assertEqual(self.occ(e, "2026-03-01", "2026-03-31"), ["2026-03-02", "2026-03-09", "2026-03-16", "2026-03-23", "2026-03-30"])
        self.assertEqual(self.occ(e, "2026-03-10", "2026-03-16"), ["2026-03-16"])
        self.assertEqual(self.occ(e, "2026-03-10", "2026-03-15"), [])
        self.assertEqual(self.occ(e, "2026-03-09", "2026-03-09"), ["2026-03-09"])
        self.assertEqual(self.occ(ev("2026-03-02", "weekly", "2026-03-16"), "2026-01-01", "2026-12-31"),
                         ["2026-03-02", "2026-03-09", "2026-03-16"])
        self.assertEqual(self.occ(ev("2026-03-02", "weekly", "2026-03-15"), "2026-01-01", "2026-12-31"),
                         ["2026-03-02", "2026-03-09"])
        self.assertEqual(self.occ(e, "2026-02-01", "2026-02-28"), [])

    def test_monthly(self):
        e = ev("2026-01-31", "monthly")
        self.assertEqual(self.occ(e, "2026-01-01", "2026-06-30"),
                         ["2026-01-31", "2026-02-28", "2026-03-31", "2026-04-30", "2026-05-31", "2026-06-30"])
        self.assertEqual(self.occ(ev("2028-01-31", "monthly"), "2028-02-01", "2028-02-29"), ["2028-02-29"])
        self.assertEqual(self.occ(ev("2026-03-15", "monthly"), "2026-12-01", "2027-03-31"),
                         ["2026-12-15", "2027-01-15", "2027-02-15", "2027-03-15"])
        self.assertEqual(self.occ(e, "2026-02-01", "2026-02-27"), [])
        self.assertEqual(self.occ(e, "2026-02-28", "2026-02-28"), ["2026-02-28"])
        self.assertEqual(self.occ(ev("2026-01-31", "monthly", "2026-03-30"), "2026-01-01", "2026-12-31"),
                         ["2026-01-31", "2026-02-28"])
        self.assertEqual(self.occ(ev("2026-01-30", "monthly"), "2026-03-01", "2026-03-29"), [])


class CalendarRecurring(unittest.TestCase):
    def setUp(self):
        self.path = os.path.join(tempfile.mkdtemp(), "a.json")
        self.cal = Calendar(Storage(self.path))

    def test_add_validation(self):
        bad = [dict(repeat="yearly"), dict(until="2026-04-01"), dict(repeat="weekly", until="2026-02-28"),
               dict(repeat="weekly", until="soon"), dict(repeat="")]
        for kw in bad:
            with self.assertRaises(AgendaError, msg=str(kw)):
                self.cal.add("x", "2026-03-02", "09:00", 30, **kw)
        e = self.cal.add("x", "2026-03-02", "09:00", 30, repeat="weekly", until="2026-03-02")
        self.assertEqual((e.repeat, e.until), ("weekly", "2026-03-02"))
        self.assertEqual(self.cal.add("y", "2026-03-02", "09:00", 30).repeat, None)

    def test_queries_expand(self):
        a = self.cal.add("Standup", "2026-03-02", "09:30", 15, repeat="weekly")
        b = self.cal.add("Once", "2026-03-09", "09:00", 45)
        self.assertEqual([e.id for e in self.cal.on_day("2026-03-16")], [a.id])
        self.assertEqual([e.id for e in self.cal.on_day("2026-03-09")], [b.id, a.id])
        self.assertEqual(self.cal.on_day("2026-03-10"), [])
        got = [(d, e.title) for d, e in self.cal.between("2026-03-01", "2026-03-16")]
        self.assertEqual(got, [("2026-03-02", "Standup"), ("2026-03-09", "Once"), ("2026-03-09", "Standup"),
                               ("2026-03-16", "Standup")])
        pairs = [(x.title, y.title) for x, y in self.cal.conflicts("2026-03-09")]
        self.assertEqual(pairs, [("Standup", "Once")])
        self.assertEqual(self.cal.conflicts("2026-03-02"), [])

    def test_persistence_and_old_file(self):
        self.cal.add("Standup", "2026-03-02", "09:30", 15, repeat="daily", until="2026-03-05")
        again = Calendar(Storage(self.path))
        self.assertEqual((again.events[0].repeat, again.events[0].until), ("daily", "2026-03-05"))
        self.assertEqual(len(again.on_day("2026-03-04")), 1)
        self.assertEqual(again.on_day("2026-03-06"), [])
        old = os.path.join(tempfile.mkdtemp(), "old.json")
        json.dump({"next_id": 2, "events": [{"id": 1, "title": "Old", "date": "2026-03-02", "time": "10:00", "minutes": 30}]},
                  open(old, "w"))
        c = Calendar(Storage(old))
        self.assertEqual(len(c.on_day("2026-03-02")), 1)
        self.assertEqual(c.on_day("2026-03-09"), [])


class Cli(unittest.TestCase):
    def setUp(self):
        os.environ["AGENDA_DB"] = os.path.join(tempfile.mkdtemp(), "a.json")
        self.addCleanup(os.environ.pop, "AGENDA_DB", None)

    def run_cli(self, *argv):
        out = io.StringIO()
        return cli.run(list(argv), out), out.getvalue()

    def test_cli(self):
        self.assertEqual(self.run_cli("add", "Standup", "2026-03-02", "09:30", "15", "--repeat", "weekly", "--until", "2026-03-20"),
                         (0, "added event 1\n"))
        self.assertEqual(self.run_cli("add", "Dentist", "2026-03-09", "09:40", "30"), (0, "added event 2\n"))
        self.assertEqual(self.run_cli("day", "2026-03-09"),
                         (0, "2026-03-09\n  09:30-09:45 Standup (id 1) [weekly]\n  09:40-10:10 Dentist (id 2)\n"))
        self.assertEqual(self.run_cli("range", "2026-03-16", "2026-03-31"),
                         (0, "2026-03-16\n  09:30-09:45 Standup (id 1) [weekly]\n"))
        self.assertEqual(self.run_cli("conflicts", "2026-03-09"),
                         (0, "09:30-09:45 Standup (id 1) [weekly] <-> 09:40-10:10 Dentist (id 2)\n"))
        self.assertEqual(self.run_cli("day", "2026-03-27"), (0, "2026-03-27: nothing\n"))

    def test_cli_errors(self):
        self.assertEqual(self.run_cli("add", "x", "2026-03-02", "09:00", "15", "--repeat", "yearly")[0], 1)
        self.assertEqual(self.run_cli("add", "x", "2026-03-02", "09:00", "15", "--until", "2026-04-01")[0], 1)
        self.assertEqual(self.run_cli("day", "2026-03-02"), (0, "2026-03-02: nothing\n"))


class ModuleAndExisting(unittest.TestCase):
    def test_module_exists(self):
        self.assertTrue(os.path.exists(os.path.join(HERE, "agenda", "recurrence.py")))

    def test_repo_tests_still_pass(self):
        p = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", "."], cwd=HERE,
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr[-500:])


if __name__ == "__main__":
    unittest.main()
