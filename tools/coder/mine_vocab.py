#!/usr/bin/env python3
"""Mine identifier vocabulary from source trees and measure how much of it the
concept base (data/coder/kb/concepts/*.kb) covers.

Usage:
  mine_vocab.py [--kb DIR] [--top N] [--json OUT] FOLDER [FOLDER ...]

Coverage = fraction of identifier words (weighted by frequency) that appear in
some term, canonical name or action of the base. Matching uses a light stemmer
(plural/-ed/-ing), so "orders" matches "order". Manual research tool; the
engine does not depend on it.
"""
import argparse, json, os, re, unicodedata
from collections import Counter

SKIP_DIRS = {"node_modules", "ref", "models", "build", "build-try", "dist", "__pycache__",
             "vendor", "third_party", "iso", "firmware"}
EXTS = {".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".js", ".ts", ".tsx", ".kt", ".java",
        ".qml", ".glsl", ".comp", ".sh", ".bat", ".cmake", ".rs", ".go", ".md", ".txt",
        ".json", ".yaml", ".yml", ".toml", ".xml", ".gradle", ".swift", ".cs", ".lua"}
MAX_BYTES = 2_000_000
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CAMEL = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+")
STOP = {"the", "and", "for", "with", "that", "this", "from", "not", "are", "was", "but", "its",
        "you", "all", "can", "has", "have", "will", "use", "used", "also", "one", "two"}


def strip_accents(s):
    return "".join(c for c in unicodedata.normalize("NFD", s) if unicodedata.category(c) != "Mn")


def stem(w):
    for suf in ("ing", "ed", "es", "s"):
        if w.endswith(suf) and len(w) - len(suf) >= 3:
            return w[: -len(suf)]
    return w


def split_ident(tok):
    out = []
    for part in tok.split("_"):
        for m in CAMEL.findall(part):
            if m.isalpha():
                out.append(m.lower())
    return out


def mine(folder):
    counts = Counter()
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
        for f in files:
            if os.path.splitext(f)[1].lower() not in EXTS:
                continue
            p = os.path.join(root, f)
            try:
                if os.path.getsize(p) > MAX_BYTES:
                    continue
                txt = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for tok in IDENT.findall(txt):
                for w in split_ident(tok):
                    if len(w) >= 3 and w not in STOP:
                        counts[strip_accents(w)] += 1
    return counts


def kb_words(kbdir):
    words = set()
    for fn in sorted(os.listdir(kbdir)) if os.path.isdir(kbdir) else []:
        if not fn.endswith(".kb"):
            continue
        for line in open(os.path.join(kbdir, fn), encoding="utf-8"):
            parts = line.strip().split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "term" and len(parts) >= 4:
                text = " ".join(parts[2:-1])
            elif parts[0] in ("canonical", "action") and len(parts) >= 2:
                text = " ".join(parts[1:])
            else:
                continue
            for tok in IDENT.findall(strip_accents(text)):
                for w in split_ident(tok):
                    words.add(w)
    return words, {stem(w) for w in words}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folders", nargs="+")
    ap.add_argument("--kb", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "coder", "kb", "concepts"))
    ap.add_argument("--top", type=int, default=300)
    ap.add_argument("--json")
    a = ap.parse_args()
    words, stems = kb_words(a.kb)
    print(f"base: {len(words)} distinct words")
    result = {}
    for folder in a.folders:
        c = mine(folder)
        total = sum(c.values())
        cov = sum(n for w, n in c.items() if w in words or stem(w) in stems)
        cov_types = sum(1 for w in c if w in words or stem(w) in stems)
        print(f"\n== {folder}: {total} word occurrences, {len(c)} distinct")
        print(f"   coverage weighted by frequency: {100.0*cov/max(total,1):.1f}%   distinct words: {100.0*cov_types/max(len(c),1):.1f}%")
        unc = [(w, n) for w, n in c.most_common() if not (w in words or stem(w) in stems)][: a.top]
        print(f"   top {a.top} uncovered words:")
        print("   " + " ".join(f"{w}({n})" for w, n in unc))
        result[folder] = {"total": total, "distinct": len(c), "coverage": cov / max(total, 1),
                          "coverage_distinct": cov_types / max(len(c), 1), "uncovered": unc}
    if a.json:
        json.dump(result, open(a.json, "w"), ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
