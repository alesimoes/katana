"""miniconf: leitor e escritor de um formato de configuracao pequeno."""
import re

_NAME = re.compile(r"[A-Za-z0-9_.-]+")
_INT = re.compile(r"-?\d+")
_FLOAT = re.compile(r"-?\d+\.\d+")
_ESC = {'"': '"', "\\": "\\", "n": "\n", "t": "\t"}


class ParseError(ValueError):
    pass


def _quoted(s, i, lineno):
    """le a string que comeca em s[i] == '"'; devolve (texto, indice depois da aspa final)"""
    out = []
    i += 1
    while i < len(s):
        c = s[i]
        if c == "\\":
            if i + 1 >= len(s) or s[i + 1] not in _ESC:
                raise ParseError("line %d: bad escape" % lineno)
            out.append(_ESC[s[i + 1]])
            i += 2
        elif c == '"':
            return "".join(out), i + 1
        else:
            out.append(c)
            i += 1
    raise ParseError("line %d: unterminated string" % lineno)


def _strip_comment(s, lineno):
    i = 0
    while i < len(s):
        if s[i] == '"':
            _, i = _quoted(s, i, lineno)
        elif s[i] == "#":
            return s[:i]
        else:
            i += 1
    return s


def _scalar(tok, lineno):
    tok = tok.strip()
    if not tok:
        raise ParseError("line %d: empty value" % lineno)
    if tok[0] == '"':
        text, end = _quoted(tok, 0, lineno)
        if tok[end:].strip():
            raise ParseError("line %d: text after closing quote" % lineno)
        return text
    if tok == "true":
        return True
    if tok == "false":
        return False
    if _INT.fullmatch(tok):
        return int(tok)
    if _FLOAT.fullmatch(tok):
        return float(tok)
    if tok[0] == "[" or '"' in tok:
        raise ParseError("line %d: bad value %r" % (lineno, tok))
    return tok


def _split_list(inner, lineno):
    items, cur, i = [], [], 0
    while i < len(inner):
        c = inner[i]
        if c == '"':
            _, j = _quoted(inner, i, lineno)
            cur.append(inner[i:j])
            i = j
        elif c == ",":
            items.append("".join(cur))
            cur = []
            i += 1
        else:
            cur.append(c)
            i += 1
    items.append("".join(cur))
    return items


def _value(raw, lineno):
    raw = raw.strip()
    if not raw:
        raise ParseError("line %d: empty value" % lineno)
    if raw[0] == "[":
        if not raw.endswith("]"):
            raise ParseError("line %d: unterminated list" % lineno)
        inner = raw[1:-1]
        if not inner.strip():
            return []
        out = []
        for tok in _split_list(inner, lineno):
            if tok.strip().startswith("["):
                raise ParseError("line %d: nested list" % lineno)
            out.append(_scalar(tok, lineno))
        return out
    return _scalar(raw, lineno)


def parse(text):
    result = {}
    cur = result
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw, lineno).strip()
        if not line:
            continue
        if line.startswith("[") and "=" not in line:
            if not line.endswith("]") or not _NAME.fullmatch(line[1:-1]):
                raise ParseError("line %d: bad section header" % lineno)
            name = line[1:-1]
            if name in result:
                raise ParseError("line %d: duplicate section %r" % (lineno, name))
            result[name] = cur = {}
            continue
        if "=" not in line:
            raise ParseError("line %d: expected key = value" % lineno)
        key, _, val = line.partition("=")
        key = key.strip()
        if not _NAME.fullmatch(key):
            raise ParseError("line %d: bad key %r" % (lineno, key))
        if key in cur:
            raise ParseError("line %d: duplicate key %r" % (lineno, key))
        cur[key] = _value(val, lineno)
    return result


def _fmt_scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return repr(v)
    if isinstance(v, float):
        if not _FLOAT.fullmatch(repr(v)):
            raise ValueError("cannot write float %r" % v)
        return repr(v)
    if isinstance(v, str):
        s = v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
        return '"%s"' % s
    raise ValueError("cannot write %r" % (v,))


def _fmt(v):
    if isinstance(v, list):
        for x in v:
            if isinstance(x, (list, dict)):
                raise ValueError("nested list or dict inside a list")
        return "[" + ", ".join(_fmt_scalar(x) for x in v) + "]"
    if isinstance(v, dict):
        raise ValueError("dict not allowed here")
    return _fmt_scalar(v)


def _check_key(k):
    if not isinstance(k, str) or not _NAME.fullmatch(k):
        raise ValueError("bad key %r" % (k,))


def dumps(data):
    if not isinstance(data, dict):
        raise ValueError("data must be a dict")
    lines = []
    for k, v in data.items():
        _check_key(k)
        if not isinstance(v, dict):
            lines.append("%s = %s" % (k, _fmt(v)))
    for k, v in data.items():
        if isinstance(v, dict):
            lines.append("")
            lines.append("[%s]" % k)
            for k2, v2 in v.items():
                _check_key(k2)
                lines.append("%s = %s" % (k2, _fmt(v2)))
    text = "\n".join(lines)
    if text.startswith("\n"):
        text = text[1:]
    return text + "\n" if text else ""
