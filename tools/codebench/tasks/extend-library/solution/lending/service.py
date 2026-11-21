"""The rules of the library. Every method that changes data saves the store before returning."""
from .errors import NotFound, NotAvailable, LimitReached, AlreadyReturned
from .fines import fine_cents
from .models import Book, Member, Loan, parse_date

MAX_OPEN_LOANS = 3


class Library:
    def __init__(self, store):
        self.store = store

    def add_book(self, title, author, copies=1):
        if copies < 1:
            raise ValueError("copies must be at least 1")
        b = Book(self.store.new_id("book"), title, author, copies)
        self.store.books.append(b)
        self.store.save()
        return b

    def add_member(self, name):
        m = Member(self.store.new_id("member"), name)
        self.store.members.append(m)
        self.store.save()
        return m

    def open_loans(self, member_id=None):
        return [l for l in self.store.loans if l.is_open and (member_id is None or l.member_id == member_id)]

    def available(self, book_id):
        b = self.store.book(book_id)
        if b is None:
            raise NotFound("no book with id %d" % book_id)
        return b.copies - sum(1 for l in self.open_loans() if l.book_id == book_id)

    def borrow(self, book_id, member_id, today):
        parse_date(today)
        if self.store.member(member_id) is None:
            raise NotFound("no member with id %d" % member_id)
        if self.available(book_id) < 1:
            raise NotAvailable("no copy of book %d is available" % book_id)
        if len(self.open_loans(member_id)) >= MAX_OPEN_LOANS:
            raise LimitReached("member %d already has %d open loans" % (member_id, MAX_OPEN_LOANS))
        loan = Loan(self.store.new_id("loan"), book_id, member_id, today)
        self.store.loans.append(loan)
        self.store.save()
        return loan

    def return_book(self, loan_id, today):
        parse_date(today)
        loan = self.store.loan(loan_id)
        if loan is None:
            raise NotFound("no loan with id %d" % loan_id)
        if not loan.is_open:
            raise AlreadyReturned("loan %d was already returned on %s" % (loan_id, loan.returned_on))
        loan.returned_on = today
        loan.fine_cents = fine_cents(loan.due, today)
        self.store.save()
        return loan

    def history(self, member_id):
        if self.store.member(member_id) is None:
            raise NotFound("no member with id %d" % member_id)
        return [l for l in self.store.loans if l.member_id == member_id]

    def fined_loans(self, member_id=None):
        if member_id is not None and self.store.member(member_id) is None:
            raise NotFound("no member with id %d" % member_id)
        return sorted((l for l in self.store.loans
                       if not l.is_open and l.fine_cents > 0 and (member_id is None or l.member_id == member_id)),
                      key=lambda l: l.id)
