import io
import os
import tempfile
import unittest

from salesrep import cli
from salesrep.errors import ReportError
from salesrep.filters import filter_rows
from salesrep.loader import Row

ROWS = [Row("2026-01-05", "Sul", "caneta", 10, 2.5), Row("2026-01-20", "Norte", "caneta", 4, 2.5),
        Row("2026-02-03", "Sul", "caderno", 3, 12.0), Row("2026-02-10", "Leste", "caneta", 6, 2.5)]


class FilterTest(unittest.TestCase):
    def test_dates_are_inclusive(self):
        self.assertEqual(len(filter_rows(ROWS)), 4)
        self.assertEqual([r.date for r in filter_rows(ROWS, start="2026-01-20", end="2026-02-03")], ["2026-01-20", "2026-02-03"])
        self.assertEqual([r.date for r in filter_rows(ROWS, end="2026-01-05")], ["2026-01-05"])
        self.assertEqual([r.date for r in filter_rows(ROWS, start="2026-02-04")], ["2026-02-10"])

    def test_regions_ignore_case(self):
        self.assertEqual([r.region for r in filter_rows(ROWS, regions=["sul", "LESTE"])], ["Sul", "Sul", "Leste"])
        self.assertEqual(len(filter_rows(ROWS, regions=[])), 4)

    def test_errors(self):
        for kw in (dict(start="2026/01/01"), dict(end="x"), dict(start="2026-02-01", end="2026-01-01")):
            with self.assertRaises(ReportError):
                filter_rows(ROWS, **kw)

    def test_cli_options(self):
        p = os.path.join(tempfile.mkdtemp(), "s.csv")
        with open(p, "w", encoding="utf-8") as f:
            f.write("date,region,product,qty,price\n2026-01-05,Sul,caneta,10,2.50\n2026-02-03,Norte,caneta,4,2.50\n")
        out = io.StringIO()
        self.assertEqual(cli.run([p, "--from", "2026-02-01", "--format", "csv"], out), 0)
        self.assertEqual(out.getvalue(), "region,qty,revenue\nNorte,4,10.00\n")
        out = io.StringIO()
        self.assertEqual(cli.run([p, "--region", "sul", "--format", "csv"], out), 0)
        self.assertEqual(out.getvalue(), "region,qty,revenue\nSul,10,25.00\n")
        self.assertEqual(cli.run([p, "--to", "bad"], io.StringIO()), 1)
