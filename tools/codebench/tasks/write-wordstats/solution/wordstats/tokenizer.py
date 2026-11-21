import re

_WORD = re.compile(r"[^\W_]+(?:['’-][^\W_]+)*")


def tokenize(text):
    return [w.lower() for w in _WORD.findall(text)]
