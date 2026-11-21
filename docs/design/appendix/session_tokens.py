#!/usr/bin/env python3
"""Token accounting of katana agent sessions (.ksess), read-only.

Stand-in for the proposed `katana tokens` (docs/design/01-token-budget.md, section 4): it reads the KSX1 block
written by sess_wext (src/agent/loop.c) and prints where the bytes of a session went. Tokens are bytes divided by
a ratio measured on the file itself (system prompt + compact history bytes over the stored token ids).

usage: session_tokens.py <file.ksess | folder> [...] [--tsv]
With --tsv: one line per session (for the experiments of docs/design/90-roadmap.md).
"""
import collections, os, re, struct, sys

FAIL = re.compile(r'\]\s*error|^error|[Ee]xit code [1-9]', re.M)
WRAP = re.compile(r'<tool_response>\s*\[resultado da ferramenta [^\]]*\]\s*')


def load(path):
    b = open(path, 'rb').read()
    o = [24]

    def u32():
        v = struct.unpack_from('<i', b, o[0])[0]; o[0] += 4; return v

    def s():
        n = u32(); v = b[o[0]:o[0] + n]; o[0] += n; return v

    def msgs():
        return [(chr(b[o[0]]), (o.__setitem__(0, o[0] + 1), s())[1]) for _ in range(u32())]

    nl, ne = u32(), u32(); o[0] += nl * ne * 4
    compact = msgs(); nids = u32(); o[0] += nids * 4
    full = msgs()
    if b[o[0]:o[0] + 4] != b'KSX1':
        return None
    o[0] += 4; s(); o[0] += 12
    sysp = s()
    cbytes = sum(len(m) for _, m in compact)
    ratio = (len(sysp) + cbytes) / nids if nids else 3.4
    return sysp, full, ratio


def calls_of(text):
    out = []
    for c in re.findall(r'<tool_call>(.*?)</tool_call>', text, re.S):
        m = re.search(r'<function=([\w.\-]+)', c) or re.search(r'"name"\s*:\s*"([^"]+)"', c)
        vals = re.findall(r'<parameter=[^>]*>\n?(.*?)\n?</parameter>', c, re.S)
        out.append((m.group(1) if m else '?', len(c.encode()) + 23, sum(len(v.encode()) for v in vals), re.sub(r'\s+', ' ', c)))
    return out


def account(path):
    got = load(path)
    if not got:
        return None
    sysp, full, ratio = got
    r = collections.Counter(); by_call = collections.Counter(); n_call = collections.Counter()
    by_res = collections.Counter(); n_res = collections.Counter()
    seen = collections.Counter(); last = []
    for role, raw in full:
        t = raw.decode('utf8', 'replace')
        if role == 'u':
            r['requests'] += 1; r['user'] += len(raw)
        elif role == 'a':
            r['turns'] += 1; r['written'] += len(raw)
            last = calls_of(t)
            rest = re.sub(r'<think>.*?</think>', '', t, flags=re.S)
            rest = re.sub(r'<tool_call>.*?</tool_call>', '', rest, flags=re.S).strip()
            if rest:
                r['text_turns'] += 1; r['text'] += len(rest.encode())
            for name, size, vals, sig in last:
                r['calls'] += 1; r['call_bytes'] += size; r['framing'] += size - vals
                by_call[name] += size; n_call[name] += 1
                if seen[sig]:
                    r['repeat_calls'] += 1
                seen[sig] += 1
        else:
            r['results'] += 1; r['read'] += len(raw)
            name = last[0][0] if len(last) == 1 else 'several'
            by_res[name] += len(raw); n_res[name] += 1
            m = WRAP.match(t)
            if m:
                r['wrapper'] += len(m.group(0)) + 40
            if FAIL.search(t):
                r['failed'] += 1; r['failed_bytes'] += len(raw) + sum(c[1] for c in last)
            r['notes'] += len(re.findall(r'\[(?:katana|plan):', t))
    r['system'] = len(sysp)
    return r, by_call, n_call, by_res, n_res, ratio


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    tsv = '--tsv' in sys.argv
    files = []
    for a in args:
        a = os.path.expanduser(a)
        if os.path.isdir(a):
            files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith('.ksess'))
        else:
            files.append(a)
    keys = ['requests', 'turns', 'text_turns', 'calls', 'results', 'failed', 'repeat_calls', 'notes']
    if tsv:
        print('\t'.join(['session'] + keys + ['tok_written', 'tok_read', 'tok_failed', 'tok_framing', 'tok_wrapper', 'bytes_per_token']))
    for f in files:
        got = account(f)
        if not got:
            print(f'{f}: no KSX1 block', file=sys.stderr); continue
        r, by_call, n_call, by_res, n_res, ratio = got
        tk = lambda x: int(x / ratio)
        if tsv:
            print('\t'.join([os.path.basename(f)] + [str(r[k]) for k in keys] +
                            [str(tk(r['written'])), str(tk(r['read'])), str(tk(r['failed_bytes'])), str(tk(r['framing'])),
                             str(tk(r['wrapper'])), f'{ratio:.2f}']))
            continue
        print(f'== {f}  ({ratio:.2f} bytes per token, measured on this file)')
        print(f"requests {r['requests']}  model turns {r['turns']} ({r['text_turns']} with text)  calls {r['calls']}  "
              f"results {r['results']} ({r['failed']} failed)  repeated calls {r['repeat_calls']}  engine notes {r['notes']}")
        print(f"tokens: system {tk(r['system'])}  user {tk(r['user'])}  written {tk(r['written'])} "
              f"(calls {tk(r['call_bytes'])}, framing {tk(r['framing'])}, text {tk(r['text'])})  "
              f"read {tk(r['read'])} (wrapper {tk(r['wrapper'])})  failed rounds {tk(r['failed_bytes'])}")
        print(f"model time at 40 ms written, 15 ms read: {(tk(r['written']) * 0.040 + tk(r['read']) * 0.015) / 60:.1f} min")
        print('  tool                 calls  tok written  per call | results  tok read  per result')
        for name in sorted(set(by_call) | set(by_res), key=lambda k: -(by_call[k] + by_res[k])):
            c, n = by_call[name], n_call[name]; rr, m = by_res[name], n_res[name]
            print(f'  {name:20s} {n:5d}  {tk(c):11d}  {tk(c / n) if n else 0:8d} | {m:7d}  {tk(rr):8d}  {tk(rr / m) if m else 0:10d}')


if __name__ == '__main__':
    main()
