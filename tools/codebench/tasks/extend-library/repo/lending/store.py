"""JSON file storage. The whole database is read at the start of a command and written back after a change."""
import json
import os

from .models import Book, Member, Loan


class Store:
    def __init__(self, path):
        self.path = path
        self.books = []
        self.members = []
        self.loans = []
        self.next_id = {"book": 1, "member": 1, "loan": 1}
        self.load()

    def load(self):
        if not os.path.exists(self.path):
            return
        with open(self.path, encoding="utf-8") as f:
            data = json.load(f)
        self.books = [Book.from_dict(d) for d in data.get("books", [])]
        self.members = [Member.from_dict(d) for d in data.get("members", [])]
        self.loans = [Loan.from_dict(d) for d in data.get("loans", [])]
        self.next_id.update(data.get("next_id", {}))

    def save(self):
        data = {
            "books": [b.to_dict() for b in self.books],
            "members": [m.to_dict() for m in self.members],
            "loans": [l.to_dict() for l in self.loans],
            "next_id": self.next_id,
        }
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.path)

    def new_id(self, kind):
        n = self.next_id[kind]
        self.next_id[kind] = n + 1
        return n

    def book(self, book_id):
        for b in self.books:
            if b.id == book_id:
                return b
        return None

    def member(self, member_id):
        for m in self.members:
            if m.id == member_id:
                return m
        return None

    def loan(self, loan_id):
        for l in self.loans:
            if l.id == loan_id:
                return l
        return None
