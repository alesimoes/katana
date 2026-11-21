# Token budget: where the tokens of a real session go

Written 2026-11-21. Source: the owner's desktop session of that afternoon (`~/.katana/desktop/sessoes/
20261121-152838-038.ksess`, local model, no teacher, second tool set), read from the `KSX1` block of the session file
(the exact text the model read and wrote, with one span per message; writer: `sess_wext` in `src/agent/loop.c`).
The agent log (`~/.katana/agente.log`) was **not** used for accounting: it truncates arguments at 399 bytes and
results at 199, and on that day it still holds the negative checks of the tool tests (3,551 `Grep` lines), so
`katana agentlog` reports 21% failures over 6,330 calls that are mostly not the agent's.

Conversion: bytes to tokens at 3.39 bytes per token, **measured on this file** (system prompt plus the compacted
history, 15,272 bytes, is 4,510 token ids in the same file). Per-category token numbers below are bytes divided by
3.39: an estimate with a measured ratio. Time uses 40 ms per generated token and 15 ms per token read (25 and 65
tokens/s, measured on the engine; `docs/handoff.md` 0k).

## 1. The reference session

Six user requests (an application to be written for a platform whose build tool needs a download and a login the
user had to provide). 215 model turns, 213 tool calls, 209 results.

| | value |
|---|---|
| model turns / tool calls | 215 / 213 (0.99 calls per turn) |
| results that were failures | 81 of 209 (39%) |
| requests that ended at the 40-round limit | 4 of 6 |
| requests in which a file was written | 2 of 6 |
| turns with any text for the user | 100 of 215 (0 in requests 1 and 3) |
| distinct files read / `Read` calls | 6 / 24 |
| tokens in the whole conversation text | about 91,900 |

### Where the tokens are

| category | tokens | share | per item |
|---|---|---|---|
| system prompt (8,488 bytes; KV cached on disk) | 2,500 | 2.7% | once |
| user messages | 255 | 0.3% | |
| **model output** | **26,900** | **29%** | |
| - tool calls | 20,400 | 22% | 96 per call |
| - of which call framing (tags and parameter names, not values) | 5,950 | 6.5% | 28 per call |
| - text for the user | 5,200 | 5.7% | |
| - reasoning | 0 | | reasoning was off (`KATANA_THINK=0`) |
| **tool results** | **58,350** | **63%** | 279 per result |
| - of which the wrapper lines around each result (134 bytes) | 8,250 | 9.0% | 39 per result |
| - of which engine notes (`[katana: ...]`, `[plan: ...]`) | 250 | 0.3% | 5 notes |

By tool (calls are tokens written, results are tokens read):

| tool | calls | tokens written | per call | tokens read | per result |
|---|---|---|---|---|---|
| `Bash` | 137 | 5,530 | 38 | 28,020 | 198 |
| `Read` | 24 | 1,190 | 46 | 16,880 | 662 |
| `Write` | 10 | 6,940 | 653 | 830 | 78 |
| `Edit` | 10 | 3,580 | 337 | 2,145 | 202 |
| `AskUserQuestion` | 14 | 2,090 | 140 | 1,080 | 72 |
| `WebFetch` | 9 | 685 | 71 | 5,260 | 550 |
| `WebSearch` | 5 | 180 | 33 | 3,860 | 727 |
| `Glob`, `TaskStop` | 4 | 190 | | 270 | |

### Waste that can be named

The three lines overlap (a failed call is often also a repeat); they are not additive.

| waste | tokens (call + result) | rounds |
|---|---|---|
| rounds whose result was a failure | 15,000 | 81 |
| a call identical to an earlier call of the session | 23,900 | 54 |
| `Read` of a file already read and not changed since | 7,550 | 18 |

Failure classes of the 81: 14 `AskUserQuestion` with a refused argument shape (the bug fixed on 2026-11-21 in
`ct_unwrap`), 10 program not found, 9 wrong usage of a command, 5 missing file, 3 `old_string` mismatch, 2
permission, 38 other non-zero exit codes.

### Time

| | minutes |
|---|---|
| generation, 26,900 tokens at 40 ms | 18 (estimate from measured rate) |
| reading results, 58,350 tokens at 15 ms | 15 (estimate from measured rate) |
| sum of the gaps between consecutive calls in the log (gaps under 10 minutes) | 119 (measured) |

The 86 minutes between the two are tool run time, compaction (a model-written summary of up to 600 tokens plus the
recomputation of the kept history, `agent_compact` in `src/agent/context.c`, triggered at system prompt + 6,000
tokens) and generation slower than nominal. **The session file cannot separate them**: it has no times. This is the
first gap to close (section 4).

### The same tools driven by a strong model

The teacher session of 2026-11-20/21 (a hosted model writing every turn through katana's first tool set, 18
requests) is the contrast: 181 turns, 419 calls (**2.3 calls per turn**), 13 failed result groups of 163 (8%), text
in 178 of 181 turns, 135,500 tokens written and 166,100 read. The strong model spends more tokens per turn and far
fewer rounds per unit of work, and it batches calls. Two facts for the design: batching is a habit that exists in
the training data, and the local model's cost is dominated by failed and repeated rounds, not by verbosity.

## 2. Reading the numbers

1. **Results are 63% of all tokens.** The model reads 2.2 tokens for each one it writes. `Bash` output alone is 30%
   of the session. Shaping results is the largest lever on tokens (`20-tools.md`).
2. **39% of rounds fail.** Each costs a call, a result and a decision. This is the largest lever on time, and it is
   mostly a model and call-validity problem (`99-review-of-current-work.md` item 1, `30-call-formats.md`,
   `60-training.md`).
3. **Fixed overhead per round is 67 tokens** (28 written as framing, 39 read as wrapper): 14,200 tokens, 15% of the
   session, that carry no information.
4. **The system prompt is not the problem.** It is 2.7% of the tokens and its KV state loads from disk in 0.05 s
   (measured, `docs/handoff.md` 0n). Shrinking it buys context room, not time
   (`99-review-of-current-work.md` item 5).
5. **Amnesia causes re-reads.** Six files were read 24 times. With compaction at 6,000 tokens of conversation, the
   model forgets a file a few rounds after reading it and reads it again (`40-context-and-memory.md`).
6. **Text for the user is cheap** (5.7%). The owner's complaint that the agent does not answer is not a token trade.

## 3. Budget

Target for the same job. "Technique" names the design; the target is an **estimate** until measured with
`katana tokens` on a comparable session. Time saved is tokens times the measured rates.

| category | today | target | technique | document |
|---|---|---|---|---|
| failed rounds | 15,000 | under 4,000 | a base model made for tool use; calls valid by construction; errors with a fault kind; refusal on the second identical failure; failure-driven training | 99, 30, 10, 60 |
| result wrapper | 8,250 | 600 | a three-token wrapper | 20 |
| call framing | 5,950 written | 5,950 at reading speed | grammar-forced tokens fed as a batch, not sampled (saves about 2.5 min) | 30 |
| `Bash` results | 28,020 | 12,000 | shaping by command class, results kept behind a handle, verdicts instead of logs | 20 |
| `Read` results | 16,880 | 8,000 | "unchanged, still in your context" stub; symbol and range reads; no amnesia | 20, 40 |
| web results | 9,120 | 5,000 | already shaped on 2026-11-21 (passages); measure | 20 |
| `Write` and `Edit` payload | 10,520 | 9,000 | anchored edits for long `old_string`; otherwise irreducible | 20 |
| questions | 3,170 | 500 | the fixed bug; one question per request | 10 |
| text for the user | 5,200 | 3,000 | one closing message per request, no narration between calls | 10 |
| compaction | unknown | 0 generated, 0 recomputed | engine ledger, masking, state snapshots | 40 |
| rounds | 213 | under 100 | several calls per turn, batch programs, fused results, verdict-first edits | 10, 20 |
| **total processed** | **about 92,000** | **about 40,000** | | |

At the measured rates the target is about 14 minutes of model time against 33, before counting rounds saved.

## 4. `katana tokens`: the accounting as a subcommand (now)

Until it exists, `docs/design/appendix/session_tokens.py` reproduces the tables above from any `.ksess` file or
folder (`--tsv` gives one line per session for the experiments). It reads; it changes nothing.

- **What**: `katana tokens <session.ksess | folder> [--by request|tool|round] [--json]` prints the tables of
  section 1 from the `KSX1` block, with exact token counts from the tokenizer (vocabulary only, as `katana tokenize`
  does) instead of the byte ratio.
- **Mechanism**: nothing can be budgeted that is not counted; the agent log cannot do it (truncated, polluted).
- **Cost**: one file in `src/app/` (about 300 lines) reusing `ksx_messages` (`src/app/learn.c`) and the call readers
  of `src/agent/dispatch.c` and `fmt_gemma4.c`. Add per-round times to the session: `agent()` already has `t0`, `t1`,
  `t2` per round (`src/agent/loop.c`, around `stats_line`); store four numbers per model turn in a `KSX2` extension
  (tokens fed, feed seconds, tokens generated, generation seconds) plus tool seconds and one record per compaction
  (tokens before, after, seconds).
- **Measure**: run on the two sessions above; the totals must match this document within the ratio error.
- **Drop**: never; this is the instrument every other proposal is judged with.

Derived metrics, used as gates in `80-evaluation.md`: tokens read and written per solved task; rounds per solved
task; failed-result rate; repeat rate; re-read rate; calls per turn; seconds of model time per solved task.

## 5. The cost model in one line

`seconds = rounds x (28 x 0.040 + 39 x 0.015) + written x 0.040 + read x 0.015 + compactions x (600 x 0.040 +
recomputed x 0.015) + tool time`

A proposal that claims a gain says which term it reduces and by how much, and is measured on that term and on the
total.
