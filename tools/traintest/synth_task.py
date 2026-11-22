#!/usr/bin/env python3
"""synth_task.py: convergent synthetic task for the tiny models (used to compare trainer options, see docs/specialization.md).

Each conversation has several turns. A turn is a user line with random filler (context only) holding a short word,
then the assistant line that has to repeat the word twice (the only trained span). Filler length varies, so a fixed
window cut puts few targets in some windows. Usage: synth_task.py out.jsonl N [seed] [turns] [filler_max]
"""
import json, random, sys
out, n = sys.argv[1], int(sys.argv[2])
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
turns = int(sys.argv[4]) if len(sys.argv) > 4 else 4
fmax = int(sys.argv[5]) if len(sys.argv) > 5 else 120
random.seed(seed)
syl = ["ka", "ta", "na", "ro", "mi", "ze", "lu", "po", "fe", "di", "su", "bo"]
al = "abcdefghijklmnopqrstuvwxyz0123456789 "
rows = []
for _ in range(n):
    text = "<|im_start|>system\nYou are katana.<|im_end|>\n"
    spans = []
    for _t in range(random.randint(2, turns)):
        w = "".join(random.choice(syl) for _ in range(2))
        fill = "".join(random.choice(al) for _ in range(random.randint(10, fmax)))
        text += "<|im_start|>user\nword " + w + " note " + fill + "<|im_end|>\n<|im_start|>assistant\n"
        a = len(text)
        text += w + " " + w + "<|im_end|>\n"
        spans.append([a, len(text) - 1])
    rows.append({"text": text, "train_spans": spans})
open(out, "w").write("\n".join(json.dumps(r) for r in rows) + "\n")
