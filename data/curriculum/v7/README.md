# Curriculum v7 (generated 2026-11-23)

Training data for the Qwen3.6-35B-A3B specialization, made by `katana curriculum` (the scenario engine in `src/app/scenario*.c`).
v7 is **everything v6 had, regenerated for the engine as it is now, plus five new families** that teach the model to use what the engine
gained after v6: the `Symbols` tool, the new `Find` outputs, the note for a project cited in another place, the read cursor, the repeat note
and the other `[katana: ...]` lines. Nothing is copied from the v6 files: `generate.sh` rebuilds all of it from the binary. Every
conversation is replayed by `katana curriculum check` against the engine's real tools (0 problems in both files).

| file | what it is | conversations |
|---|---|---|
| `v7-train.jsonl.gz` | training file (raw conversations, before `katana import`) | 1042 |
| `v7-validation.jsonl.gz` | held-out file made with reserved slot values (`--holdout`) | 239 |
| `generate.sh` | the exact commands that built them (pools, holdouts, `pick`, `check`) | |
| `train.sh` | the training run; chunks and steps per chunk are parameters (**not run**) | |
| `check-*.txt`, `family-counts.txt`, `variety.txt`, `dropped.txt` | outputs of `check`, counts per family, `curriculum variety`, the lines dropped by the repeatability filter | |
| `SHA256SUMS` | sha256 of the two data files and the two scripts | |

## Families and counts

Old families (v1 to v6), same weights and same recipe as v6 (`pick` of 520 by coverage from a pool of 1800, plus extra conversations of
`x-find6` 90, `x-batch` 60, `x-script` 50, `x-docs` 20): 712 conversations in the training file (v6: 697). Their share of languages is
unchanged (50% Portuguese requests about English code).

| family | train | validation | | family | train | validation |
|---|---|---|---|---|---|---|
| x-rare | 152 | 27 | | x-find6 | 90 | 11 |
| x-batch | 64 | 3 | | x-script | 60 | 8 |
| x-concept | 53 | 19 | | x-web | 34 | 7 |
| x-mcp | 33 | 13 | | x-config | 30 | 6 |
| x-docs | 21 | 11 | | x-ps1 | 19 | 7 |
| x-ability | 19 | 8 | | x-rename | 16 | 7 |
| x-clean | 16 | 7 | | x-investigate | 14 | 2 |
| x-fix | 14 | 9 | | x-explain | 14 | 1 |
| x-tools | 13 | 9 | | x-make | 13 | 21 |
| x-find | 10 | 3 | | x-add | 10 | 6 |
| x-skill | 9 | 3 | | x-run | 8 | 1 |

New families (about 10% of their requests in Portuguese, about English code):

| family | train | validation | situations |
|---|---|---|---|
| `x-symbols` | 110 | 15 | callers, def, callees, impact, impact-none, at, deps, parallel (two independent `Symbols` in one turn), outline-read, typo (low confidence), missing (NOT FOUND) |
| `x-find7` | 70 | 9 | ident (`Found by name or text`, then `mode=refs`), quoted (`Literal text found`), quoted-missing (`NOT FOUND (literal)`), typo (`NOT FOUND ... [confidence low]`, then `Symbols def`), concept, no-match (`try recall(query)`) |
| `x-place` | 60 | 7 | the `[katana: you mention ...]` note with `path` from the first call (Symbols, Find, Glob, Grep, absolute Read), did-you-mean, folder-exists, home-refusal |
| `x-cursor` | 40 | 8 | rest (`lines 1-N ... already given`), chunks, same-offset, map of a big file, repeat-switch, repeat-answer, stuck (third repeat) |
| `x-katananotes` | 50 | 11 | repeat note (Symbols, Grep, Find refs), `plan first` (three Edits, then a plan), open todo list (the model closes it and repeats the answer) |

Whole file: 183 conversations call `Symbols`, 195 call `Find`, 294 have two or more calls in one assistant turn, 125 (12%) carry the real
full prompt (`real` mode, with the current wording) and the rest the `blocks` mode. Notes the model sees in tool results or requests:
175 `symbol index created`, 42 requests with the `[katana: you mention ...]` line, 24 read-cursor notes, 27 repeat notes, 14 `plan first`,
17 open-list reminders, 8 `Did you mean`, 7 `The folder ... exists at`, 6 refusals of the home folder, 65 `NOT FOUND`, 28 `No match ... recall(query)`.
Families that start with the same tool by design: x-symbols, x-find7, x-cursor.

## What changed in format against v6

* **System prompt of every example** is the current one (`katana agentsys`): `Symbols` on the Files line, the rule that `[katana: ...]` lines
  come from the program and must be followed, "Find first; Grep only for an exact pattern; Glob only for file names by pattern". The 12%
  of v6 examples in `real` mode carried the old "by meaning" wording; here they carry the current text.
* **x-find6** (and the Find calls of x-batch, x-find7): `Find` now answers with `Found by name or text (checked in the files)` and
  `[confidence high|low]` lines for identifiers, `Literal text found` for quoted text, `NOT FOUND` with similar names, and `mode=refs` is served by the symbol index
  (`Defined`, `Used N times in M files`, `line (in function)`). The generator drops the cells where the real output no longer gives what the policy
  expects: the shell stack of x-find6 loses most of its `refs` cells (the symbol index does not see shell calls). Two of its own "say" lines
  mentioned searching "by meaning" and were reworded.
* **Every family**: the repeat control of the agent loop is now part of the generator (`rr_check`/`rr_after`/`rr_reset` in `x_run`/`x_user`):
  an identical read repeated in one request returns the engine's repeat note, a tool that changes files clears it, and the `Read` cursor
  starts over at each request. In the old families this changed a handful of conversations only (they almost never repeat a read).
* **Tool outputs**: all of them come from the engine at generation time (nothing is written by hand in any family), so every family also
  carries today's `Read` map of a big file, `Did you mean` for missing paths and the home-folder refusal wherever its policy meets them.
* **Language** of the old families is unchanged; the new ones are 10% Portuguese.

## How to generate

```
sh data/curriculum/v7/generate.sh <katana binary> <work folder>
```
The files depend on the generator's source, on the engine's prompt and tools, and on the vocabulary of the model (`KATANA_VOCAB_MODEL`,
used by `x-docs`): a binary of another commit will not give the same bytes. These files were built from the binary of the commit that
contains this README. The `x-place` family searches the disks of the machine for the project folder, so it needs a machine where the
random project names do not exist elsewhere (the generator drops a scenario whose note points to another place).
`generate.sh` drops the lines that `check` reports as not repeatable (a real `Grep` listing two files written in the same instant);
`dropped.txt` lists them (1 of the validation file, none in the training file). Checks: `check-train.txt`, `check-validation.txt`.

## How to train

`gunzip -c v7-train.jsonl.gz > v7-train.jsonl`, then `KATANA_THINK=0 katana import <model.gguf> v7-train.jsonl data.jsonl --system keep --mask-prefill`
and `katana train ...` as in `train.sh`: `sh train.sh [chunks] [steps per chunk]` (default 5 x 60, as v6; `--train 'blk.*.attn_*' --rank 8
--seq 512 --lr 1e-4 --sched const --batch 4 --no-tool-decl --resume`). The file is marked `--no-tool-decl`: the tools section is left out
of the prompt, so the model learns the names and arguments of `Symbols` and `Find` from the data. The training was **not run**.

## Not verified

No model was run, trained or measured. Nothing shows that a model trained on this data uses `Symbols`, follows the notes or stops repeating
a call. The projects are tiny and calls are matched by name only; the scenarios show the tools working. Wrong calls that the engine
answers with a note (a repeated read, a `Find` in the home folder, a path that does not exist) are in the data as context only (not trained);
the model's own mistakes would look different. `recall` appears only in the text of the `No match` output: no conversation calls it.
