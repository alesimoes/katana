#!/usr/bin/env python3
"""Table for tools/traintest/methods.sh: mean and sample standard deviation over seeds, per method.

usage: methods_table.py results.txt      (lines: "<method> {json from katana compare}", base row first of each method)
Columns: held-out loss, loss on tool-call tokens, drift loss minus base drift loss (forgetting, lower is better),
mean KL from the base on the drift file (lower is less change), and how many seeds beat the plain LoRA run of the same seed.
"""
import json, statistics, sys
from collections import OrderedDict

rows = OrderedDict()
base = None
for line in open(sys.argv[1]):
    name, js = line.split(" ", 1)
    r = json.loads(js)
    if r["spec"] == "base":
        base = r
        continue
    rows.setdefault(name, []).append(r)


def ms(xs):
    return "%.3f ± %.3f" % (statistics.mean(xs), statistics.stdev(xs) if len(xs) > 1 else 0.0)


print("base (before fine-tuning): held-out %.3f, tool tokens %.3f, drift %.3f" % (base["val"], base["tool"], base["drift"]))
print()
print("| method | held-out loss | tool-call tokens | drift loss - base | KL(base,spec) | seeds better than LoRA (held-out) |")
print("|---|---|---|---|---|---|")
ref = rows.get("lora")
for name, rs in rows.items():
    better = "-"
    if ref and name != "lora" and len(ref) == len(rs):
        better = "%d/%d" % (sum(a["val"] < b["val"] for a, b in zip(rs, ref)), len(rs))
    print("| %s | %s | %s | %s | %s | %s |" % (name, ms([r["val"] for r in rs]), ms([r["tool"] for r in rs]),
                                              ms([r["drift"] - base["drift"] for r in rs]), ms([r["kl"] for r in rs]), better))
