import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from lending import cli
from lending.errors import BadDate
from lending.models import Loan
from lending.service import Library
from lending.store import Store


class FinesModule(unittest.TestCase):
    def test_exists(self):
        self.assertTrue(os.path.exists(os.path.join(HERE, "lending", "fines.py")))

    def test_due_date(self):
        from lending.fines import due_date
        self.assertEqual(due_date("2026-01-10"), "2026-01-24")
        self.assertEqual(due_date("2026-02-20"), "2026-03-06")
        self.assertEqual(due_date("2027-12-25"), "2028-01-08")
        with self.assertRaises(BadDate):
            due_date("25/12/2027")

    def test_fine_cents(self):
        from lending.fines import fine_cents
        self.assertEqual(fine_cents("2026-01-24", "2026-01-10"), 0)
        self.assertEqual(fine_cents("2026-01-24", "2026-01-24"), 0)
        self.assertEqual(fine_cents("2026-01-24", "2026-01-25"), 50)
        self.assertEqual(fine_cents("2026-01-24", "2026-01-31"), 350)
        self.assertEqual(fine_cents("2026-01-24", "2026-03-24"), 2000)
        self.assertEqual(fine_cents("2026-01-24", "2026-01-27", per_day=100), 300)
        self.assertEqual(fine_cents("2026-01-24", "2026-02-24", per_day=100, cap=500), 500)
        self.assertEqual(fine_cents("2026-02-27", "2026-03-02"), 150)    # atravessa o fim de fevereiro
        with self.assertRaises(BadDate):
            fine_cents("2026-01-24", "ontem")
        with self.assertRaises(BadDate):
            fine_cents("x", "2026-01-24")

    def test_format_money(self):
        from lending.fines import format_money
        self.assertEqual([format_money(c) for c in (350, 5, 0, 2000, 123456, 100, 99)],
                         ["R$ 3,50", "R$ 0,05", "R$ 0,00", "R$ 20,00", "R$ 1234,56", "R$ 1,00", "R$ 0,99"])


class Model(unittest.TestCase):
    def test_due_default_and_roundtrip(self):
        l = Loan(1, 2, 3, "2026-01-10")
        self.assertEqual((l.due, l.fine_cents), ("2026-01-24", 0))
        self.assertEqual(Loan.from_dict(l.to_dict()), l)
        self.assertEqual(l.to_dict()["due"], "2026-01-24")
        self.assertEqual(Loan(1, 2, 3, "2026-01-10", due="2026-02-01").due, "2026-02-01")

    def test_old_database_without_due(self):
        path = os.path.join(tempfile.mkdtemp(), "old.json")
        old = {"books": [{"id": 1, "title": "T", "author": "A", "copies": 1}], "members": [{"id": 1, "name": "Ana"}],
               "loans": [{"id": 1, "book_id": 1, "member_id": 1, "borrowed_on": "2026-01-10", "returned_on": None},
                         {"id": 2, "book_id": 1, "member_id": 1, "borrowed_on": "2025-12-30", "returned_on": "2026-01-05"}],
               "next_id": {"book": 2, "member": 2, "loan": 3}}
        json.dump(old, open(path, "w"))
        st = Store(path)
        self.assertEqual([(l.due, l.fine_cents) for l in st.loans], [("2026-01-24", 0), ("2026-01-13", 0)])
        st.save()
        saved = json.load(open(path))
        self.assertEqual(saved["loans"][0]["due"], "2026-01-24")


class Service(unittest.TestCase):
    def setUp(self):
        self.lib = Library(Store(os.path.join(tempfile.mkdtemp(), "db.json")))
        self.b = self.lib.add_book("T", "A", copies=3)
        self.m = self.lib.add_member("Ana")

    def test_return_on_time_and_late(self):
        l1 = self.lib.borrow(self.b.id, self.m.id, "2026-01-10")
        l2 = self.lib.borrow(self.b.id, self.m.id, "2026-01-10")
        self.assertEqual(self.lib.return_book(l1.id, "2026-01-24").fine_cents, 0)
        self.assertEqual(self.lib.return_book(l2.id, "2026-01-31").fine_cents, 350)
        again = Store(self.lib.store.path)
        self.assertEqual([l.fine_cents for l in again.loans], [0, 350])
        self.assertEqual(again.loan(l2.id).due, "2026-01-24")

    def test_fine_is_capped(self):
        l = self.lib.borrow(self.b.id, self.m.id, "2026-01-01")
        self.assertEqual(self.lib.return_book(l.id, "2027-01-01").fine_cents, 2000)


class Cli(unittest.TestCase):
    def setUp(self):
        os.environ["LENDING_DB"] = os.path.join(tempfile.mkdtemp(), "db.json")
        self.addCleanup(os.environ.pop, "LENDING_DB", None)

    def run_cli(self, *argv):
        out = io.StringIO()
        return cli.run(list(argv), out), out.getvalue()

    def setup_data(self):
        self.run_cli("add-book", "Dom Casmurro", "Machado", "--copies", "5")
        self.run_cli("add-member", "Ana")
        self.run_cli("add-member", "Beto")

    def test_return_prints_fine(self):
        self.setup_data()
        self.run_cli("borrow", "1", "1", "--date", "2026-01-10")
        self.run_cli("borrow", "1", "1", "--date", "2026-01-10")
        self.assertEqual(self.run_cli("return", "1", "--date", "2026-01-24"), (0, "loan 1 returned\n"))
        self.assertEqual(self.run_cli("return", "2", "--date", "2026-01-31"), (0, "loan 2 returned\nmulta: R$ 3,50\n"))

    def test_fines_command(self):
        self.setup_data()
        self.assertEqual(self.run_cli("fines"), (0, "sem multas\n"))
        for who in ("1", "2", "1", "2"):
            self.run_cli("borrow", "1", who, "--date", "2026-01-10")
        self.run_cli("return", "1", "--date", "2026-01-31")      # 7 dias: 3,50  (Ana)
        self.run_cli("return", "2", "--date", "2026-01-20")      # em dia (Beto)
        self.run_cli("return", "3", "--date", "2026-02-24")      # 31 dias: 15,50 (Ana)
        self.run_cli("return", "4", "--date", "2026-01-25")      # 1 dia: 0,50 (Beto)
        self.assertEqual(self.run_cli("fines"), (0, "loan 1 member 1 R$ 3,50\nloan 3 member 1 R$ 15,50\nloan 4 member 2 R$ 0,50\n"
                                                    "total: R$ 19,50\n"))
        self.assertEqual(self.run_cli("fines", "--member", "2"), (0, "loan 4 member 2 R$ 0,50\ntotal: R$ 0,50\n"))
        self.run_cli("add-member", "Caio")
        self.assertEqual(self.run_cli("fines", "--member", "3"), (0, "sem multas\n"))
        self.assertEqual(self.run_cli("fines", "--member", "9")[0], 1)

    def test_open_loans_have_no_fines(self):
        self.setup_data()
        self.run_cli("borrow", "1", "1", "--date", "2020-01-10")
        self.assertEqual(self.run_cli("fines"), (0, "sem multas\n"))

    def test_existing_output_unchanged(self):
        self.setup_data()
        self.run_cli("borrow", "1", "1", "--date", "2026-01-10")
        self.assertEqual(self.run_cli("loans"), (0, "loan 1: Dom Casmurro -> Ana, borrowed 2026-01-10, open\n"))


class ExistingTests(unittest.TestCase):
    def test_repo_tests_still_pass(self):
        p = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", "."], cwd=HERE,
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr[-500:])


if __name__ == "__main__":
    unittest.main()
