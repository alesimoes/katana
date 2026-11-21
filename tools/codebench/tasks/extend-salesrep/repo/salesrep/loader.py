"""Reading the sales CSV."""
import csv
import re
from dataclasses import dataclass

from .errors import LoadError

COLUMNS = ["date", "region", "product", "qty", "price"]
_DATE = re.compile(r"\d{4}-\d\d-\d\d")


@dataclass
class Row:
    date: str
    region: str
    product: str
    qty: int
    price: float

    @property
    def revenue(self):
        return self.qty * self.price

    @property
    def month(self):
        return self.date[:7]


def load_rows(path):
    try:
        f = open(path, newline="", encoding="utf-8")
    except OSError as e:
        raise LoadError("cannot open %s: %s" % (path, e.strerror))
    with f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header != COLUMNS:
            raise LoadError("line 1: expected the header %s" % ",".join(COLUMNS))
        rows = []
        for n, rec in enumerate(reader, 2):
            if not rec:
                continue
            if len(rec) != len(COLUMNS):
                raise LoadError("line %d: expected %d columns" % (n, len(COLUMNS)))
            date, region, product, qty, price = [x.strip() for x in rec]
            if not _DATE.fullmatch(date):
                raise LoadError("line %d: bad date %r" % (n, date))
            try:
                q, p = int(qty), float(price)
            except ValueError:
                raise LoadError("line %d: bad number" % n)
            if q < 0 or p < 0:
                raise LoadError("line %d: negative value" % n)
            rows.append(Row(date, region, product, q, p))
    return rows
