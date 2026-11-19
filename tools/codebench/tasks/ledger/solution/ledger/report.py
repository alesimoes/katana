"""Totals of a list of entries."""
from ledger.rates import convert


def _eur(e, rates):
    if rates is None:
        if e.currency != "EUR":
            raise ValueError("entry in %s needs rates" % e.currency)
        return e.amount
    return convert(e.amount, e.currency, rates)


def totals_by_category(entries, rates=None):
    totals = {}
    for e in entries:
        totals[e.category] = round(totals.get(e.category, 0.0) + _eur(e, rates), 2)
    return totals


def monthly(entries, rates=None):
    out = {}
    for e in entries:
        m = out.setdefault(e.date[:7], {})
        m[e.category] = round(m.get(e.category, 0.0) + _eur(e, rates), 2)
    return out


def over_budget(entries, budgets, rates=None):
    res = []
    by_month = monthly(entries, rates)
    for month in sorted(by_month):
        for cat in sorted(by_month[month]):
            if cat not in budgets:
                continue
            total = by_month[month][cat]
            spent = round(-total, 2) if total < 0 else 0
            if spent > budgets[cat]:
                res.append((month, cat, spent, budgets[cat]))
    return res


def format_report(entries):
    totals = totals_by_category(entries)
    return "\n".join("%s: %.2f" % (k, totals[k]) for k in sorted(totals))
