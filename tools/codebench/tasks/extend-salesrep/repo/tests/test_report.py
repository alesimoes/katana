import io
import os
import tempfile
import unittest

from salesrep import cli
from salesrep.aggregate import group, order, to_table
from salesrep.errors import LoadError, UnknownFormat
from salesrep.formats import render
from salesrep.loader import load_rows

CSV = """date,region,product,qty,price
2026-01-05,Sul,caneta,10,2.50
2026-01-20,Norte,caneta,4,2.50
2026-02-03,Sul,caderno,3,12.00

2026-02-10,Sul,caneta,6,2.50
"""


def write(text):
    p = os.path.join(tempfile.mkdtemp(), "s.csv")
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


class ReportTest(unittest.TestCase):
    def test_load_and_group(self):
        rows = load_rows(write(CSV))
        self.assertEqual(len(rows), 4)
        self.assertEqual(group(rows, "region"), [("Sul", 19, 76.0), ("Norte", 4, 10.0)])
        self.assertEqual(order(group(rows, "product"), "revenue"), [("caneta", 20, 50.0), ("caderno", 3, 36.0)])
        self.assertEqual(order(group(rows, "month"), "key"), [("2026-01", 14, 35.0), ("2026-02", 9, 51.0)])
        self.assertEqual(order(group(rows, "region"), "qty", top=1), [("Sul", 19, 76.0)])

    def test_load_errors(self):
        for text in ("a,b\n", CSV + "2026-13,x,y,1,1\n", CSV + "2026-01-01,x,y,two,1\n", CSV + "2026-01-01,x,y\n",
                     CSV + "2026-01-01,x,y,-1,1\n"):
            with self.assertRaises(LoadError):
                load_rows(write(text))
        with self.assertRaises(LoadError):
            load_rows("/nonexistent.csv")

    def test_text_and_csv(self):
        t = to_table([("Sul", 19, 76.0), ("Norte", 4, 10.0)], "region")
        self.assertEqual(render("text", t), "region  qty  revenue\nSul      19    76.00\nNorte     4    10.00")
        self.assertEqual(render("csv", t), "region,qty,revenue\nSul,19,76.00\nNorte,4,10.00")
        with self.assertRaises(UnknownFormat):
            render("xml", t)

    def test_cli(self):
        out = io.StringIO()
        code = cli.run([write(CSV), "--by", "region", "--sort", "revenue", "--format", "csv"], out)
        self.assertEqual((code, out.getvalue()), (0, "region,qty,revenue\nSul,19,76.00\nNorte,4,10.00\n"))
        self.assertEqual(cli.run([write(CSV), "--top", "0"], io.StringIO()), 1)
        self.assertEqual(cli.run(["/nonexistent.csv"], io.StringIO()), 1)
