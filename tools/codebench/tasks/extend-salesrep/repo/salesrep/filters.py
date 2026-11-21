"""Choosing which rows enter the totals: by date range and by region."""
import re

from .errors import ReportError

_DATE = re.compile(r"\d{4}-\d\d-\d\d")


def check_date(text, what):
    if text is not None and not _DATE.fullmatch(text):
        raise ReportError("bad %s date %r (use YYYY-MM-DD)" % (what, text))
    return text


def filter_rows(rows, start=None, end=None, regions=None):
    """keeps the rows with start <= date <= end (both inclusive, either may be None) whose region is one of
    `regions` (compared ignoring case; None or an empty list keeps every region)"""
    check_date(start, "start")
    check_date(end, "end")
    if start is not None and end is not None and end < start:
        raise ReportError("the end date is before the start date")
    wanted = {r.lower() for r in regions or []}
    out = []
    for r in rows:
        if start is not None and r.date < start:
            continue
        if end is not None and r.date > end:
            continue
        if wanted and r.region.lower() not in wanted:
            continue
        out.append(r)
    return out
