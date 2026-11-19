import os
import tempfile
import unittest

from ledger.parser import parse_line, parse_file
from ledger.rates import load_rates, convert
from ledger.report import totals_by_category, monthly, over_budget, format_report


def P(*lines):
    return [parse_line(x) for x in lines]


class T(unittest.TestCase):
    def test_old_behaviour(self):
        e = parse_line("2026-01-05 food -45.20 lunch with Bob")
        self.assertEqual((e.date, e.category, e.amount, e.note, e.currency), ("2026-01-05", "food", -45.2, "lunch with Bob", "EUR"))
        es = P("2026-01-05 food -10", "2026-01-06 food -5.5", "2026-01-07 pay 100")
        self.assertEqual(totals_by_category(es), {"food": -15.5, "pay": 100.0})
        self.assertEqual(format_report(es), "food: -15.50\npay: 100.00")
        with self.assertRaises(ValueError):
            parse_line("2026-01-05 food")

    def test_currency_parse(self):
        e = parse_line("2026-01-05 food -45.20 USD lunch with Bob")
        self.assertEqual((e.amount, e.currency, e.note), (-45.2, "USD", "lunch with Bob"))
        e = parse_line("2026-01-05 food -3 GBP")
        self.assertEqual((e.currency, e.note), ("GBP", ""))
        e = parse_line("2026-01-05 food -3 Bob paid")
        self.assertEqual((e.currency, e.note), ("EUR", "Bob paid"))
        e = parse_line("2026-01-05 food -3 USDA beef")
        self.assertEqual((e.currency, e.note), ("EUR", "USDA beef"))

    def test_parse_file(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "l.txt")
        with open(p, "w") as f:
            f.write("# c\n\n2026-01-05 food -1 USD x\n2026-01-06 rent -2\n")
        es = parse_file(p)
        self.assertEqual([(e.category, e.currency) for e in es], [("food", "USD"), ("rent", "EUR")])

    def test_rates(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "r.txt")
        with open(p, "w") as f:
            f.write("# rates\nUSD 0.5\n\nGBP 1.25\n")
        r = load_rates(p)
        self.assertEqual(r, {"EUR": 1.0, "USD": 0.5, "GBP": 1.25})
        self.assertEqual(convert(10, "USD", r), 5.0)
        self.assertEqual(convert(-3.333, "GBP", r), -4.17)
        self.assertEqual(convert(7.5, "EUR", r), 7.5)
        with self.assertRaises(KeyError):
            convert(1, "JPY", r)

    def test_totals_with_rates(self):
        r = {"EUR": 1.0, "USD": 0.5}
        es = P("2026-01-05 food -10 USD", "2026-01-06 food -5.5", "2026-01-07 pay 100 USD bonus")
        self.assertEqual(totals_by_category(es, r), {"food": -10.5, "pay": 50.0})
        with self.assertRaises(ValueError):
            totals_by_category(es)
        with self.assertRaises(KeyError):
            totals_by_category(P("2026-01-05 food -10 JPY"), r)

    def test_monthly(self):
        r = {"EUR": 1.0, "USD": 0.5}
        es = P("2026-01-05 food -10 USD", "2026-01-20 food -5", "2026-02-01 food -7", "2026-02-03 rent -700", "2025-12-31 pay 10")
        self.assertEqual(monthly(es, r), {"2026-01": {"food": -10.0}, "2026-02": {"food": -7.0, "rent": -700.0}, "2025-12": {"pay": 10.0}})
        with self.assertRaises(ValueError):
            monthly(es)
        self.assertEqual(monthly(P("2026-03-01 a 1", "2026-03-02 a 2.5")), {"2026-03": {"a": 3.5}})

    def test_over_budget(self):
        r = {"EUR": 1.0, "USD": 0.5}
        es = P("2026-01-05 food -100 USD", "2026-01-20 food -60", "2026-02-01 food -100", "2026-02-03 rent -700",
               "2026-02-04 fun -30", "2026-01-09 fun -80", "2026-01-10 fun 60 refund", "2026-01-11 pay 3000")
        b = {"food": 100, "fun": 25, "pay": 10}
        self.assertEqual(over_budget(es, b, r), [("2026-01", "food", 110.0, 100), ("2026-02", "fun", 30.0, 25)])
        self.assertEqual(over_budget(es, {}, r), [])
        self.assertEqual(over_budget(P("2026-02-01 food -100"), {"food": 100}), [])
        self.assertEqual(over_budget(P("2026-02-01 food -100.01"), {"food": 100}), [("2026-02", "food", 100.01, 100)])


if __name__ == "__main__":
    unittest.main()
