import unittest

from ledger.parser import parse_line
from ledger.report import totals_by_category, format_report


class T(unittest.TestCase):
    def test_parse(self):
        e = parse_line("2026-01-05 food -45.20 lunch with Bob")
        self.assertEqual((e.date, e.category, e.amount, e.note), ("2026-01-05", "food", -45.2, "lunch with Bob"))

    def test_totals(self):
        es = [parse_line(x) for x in ("2026-01-05 food -10", "2026-01-06 food -5.5", "2026-01-07 pay 100")]
        self.assertEqual(totals_by_category(es), {"food": -15.5, "pay": 100.0})
        self.assertEqual(format_report(es), "food: -15.50\npay: 100.00")


if __name__ == "__main__":
    unittest.main()
