"""Cart totals. Prices are integer cents."""


def line_total(item):
    return item["price"] * item["qty"]


def total(items):
    return sum(i["price"] for i in items)
