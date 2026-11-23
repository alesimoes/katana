# Curriculum v6 (generated 2026-11-22)

Training data for the Qwen3.6-35B-A3B specialization, made by `katana curriculum` (the scenario engine in
`src/app/scenario*.c`). Every conversation is replayed by `katana curriculum check` against the engine's real tools.

| file | what it is | conversations |
|---|---|---|
| `v6-train.jsonl.gz` | training file (raw conversations, before `katana import`) | 697 |
| `v6-validation.jsonl.gz` | held-out file made with reserved slot values (`--holdout`) | 190 |
| `generate.sh` | the exact commands that built them (pool, holdout, `pick`, `check`) | |
| `train.sh` | the training run (5 chunks of 60 steps, `--resume` between chunks) | |
| `check-*.txt`, `train-plan.log` | the generator's `check` output and the training memory plan | |

**Content of the training file.** The 520 conversations chosen by `pick` (coverage of tool sequences, cells and request
shapes) plus conversations of the new v6 families added from the pool because `pick` takes few of them: `x-find6` 90,
`x-batch` 60, `x-script` 50, `x-docs` 20 (families described in `docs/curriculum.md`, section "Version 6"). 108 conversations
call `Find`; 246 have two or more calls in one assistant turn. `check`: 0 problems in both files.

**How to use.** `gunzip -c v6-train.jsonl.gz > v6-train.jsonl`, then
`KATANA_THINK=0 katana import <model.gguf> v6-train.jsonl data.jsonl --system keep --mask-prefill` and `katana train ...`
(see `train.sh`). The imported file depends on the model's tokenizer and chat format.

**To regenerate.** The files depend on the generator's source and on the vocabulary of the model (`KATANA_VOCAB_MODEL`,
used by the `x-docs` family): a later binary will not give the same bytes. They were built from the source at commit
`440f666` (the `./katana` of that moment, copied to `/mnt/dados/cu-v6/k6`, which is not in git). `generate.sh` needs
`k6` replaced by a binary of that commit to reproduce them.

**Known mismatch.** After generation the system prompt wording for `Find` was changed (commit `b56c616`: "searches the project
for what you describe, matching words, names and the links between files" instead of "by meaning"). The 12% of conversations
made in `real` prompt mode carry the old wording; the `blocks` mode (88%) does not contain it.

**Not verified.** Nothing shows yet that a model trained on this data uses `Find`, groups calls or writes scripts. The
training run (`train.sh`) was the first of the real Qwen3.6 and had not finished when this file was written.

## Training result (2026-11-22 22:09 to 2026-11-23 05:48)

First training of the real Qwen3.6-35B-A3B in this project. 5 chunks of 60 steps, batch 4 (1,200 windows, about 0.7 epoch),
rank-8 adapters on the attention matrices (5.74 M trainable weights), constant learning rate 1e-4, no validation.
About 90 s per step (about 1h30 per chunk, 7h39 in total). The final file is `v6.kspec` (23 MB, marked
`--no-tool-decl`: the agent leaves the tools section out of the prompt when it is applied). `train.log` is the full log.
The per-chunk files and the optimizer state (`.kopt`) are in the backup `~/.katana/curriculum-backup/`, not in git.

| chunk | mean training loss over its 60 steps |
|---|---|
| 1 | 1.4936 |
| 2 | 0.9902 |
| 3 | 0.9320 |
| 4 | 0.7693 |
| 5 | 0.9654 |

The loss fell from the first chunk to the fourth and rose again in the fifth; single-step values are very noisy
(0.25 to 1.5 inside one chunk) because each step draws different windows. There was no validation, so how the model
behaves on conversations it has not seen is unknown, and **it was not tested whether it uses `Find`, groups calls or writes
scripts.** Use: `KATANA_SPEC=<path>/v6.kspec katana agent <model.gguf>`; `katana merge <model.gguf> v6.kspec <out.gguf>`
writes a GGUF with it inside.
