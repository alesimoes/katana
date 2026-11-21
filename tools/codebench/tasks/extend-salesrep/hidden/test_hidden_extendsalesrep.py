import io
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from salesrep import cli, formats
from salesrep.errors import UnknownFormat
from salesrep.table import Table

CSV = """date,region,product,qty,price
2026-01-05,Sul,caneta,10,2.50
2026-01-20,Norte,caneta,4,2.50
2026-02-03,Sul,caderno,3,12.00
2026-02-10,São Paulo,caneta,6,2.50
"""


def write(text):
    p = os.path.join(tempfile.mkdtemp(), "s.csv")
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


class Markdown(unittest.TestCase):
    def render(self, *a):
        from salesrep.mdtable import render_markdown
        return render_markdown(*a)

    def test_basic(self):
        t = Table(["region", "qty", "revenue"], [["Sul", "19", "76.00"], ["Norte", "4", "10.00"]], ["l", "r", "r"])
        self.assertEqual(self.render(t), "| region | qty | revenue |\n| :----- | --: | ------: |\n"
                                         "| Sul    |  19 |   76.00 |\n| Norte  |   4 |   10.00 |")

    def test_min_width_and_no_rows(self):
        self.assertEqual(self.render(Table(["a", "b"], [], ["l", "r"])), "| a   |   b |\n| :-- | --: |")
        self.assertEqual(self.render(Table(["a", "b"], [["x", "1"]], ["r", "l"])), "|   a | b   |\n| --: | :-- |\n|   x | 1   |")

    def test_escape_and_newlines(self):
        t = Table(["name", "n"], [["a|b", "1"], ["line1\nline2", "22"], ["x\r\ny", "3"]], ["l", "r"])
        self.assertEqual(self.render(t), "| name        |   n |\n| :---------- | --: |\n| a\\|b        |   1 |\n"
                                         "| line1 line2 |  22 |\n| x y         |   3 |")

    def test_unicode_cells_use_character_count(self):
        t = Table(["cidade", "n"], [["São Paulo", "1"], ["Ijuí", "20"]], ["l", "r"])
        self.assertEqual(self.render(t).splitlines()[2:], ["| São Paulo |   1 |", "| Ijuí      |  20 |"])


class Registry(unittest.TestCase):
    def test_render_md_and_unknown(self):
        t = Table(["a"], [["x"]], ["l"])
        self.assertEqual(formats.render("md", t), "| a   |\n| :-- |\n| x   |")
        self.assertEqual(formats.render("text", t), "a\nx")
        with self.assertRaises(UnknownFormat) as cm:
            formats.render("xml", t)
        self.assertIn("csv, md, text", str(cm.exception))


class Cli(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        return cli.run(list(argv), out), out.getvalue()

    def test_format_md(self):
        code, text = self.run_cli(write(CSV), "--by", "region", "--sort", "revenue", "--format", "md")
        self.assertEqual((code, text), (0, "| region    | qty | revenue |\n| :-------- | --: | ------: |\n"
                                           "| Sul       |  13 |   61.00 |\n| São Paulo |   6 |   15.00 |\n"
                                           "| Norte     |   4 |   10.00 |\n"))

    def test_old_formats_unchanged(self):
        self.assertEqual(self.run_cli(write(CSV), "--by", "month", "--format", "csv"),
                         (0, "month,qty,revenue\n2026-01,14,35.00\n2026-02,9,51.00\n"))
        self.assertEqual(self.run_cli(write(CSV), "--by", "month")[1], "month    qty  revenue\n2026-01   14    35.00\n2026-02    9    51.00\n")

    def test_formats_come_from_the_registry(self):
        formats.FORMATS["shout"] = lambda t: "HELLO"
        self.addCleanup(formats.FORMATS.pop, "shout", None)
        self.assertEqual(self.run_cli(write(CSV), "--format", "shout"), (0, "HELLO\n"))

    def test_output_file(self):
        d = tempfile.mkdtemp()
        target = os.path.join(d, "out.md")
        code, text = self.run_cli(write(CSV), "--by", "product", "--format", "md", "--output", target)
        self.assertEqual((code, text), (0, "escrito %s\n" % target))
        with open(target, encoding="utf-8", newline="") as f:
            got = f.read()
        self.assertTrue(got.endswith("|\n") and not got.endswith("\n\n"))
        self.assertTrue(got.startswith("| product "))
        code, text = self.run_cli(write(CSV), "--by", "region", "--output", target)      # tambem funciona com text
        self.assertEqual(open(target, encoding="utf-8").read().splitlines()[0], "region     qty  revenue")

    def test_output_errors(self):
        d = tempfile.mkdtemp()
        self.assertEqual(self.run_cli(write(CSV), "--output", os.path.join(d, "no", "such", "dir", "x.md"))[0], 1)
        bad = os.path.join(d, "bad.md")
        code, text = self.run_cli(write(CSV + "oops\n"), "--format", "md", "--output", bad)
        self.assertEqual((code, text), (1, ""))
        self.assertFalse(os.path.exists(bad))

    def test_unknown_format_is_a_usage_error(self):
        with self.assertRaises(SystemExit) as cm:
            cli.run([write(CSV), "--format", "xml"], io.StringIO())
        self.assertEqual(cm.exception.code, 2)


class ModuleAndExisting(unittest.TestCase):
    def test_module_exists(self):
        self.assertTrue(os.path.exists(os.path.join(HERE, "salesrep", "mdtable.py")))

    def test_repo_tests_still_pass(self):
        p = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", "."], cwd=HERE,
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr[-500:])


if __name__ == "__main__":
    unittest.main()
