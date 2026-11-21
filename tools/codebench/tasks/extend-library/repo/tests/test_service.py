import os
import tempfile
import unittest

from lending.errors import NotFound, NotAvailable, LimitReached, AlreadyReturned, BadDate
from lending.models import Loan
from lending.service import Library
from lending.store import Store


class ServiceTest(unittest.TestCase):
    def setUp(self):
        self.path = os.path.join(tempfile.mkdtemp(), "db.json")
        self.lib = Library(Store(self.path))
        self.book = self.lib.add_book("Dom Casmurro", "Machado de Assis")
        self.member = self.lib.add_member("Ana")

    def test_borrow_and_return(self):
        loan = self.lib.borrow(self.book.id, self.member.id, "2026-01-10")
        self.assertTrue(loan.is_open)
        self.assertEqual(self.lib.available(self.book.id), 0)
        self.lib.return_book(loan.id, "2026-01-12")
        self.assertEqual(self.lib.available(self.book.id), 1)
        self.assertEqual(self.lib.store.loan(loan.id).returned_on, "2026-01-12")

    def test_no_copy_left(self):
        self.lib.borrow(self.book.id, self.member.id, "2026-01-10")
        with self.assertRaises(NotAvailable):
            self.lib.borrow(self.book.id, self.member.id, "2026-01-11")

    def test_limit(self):
        for i in range(3):
            b = self.lib.add_book("B%d" % i, "X")
            self.lib.borrow(b.id, self.member.id, "2026-01-10")
        b = self.lib.add_book("B9", "X")
        with self.assertRaises(LimitReached):
            self.lib.borrow(b.id, self.member.id, "2026-01-10")

    def test_unknown_ids_and_dates(self):
        with self.assertRaises(NotFound):
            self.lib.borrow(99, self.member.id, "2026-01-10")
        with self.assertRaises(NotFound):
            self.lib.borrow(self.book.id, 99, "2026-01-10")
        with self.assertRaises(NotFound):
            self.lib.return_book(5, "2026-01-10")
        with self.assertRaises(BadDate):
            self.lib.borrow(self.book.id, self.member.id, "10/01/2026")

    def test_double_return(self):
        loan = self.lib.borrow(self.book.id, self.member.id, "2026-01-10")
        self.lib.return_book(loan.id, "2026-01-11")
        with self.assertRaises(AlreadyReturned):
            self.lib.return_book(loan.id, "2026-01-12")

    def test_persistence(self):
        loan = self.lib.borrow(self.book.id, self.member.id, "2026-01-10")
        again = Library(Store(self.path))
        self.assertEqual(again.store.loan(loan.id), Loan(loan.id, self.book.id, self.member.id, "2026-01-10"))
        self.assertEqual(again.add_member("Bia").id, 2)
