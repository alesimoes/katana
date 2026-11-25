#!/usr/bin/env python3
# Text helpers (tests of incremental editing). Formatting here is deliberately uneven.
import os, sys
from typing import List


def slugify(text, sep='-', maxlen=40):
    """Lower-case slug."""
    out = []
    for ch in text.lower():
        if ch.isalnum():
            out.append(ch)
        elif out and out[-1] != sep:
            out.append(sep)
    return "".join(out).strip(sep)[:maxlen]


def wrap_words(words: List[str],
               width: int = 20,
               indent: str = "") -> List[str]:
    lines = []
    cur = indent
    for w in words:
        if len(cur) + len(w) + 1 > width and cur.strip():
            lines.append(cur.rstrip())
            cur = indent
        cur += w + " "
    if cur.strip():
        lines.append(cur.rstrip())
    return lines


def count_vowels(s): return sum(1 for c in s if c in "aeiou")


def outer(n):
    def inner(k):
        return k * 2
    total = 0
    for i in range(n):
        total += inner(i)
    return total


def first_word(s):
    # the first word, or empty
    parts = s.split()
    if not parts:
        return ""
    return parts[0]


def clamp(x, lo, hi):
    if x < lo:
        return lo
    elif x > hi:
        return hi
    else:
        return x


def safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        return None
    finally:
        pass


def long_call_site():
    return wrap_words(
        ["alpha", "beta", "gamma"],
        width=10,
    )


def uses_wrap():
    a = wrap_words(["x", "y"], 5)
    b = wrap_words(words=["z"], width=3, indent="  ")
    return a, b
