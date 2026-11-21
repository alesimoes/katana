"""Command line: python3 -m agenda COMMAND ...   (the database file is $AGENDA_DB, default agenda.json)"""
import argparse
import os
import sys

from .calendar import Calendar
from .errors import AgendaError
from .render import day_text, range_text, conflict_text
from .storage import Storage


def build_parser():
    p = argparse.ArgumentParser(prog="agenda")
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add")
    a.add_argument("title")
    a.add_argument("date")
    a.add_argument("time")
    a.add_argument("minutes", type=int)
    a = sub.add_parser("remove")
    a.add_argument("id", type=int)
    a = sub.add_parser("day")
    a.add_argument("date")
    a = sub.add_parser("range")
    a.add_argument("start")
    a.add_argument("end")
    a = sub.add_parser("conflicts")
    a.add_argument("date")
    return p


def run(argv, out=None):
    out = out or sys.stdout
    args = build_parser().parse_args(argv)
    cal = Calendar(Storage(os.environ.get("AGENDA_DB") or "agenda.json"))
    try:
        if args.cmd == "add":
            e = cal.add(args.title, args.date, args.time, args.minutes)
            print("added event %d" % e.id, file=out)
        elif args.cmd == "remove":
            e = cal.remove(args.id)
            print("removed event %d" % e.id, file=out)
        elif args.cmd == "day":
            print(day_text(args.date, cal.on_day(args.date)), file=out)
        elif args.cmd == "range":
            print(range_text(cal.between(args.start, args.end)), file=out)
        elif args.cmd == "conflicts":
            print(conflict_text(cal.conflicts(args.date)), file=out)
    except AgendaError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


def main():
    sys.exit(run(sys.argv[1:]))
