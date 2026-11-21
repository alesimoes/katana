import os
import tempfile
import unittest

from agenda.calendar import Calendar
from agenda.errors import AgendaError
from agenda.storage import Storage


class CalendarTest(unittest.TestCase):
    def setUp(self):
        self.path = os.path.join(tempfile.mkdtemp(), "a.json")
        self.cal = Calendar(Storage(self.path))

    def test_add_and_query(self):
        a = self.cal.add("Dentist", "2026-03-02", "14:00", 45)
        b = self.cal.add("Standup", "2026-03-02", "09:30", 15)
        self.cal.add("Other day", "2026-03-03", "10:00", 30)
        self.assertEqual([e.id for e in self.cal.on_day("2026-03-02")], [b.id, a.id])
        self.assertEqual(a.end, 14 * 60 + 45)

    def test_between_is_inclusive_and_sorted(self):
        self.cal.add("c", "2026-03-04", "08:00", 10)
        self.cal.add("a", "2026-03-02", "10:00", 10)
        self.cal.add("b", "2026-03-02", "09:00", 10)
        self.cal.add("out", "2026-03-05", "09:00", 10)
        got = [(d, e.title) for d, e in self.cal.between("2026-03-02", "2026-03-04")]
        self.assertEqual(got, [("2026-03-02", "b"), ("2026-03-02", "a"), ("2026-03-04", "c")])
        with self.assertRaises(AgendaError):
            self.cal.between("2026-03-04", "2026-03-02")

    def test_conflicts(self):
        self.cal.add("a", "2026-03-02", "09:00", 60)
        self.cal.add("b", "2026-03-02", "10:00", 30)      # encosta no fim de a: nao conflita
        self.cal.add("c", "2026-03-02", "09:30", 60)
        pairs = [(a.title, b.title) for a, b in self.cal.conflicts("2026-03-02")]
        self.assertEqual(pairs, [("a", "c"), ("b", "c")])

    def test_validation(self):
        for args in (("", "2026-03-02", "09:00", 10), ("x", "2026-13-02", "09:00", 10), ("x", "2026-03-02", "9:00", 10),
                     ("x", "2026-03-02", "24:00", 10), ("x", "2026-03-02", "23:30", 31), ("x", "2026-03-02", "09:00", 0)):
            with self.assertRaises(AgendaError, msg=str(args)):
                self.cal.add(*args)

    def test_remove_and_persistence(self):
        a = self.cal.add("a", "2026-03-02", "09:00", 10)
        again = Calendar(Storage(self.path))
        self.assertEqual([e.title for e in again.on_day("2026-03-02")], ["a"])
        again.remove(a.id)
        self.assertEqual(Calendar(Storage(self.path)).on_day("2026-03-02"), [])
        with self.assertRaises(AgendaError):
            again.remove(a.id)
        self.assertEqual(again.add("n", "2026-03-02", "09:00", 10).id, 2)
