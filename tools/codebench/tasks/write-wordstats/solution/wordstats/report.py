from .counter import top


def render(text, n=3):
    rows = top(text, n)
    if not rows:
        return "(no words)"
    return "\n".join("%s: %d" % (w, c) for w, c in rows)
