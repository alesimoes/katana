"""Totals by region, product or month, and the conversion to a Table."""
from .table import Table

KEYS = {"region": lambda r: r.region, "product": lambda r: r.product, "month": lambda r: r.month}
SORTS = ("key", "qty", "revenue")


def group(rows, by):
    """list of (key, qty, revenue) for every distinct key, revenue rounded to 2 decimals"""
    keyf = KEYS[by]
    acc = {}
    for r in rows:
        q, v = acc.get(keyf(r), (0, 0.0))
        acc[keyf(r)] = (q + r.qty, v + r.revenue)
    return [(k, q, round(v, 2)) for k, (q, v) in acc.items()]


def order(groups, sort="key", top=None):
    """'key' ascending by name, 'qty' and 'revenue' descending (ties by name); top keeps the first N"""
    if sort == "key":
        out = sorted(groups, key=lambda g: g[0])
    elif sort == "qty":
        out = sorted(groups, key=lambda g: (-g[1], g[0]))
    elif sort == "revenue":
        out = sorted(groups, key=lambda g: (-g[2], g[0]))
    else:
        raise ValueError("unknown sort %r" % sort)
    return out[:top] if top is not None else out


def to_table(groups, by):
    rows = [[k, "%d" % q, "%.2f" % v] for k, q, v in groups]
    return Table([by, "qty", "revenue"], rows, ["l", "r", "r"])
