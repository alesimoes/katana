"""Plain data classes. Dates are strings in the form YYYY-MM-DD everywhere (storage, service, CLI)."""
from dataclasses import dataclass, asdict
from datetime import date

from .errors import BadDate


def parse_date(text):
    """YYYY-MM-DD -> datetime.date; BadDate for anything else"""
    try:
        return date.fromisoformat(text)
    except (TypeError, ValueError):
        raise BadDate("bad date: %r (use YYYY-MM-DD)" % (text,))


@dataclass
class Book:
    id: int
    title: str
    author: str
    copies: int = 1

    def to_dict(self):
        return asdict(self)

    @classmethod
    def from_dict(cls, d):
        return cls(d["id"], d["title"], d["author"], d.get("copies", 1))


@dataclass
class Member:
    id: int
    name: str

    def to_dict(self):
        return asdict(self)

    @classmethod
    def from_dict(cls, d):
        return cls(d["id"], d["name"])


@dataclass
class Loan:
    id: int
    book_id: int
    member_id: int
    borrowed_on: str
    returned_on: str = None

    @property
    def is_open(self):
        return self.returned_on is None

    def to_dict(self):
        return asdict(self)

    @classmethod
    def from_dict(cls, d):
        return cls(d["id"], d["book_id"], d["member_id"], d["borrowed_on"], d.get("returned_on"))
