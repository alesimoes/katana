# Specialization: katana's own training engine

Goal set by the owner (2026-11-20): katana specializes models itself, with no external tool or package. Techniques
are fair game (backpropagation, AdamW, low-rank adapters are knowledge, not dependencies); the code is ours, in the
same C binary that runs and serves the model.

## What makes it different

1. **One binary does everything.** The engine that serves a model also trains it: `katana train` writes a
   specialization, `KATANA_SPEC` loads it, `katana serve` answers with it. No Python, no framework, no conversion step.
2. **The frozen base stays quantized.** Products with frozen weights, forward and backward (the transposed product
   that carries the gradient through a frozen layer), read each row straight from the GGUF format (Q8_0, Q4_K, Q6_K,
   ...), dequantized on the fly. Only what is trained gets a float master, a gradient and two AdamW moments (16 bytes
   per trained weight). Memory = the quantized model + the trained state + the activations of one sequence.
3. **Specializations are small files on top of an untouched base.** A `.kspec` holds only the trained tensors (whole,
   or as low-rank adapters A, B). The base GGUF is never rewritten; the file checks that it was trained on the same
   base (file size and parameter count) and refuses another.
4. **The data comes from the agent's own verified work.** Sessions record the exact text the model read and wrote in
   its own chat format; codebench says which sessions solved their task (`docs/training-data.md`). Training only on
   the model's turns of passed sessions turns "what worked" into "what the model does by default", in the exact tool
   format the agent parses.

5. **Heat-guided specialization of mixture-of-experts models** (`--experts N`): before training, the base model's
   router is run over the training data and, in each MoE layer, the N experts it picks most often at the positions
   that count for the loss become trainable (whole, or with adapters under `--rank`), together with the router.
   A small fraction of the experts carries the specialization; they are the experts that topic routes to anyway.

Points 1 to 5 are implemented. The next ones are the plan (below), in order.

6. **Specializations per request** (next): the server keeps one base in memory and applies the adapters of the
   specialization named in the request's `model` field, unmerged (`W x + s B (A x)`), so one machine serves many
   specialists without reloading.

## One specialized model (2026-11-21)

Owner's goal: take a ready model and specialize it, inside the engine, with katana's tools and with the knowledge
kept about the owner's projects, into ONE specialized model. `katana specialize` does it end to end, in C, in the
same binary:

```
katana specialize <model.gguf> [all|tools|code|teacher|knowledge] [--from DIR] [--sessions DIR] [--corpus FILE]
    [--mix tools=20,code=25,teacher=15,knowledge=15,conventions=25] [--curriculum FILE] [--out DIR] [--fresh] [--hours H] [--steps N] [--rank R]
    [--seq T] [--lr X] [--margin-mb MB] [--replace REGEX=TEXT]... [--cut-matching REGEX] [--verified]
    [--plan-updates] [--dry-run] [--no-check]
    [--knowledge DIR] [--no-refresh] [--training-dir DIR] [--wait-idle [HOURS]]
katana specialize --status [--training-dir DIR]
katana specialize --verify-data data.jsonl
```

The nightly use is `katana specialize <model>` alone: see "The nightly routine" below.

- **`all` (the default kind)** builds one data set from every source given, each source selected with the rules of
  its own kind (below), and trains one specialization, `katana.kspec`. This is the supported product: adapters
  trained separately, each from the base, and summed at load are not something either training saw.
- **The mix.** The budget is a few hundred windows of 512 tokens per night, so the share of each source is explicit:
  `--mix tools=20,code=25,teacher=15,knowledge=15,conventions=25` (percent of the training windows; default that split,
  renormalised over the sources that have data; a source with weight 0 or not listed is left out). Every data line
  carries its `"source"`; `katana train --mix` draws each step's window from the source that is furthest behind its
  share (exact to one window) and prints the realised counts (`mistura realizada: ...`).
- **Nights add up.** Unless `--fresh`, `specialize` continues from the newest `.kspec` of the same kind in the output
  root (`~/.katana/training/<kind>-*/`), through `katana train --resume FILE`: the trained tensors and adapters are the
  starting point; the AdamW moments are not stored in the file, so the optimizer state restarts (and the warm-up runs
  again). Each night appends one line to `<root>/history.jsonl`: date, kind, steps, hours, mean loss of the first and
  last 10 steps, the `.kspec` it resumed from, and conversations, turns and characters per source.
- **The flag instead of the file name.** A specialization trained with the short system prompt (no tools section) is
  marked inside the `.kspec` (`katana train --no-tool-decl`; trailer `KSXF` + flags at the end of the file, which
  older readers ignore and older files lack). When it is applied, the agent leaves the tools section out. The old
  rule (a file named `tools.kspec`) stays as a fallback for files written before the flag.
- **The final product is a GGUF.** `katana merge <model.gguf> katana.kspec <out.gguf>` writes the specialization into
  a new GGUF and carries the flag as `katana.no_tool_decl = "1"`; the loader reads it, so the merged model also runs
  without the tools section.
- Common steps: the agent's own system prompt built in-process in the model's format (ChatML XML/JSON, Gemma 4,
  GLM); `katana import` of every input with key-shaped strings (8-4-4-4-12 hex) always masked plus `--replace` /
  `--cut-matching`; the memory plan (`--plan 1`), refused when activations + trainer state + `--margin-mb` (default
  6000) exceed the available memory; the time of two steps measured and the number of steps that fit in `--hours`
  (default 5, between 4 and 400 steps; `--steps N` skips the measurement); `katana train --train 'blk.*.attn_*'
  --rank 8 --seq 512 --lr 2e-4 --val 0 --final 0` on the CPU; a check that the model loads and answers with the
  result (`--no-check` skips it). Everything lands in `--out` (default `<training-dir>/<kind>-<YYYYMMDD-HHMMSS>/`):
  `sys.txt`, `data.jsonl`, `import.log`, `plan.log`, `measure.log`, `train.log`, `steps.jsonl`, `report.txt`,
  `<kind>.kspec` (`katana.kspec` for `all`). `--dry-run` stops after the data and the plan. It never activates
  anything: the end of the report says how to test (`KATANA_SPEC=...`), how to use it in the desktop (copy into
  `~/.katana/specs/`) and how to merge it.

### The sources and the separate kinds

The separate kinds stay for experiments; each one is also the rule its source follows inside `all`.

| kind | input | system prompt | what counts for the loss |
|---|---|---|---|
| `tools` | `--from DIR` (transcripts of a coding agent, a root with one folder per project is fine) and `--sessions DIR` (every `.ksess`) | without the tools section | every model turn |
| `code` (planned work, experimental) | `--from DIR`, `--sessions DIR` | full (with `all`: short) | the plan and the first action (`--train-first 2`), turns that call `TodoWrite`, `Write` or `Edit`, the final answer; conversations with 4+ calls, no streak of 4 failures |
| `teacher` | `.ksess` with a `.teacher` file in `--sessions DIR` (default `~/.katana/desktop/sessoes`) | full (with `all`: short) | the teacher's turns |
| `knowledge` | `--corpus FILE`: one `{"messages": [user, assistant]}` per line, questions about the owner's projects and their answers | without the tools section | every answer |
| `conventions` | `--curriculum FILE` (or generated, below): synthetic conversations that teach katana's own conventions (project instruction file, skills, MCP servers), each with `"source": "curriculum"` | the conversation's own, KEPT (`import --system keep`) | every assistant turn |

**System prompt per source.** `katana import --system file:F` replaces the conversation's system prompt with F; `--system keep` keeps the one in the conversation (a `file:` prompt and a kept one are never combined). `specialize` passes `file:` (today's agent prompt) for every source except `conventions`, whose scenario (project file, skills line, MCP cards) lives inside its own system message.

**The fifth source in `all`.** Without `--curriculum`, `specialize` runs `katana curriculum --help` and, if it exits 0, generates a fresh file in the output folder: `katana curriculum <out>/curriculum.jsonl --n N --seed S` with N = twice the conversations the mix gives this source in a night (the steps given, or `--hours` at about 245 s per window, an estimate; at least 20) and S = minutes since the epoch, so no night sees the same synthetic conversation twice. If the subcommand is missing it says so in one line and goes on without the source. `--mix` default: `tools=20,code=25,teacher=15,knowledge=15,conventions=25`, renormalised over the sources that have data. Tested only with a hand-written file of three conversations (the generator did not exist when this was written).
### Selection options of `katana import`

These decide what a specialization learns. They work on every input (messages JSONL, transcripts, `.ksess`).

- `--train-tools A,B,C`: only model turns that call one of these tools count; the others stay as context.
  `--train-final`: the last model turn (the answer in words) counts too. `--train-first N`: the first N model turns
  count. Without any of the three, every model turn counts.
- `--require-tools A,B`: drop conversations that never call any of them. `--min-calls N`: drop conversations with
  fewer calls.
- `--drop-failed-streak N`: drop a conversation with N or more failed tool results in a row (a result starting with
  `error:`, or with a non-zero `Exit code N` / `[exit code N]`).
- `--verified`: keep a conversation only if a `Bash` call that did not fail comes after its last `Write`/`Edit` (the
  cheapest proxy for "the code was checked"). Off by default in `code`; `specialize ... --verified` turns it on.
- `--plan-first` (experimental): for each conversation with at least 4 tool calls, a `TodoWrite` call is inserted
  before the first model turn, followed by the result the agent returns for it (`Todos updated: N tasks (0
  completed, 1 in progress, N-1 pending).`). The steps are derived from what was actually done afterwards: the
  `description` of each call (or the tool and its main argument, e.g. `Edit src/x.c`), near-duplicate consecutive
  descriptions merged, a step seen before reused, at most 8 steps (the eighth absorbs the rest), the first
  `in_progress`. `--plan-updates` also inserts a `TodoWrite` update each time the work moves to a later step. The
  `description` stays in the `Bash` arguments of the training text.
- At the end `import` prints a table: conversations kept and dropped per reason, model turns that count, characters to
  learn in total and per tool (from the calls of the counting turns), an estimate of 512-token windows (characters /
  3.3 / 512, labelled as an estimate), plans inserted, and for a folder of project folders, episodes kept per folder.

### Planned work: what the `code` source tries to teach

The local model iterates call by call; the transcripts show a model that announces what it will do and works through
it in order. The `code` source tries to teach: write the plan as a task list before the first action, keep each action
described, follow the list in order, finish with a message. Where the plan comes from is its weakness: the
transcripts contain no task-list calls at all, so the plan is derived in hindsight from the described actions; it
includes the detours and dead ends of the real work, which a real plan would not have predicted.

### What 100 to 150 windows per night can and cannot do

Measured on the real 35B model on the CPU: about 245 s per 512-token window, so one night is 100 to 150 steps. That
is the central constraint; data selection matters far more than volume.

- It can shift habits that show up in every conversation and cost few tokens: writing the plan before acting,
  reading before editing, writing the file instead of describing it, running something after writing, closing with a
  message, the tool names and argument shapes. It can also fix a few dozen facts of the knowledge corpus, if they are
  repeated.
- It cannot teach a language, a library or a codebase: a night sees well under 100k tokens.
- Planning turns: on the synthetic data of `tools/traintest` the plan turn is 320 characters (about 100 tokens,
  measured on the tiny model's data, 4 steps). Estimate for the real transcripts (not measured): with up to 8 steps
  of 5 to 10 words, 300 to 600 characters, 100 to 200 tokens. With the full system prompt (8.8k characters, about
  2.7k tokens) the plan sits in its own window, so a night that drew only `code` windows would see at most about one
  plan per window, 100 to 150 plans; with the default mix (30% `code`) about 30 to 45 per night, plus the other
  `code` turns those windows hold. With the short prompt (4.1k characters) it is the same order: one plan per window.
- Training on the GPU (the `tmm` kernel; never run on a real card) is what would change the budget.

**Nothing in this section was run on a real model.** All of it was checked only on the tiny random models of
`tools/traintest/run.sh`: the selection options, the plan insertion, the three single kinds plus `knowledge`, `all`
with a 50/30/20 mix (realised exactly: 10, 6 and 4 windows of 20), two nights where the second resumed from the first
(mean loss of the first 10 steps 12.25 on night 1, 10.02 on night 2; night 1 ended at 10.48), the flag in the
`.kspec` and in a merged GGUF (the agent's saved system prompt has no tools section in both cases, and has it with the
base model). Not measured: whether any of this changes the behaviour of Qwen3.6 or Gemma 4, the import of the owner's
841 MB of transcripts (time and memory). The waiting for an idle agent that the old shell script did is back as
`--wait-idle` (below).

### The nightly routine: one command (2026-11-21)

```
katana specialize <model.gguf>
```

With nothing else on the line, `specialize` (kind `all`) does the whole routine and every default can be overridden:

1. **Wait** (only with `--wait-idle [HOURS]`, default 6 h): while a `katana agent` process runs, wait until the newest
   `.ksess` in `~/.katana/desktop/sessoes` is older than 10 minutes and no teacher call is running, seen in 3 checks 30
   s apart (the rule of the old shell script; `KATANA_IDLE_SECS` and `KATANA_IDLE_POLL` change the 600 s and the 30
   s, for tests). No agent running: go at once. Over the limit: give up with exit code 2, nothing trained. The check
   is a proxy (the session file's modification time), not a look at the agent's output.
2. **Sources.** Transcripts: `--from DIR`, else `transcripts=DIR` in the config file; there is no built-in default
   path, and with neither the run says so and continues with the other sources. The agent's own sessions:
   `--sessions DIR`, else `~/.katana/desktop/sessoes` when it exists.
3. **Knowledge**: `katana knowledge build` runs incrementally into `~/.katana/knowledge` (`--knowledge DIR` or
   `KATANA_KNOWLEDGE`), then `katana knowledge corpus` regenerates `<training-dir>/corpus.jsonl` and
   `<training-dir>/holdout.jsonl` from the notes. `--corpus FILE` uses that file as it is; `--no-refresh` skips both
   steps and uses the existing `corpus.jsonl`. With `--dry-run` the notes, corpus and holdout go inside `--out`, so a
   dry run touches nothing outside it.
4. **Data, memory, training**: as below. The data file is checked (next section) before anything is trained. The
   training draws its windows with `--seed <night number>`: before this, every night used the default seed and so
   shuffled the windows the same way (the same first windows each night, given the same data).
5. **Report** (also with `katana specialize --status`): which night this is; windows trained so far per source against
   the windows that exist per source (the fraction of the acervo the model has seen); the loss at the start and end
   of each night; the exact commands to test the result and to activate it in the desktop; the command that measures
   the knowledge (`katana knowledge quiz`, below). The windows trained and available are recorded per night in
   `history.jsonl` (`windows`, `available`, `night`, `model`). Nights draw their windows at random, so part repeats:
   the report prints the distinct coverage expected from that, `sum N (1 - exp(-n/N))` (an estimate, not measured).
   `--status` reads the history of the latest chain, from the last night that started from the base model.

**Config file.** `specialize` reads `~/.katana/katana.cfg` and then `./katana.cfg` (the latter wins for single keys;
`replace` lines add up). It is the file that also holds the GPU tuning (`gpu=`, `seg=`, `rows=`, `rowsb=`), which the
GPU code ignores lines it does not know; `katana tune` rewrites only its own four lines and keeps the others. Lines
are `key=value`; `#` starts a comment.

| key | meaning | default |
|---|---|---|
| `transcripts=DIR` | root of the coding-agent session transcripts (one folder per project) | none: no transcripts |
| `training-dir=DIR` | root of the nights (`<kind>-<date>/`, `history.jsonl`, `corpus.jsonl`, `holdout.jsonl`); `~/` is expanded | `~/.katana/training` |
| `replace=REGEX=TEXT` | repeatable; passed as `--replace` to every `import` and to `knowledge build` (paths and names of the machine) | none |
| `cut-matching=REGEX` | passed as `--cut-matching` to the same commands | none |

On the command line `--training-dir`, `--from`, `--replace` (added to the file's) and `--cut-matching` (replaces the
file's) win. `--out DIR` sets the night's folder itself; the root for resuming and the history is then its parent.

**Nothing big under `/tmp`.** Checked with `strace -f` over a whole run (specialize, import, knowledge, train, chat):
the only thing created outside the output was an empty directory for the system-prompt builder, which now follows
`TMPDIR`; `specialize` points `TMPDIR` at `<out>/tmp`. Data, logs, notes, corpus and adapters all live under the output
or the knowledge folder.

**Dry run on the real data (2026-11-21, Qwen3.6-35B-A3B Q4_K_M, `katana specialize <model> --from <transcripts root> --dry-run`, 5 min 24 s in all, run niced beside a real training and the desktop agent):** knowledge base built from scratch inside `--out` in 11 s (1270 transcripts, 529 of sub-agents skipped, 734 notes, 1483 episodes), corpus 1336 pairs and holdout 668; data in about 2 min 15 s: 4591 conversations kept (tools 2011, code 1243, teacher 1, knowledge 1336), 768 dropped, all lines checked; memory plan (about 3 min to load) 9486 MB of activations + 88 MB of trainer state + 6000 MB margin against 45859 MB available; windows of 512 tokens that exist: tools 45737, code 16701, teacher 418, knowledge 1583 (total 64439, measured by the trainer's tokenizer; the character estimate said 36912). At 100 to 150 windows a night the first night would see well under 1% of the acervo. `conventions` was 0: the generator was not in this binary. No training was run.

### Writes that fail loudly

A dry run on the real data once wrote a `data.jsonl` cut in the middle when `--out` was on a full filesystem (540
conversations missing, two invalid JSON lines, one source gone) and `specialize` said it had worked. Now:

- Every data write in `specialize`, in `katana import` and `export` (the output file and the concatenation of the per
  source files) and in `katana knowledge` (notes, markers, `INDEX.md`, corpus, holdout) goes through `io_write` /
  `io_close` (`src/app/importer.c`): each `fwrite`, the `fflush` and the `fclose` are checked; the first failure prints
  the file and `strerror(errno)` and the command exits non-zero (`import` and `export`: 4; `specialize`: 3 for a
  failed source, 1 for its own files, 8 for the history). Notes, markers, indexes, corpus and holdout are written to
  `<name>.part` and renamed only when complete, so a failure leaves the previous file as it was; a note that failed
  leaves no marker, so the next run redoes that transcript.
- `import` exit codes: 0 conversations written, 1 none kept (normal for a single session), 4 write failure.
  `specialize` accepts 1 only when the table `import_stats:` is there; any other failure stops the night.
- After the sources, `specialize` checks `data.jsonl`: every line is a complete JSON object (a strict parser, not the
  lenient one the engine uses), has `text` and `train_spans` and a known `source`; the line count (total and per
  source) equals what the sources reported kept; the last line ends with a newline. Otherwise it stops with exit
  code 7 before any training. `katana specialize --verify-data FILE` runs the shape check on any data file.
- Test hook: `KATANA_FAIL_WRITE_AFTER=N` makes the process write N bytes and then fail every write with ENOSPC (the
  last write is cut, like a real full disk). `tools/traintest/specialize_oneshot.sh` uses it with N = 200 (the parent
  fails), 40000 (an `import` child fails), on `knowledge build` and `knowledge corpus`, and also `import ... /dev/full`
  (a real ENOSPC), and a data file cut in the middle and one with invalid lines for `--verify-data`.

Not covered: the child processes' log files (`part.log`, `train.log`) are written by shell-less redirection and a full
disk there is caught only through the missing `import_stats:` line or the failed exit code; `katana train` writing the
`.kspec` is checked by its existing return value (not re-tested here with a full disk).

## Commands

```
katana gradcheck <model.gguf> [T]
katana compare <model.gguf> (--val-data F | --data F --val X --seed N) [--drift-data G] a.kspec [b.kspec ...]
katana train <model.gguf> <data.jsonl> [--train PATTERNS] [--rank R] [--alpha A] [--steps N] [--lr X]
             [--seq T] [--wd X] [--clip X] [--seed N] [--every N] [--out file.kspec] [--allow-worse]
             [--ema D] [--neft A] [--adapter-dropout P] [--dora] [--anchor L] [--label-smoothing E]
             [--tool-weight W] [--optimizer adamw|muon] [--pissa] [--spike F] [--max-skips N]
KATANA_SPEC=file.kspec katana serve|chat|agent|run <model.gguf> ...
```

- `--train`: comma-separated patterns over GGUF tensor names, `*` matches anything (`blk.*.ffn_*`,
  `blk.1*.attn_q.weight`, `output_norm.weight`, `all`). Default `all`.
- `--experts N` (MoE models): heat-guided choice of N trainable experts per layer (see point 5). Without `--train`,
  the router is trained too; with `--train`, the patterns add to the hot experts. Expert tensors are addressed as
  `blk.<l>.ffn_{gate,up,down}_exps.weight.e<expert>`.
- `--rank R`: matrices get a rank-R adapter instead of training the whole matrix (A random, B zero: training starts
  exactly at the base model); norms and the embedding still train whole. `--alpha` sets the scale `s = alpha / R`
  (default `alpha = R`).
- Data: JSONL lines with `text` and optional `train_spans` (character offsets of the parts to learn, as written by
  `tools/dataset/ksess2jsonl.py`). Without `train_spans` every token is a target. Long texts are cut into windows of
  `--seq` tokens; windows without targets are skipped. A line whose tokens do not reproduce its text byte for byte is
  skipped with a warning (the spans would not line up).
- `--plan 1`: load the model and the data, choose what trains (including the hot experts) and print the memory plan
  (activations of one window, trainer state, model size) without training. Use it first on a big model.
- `katana merge <model.gguf> <spec.kspec> <out.gguf>`: writes a new GGUF with the specialization inside (whole
  tensors replaced, adapters merged as `W + s B A`, trained experts written into their rows of the stacked tensor),
  each changed tensor in its original type when katana can encode it (F32, F16, Q8_0, Q6_K, Q5_K, Q4_K, Q2_K),
  otherwise Q8_0; everything else copied byte for byte; the file records `katana.specialization`. Checked: the merged
  GGUF answers exactly like the base with `KATANA_SPEC` on five tiny layouts (whole tensors, adapters on Q8_0, MoE hot
  experts, qwen35moe, gemma4). Gemma 4 experts come from one fused gate/up tensor; their parts are named
  `ffn_gate_up_exps.weight.eN.gate` / `.up` and go back to rows `2N·H` and `(2N+1)·H`.
- Optimizer: AdamW (betas 0.9/0.999), global gradient-norm clipping (default 1.0), weight decay only on matrices.
  Learning rate: linear warm-up (`--warmup`, default 5% of the steps, at least 10), then cosine down to 10% of `--lr`
  (`--sched cos`, default) or constant (`--sched const`).
- Batches: `--batch N` accumulates the gradients of N windows per optimizer step (each window weighted 1/N). Windows
  are drawn in a shuffled order, each once per epoch.
- Validation: `--val-data FILE`, or a fraction of the conversations (`--val F`; default 10% when the data has 20 or
  more conversations), split by whole conversation so nothing leaks into training. The held-out loss is measured every
  `--eval N` steps (default a tenth of the run); the `.kspec` written is the one from the best evaluation, and
  `--patience P` stops after P evaluations without improvement.
- `--log FILE` appends one JSON line per step and evaluation; the last line of the output, `resultado: {...}`, is a
  one-line JSON summary (output file, steps, train and validation loss, best step, conversations used) for scripts and
  the `katana learn` loop.
- Checked on a tiny model with 60 "repeat the word" conversations (50 trained, 10 held out): held-out loss 1.89 -> 0.092
  with batch 4 and the cosine schedule; with a deliberately high constant lr the run stops early and keeps the best
  step (160 of 220).
- Loading: whole tensors replace the base tensor; adapters are merged at load (`W + s B A`, dequantizing only the
  adapted matrices to float). The specialization is applied before the GPU is prepared. A specialization that
  changes experts runs on the CPU for now (the GPU expert cache assumes one format for all experts of a layer).

## Fine-tuning methods and an honest check of what training did (2026-11-22)

Code: `src/app/train_methods.c` (included by `train.c`; small hooks in the products, `t_step`, the optimizer and the
`.kspec` reader/writer). Every method is an option and **off by default**: without options training is what it was.

### Did the run make the model better or worse?

- **Baseline before step 1.** With a validation set (`--val-data` or `--val`) `katana train` first measures the loss of
  the *base* model (adapters, float masters and DoRA magnitudes switched off, `tm_off`), per source. On a fresh start it
  also checks that adapters at zero give exactly the base model (maximum logit difference must be 0; with whole trained
  tensors a 1e-3 tolerance, because the GPU sums in another order) and refuses to train if not.
- **No `.kspec` that does not beat the base.** The file is only written when a validation loss is below the baseline.
  Otherwise nothing is written, the run prints `VALIDACAO: base X; nenhum estado treinado a venceu`, exits with code 2
  and the `resultado:` line carries `"refused": true`. `--allow-worse` writes anyway. Without validation data it
  cannot tell and prints a warning. The end of every run prints `VALIDACAO: base X -> gravado Y` and the loss per source
  of both; `resultado:` has a new `base_val_loss`.
- **`katana compare <model> (--val-data F | --data F --val X --seed N) [--drift-data G] a.kspec [b.kspec ...]`**
  (also `katana train <model> --compare ...`): the base and each specialization on the same held-out windows: loss
  overall, per source and on tool-call tokens, and on a second unrelated file the loss and the mean KL(base || spec)
  of the next-token distribution (forgetting). Prints a table, a verdict per file and `compare-json:` lines.
  Use a `--val-data` that no training run used for selection.
- **Divergence guards.** A non-finite loss or gradient norm skips the step (no update); so does a loss above
  `--spike F` (default 5) times the median of the last 20 accepted steps *and* more than 2 nats above it.
  `--max-skips N` (default 5) consecutive skips stop the run. (Checks use the bit pattern: the build uses
  `-ffast-math`, which folds `isfinite`.) A guard cannot undo an update that already happened: a learning rate of 1e30
  corrupts step 1 and the guard then stops the run after 5 skips; the refusal above keeps the file from being written.

### Methods (option, reference, what we implemented)

| option | method | notes |
|---|---|---|
| `--ema D` | exponential moving average of the weights (adapters, magnitudes, whole tensors) with a warm-up of the decay | at each validation both raw and averaged weights are evaluated, the better one is saved. Averaging A and B separately is not averaging the product B A. |
| `--neft A` | NEFTune: uniform noise in [-1, 1] added to the input embeddings, scaled by `A / sqrt(L d)` (Jain et al. 2023, arXiv:2310.05914; formula read in the paper) | noise is a function of (seed, step, window): deterministic. |
| `--adapter-dropout P` | dropout on the input of the adapter product `A x` (LoRA dropout) | mask from a counter hash of (seed, step, window, tensor, row, column), same in forward and backward. |
| `--dora` | DoRA (Liu et al. 2024, arXiv:2402.09353): `y = m/‖V‖ ⊙ (W x + s B A x)` with a trained magnitude `m` per output row, `m` starts at `‖W‖` | as the paper recommends the norm is treated as a constant in the gradient (read in the paper). The paper's "norm across each column" is ambiguous about orientation; we normalise per output unit (row of `W` in the `[out][in]` layout), which we believe matches the reference implementation (not checked against its code). Costs one extra `R×r×C` product per adapted matrix per forward and one extra forward product in the backward. |
| `--anchor L` | KL(base ‖ model) on the positions that are not training targets, weight L; the base distribution comes from an extra forward with everything off | +1 forward per window (about +33% of a step). Limits forgetting of the context distribution. Not a method from one paper: standard KL regularisation toward the reference model. |
| `--label-smoothing E` | label smoothing (Szegedy et al. 2016, arXiv:1512.00567) | |
| `--tool-weight W` | loss weight W on tokens inside tool calls (`<tool_call>...</tool_call>` and Gemma 4's `<\|tool_call>...<tool_call\|>`), found by decoding the window | weights normalised so the mean loss scale is unchanged. |
| `--optimizer muon` | Muon (Jordan et al. 2024) on the adapter matrices: Nesterov momentum 0.95, 5 quintic Newton-Schulz iterations, update scaled by `0.2 sqrt(max(rows, cols))` and decoupled weight decay (Liu et al. 2025, arXiv:2502.16982, "Muon is Scalable", scale and decay read in the paper) | the learning rate is the AdamW one; not tuned. Norms/whole tensors stay on AdamW. |
| `--pissa` | PiSSA (Meng et al. 2024, arXiv:2404.02948): `A`, `B` start at the top-r singular components of `W`, the rest is the frozen residual | randomised subspace iteration (Halko et al. 2011), not a full SVD. The residual cannot replace the quantized base, so the adapter has rank 2r: `[A; A0]`, `[B, -B0]` with the second half frozen (kept fixed even under weight decay); the file stores rank 2r and loads in old readers. Cannot be combined with `--resume` or Muon. |

`DoRA` in the `.kspec`: the file starts with the magic `KSPEC3` instead of `KSPEC2`, then the tensors as always, then a
section `KSDM` (name, rows, magnitudes), 8 bytes with the offset of that section, and the usual `KSXF` trailer with flag
2. A reader from before this change refuses a `KSPEC3` file ("arquivo de especializacao invalido", checked with a build
of the previous integration branch) instead of silently loading only the LoRA part, which would be a different model.
Files without DoRA keep `KSPEC2` and load everywhere (PiSSA files too: rank 2r adapters). Readers of this version accept
both; they apply the magnitudes at load (`KATANA_SPEC`), in `katana merge` and in `--resume` (a file with magnitudes
needs `--dora`).

Interaction with the optimizer side file (`.kopt`): it only stores the AdamW moments of whole tensors and adapters, not
DoRA magnitudes, Muon momentum, the moving average or the PiSSA frozen part. With any of `--dora`, `--optimizer muon`,
`--ema` or `--pissa` no `.kopt` is written and `--resume` says that the optimizer restarts from zero. The B matrices
honour `--lr-b` in every optimizer; `--probe-top` frees the method state of the tensors it drops.

### In the real command: `katana specialize`

`specialize` used to pass `--val 0`, so the baseline guard never acted. It now holds out a validation set by whole
conversation (`--val F`, default 0.1; the training seed fixes which), capped at `--val-max N` windows (default
`steps / 10`, at least 6) and measured three times (the base before step 1, half way and at the end), which is about 10%
of the training cost. A training that does not beat the base is refused (exit code 2 of `katana train`, 9 of
`specialize`, with a `RECUSADA` line and no history entry); `--allow-worse` writes anyway, `--val 0` turns the check off
with a warning. `katana learn` treats exit code 2 of the training as a refused round (`"train_refused": true` in its
log) and goes on to the next one. With the window cut of the light step, the KL anchor keeps the whole window (it needs
the context after the last target) and gathers the output rows of the context positions too.

### Tests

- `katana gradcheck` now also checks, tensor by tensor with directional finite differences, the gradient of the loss
  with: NEFTune, adapter dropout, label smoothing + tool weights, KL anchor, DoRA (A, B and magnitude; norm frozen), and
  all of them together, on every tiny layout in `tools/traintest/run.sh` (the shaped loss itself is what is
  differentiated). EMA, Muon and the PiSSA initialisation do not change gradients. Sensitivity was checked by breaking
  each of four things on purpose (dropout mask in the backward, DoRA scaling of `dy`, smoothing term, anchor scale):
  the check fails every time.
- `katana merge` of a DoRA and of a PiSSA file gives a GGUF whose held-out loss equals the loss measured by `katana
  compare` on the base + file (0.5895 and 0.3693 on the tiny model).
- `katana serve` with `KATANA_SPEC` set to a DoRA file (tiny model, temperature 0, three prompts) answers the same as the
  merged GGUF and differently from the base.

### Comparison on a tiny model (`sh tools/traintest/methods.sh`)

A 0.1M-parameter llama layout with random weights is first trained on general text (so it has something to forget),
then rank-8 adapters on all blocks learn a task ("repeat the word" and "open the word" answered with a tool call whose
arguments copy the word), 96 conversations, 300 steps of batch 4 and windows of 64 tokens (equal training tokens), 3
seeds, lr 3e-3. The state of the last step is kept (no choice on validation), except `--ema`, which picks between raw
and averaged on `val.jsonl`. Everything is measured by `katana compare` on `test.jsonl` (100 new conversations) and on
`drift.jsonl` (general text, other sentences). Base before fine-tuning: held-out 10.555, tool tokens 8.839, drift loss
0.752. Mean ± sample standard deviation over 3 seeds:

| method | held-out loss | tool-call tokens | drift loss - base (forgetting) | KL(base,spec) on drift | seeds better than LoRA |
|---|---|---|---|---|---|
| lora (control) | 0.582 ± 0.034 | 0.050 ± 0.002 | 3.614 ± 0.187 | 3.689 ± 0.202 | - |
| ema 0.9 | 0.583 ± 0.033 | 0.050 ± 0.002 | 3.606 ± 0.193 | 3.681 ± 0.207 | 0/3 |
| neftune 5 | 0.633 ± 0.065 | 0.051 ± 0.005 | 3.712 ± 0.032 | 3.794 ± 0.055 | 0/3 |
| neftune 15 | 1.266 ± 0.150 | 0.087 ± 0.003 | 3.131 ± 0.196 | 3.208 ± 0.208 | 0/3 |
| adapter-dropout 0.1 | 0.636 ± 0.055 | 0.049 ± 0.006 | 3.218 ± 0.098 | 3.297 ± 0.121 | 0/3 |
| adapter-dropout 0.3 | 0.820 ± 0.067 | 0.054 ± 0.005 | 2.767 ± 0.082 | 2.838 ± 0.096 | 0/3 |
| anchor 0.5 | 0.562 ± 0.031 | 0.043 ± 0.005 | 1.987 ± 0.064 | 2.033 ± 0.069 | 3/3 |
| anchor 2 | 0.598 ± 0.025 | 0.060 ± 0.010 | 1.692 ± 0.117 | 1.718 ± 0.112 | 0/3 |
| label-smoothing 0.1 | 0.726 ± 0.063 | 0.172 ± 0.008 | 3.075 ± 0.067 | 3.132 ± 0.090 | 0/3 |
| tool-weight 3 | 0.572 ± 0.019 | 0.050 ± 0.005 | 3.656 ± 0.254 | 3.722 ± 0.267 | 2/3 |
| dora | 0.552 ± 0.036 | 0.046 ± 0.003 | 3.689 ± 0.301 | 3.767 ± 0.319 | 3/3 |
| muon (same lr) | 2.675 ± 0.022 | 0.206 ± 0.050 | 1.408 ± 0.034 | 1.476 ± 0.034 | 0/3 |
| muon (lr 1e-3) | 6.991 ± 0.008 | 4.259 ± 0.057 | 0.144 ± 0.011 | 0.148 ± 0.009 | 0/3 |
| pissa | 0.375 ± 0.008 | 0.020 ± 0.004 | 4.381 ± 0.303 | 4.447 ± 0.289 | 3/3 |
| pissa (lr 1e-3) | 1.269 ± 0.034 | 0.058 ± 0.003 | 2.009 ± 0.009 | 2.080 ± 0.007 | 0/3 |

Reading it (tiny model, one task, one learning rate per method, 3 seeds: this is evidence about this setup, not about
the real models, where nothing was trained or measured):

- Forgetting is large in this setup (3.6 nats of drift loss for plain LoRA), so the table is informative about it.
  Lower forgetting came with higher held-out loss in nearly every row (dropout, NEFTune, label smoothing, Muon and
  PiSSA at lower lr are partly *under-trained* at 300 steps). The honest reading of the low-forgetting rows is a
  trade-off, not a gain.
- **KL anchor 0.5** is the one result that looks like a free improvement: held-out 0.562 vs 0.582 (3/3 seeds
  better, but the difference is about 0.6 standard deviations) and forgetting cut from 3.61 to 1.99. Anchor 2 cuts
  forgetting more and costs held-out loss.
- **PiSSA** (lr 3e-3) fit the task best (0.375, tool tokens 0.020, 3/3 seeds) and forgot the most (4.38); at lr 1e-3 it
  under-trains. **DoRA** was slightly better than LoRA on the task (0.552, 3/3) with the same forgetting; the gap is
  about one standard deviation.
- **Did not help here:** EMA (identical to LoRA; a plausible reason, not tested, is that the cosine schedule already
  decays the lr, so the last weights are already close to their average), NEFTune and adapter dropout (worse on the task; they reduce forgetting only by learning less),
  label smoothing (hurts the tool tokens, 0.17 vs 0.05), Muon (needs its own learning rate, none of the two tried
  worked), tool-weight 3 (no clear change on tool-call tokens: 0.050 vs 0.050; the loss is already low).
- Four runs skipped one step each through the loss-spike guard (seed 3 of anchor 0.5/2 and dropout 0.1/0.3).

`tools/traintest/methods.sh` regenerates the table in about 6 minutes on 8 cores; `methods_table.py` makes the table.

### PiSSA cost, measured

Gemma 4 26B-A4B (Q4_K_M), CPU, 16 attention matrices (layers 0 to 3), rank 8: **3.26 s in total, 0.20 s per matrix**
(one run, shared machine, under the measurement lock). The cost is a few passes over each matrix (dequantize, four
power iterations, one projection), so it scales with the number of adapted matrices and their size; a night that
adapts every matrix of the model has not been measured, and neither have the large FFN and expert matrices, so no
total for the whole model is claimed. On the tiny model it is below the clock resolution. Nothing was trained on the
real model (one optimizer step on 16 tokens, nothing kept, no validation).

### References

- Jain et al., *NEFTune: Noisy Embeddings Improve Instruction Finetuning*, arXiv:2310.05914.
- Liu et al., *DoRA: Weight-Decomposed Low-Rank Adaptation*, arXiv:2402.09353.
- Meng, Wang, Zhang, *PiSSA: Principal Singular Values and Singular Vectors Adaptation*, arXiv:2404.02948.
- Liu et al., *Muon is Scalable for LLM Training*, arXiv:2502.16982; K. Jordan et al., Muon optimizer, 2024.
- Halko, Martinsson, Tropp, *Finding structure with randomness*, SIAM Review 2011 (the SVD used by PiSSA).
- Szegedy et al., *Rethinking the Inception Architecture*, arXiv:1512.00567 (label smoothing).

What was actually read in the paper text (PDF): the NEFTune noise rule, the DoRA decomposition and the norm-as-constant
advice, the Muon update scale and weight decay. Only the abstracts of PiSSA were read (the initialisation formula
`s B A = U S V^T` with `A = B^T`-style split of `sqrt(S)` is ours, derived to make the product equal the top-r part
of `W`); label smoothing and the Halko et al. algorithm are cited from memory and implemented from the textbook form.

## How correctness is checked

`katana gradcheck` runs two tests on a random sequence:

1. **Training forward against the engine**: the training forward (written separately, keeping every activation)
   must give the same logits as the engine's own `forward()` at every position.
2. **Gradients against finite differences**: for every tensor (and, in a second pass, for the A and B of rank-3
   adapters on every matrix), the analytic gradient projected on a random direction is compared with a central
   difference of the loss along that direction. Tolerance: 1% relative plus 1e-4 absolute (float rounding dominates
   on the smallest gradients; a wrong formula shows up as a large, step-independent error). In MoE layers the choice
   of the K experts is a step function with no derivative: the finite differences reuse the experts chosen in the
   base pass, which is exactly the gradient the trainer uses (the selection is held fixed, the weights of the chosen
   experts and the router logits behind them get the gradient). Each direction is estimated twice, by a plain
   central difference and by Richardson extrapolation (central differences at h and h/2, which removes the h^2
   curvature error that matters for parameters inside exponentials such as the DeltaNet decay); the closer estimate
   counts. A wrong formula misses both.

`tools/traintest/run.sh` runs the gradcheck on three layouts of the tiny random model (`tools/tinymodel`), trains it
with whole tensors and with adapters on a Q8_0 base, and checks that the server with `KATANA_SPEC` answers exactly the
trained replies at temperature 0.

## Measured (2026-11-20, cloud machine with 2 cores, no GPU, tiny random models only)

| check | result |
|---|---|
| training forward vs engine, max logit difference | 4.3e-6 (llama), 3.8e-6 (qwen2 layout with GQA and q/k/v bias), 5.0e-6 (Q8_0 base) |
| gradcheck, 21 whole tensors + 36 adapter A/B | pass on all three; largest relative error 1.4e-2, on gradients of ~0.002 |
| train all tensors, 150 steps, 3 examples | loss 12.0 -> 0.0001 in 1.0 s; `.kspec` 457 KB |
| rank-8 adapters on a frozen Q8_0 base, 400 steps | 0.018M trainable weights (15% of the model), loss 12.0 -> 0.0002 in 2.4 s; `.kspec` 70 KB |
| server with `KATANA_SPEC`, temperature 0 | the three trained answers word for word, each ending on its own end-of-turn token (before: random bytes) |
| training forward vs engine, MoE layouts | 5.5e-6 (qwen3, per-head q/k norm), 2.9e-6 (qwen3moe), 3.8e-6 (qwen3moe with shared expert, sigmoid gate, a leading dense layer and GQA), 3.6e-6 (olmoe: whole-vector q/k norm, no renormalization, K=3) |
| gradcheck on those four | pass (65 to 197 tensors and adapter parts each) |
| heat-guided MoE specialization: 2 hot experts of 8 per layer + router + attention and shared-expert adapters (rank 8) | the 2 hot experts cover 43% of the router's choices on the data; loss 12.9 -> 0.0002 in 2.5 s; `.kspec` 50 KB; server answers the three replies word for word |
| hot experts and router only (no attention) | loss 12.9 -> 0.49: on this tiny random model the experts alone do not carry enough capacity; with attention adapters they do |
| training forward vs engine, Qwen3.6 layout (qwen35moe: Gated DeltaNet + gated attention with partial RoPE + MoE with gated shared expert) | 3.8e-6; qwen35 dense: 5.4e-6 |
| gradcheck on qwen35moe and qwen35 | pass (204 and 72 tensors and adapter parts; DeltaNet tensors within 1%) |
| qwen35moe, rank-8 adapters on all layers, data in the model's thinking format | loss 12.5 -> 0.0001 in 2.2 s; `.kspec` 190 KB; the server answers the three replies word for word, with the empty thinking block kept out of `content` |
| training forward vs engine, Gemma 4 layout (gemma4: a sliding-window layer with head width 8 and 2 kv heads, a global layer with head width 16, 1 kv head and no `attn_v`, extra norms, GELU, dense FFN in parallel with 8 experts from a fused gate/up tensor, per-expert scales, embedding scale, logit softcap 30) | 1.2e-6; dense variant 9.2e-7; Q8_0 base 8.1e-7 |
| gradcheck on gemma4 (MoE, dense, Q8_0) | pass (237 tensors and adapter parts on the MoE one; the only parts above 1% relative are three expert gradients of size ~1e-5, inside the absolute floor) |
| gemma4, 2 hot experts of 8 + router + rank-8 adapters on attention + all norms | loss 13 -> 0.13 in 3.2 s; the server answers the three replies word for word, with the `.kspec` and after `katana merge` |
| AddressSanitizer + UndefinedBehaviorSanitizer | gradcheck (dense, MoE, DeltaNet, Gemma 4), train and loading: no reports |

What this proves: the backward pass is right for this layer layout, the optimizer converges, the frozen quantized
path works, and the file round-trips into the serving engine. What it does not prove: anything about quality on a
real model, or speed. Nothing was run yet on the real Qwen3.6 or Gemma 4 files (no model or GPU in the session that
wrote this); both layouts are covered on tiny random models.

## Training on the GPU (Vulkan)

The three products with a weight, which are most of the arithmetic, run on the GPU through `vk/shaders/tmm.comp`:
forward `Y = X W^T`, transposed `dX = dY W` and the weight gradient `G = dY^T X`. Attention, norms, the DeltaNet
recurrence and routing stay on the CPU for now.

- Frozen weights go to VRAM once, in their GGUF format (F32, F16, BF16, Q8_0, Q4_K, Q5_K, Q6_K; no repacking), while
  they fit. The rest is uploaded per product in slices. Other formats fall back to the CPU for that tensor.
- Float masters of trained tensors are uploaded again after each optimizer step (a version counter).
- One float32 kernel for all three modes and formats (64x64 tiles, each weight element dequantized once per tile and
  reused 64 times). It does not use cooperative matrices yet, so it runs on any Vulkan 1.2 GPU and is far from the
  card's peak.
- `KATANA_TRAIN_GPU=0` turns it off; `KATANA_TRAIN_VRAM=<GB>` caps the resident weights; `KATANA_TRAIN_GPU_MIN=<n>`
  is the smallest product (multiply-adds) sent to the GPU (default 4M; 0 sends everything, for testing).
- `katana train` prints `velocidade: N tokens/s` and the GPU totals at the end. To compare, run the same command
  with and without `KATANA_TRAIN_GPU=0`, alternating, in the same hour.

How it was checked, without a GPU (none in the session that wrote it; a software Vulkan device could not be built
there):

| check | result |
|---|---|
| the shader's own code (`tmm_core.glsl` is GLSL and C++ at once) run on the CPU with 256 real threads and barriers per workgroup, 7 formats x 3 modes, odd sizes and an offset weight, against the CPU dequant in double (`tools/traintest/tmm_emu.cpp`) | max relative error 1.1e-6 |
| katana with an emulated GPU (`-DKATANA_TG_EMU`: the `kvk_*` calls of the trainer go to host memory and run the shader code; `tools/traintest/tg_emu.*`), every product on it, gradcheck on the 11 tiny layouts, weights resident and weights forced into 4 KB slices | forward equals the engine (1e-6 to 1e-5); gradcheck passes on 10 of 11; olmoe misses on one adapter gradient of 4e-4 by 1.2e-4: the analytic value is identical in every run and the finite difference converges to it with a larger step, so it is float noise, not a gradient bug |
| emulated GPU, rank-8 adapters on Q8_0 and whole-tensor training | the server answers the three trained replies word for word |

Not measured: speed on a real card, and the kernel on a real driver. `tools/traintest/run.sh` runs gradcheck on the
real GPU (5 layouts) when one is present; that is the first thing to run on the owner's machine.

## Learning from its own use: `katana learn`

`katana export <out.jsonl> <sessions.ksess or folders> [--bench results.jsonl]` turns agent sessions into training
data (the same JSONL `katana train` reads), in C. With `--bench` it keeps only the sessions whose hidden tests passed.
Sessions where the agent compacted its context are skipped unless `--inexact`. Found while writing it: the Python
exporter (`tools/dataset/ksess2jsonl.py`) used byte offsets as character offsets, so any non-ASCII text before a
model turn shifted its training span; fixed, and both now give the same spans.

`katana learn <model.gguf> [options] [-- katana train options]` runs the self-improvement loop, all on the machine:

1. **Collect**: the current best model (base + best specialization so far) solves the training tasks of the codebench
   `--repeat K` times at `--temp` (default 0.7), so the attempts differ.
2. **Filter**: only sessions that passed the hidden tests become data (rejection by verification). With `--teacher URL
   --teacher-model M` a teacher also solves the same tasks through katana's tools; its passed sessions are imported.
   `--extra FILE` adds data of your own.
3. **Train**: `katana train` on everything collected so far, always from the base model, with the options after `--`.
4. **Prove**: the candidate solves the evaluation tasks, which never enter the data (default: every fourth task in
   alphabetical order; `--tasks` and `--eval-tasks` choose). It replaces the best only if it solves more.

Agent runs use an empty home folder (HOME/USERPROFILE), so the owner's agent memory and MCP servers stay out of the
data. Output in `--dir` (default `<model>.learn`): `best.kspec`, `data.jsonl`, one folder per round and `learn.jsonl`
with one line per round (collected, passed, data size, train and validation loss, evaluation score, accepted).
Example for a real model: `katana learn qwen3.6.gguf --rounds 3 --repeat 4 -- --rank 16 --batch 4 --steps 300 --lr 1e-4`.

Checked with a stand-in codebench (`tools/traintest/mock_bench`, real agent sessions on a tiny model, scripted pass or
fail): round 1 accepted, round 2 rejected, data accumulated 6 -> 12 conversations. Not run with a real model yet: with
39 codebench tasks the evaluation set is about 10 tasks, so one more task solved is within noise; use `--eval-repeat`
for a steadier score.

## A folder of specializations (2026-11-21)

`KATANA_SPEC` takes a `.kspec` file or a folder. With a folder, every `.kspec` in it that was trained on the loaded
base model is applied, in name order; files trained on another base are skipped with a notice. Adapters from different
files add up on the same matrix; a whole tensor replaces what came before, so the last file wins. The desktop passes
`~/.katana/specs` when that folder holds any `.kspec` (and no `KATANA_SPEC` is set), so a training copied there is in
use from the next start; the engine alone (bench, selftest, `katana agent` in a terminal) loads nothing unless
`KATANA_SPEC` is set, so measurements stay on the base model. `katana specialize` writes `<kind>.kspec` (or
`katana.kspec`) and never copies it there by itself.

Checked on a tiny model: a folder with one matching and one foreign `.kspec` gives the three trained answers
(`tools/traintest/run.sh`). Not checked: two specializations of the same base applied together (they were trained
separately, each from the base, so their sum is not something either training saw), and the desktop start with a
real `.kspec` in the folder. Found on the way: `KATANA_SPEC=<file>` also switched off the speculative decoding of
`katana chat`, which reads the same variable as a 0/1 switch; now only the value `0` does.

## Tools known by training instead of by the prompt (2026-11-21)

Owner's decision: the tool declarations should not cost context in every conversation. When a specialization named
`tools.kspec` is applied, the agent's system prompt leaves out the whole tools section (the declarations and the call
format): 4.1k characters instead of 8.8k (measured with `katana agentsys`). Without that file the section stays,
because the base model does not know the names or the arguments. `KATANA_TOOL_DECL=0` leaves it out always, `=1`
keeps it always. The training data for `tools.kspec` must be rendered with the same short prompt:

```
KATANA_TOOL_DECL=0 ./katana agentsys | sed '$d' > sys.txt
./katana import <model> <transcripts folder> data.jsonl --system file:sys.txt [--replace ...] [--cut-matching ...]
KATANA_TRAIN_GPU=0 ./katana train <model> data.jsonl --train 'blk.*.attn_*' --rank 8 --seq 512 --lr 2e-4 \
    --final 0 --val 0 --steps 75 --out tools.kspec
```

`--val 0` because the default validation (10% of the windows every 20 steps) costs more than the training itself on
this model. First run started on 2026-11-21 on Qwen-AgentWorld-35B-A3B (CPU, 369 episodes, 4803 calls, 75 steps of one
512-token window, about 1% of the windows; first step of the earlier attempt: 77.5 s). **Not measured**: whether a
model trained this way calls the tools correctly without the section. Do not copy `tools.kspec` to `~/.katana/specs`
before codebench with it (and no declarations) matches the base model with declarations. `teacher.kspec` from
`katana specialize ... teacher` is still rendered with the declarations; `tools`, `knowledge` and `all` are rendered
without them and carry the flag in the `.kspec`.

## Plan: teach the models katana's tools (2026-11-21)

Goal: Qwen3.6-35B-A3B and Gemma 4 26B-A4B call the agent's current tools (the second set, `src/tools/`) in the right
format, with fewer wasted and invalid calls, without losing tasks on codebench. Nothing below has been run yet.

| Step | What | Done when |
|---|---|---|
| 0 | Baseline: codebench with the first set (`KATANA_TOOLS=1`) and the second, alternating, both models, `--repeat 2` | solved tasks, time and invalid calls per run recorded; this is the number every later step is compared with |
| 1 | Speed of training on the card: `sh tools/traintest/run.sh`, then 4 steps of `katana train` with and without `KATANA_TRAIN_GPU=0`, alternating | seconds per step on the GPU known (CPU measured: 229 s per 512-token window) |
| 2 | Data with the second set's names (below) and a fixed validation split by conversation | one JSONL per model, sizes in tokens and windows recorded |
| 3 | First run, Qwen3.6: `--plan 1`, then attention adapters rank 8, `--seq 512`, `--final 0`, one night | validation loss below its starting value at the best step |
| 4 | Measure: codebench base against base + `.kspec`, alternating, `--repeat 2`, `katana codebench compare`; count calls the agent rejected (unknown tool, bad arguments) in both | accepted if it solves at least as many tasks and makes fewer rejected calls; one task of difference is noise |
| 5 | Gemma 4: steps 2 to 4 with the data imported in its format | same criterion |
| 6 | Loop: `katana learn` rounds (passed codebench sessions, optional teacher), from the accepted `.kspec` | evaluation tasks solved do not drop over two rounds |
| 7 | Use: `KATANA_SPEC` in the agent and desktop, or `katana merge` | the owner's everyday agent runs with it |
| 8 | One specialized model: `katana specialize <model> all` with transcripts, teacher sessions and the knowledge corpus, resumed night after night, then `katana merge` (built 2026-11-21, tiny models only) | codebench with the merged GGUF and no tools section matches or beats the base with the section |

Data sources, all through `katana import` or `katana export`:

- *Development transcripts* (`katana import <model> <folder> out.jsonl --system file:sys.txt --only-known-tools`,
  with `--replace` and `--cut-matching`): 364 episodes, 4774 calls, 5.5M characters to learn. About 80% of the calls
  are `Bash` on this repository, so cap the Bash-only episodes or the model learns mostly that.
- *Conversations and sessions with the first set's names* (messages format, or a `.ksess` session of the agent):
  `katana import` converts names and arguments and renders the session again with the current system prompt. The
  owner's teacher session of 2026-11-20/21 gives 178 turns and 416 converted calls (467k characters to learn): small
  enough for one night, and the best first data set, because it is real use of the agent with a teacher.
- *New sessions with the second set*: codebench with `--sessions --repeat` (only the passed ones) and teacher-mode
  sessions (`katana export --teacher`). These are the only data with a pass or fail label.

Budget (estimate from the measured 229 s per step, not a measurement of this data): the transcripts are about 5M
tokens, close to 10,000 windows of 512 tokens, which is 26 days per epoch on the CPU. One night is about 150 steps. So
step 3 uses a subset (a few hundred windows, short episodes first, every tool represented) unless step 1 shows the
GPU is several times faster.

Open points:

- 96% of the reasoning blocks in the transcripts are empty. A model trained on them with reasoning on learns to skip
  its reasoning. Either import with reasoning off (`KATANA_THINK=0`, the model's empty block) and serve the same way,
  or keep only the turns that carry reasoning text. Decide before step 3; measure both if the night allows.
- Converted conversations carry tool results in the first set's format.
- Sessions hold what the user typed, keys and tokens included: mask them with `--replace` before training.
- The transcripts are answers of a hosted model: check the provider's terms before training on them.
- With 39 codebench tasks one more solved is within noise; the rejected-call count is the more sensitive measure.

## Making each trained token teach more: windows, optimizer, probe (2026-11-22)

Goal: fewer steps and less waste to reach the same validation loss. Everything below is an option; no default changed.
What was measured is separated from what is only an estimate.

### The real data (measured, `gemma4-conventions/data.jsonl`, 1,800 conversations, Gemma 4 tokenizer)

1,126,556 tokens, 223,725 target tokens (19.9% of the tokens are targets), 626 tokens per conversation on average.
Printed by `katana train` at every start (the "janelas" lines; `--plan 1` prints them and stops).

| window | mode | windows | trained tokens | targets per window | density (targets / trained tokens) |
|---|---|---|---|---|---|
| 128 | fixed (old) | 6,295 | 693,536 | 35.5 | 32.3% |
| 128 | dense, ctx 64, win-min 3 | 5,338 | 630,345 | 41.9 | 35.5% |
| 256 | fixed (old) | 4,244 | 867,625 | 52.7 | 25.8% |
| 256 | dense, ctx 64, win-min 3 | 3,343 | 697,806 | 66.9 | 32.1% |
| 512 | fixed (old) | 2,904 | 1,046,411 | 77.0 | 21.4% |
| 512 | dense, ctx 64, win-min 3 | 2,166 | 730,029 | 103.3 | 30.6% |

Findings:

- **Nothing is wasted after the last target.** Every conversation ends on a target (0 tokens after the last one), so
  "end the window at the last target" saves nothing on this data. It would on data that ends with a user turn.
- **Context-only windows are already free.** Windows without a target are never trained (3,445 of them at 128, with
  441k tokens, are skipped by the old code too). What the old cut wastes is the *front* of each trained window: the
  part before its first target (system prompt, user text, tool results). The dense mode trims that to `--win-ctx`
  tokens before the first target the window owns: 20% fewer trained tokens at 256 for the same 223.7k targets
  (30% at 512, 9% at 128). Cost: the first target of a window sees at most 64 tokens of context instead of up to the
  whole window. Whether that hurts a real tool call (the file read that the call refers to may be further back) is
  **not measured on the real model**: it needs a real run, compared by codebench.
- **Duplicated context is 1 token per window** (the overlap), nothing to gain.
- Low-density windows: with 128-token windows 128 windows (2%) have under 5% targets; dense plus `--win-min 3`
  leaves 51 and loses 70 of 223,725 targets (0.03%). At 256: 130 -> 16 windows, 12 targets lost.
- **Ordering by length does nothing for the step cost**: windows are run one at a time (gradient accumulated), there
  is no padding to avoid. Not implemented.
- **Packing several conversations in one window** (needs attention and DeltaNet state resets at the boundaries, in the
  step code, not touched): in tokens it would pay nothing, because there is no padding to remove (mean window length
  is 110 tokens at 128, 204 at 256, 360 at 512, trained as they are). It could only pay if a step has a large
  constant cost per window (weights read once per product no matter the length), which makes short windows
  inefficient. That is a measurement for the speed agents: time per step at T = 128, 256 and 512 on the real model.
  Estimate, not measured: larger `--seq` gives the same effect without touching the step code.

New windowing options (training windows only; validation is always cut with fixed full windows, so loss is comparable
between runs): `--window fixed|dense`, `--win-ctx N` (default 64), `--win-min N` (drop windows with fewer targets).

### Optimizer and schedule options

| option | what it does |
|---|---|
| `--lr-b X` | learning-rate multiplier for the adapter B matrix (B starts at zero, A random) |
| `--init-a X` | scales the initial A (default 1: uniform, +-1/sqrt(cols)) |
| `--rs 1` | adapter scale alpha/sqrt(rank) instead of alpha/rank |
| `--weight tgt` | in an accumulated step, each window weighs by its number of targets (sum over targets / total) instead of the plain mean of window means |
| `--batch-tgt N` | accumulate windows until the step has N targets (implies `--weight tgt`) |
| `--val-max N` | deterministic subsample of the validation windows (evenly spaced), to make evaluation cheaper |
| `--min-rel X` | with `--patience`, an evaluation only counts as an improvement if it is better by the relative margin X |
| `--opt-state 0/1`, `--resume-opt 0/1` | optimizer moments and step count are written next to the `.kspec` (`<file>.kopt`, only when at most 32M weights train; default on) and read back by `--resume`, so Adam continues with its bias correction instead of restarting. The `.kspec` itself is unchanged, old readers and old files work as before (no `.kopt`: moments restart, as always) |
| `--probe N`, `--probe-top F` | gradient norm per tensor (B for adapters, whole tensors for the others) over N windows, plus the share per projection and per layer; `--probe-top F` then keeps only the F fraction of tensors with the largest norm (`--probe N` alone prints and stops) |

Bias correction was already right (`1 - beta^step`); with `--resume` it continues from the step stored in the `.kopt`.
Warm-up for short runs already exists (`--warmup`, default max(10, steps/20)).

### Tiny-model measurements (measured; not evidence for the real model)

Task: `tools/traintest/synth_task.py` (a word to copy twice, buried in 10-120 characters of random filler, 2 to 4
turns per conversation, only the copy is trained), 300 conversations, validation on 60 other conversations with fixed
windows. Tiny llama (2 layers, dim 64), rank-8 adapters on attention, `--seq 128 --batch 4`, cosine schedule, CPU.
Harness: `tools/traintest/opt_compare.py` (sets the steps so every config trains the same 200,000 window tokens,
3 seeds; the seed changes A and the order of windows). Numbers are the final validation loss, mean (spread over the 3
seeds is in the column; it is large for some configs, so differences below ~0.05 are not significant).

Old behaviour learning-rate sweep: 1e-3 2.363, 3e-3 1.337, **1e-2 1.211 +- 0.006**, 3e-2 1.992.

| config (lr 1e-2 unless noted) | final val loss | sd |
|---|---|---|
| old: fixed windows | 1.211 | 0.006 |
| dense, ctx 64, win-min 3 | 1.172 | 0.037 |
| dense, ctx 32 | 1.112 | 0.030 |
| dense, ctx 16 | 1.030 | 0.089 |
| `--weight tgt` | 1.182 | 0.054 |
| `--batch-tgt 43` | 1.194 | 0.062 |
| `--lr-b 4` at lr 3e-3 | 1.167 | 0.014 |
| `--lr-b 4` | 1.377 | 0.026 |
| `--lr-b 16` at lr 1e-3 | 1.201 | 0.037 |
| `--rs 1` | 1.293 | 0.025 |
| `--rs 1` at lr 3e-3 | 1.206 | 0.018 |
| `--init-a 3` | 1.041 | 0.057 |
| `--init-a 6` | 1.067 | 0.084 |
| `--init-a 10` at lr 3e-3 | 1.070 | 0.080 |
| `--init-a 3` + dense ctx 32 | **0.842** | 0.044 |
| `--init-a 3` + dense ctx 64 | 1.041 | 0.072 |
| `--init-a 3` + `--weight tgt` | 1.086 | 0.024 |
| `--init-a 3` + `--lr-b 4` at lr 3e-3 | 1.082 | 0.086 |

Reading: at equal trained tokens, dense windows with a short context and a larger initial A each help clearly on this
task, and they add up (1.21 -> 0.84, a loss the old setup does not reach in 200k tokens). `--weight tgt`, `--batch-tgt`,
`--rs` are inside the noise or worse once the learning rate is tuned for the old behaviour; `--lr-b 4` at a lower learning rate gives a small gain (1.167 against 1.211) that disappears when combined with `--init-a`. Limits of
this evidence: one synthetic task whose useful context is short (which favours trimming the window front), a 64-wide
model, one learning-rate sweep for the old behaviour only (the variants got two or three learning rates), and only
200k tokens; the effect of `--init-a` is probably a faster effective step on B for this size and says nothing about
dim 2816. **Defaults unchanged**: the evidence does not cover the real model.

The step times of these runs were not used (the machine was at a load of 40 to 50 during them): the comparison is
per trained token, which is what the windowing changes; per-step cost of the new options is nil (the dense windows
are shorter on average, which only makes a step cheaper).

What did not help on the tiny task: `--rs 1`, a larger B rate on its own (`--lr-b 4` at the best old lr made it
worse, at a lower lr only equal), weighting by targets, a fixed number of targets per step.

### `--probe` (measured on the tiny model only; not run on the real model: the measurement lock queue was too long)

On the tiny llama with adapters on attention and the feed-forward, B's gradient is not spread evenly: layer 0 held 70%
of the squared norm, and `ffn_up`/`ffn_gate` 46% against 3.8% for `attn_q`/`attn_k`. The point of the mode is to read
this on the real model before choosing `--train` patterns, or to let `--probe-top` choose. Training only the upper
layers would let the step code stop its backward pass at the lowest trained layer (not implemented here).
Note the norm is of B at its zero start, so it ranks tensors by how much the loss wants to move them, not by what they
would gain after training.

## Limits today

- Architectures: llama, qwen2, qwen3, olmoe, qwen3moe-style (RMSNorm, GQA, RoPE in pairs or halves, SwiGLU,
  optional q/k/v/o bias, q/k norm per head or whole-vector, softmax-routed MoE with top-K, renormalization and scale,
  shared experts with an optional sigmoid gate, leading dense layers), and the Qwen3.6 family (qwen35moe, qwen35):
  Gated DeltaNet (causal convolution, L2-normalized q/k, gated delta rule, gated RMS output) and gated attention
  with partial RoPE, and Gemma 4 (gemma4: per-layer head width and kv heads, sliding window on local layers, k used
  as v when `attn_v` is missing, weightless v norm, extra norms after attention and around both FFN branches, GELU,
  dense FFN in parallel with the experts, router input scale and per-expert scales trainable, embedding scale and
  logit softcap in forward and backward). `layer_output_scale` stays frozen. Not yet: sigmoid routers with bias
  (DeepSeek/GLM), gpt-oss, MLA.
- Memory: the DeltaNet keeps its recurrent state only every 64 tokens (`KATANA_TRAIN_DNCK`) and the backward pass
  recomputes one block at a time from that checkpoint; attention probabilities are not stored either (each row is
  recomputed in the backward pass). Activations still grow with the window times the model width; the window is
  `--seq`. MoE tokens are grouped by expert, so each expert row is read once per window, not once per token.
- Only the weight products run on the GPU; attention, DeltaNet and MoE routing are on the CPU, and each product copies
  its activations to the GPU and back. One sequence per step.
- Adapters are merged at load, so an adapted matrix costs float memory at serving time.

## Next steps, in order

1. Run `tools/traintest/run.sh` on the owner's machine, then a first real run on a small dense model with sessions
   exported from the agent.
2. Sigmoid routers with bias, gpt-oss and MLA.
3. The first real specialization of Qwen3.6-35B-A3B and Gemma 4 26B-A4B on the owner's machine: `--plan 1` first, then a short run on
   exported sessions, measured with codebench against the base model.
4. ~~Gradient accumulation over several windows~~ (done: `--batch`, validation, best step, early stop) and the
   `katana learn` loop (done). Next for learning: a teacher that writes new tasks with their hidden tests, so the
   loop is not limited to the codebench tasks.
5. Speed on the GPU, after measuring the first version on the owner's card: activations kept in VRAM between
   products (no copy per product), attention and the DeltaNet recurrence on the GPU, cooperative-matrix variants of
   `tmm`, MoE experts in one grouped dispatch, adapters on the GPU.
6. Serving: adapters applied unmerged in the products and selected per request (`model` field), hot-swapped.
7. Optional: merge a specialization into a new GGUF and requantize it (`katana quantize` already writes GGUF).
