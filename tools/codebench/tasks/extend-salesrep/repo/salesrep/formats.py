"""Output formats. FORMATS maps a name to a function that turns a Table into text (without a final newline)."""
import csv
import io

from .errors import UnknownFormat


def render_text(table):
    widths = [len(h) for h in table.headers]
    for row in table.rows:
        widths = [max(w, len(c)) for w, c in zip(widths, row)]

    def line(cells):
        return "  ".join(c.ljust(w) if a == "l" else c.rjust(w) for c, w, a in zip(cells, widths, table.align)).rstrip()

    return "\n".join([line(table.headers)] + [line(r) for r in table.rows])


def render_csv(table):
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(table.headers)
    w.writerows(table.rows)
    return buf.getvalue().rstrip("\n")


FORMATS = {"text": render_text, "csv": render_csv}


def render(name, table):
    try:
        fn = FORMATS[name]
    except KeyError:
        raise UnknownFormat("unknown format %r (use %s)" % (name, ", ".join(sorted(FORMATS))))
    return fn(table)
