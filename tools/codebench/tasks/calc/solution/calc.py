import re

TOKEN = re.compile(r"\s*(?:(\d+\.?\d*|\.\d+)|(.))")

def _tokens(expr):
    pos, out = 0, []
    expr = expr.rstrip()
    while pos < len(expr):
        m = TOKEN.match(expr, pos)
        if not m:
            raise ValueError("bad input")
        if m.group(1) is not None:
            out.append(("num", float(m.group(1))))
        else:
            ch = m.group(2)
            if ch not in "+-*/^()":
                raise ValueError("bad character")
            out.append((ch, None))
        pos = m.end()
    return out

def evaluate(expr):
    toks = _tokens(expr)
    if not toks:
        raise ValueError("empty")
    i = 0

    def peek():
        return toks[i][0] if i < len(toks) else None

    def expression():
        nonlocal i
        v = term()
        while peek() in ("+", "-"):
            op = toks[i][0]; i += 1
            r = term()
            v = v + r if op == "+" else v - r
        return v

    def term():
        nonlocal i
        v = unary()
        while peek() in ("*", "/"):
            op = toks[i][0]; i += 1
            r = unary()
            if op == "*":
                v = v * r
            else:
                if r == 0:
                    raise ZeroDivisionError("division by zero")
                v = v / r
        return v

    def unary():
        nonlocal i
        if peek() in ("+", "-"):
            op = toks[i][0]; i += 1
            v = unary()
            return -v if op == "-" else v
        return power()

    def power():
        nonlocal i
        base = atom()
        if peek() == "^":
            i += 1
            return base ** unary()
        return base

    def atom():
        nonlocal i
        t = peek()
        if t == "num":
            v = toks[i][1]; i += 1
            return v
        if t == "(":
            i += 1
            v = expression()
            if peek() != ")":
                raise ValueError("unbalanced")
            i += 1
            return v
        raise ValueError("unexpected token")

    v = expression()
    if i != len(toks):
        raise ValueError("trailing input")
    return float(v)
