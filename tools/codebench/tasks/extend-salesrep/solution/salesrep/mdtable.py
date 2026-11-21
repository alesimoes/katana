"""Markdown table output."""
import re


def _clean(cell):
    return re.sub(r"[\r\n]+", " ", cell).replace("|", "\\|")


def render_markdown(table):
    headers = [_clean(h) for h in table.headers]
    rows = [[_clean(c) for c in r] for r in table.rows]
    widths = [max([3, len(h)] + [len(r[i]) for r in rows]) for i, h in enumerate(headers)]

    def line(cells):
        return "| " + " | ".join(c.ljust(w) if a == "l" else c.rjust(w)
                                 for c, w, a in zip(cells, widths, table.align)) + " |"

    sep = ["-" * (w - 1) + ":" if a == "r" else ":" + "-" * (w - 1) for w, a in zip(widths, table.align)]
    return "\n".join([line(headers), "| " + " | ".join(sep) + " |"] + [line(r) for r in rows])
