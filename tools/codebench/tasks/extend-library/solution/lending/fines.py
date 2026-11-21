"""Late fees: due dates, the fine for a late return and money formatting."""
from datetime import date, timedelta

from .errors import BadDate

LOAN_DAYS = 14


def _parse(text):
    try:
        return date.fromisoformat(text)
    except (TypeError, ValueError):
        raise BadDate("bad date: %r (use YYYY-MM-DD)" % (text,))


def due_date(borrowed_on):
    return (_parse(borrowed_on) + timedelta(days=LOAN_DAYS)).isoformat()


def fine_cents(due, returned_on, per_day=50, cap=2000):
    late = (_parse(returned_on) - _parse(due)).days
    if late <= 0:
        return 0
    return min(late * per_day, cap)


def format_money(cents):
    if cents < 0:
        raise ValueError("negative amount")
    return "R$ %d,%02d" % (cents // 100, cents % 100)
