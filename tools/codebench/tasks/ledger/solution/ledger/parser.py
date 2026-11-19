"""Reads ledger lines: DATE CATEGORY AMOUNT [CURRENCY] [NOTE...]"""
from collections import namedtuple

Entry = namedtuple("Entry", "date category amount note currency", defaults=("EUR",))


def parse_line(line):
    parts = line.split()
    if len(parts) < 3:
        raise ValueError("bad line: %r" % line)
    date, category = parts[0], parts[1]
    if len(date) != 10 or date[4] != "-" or date[7] != "-":
        raise ValueError("bad date: %r" % date)
    amount = float(parts[2])
    rest = parts[3:]
    currency = "EUR"
    if rest and len(rest[0]) == 3 and rest[0].isalpha() and rest[0].isupper():
        currency = rest[0]
        rest = rest[1:]
    return Entry(date, category, amount, " ".join(rest), currency)


def parse_file(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            out.append(parse_line(line))
    return out
