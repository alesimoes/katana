from .tokenizer import tokenize


def count(text):
    out = {}
    for w in tokenize(text):
        out[w] = out.get(w, 0) + 1
    return out


def top(text, n=3):
    items = sorted(count(text).items(), key=lambda kv: (-kv[1], kv[0]))
    return items[:max(n, 0)]
