"""Reads ledger lines: DATE CATEGORY AMOUNT [NOTE...]"""
from collections import namedtuple

Entry = namedtuple("Entry", "date category amount note")


def parse_line(line):
    parts = line.split()
    if len(parts) < 3:
        raise ValueError("bad line: %r" % line)
    date, category = parts[0], parts[1]
    if len(date) != 10 or date[4] != "-" or date[7] != "-":
        raise ValueError("bad date: %r" % date)
    amount = float(parts[2])
    return Entry(date, category, amount, " ".join(parts[3:]))


def parse_file(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            out.append(parse_line(line))
    return out
