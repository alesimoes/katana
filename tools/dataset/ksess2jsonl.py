#!/usr/bin/env python3
"""ksess2jsonl: turn katana agent sessions (.ksess) into training data (JSONL).

Each .ksess holds the whole conversation of one agent session. Files written by a katana with the KSX1 block
(src/agent/loop.c, sess_wext) also hold the system prompt, the exact text of the whole conversation in the model's
own chat format, and where each message sits in that text. That is what a fine-tuning run needs: the text the model
really reads and writes, tool calls in the exact format the agent parses, and the spans to train on (the model's
own turns plus the token that ends each turn).

  ksess2jsonl.py SESSIONS... -o out.jsonl                   one JSON line per session
  ksess2jsonl.py DIR --bench results.jsonl --only-passed    keep only runs whose hidden tests passed (codebench)
  ksess2jsonl.py DIR --format messages                      role/content lists, for trainers that apply a template
  ksess2jsonl.py DIR --stats                                counts only

Output (--format text, the default): {"text", "train_spans": [[start, end], ...] (character offsets into text),
"system", "messages", "arch", "exact", "saved_at", "source", plus the codebench fields when --bench is given}.
To train only on the model's turns, tokenize "text" with offsets and set the label of every token outside
train_spans to -100. "exact" is false when the agent compacted its context during the session: the model then saw
summaries instead of part of the conversation, so the full text is not exactly what it was conditioned on.

Sessions also contain whatever the tools returned (file contents, command output) and the agent's memory inside
the system prompt. Read the data before sharing it; --redact-home replaces the home folder path.
"""
import argparse, glob, json, os, struct, sys

SESS_MAGIC = 0x3153534553544B4B
EXT_MAGIC = 0x3158534B                     # "KSX1"
ROLES = {"u": "user", "a": "assistant", "t": "tool"}
# token that ends a model turn, as it appears in the text right after the turn (tool call or final answer)
STOPS = ("<|im_end|>", "<|tool_response>", "<turn|>", "<|observation|>", "<|user|>")


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def take(self, n):
        if self.p + n > len(self.b):
            raise ValueError("truncated file")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def i32(self): return struct.unpack("<i", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def i64(self): return struct.unpack("<q", self.take(8))[0]

    def s(self):
        n = self.i32()
        if n < 0 or n > (1 << 26):
            raise ValueError("bad string length")
        return self.take(n).decode("utf-8", "replace")

    def msgs(self):
        n = self.i32()
        if n < 0 or n > 1000000:
            raise ValueError("bad message count")
        out = []
        for _ in range(n):
            r = chr(self.take(1)[0])
            out.append((r, self.s()))
        return out

    def left(self): return len(self.b) - self.p


def read_ksess(path):
    r = Reader(open(path, "rb").read())
    if r.u64() != SESS_MAGIC:
        raise ValueError("not a katana session")
    r.u64()                                 # model fingerprint
    saved = r.i64()
    nl, ne = r.i32(), r.i32()
    r.take(4 * nl * ne)                     # expert heat
    compact = r.msgs()
    nid = r.i32()
    r.take(4 * nid)                         # token ids of the compact conversation
    full = r.msgs()
    d = dict(source=path, saved_at=saved, messages=full, compact_n=len(compact), ext=False)
    if r.left() >= 4 and r.u32() == EXT_MAGIC:
        d["arch"] = r.s()
        d["chat_fmt"] = r.i32()
        d["think"] = r.i32()
        d["exact"] = bool(r.i32())
        d["system"] = r.s()
        n = r.i32()
        if n < 0 or n > (1 << 26):
            raise ValueError("bad string length")
        raw = r.take(n)                     # the offsets below are byte offsets into this text
        d["text"] = raw.decode("utf-8", "replace")
        n = r.i32()
        d["spans"] = []
        for _ in range(n):
            role = chr(r.take(1)[0])
            a, b = r.i32(), r.i32()
            # byte offset -> character offset of the decoded text (they differ as soon as there is any non-ASCII text)
            ca = len(raw[:a].decode("utf-8", "replace")) if a >= 0 else a
            cb = len(raw[:b].decode("utf-8", "replace")) if b >= 0 else b
            d["spans"].append((role, ca, cb))
        d["ext"] = True
    return d


def eot_for(text):
    if "<|turn>" in text:
        return "<turn|>"
    if "<|assistant|>" in text:
        return "<|user|>"
    return "<|im_end|>"


def train_spans(d):
    """model turns plus the stop token that follows each one; the last turn gets the end-of-turn token appended"""
    text, out = d["text"], []
    spans = d["spans"]
    for i, (role, a, b) in enumerate(spans):
        if role != "a" or a < 0:
            continue
        end = b
        for st in STOPS:
            if text.startswith(st, b):
                end = b + len(st)
                break
        else:
            if i == len(spans) - 1 and b == len(text):
                text += eot_for(text)
                end = len(text)
        out.append([a, end])
    d["text"] = text
    return out


def redact(o, home):
    if isinstance(o, str):
        return o.replace(home, "~")
    if isinstance(o, list):
        return [redact(x, home) for x in o]
    if isinstance(o, dict):
        return {k: redact(v, home) for k, v in o.items()}
    return o


def load_bench(path):
    by = {}
    for line in open(path):
        if line.strip():
            r = json.loads(line)
            if r.get("session"):
                by[os.path.abspath(r["session"])] = r
                by[os.path.basename(r["session"])] = r
    return by


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="+", help=".ksess files or folders")
    ap.add_argument("-o", "--out", help="output JSONL (default: stdout)")
    ap.add_argument("--format", choices=["text", "messages"], default="text")
    ap.add_argument("--bench", help="codebench results JSONL (katana codebench run --sessions) to label each session")
    ap.add_argument("--only-passed", action="store_true", help="keep sessions whose hidden tests passed")
    ap.add_argument("--only-exact", action="store_true", help="drop sessions where the agent compacted its context")
    ap.add_argument("--redact-home", action="store_true")
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()

    files = []
    for x in a.inputs:
        files += sorted(glob.glob(os.path.join(x, "*.ksess"))) if os.path.isdir(x) else [x]
    bench = load_bench(a.bench) if a.bench else {}
    home = os.path.expanduser("~")
    out = open(a.out, "w") if a.out and not a.stats else sys.stdout
    st = dict(read=0, written=0, failed=0, no_ext=0, not_exact=0, not_passed=0, no_label=0, turns=0)

    for f in files:
        try:
            d = read_ksess(f)
        except Exception as e:
            st["failed"] += 1
            print("skip %s: %s" % (f, e), file=sys.stderr)
            continue
        st["read"] += 1
        meta = bench.get(os.path.abspath(f)) or bench.get(os.path.basename(f))
        if a.bench and not meta:
            st["no_label"] += 1
        if a.only_passed and not (meta and meta.get("passed")):
            st["not_passed"] += 1
            continue
        if not d["ext"]:
            st["no_ext"] += 1
            if a.format == "text":
                print("skip %s: written before the KSX1 block (no system prompt or exact text); use --format messages"
                      % f, file=sys.stderr)
                continue
        if d["ext"] and not d["exact"]:
            st["not_exact"] += 1
            if a.only_exact:
                continue
        msgs = [{"role": ROLES.get(r, r), "content": s} for r, s in d["messages"]]
        st["turns"] += sum(1 for m in msgs if m["role"] == "assistant")
        rec = dict(source=os.path.basename(f), saved_at=d["saved_at"])
        if d["ext"]:
            rec.update(arch=d["arch"], exact=d["exact"])
        if a.format == "text":
            spans = train_spans(d)
            rec.update(text=d["text"], train_spans=spans, system=d["system"], messages=msgs)
        else:
            sysm = [{"role": "system", "content": d["system"]}] if d.get("system") else []
            rec.update(messages=sysm + msgs)
        if meta:
            rec.update({k: meta[k] for k in ("task", "passed", "attempt", "label", "seconds", "tool_calls", "timed_out")
                        if k in meta})
        if a.redact_home and home and home != "/":
            rec = redact(rec, home)
        st["written"] += 1
        if not a.stats:
            out.write(json.dumps(rec, ensure_ascii=False) + "\n")
    if out is not sys.stdout:
        out.close()
    print(" ".join("%s=%d" % kv for kv in st.items()), file=sys.stderr)
    return 0 if st["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
