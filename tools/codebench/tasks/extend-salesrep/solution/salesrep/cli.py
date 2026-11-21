"""Command line: python3 -m salesrep FILE --by region|product|month [--format text|csv|md] [--sort key|qty|revenue] [--top N]
[--from DATE] [--to DATE] [--region NAME ...] [--output FILE]"""
import argparse
import sys

from .aggregate import KEYS, SORTS, group, order, to_table
from .errors import ReportError
from .filters import filter_rows
from .formats import FORMATS, render
from .loader import load_rows


def build_parser():
    p = argparse.ArgumentParser(prog="salesrep")
    p.add_argument("file")
    p.add_argument("--by", choices=sorted(KEYS), default="region")
    p.add_argument("--format", choices=sorted(FORMATS), default="text")
    p.add_argument("--sort", choices=SORTS, default="key")
    p.add_argument("--top", type=int)
    p.add_argument("--from", dest="start", help="first date to count, YYYY-MM-DD")
    p.add_argument("--to", dest="end", help="last date to count, YYYY-MM-DD")
    p.add_argument("--region", action="append", help="only this region (repeat the option for several)")
    p.add_argument("--output")
    return p


def run(argv, out=None):
    out = out or sys.stdout
    args = build_parser().parse_args(argv)
    try:
        if args.top is not None and args.top < 1:
            raise ReportError("--top must be at least 1")
        rows = filter_rows(load_rows(args.file), args.start, args.end, args.region)
        table = to_table(order(group(rows, args.by), args.sort, args.top), args.by)
        text = render(args.format, table)
        if args.output:
            try:
                with open(args.output, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text + "\n")
            except OSError as e:
                raise ReportError("cannot write %s: %s" % (args.output, e.strerror))
            print("escrito %s" % args.output, file=out)
        else:
            print(text, file=out)
    except ReportError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


def main():
    sys.exit(run(sys.argv[1:]))
