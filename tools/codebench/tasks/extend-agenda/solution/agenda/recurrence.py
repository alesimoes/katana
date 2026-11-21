"""Which dates a (possibly recurring) event happens on."""
import calendar as _cal
from datetime import date, timedelta

from .events import parse_date

REPEATS = ("daily", "weekly", "monthly")


def occurrences(event, start, end):
    a, b = parse_date(start), parse_date(end)
    first = parse_date(event.date)
    if not event.repeat:
        return [event.date] if a <= first <= b else []
    last = min(b, parse_date(event.until)) if event.until else b
    lo = max(a, first)
    if lo > last:
        return []
    out = []
    if event.repeat == "daily":
        d = lo
        while d <= last:
            out.append(d.isoformat())
            d += timedelta(days=1)
    elif event.repeat == "weekly":
        k = -(-(lo - first).days // 7)
        d = first + timedelta(days=7 * k)
        while d <= last:
            out.append(d.isoformat())
            d += timedelta(days=7)
    elif event.repeat == "monthly":
        y, m = lo.year, lo.month
        while (y, m) <= (last.year, last.month):
            d = date(y, m, min(first.day, _cal.monthrange(y, m)[1]))
            if lo <= d <= last:
                out.append(d.isoformat())
            y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    return out
