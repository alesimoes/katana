"""Totals of a list of entries."""


def totals_by_category(entries):
    totals = {}
    for e in entries:
        totals[e.category] = round(totals.get(e.category, 0.0) + e.amount, 2)
    return totals


def format_report(entries):
    totals = totals_by_category(entries)
    return "\n".join("%s: %.2f" % (k, totals[k]) for k in sorted(totals))
