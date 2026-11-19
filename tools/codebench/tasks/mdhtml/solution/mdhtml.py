import re


def _esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _inline(s):
    parts = _esc(s).split("`")
    out = []
    for i, p in enumerate(parts):
        if i % 2 == 1 and i < len(parts) - 1:
            out.append("<code>" + p + "</code>")
            continue
        if i % 2 == 1:
            p = "`" + p
        p = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", p)
        p = re.sub(r"\*(.+?)\*", r"<em>\1</em>", p)
        p = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', p)
        out.append(p)
    return "".join(out)


_OL = re.compile(r"\d+\. ")
_H = re.compile(r"(#{1,6}) (.*)")


def _starts_block(line):
    return line.startswith("```") or line.startswith("- ") or _OL.match(line) or _H.match(line)


def render(text):
    lines = text.split("\n")
    out = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.strip() == "":
            i += 1
            continue
        if line.startswith("```"):
            i += 1
            buf = []
            while i < n and not lines[i].startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            out.append("<pre><code>" + _esc("\n".join(buf)) + "</code></pre>")
            continue
        m = _H.match(line)
        if m:
            k = len(m.group(1))
            out.append("<h%d>%s</h%d>" % (k, _inline(m.group(2).strip()), k))
            i += 1
            continue
        if line.startswith("- "):
            items = []
            while i < n and lines[i].startswith("- "):
                items.append(lines[i][2:].strip())
                i += 1
            out.append("<ul>" + "".join("<li>%s</li>" % _inline(x) for x in items) + "</ul>")
            continue
        if _OL.match(line):
            items = []
            while i < n and _OL.match(lines[i]):
                items.append(_OL.sub("", lines[i], count=1).strip())
                i += 1
            out.append("<ol>" + "".join("<li>%s</li>" % _inline(x) for x in items) + "</ol>")
            continue
        buf = []
        while i < n and lines[i].strip() != "" and not _starts_block(lines[i]):
            buf.append(lines[i].strip())
            i += 1
        out.append("<p>" + _inline(" ".join(buf)) + "</p>")
    return "\n".join(out)
