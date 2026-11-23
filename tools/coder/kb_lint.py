#!/usr/bin/env python3
"""Validate data/coder/kb/concepts/*.kb: required fields, unique IDs, weights in 0..1,
existing related targets, duplicate terms inside one concept.

Usage: kb_lint.py [DIR_OR_FILES ...]   (default: data/coder/kb/concepts)
Exit code 1 when errors exist. Prints per-file counts of concepts and terms.
"""
import os, re, sys

ID_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
CANON_RE = re.compile(r"^[a-z][a-z0-9_]*$")
LANGS = {"pt", "en", "code"}
CATS = {"BUSINESS_OPERATION", "BUSINESS_ENTITY", "SECURITY", "DATA", "NETWORK", "CONCURRENCY", "UI",
        "SYSTEM", "BUILD_TOOLING", "ERROR_HANDLING", "ALGORITHM", "MEDIA", "AI_ML", "MOBILE"}


def new(cid, loc):
    return {"id": cid, "loc": loc, "terms": set(), "cat": 0, "canon": 0, "src": 0, "nterms": 0, "rel": []}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    args = sys.argv[1:] or [os.path.join(here, "..", "..", "data", "coder", "kb", "concepts")]
    files = []
    for a in args:
        if os.path.isdir(a):
            files += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.endswith(".kb")]
        else:
            files.append(a)
    errors, warns = [], []
    ids = {}
    concepts = []
    stats = {}
    cur = None
    for fn in files:
        nc = nt = 0
        for ln, raw in enumerate(open(fn, encoding="utf-8"), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            kw = p[0]
            loc = f"{os.path.basename(fn)}:{ln}"
            if kw == "concept":
                if cur is not None:
                    errors.append(f"{loc}: concept starts before 'end' of {cur['id']}")
                if len(p) != 2 or not ID_RE.match(p[1]):
                    errors.append(f"{loc}: bad concept ID")
                    cur = new("?", loc)
                    continue
                if p[1] in ids:
                    errors.append(f"{loc}: duplicate ID {p[1]} (first at {ids[p[1]]})")
                ids[p[1]] = loc
                cur = new(p[1], loc)
                nc += 1
            elif kw == "end":
                if cur is None:
                    errors.append(f"{loc}: 'end' without concept")
                    continue
                for f, name in (("cat", "category"), ("canon", "canonical"), ("src", "source")):
                    if not cur[f]:
                        errors.append(f"{cur['loc']}: {cur['id']} missing {name}")
                if cur["nterms"] == 0:
                    errors.append(f"{cur['loc']}: {cur['id']} has no terms")
                elif cur["nterms"] < 6:
                    warns.append(f"{cur['loc']}: {cur['id']} has only {cur['nterms']} terms")
                concepts.append((fn, cur["loc"], cur["id"], cur["rel"]))
                cur = None
            elif cur is None:
                errors.append(f"{loc}: '{kw}' outside concept")
            elif kw == "category":
                if len(p) != 2 or p[1] not in CATS:
                    warns.append(f"{loc}: unusual category {p[1:]}")
                cur["cat"] += 1
            elif kw == "canonical":
                if len(p) != 2 or not CANON_RE.match(p[1]):
                    errors.append(f"{loc}: canonical must be snake_case")
                cur["canon"] += 1
            elif kw == "term":
                if len(p) < 4 or p[1] not in LANGS:
                    errors.append(f"{loc}: bad term line")
                    continue
                try:
                    w = float(p[-1])
                    if not 0.0 <= w <= 1.0:
                        raise ValueError
                except ValueError:
                    errors.append(f"{loc}: weight not in 0..1")
                key = (p[1], " ".join(p[2:-1]).lower())
                if key in cur["terms"]:
                    errors.append(f"{loc}: duplicate term {key[1]!r} ({key[0]}) in {cur['id']}")
                cur["terms"].add(key)
                cur["nterms"] += 1
                nt += 1
            elif kw == "action":
                if len(p) != 2:
                    errors.append(f"{loc}: action takes one word")
            elif kw == "related":
                try:
                    assert len(p) == 3 and 0.0 <= float(p[2]) <= 1.0
                except Exception:
                    errors.append(f"{loc}: bad related line")
                    continue
                if p[1] == cur["id"]:
                    errors.append(f"{loc}: self related")
                cur["rel"].append((p[1], loc))
            elif kw == "source":
                cur["src"] += 1
            elif kw == "note":
                pass
            else:
                errors.append(f"{loc}: unknown keyword {kw}")
        if cur is not None:
            errors.append(f"{fn}: unterminated concept {cur['id']}")
            cur = None
        stats[os.path.basename(fn)] = (nc, nt)
    for fn, loc, cid, rel in concepts:
        for target, rl in rel:
            if target not in ids:
                errors.append(f"{rl}: related {target} does not exist")
    for k, (c, t) in stats.items():
        print(f"{k:16s} concepts={c:4d} terms={t:5d}")
    print(f"{'TOTAL':16s} concepts={sum(c for c, _ in stats.values()):4d} terms={sum(t for _, t in stats.values()):5d}")
    for w in warns[:50]:
        print("warning:", w)
    if len(warns) > 50:
        print(f"... {len(warns) - 50} more warnings")
    for e in errors[:200]:
        print("ERROR:", e)
    print(f"{len(errors)} errors, {len(warns)} warnings")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
