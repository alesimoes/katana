"""Inventory helpers: a small stock tracker used by the incremental tests."""
import math
import re
from collections import defaultdict

LIMIT = 100  # maximum items per bin
NAMES = ["alpha", "beta"]


# Compute the price with tax.
def price_with_tax(price, rate=0.2):
    """Return the gross price."""
    total = price * (1 + rate)  # keep the cents
    if total > LIMIT:
        total = LIMIT
    return round(total, 2)


def total_cost(items):
    total = 0
    for item in items:
        total += price_with_tax(item["price"])
    return total


@staticmethod
def decorated(x):
    return x + 1


class Stock:
    """A bin of items."""

    kind = "bin"

    def __init__(self, name, qty=0):
        self.name = name
        self.qty = qty

    def add(self, n):
        # add n units
        self.qty += n
        return self.qty

    def remove(self, n):
        if n > self.qty:
            raise ValueError("not enough")
        self.qty -= n
        return self.qty

    def report(self):
        return "%s: %d" % (self.name, self.qty)


def summary(stocks):
    lines = []
    for s in stocks:
        lines.append(s.report())
    return "\n".join(lines)


if __name__ == "__main__":
    s = Stock("a", 3)
    print(total_cost([{"price": 10}]), summary([s]))
