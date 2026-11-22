#!/usr/bin/env python3
"""opt_compare.py: compares trainer options on the tiny model at EQUAL trained tokens, 3 seeds each.

  opt_compare.py <katana> <tiny.gguf> <train.jsonl> <val.jsonl> <budget_tokens> name1="--opt a --opt b" name2="..." ...

For every config it first asks `--plan 1` for the mean window length, sets --steps so that steps * batch * mean_len is the
token budget, trains with seeds 1..3 and prints the final validation loss (mean and spread) plus loss at the intermediate
checkpoints. The validation set is always cut with fixed windows, so it is identical across configs.
Common flags go in COMMON (env), e.g. COMMON="--rank 8 --train blk.*.attn_*  --seq 128".
"""
import concurrent.futures as cf, os, re, statistics as st, subprocess, sys

K, M, TR, VA, BUD = sys.argv[1:6]
budget = int(BUD)
common = os.environ.get("COMMON", "--rank 8 --train blk.*.attn_* --seq 128 --batch 4").split()
seeds = [int(x) for x in os.environ.get("SEEDS", "1 2 3").split()]
env = dict(os.environ, KATANA_TRAIN_GPU="0")
jobs = [a.split("=", 1) for a in sys.argv[6:]]


def run(args):
    p = subprocess.run([K, "train", M, TR, "--val-data", VA] + args, capture_output=True, text=True, env=env)
    return p.stdout


def mean_len(args):
    out = run(args + ["--plan", "1", "--seed", "1"])
    m = re.search(r"\(([\d.]+) alvos e ([\d.]+) tokens por janela", out)
    return float(m.group(2))


def one(name, opts, seed):
    a = common + opts.split()
    bi = a.index("--batch") + 1 if "--batch" in a else None
    batch = int(a[bi]) if bi else 1
    ml = mean_len(a)
    steps = max(10, round(budget / (batch * ml)))
    ev = max(1, steps // 5)
    t0 = __import__("time").time()
    out = run(a + ["--steps", str(steps), "--eval", str(ev), "--seed", str(seed)])
    el = __import__("time").time() - t0
    vals = [(int(s), float(l), int(t)) for s, l, t in re.findall(r"validacao  passo +(\d+)  perda ([\d.]+) em \d+ alvos  tokens (\d+)", out)]
    return name, seed, steps, vals, el


res = {}
with cf.ThreadPoolExecutor(3) as ex:
    futs = [ex.submit(one, n, o, s) for n, o in jobs for s in seeds]
    for f in futs:
        n, s, steps, vals, el = f.result()
        res.setdefault(n, []).append((s, steps, vals, el))
print("%-28s %6s  %s   %s" % ("config", "steps", "val loss at 20/40/60/80/100% of the token budget (mean over seeds)", "final mean +- sd  [per seed]"))
for n, _ in jobs:
    rs = res[n]
    if any(len(r[2]) < 5 for r in rs):
        print(n, "FAILED", rs[0][2]); continue
    cps = [st.mean(r[2][i][1] for r in rs) for i in range(5)]
    fin = [r[2][-1][1] for r in rs]
    print("%-28s %6d  %s   %.4f +- %.4f  [%s]" % (n, rs[0][1], " ".join("%.3f" % c for c in cps), st.mean(fin),
          st.pstdev(fin), " ".join("%.3f" % f for f in fin)))
