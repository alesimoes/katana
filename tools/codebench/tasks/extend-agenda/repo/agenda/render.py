"""Text output of the CLI."""
from .events import fmt_time


def event_line(e):
    return "%s-%s %s (id %d)" % (fmt_time(e.start), fmt_time(e.end), e.title, e.id)


def day_text(day, events):
    if not events:
        return "%s: nothing" % day
    return "\n".join([day] + ["  " + event_line(e) for e in events])


def range_text(pairs):
    """pairs is the (date, event) list of Calendar.between; days without events are not shown"""
    out = []
    current = None
    for day, e in pairs:
        if day != current:
            out.append(day)
            current = day
        out.append("  " + event_line(e))
    return "\n".join(out) if out else "nothing"


def conflict_text(pairs):
    if not pairs:
        return "no conflicts"
    return "\n".join("%s <-> %s" % (event_line(a), event_line(b)) for a, b in pairs)
