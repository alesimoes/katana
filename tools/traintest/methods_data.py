#!/usr/bin/env python3
"""Synthetic data for tools/traintest/methods.sh (comparison of fine-tuning methods on a tiny model).

  pretrain.jsonl  general text (random sentences from a small word list): the tiny base is first trained on it, so that
                  it HAS something to forget
  train.jsonl     the task to learn: "repeat <word>" answers, and "open <word>" answers with a tool call that copies
                  the word into its arguments (source "chat" and "tools")
  val.jsonl       same task, other words: used inside the training run (choice between raw and averaged weights)
  test.jsonl      same task, other words again: the held-out set of the comparison (never seen by a training run)
  drift.jsonl     general text, other sentences than pretrain.jsonl: measures forgetting (loss and KL from the base)

usage: methods_data.py OUTDIR
"""
import json, random, sys

out = sys.argv[1]
SYS = "<|im_start|>system\nYou are katana.<|im_end|>\n"
SYL = ["ka", "ta", "na", "ro", "mi", "ze", "lu", "po", "fe", "di", "su", "bo"]
WORDS = ["the", "sun", "river", "stone", "walks", "quiet", "over", "blue", "hills", "and", "a", "small", "bird", "sings", "of", "rain",
         "slow", "wind", "moves", "across", "green", "field", "while", "children", "play", "near", "old", "bridge"]


def word(r):
    return "".join(r.choice(SYL) for _ in range(3))


def task(r, n):
    rows = []
    for _ in range(n):
        w = word(r)
        if r.random() < 0.5:
            pre = SYS + "<|im_start|>user\nrepeat " + w + "<|im_end|>\n<|im_start|>assistant\n"
            ans = w + " " + w + "<|im_end|>"
            src = "chat"
        else:
            pre = SYS + "<|im_start|>user\nopen " + w + "<|im_end|>\n<|im_start|>assistant\n"
            ans = "ok.\n<tool_call>\n{\"name\": \"Read\", \"arguments\": {\"file_path\": \"" + w + ".c\"}}\n</tool_call><|im_end|>"
            src = "tools"
        rows.append({"text": pre + ans, "train_spans": [[len(pre), len(pre + ans)]], "source": src})
    return rows


def general(r, n):
    rows = []
    for _ in range(n):
        s = " ".join(r.choice(WORDS) for _ in range(r.randint(18, 30))) + "."
        rows.append({"text": s})
    return rows


def dump(name, rows):
    with open(out + "/" + name, "w") as f:
        f.write("\n".join(json.dumps(x) for x in rows) + "\n")


dump("pretrain.jsonl", general(random.Random(10), 600))
dump("drift.jsonl", general(random.Random(11), 40))
dump("train.jsonl", task(random.Random(1), 96))
dump("val.jsonl", task(random.Random(2), 40))
dump("test.jsonl", task(random.Random(3), 100))
