"""Command line: python3 -m lending COMMAND ...   (the database file is $LENDING_DB, default lending.json)"""
import argparse
import os
import sys

from .errors import LendingError
from .report import loan_line, book_line, member_line
from .service import Library
from .store import Store


def build_parser():
    p = argparse.ArgumentParser(prog="lending")
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add-book")
    a.add_argument("title")
    a.add_argument("author")
    a.add_argument("--copies", type=int, default=1)
    a = sub.add_parser("add-member")
    a.add_argument("name")
    a = sub.add_parser("borrow")
    a.add_argument("book_id", type=int)
    a.add_argument("member_id", type=int)
    a.add_argument("--date", required=True, help="today, YYYY-MM-DD")
    a = sub.add_parser("return")
    a.add_argument("loan_id", type=int)
    a.add_argument("--date", required=True, help="today, YYYY-MM-DD")
    a = sub.add_parser("loans")
    a.add_argument("--member", type=int)
    sub.add_parser("books")
    sub.add_parser("members")
    return p


def run(argv, out=None):
    out = out or sys.stdout
    args = build_parser().parse_args(argv)
    lib = Library(Store(os.environ.get("LENDING_DB") or "lending.json"))
    try:
        if args.cmd == "add-book":
            b = lib.add_book(args.title, args.author, args.copies)
            print("added book %d" % b.id, file=out)
        elif args.cmd == "add-member":
            m = lib.add_member(args.name)
            print("added member %d" % m.id, file=out)
        elif args.cmd == "borrow":
            loan = lib.borrow(args.book_id, args.member_id, args.date)
            print("loan %d created" % loan.id, file=out)
        elif args.cmd == "return":
            loan = lib.return_book(args.loan_id, args.date)
            print("loan %d returned" % loan.id, file=out)
        elif args.cmd == "loans":
            for loan in lib.open_loans(args.member):
                print(loan_line(loan, lib.store), file=out)
        elif args.cmd == "books":
            for b in lib.store.books:
                print(book_line(b, lib.available(b.id)), file=out)
        elif args.cmd == "members":
            for m in lib.store.members:
                print(member_line(m, len(lib.open_loans(m.id))), file=out)
    except LendingError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


def main():
    sys.exit(run(sys.argv[1:]))
