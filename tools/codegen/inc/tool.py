#!/usr/bin/env python3
"""A tiny command line tool."""
import sys


def parse_args(argv):
    # naive parsing: --flag value pairs
    opts = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith("--"):
            opts[argv[i][2:]] = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
        else:
            i += 1
    return opts


def load(path):
    with open(path) as fh:
        return fh.read()


def run(argv):
    opts = parse_args(argv)
    text = load(opts.get("in", "/dev/null"))
    print(len(text))
    return 0


# entry point
if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
