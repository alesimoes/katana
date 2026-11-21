import argparse
import sys

from .report import render


def main(argv=None):
    ap = argparse.ArgumentParser(prog="wordstats")
    ap.add_argument("file")
    ap.add_argument("-n", type=int, default=3)
    a = ap.parse_args(argv)
    try:
        with open(a.file, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        print("wordstats: cannot read %s: %s" % (a.file, e.strerror), file=sys.stderr)
        return 1
    sys.stdout.reconfigure(encoding="utf-8")
    print(render(text, a.n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
