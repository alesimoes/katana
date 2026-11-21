# Call formats and constrained generation

How a call is written, what the engine guarantees about it, and what the sampler can do that a prompt cannot.

## Where katana is today

- Each model writes its native format: ChatML with XML calls and raw parameter values for Qwen3.6
  (`tool_xml_parse`, `src/agent/dispatch.c`), the native call tokens for Gemma 4 (`g4_call_parse`,
  `src/agent/fmt_gemma4.c`). Measured with the real tokenizers by the call-format work of 2026-11-21: a one-line
  `Bash` call is 38 tokens in Qwen3.6's format and 26 in Gemma 4's, of which 14 and 15 are the command. The
  decision recorded there (keep native formats, never put code inside JSON strings) is right and matches the
  evidence [7][28].
- In progress, not merged, not measured with a model: `src/agent/callgrammar.c`. A byte-level state machine follows
  an open call and answers whether the text can still become a valid call (known tool, declared parameters, each
  once, required ones present, typed values valid, strings free). It is lazy: the sampled token is checked, and only
  a rejected token triggers a scan of the vocabulary.
- In the reference session, 14 of 81 failures were argument-shape errors, 5 were paths that do not exist and 3 were
  `Edit` strings not found in the file. The grammar as designed removes the first group only.

## What the frontier does

- **Deployed.** Hosted vendors guarantee schema-valid arguments by masking invalid tokens, and offer free-form
  payloads with an optional grammar so code is never escaped. One open-weights family publishes a verifier for
  third-party hosts after measuring that parser and template mismatches, not the model, caused most broken calls;
  an independent host measured schema accuracy going from 92% to 100% with constrained decoding [21]. Grammar
  engines in serving stacks switch between free text and constrained regions on a trigger tag and skip sampling
  when only one continuation is legal [20]. The newest open families moved to XML-style calls with raw string
  parameters and state that this removes escaping failures [25][28].
- **Published.** Format restrictions can lower reasoning quality [22]; later work attributes most of the loss to
  the prompt and to constraining the reasoning itself, and recovers it by reasoning freely and constraining only the
  final structure [23][24].
- **Unsolved.** No measurement of the per-token cost of masking on a single-stream local engine. No constraint
  that covers the *meaning* of an argument (a path that exists, a string that occurs in a file).

**For a slow engine:** a failed call costs a whole round, so validity by construction is worth more here than
anywhere; and the engine, unlike a hosted API, knows the state of the machine while the call is being written.

## Design

### Now

**F1. Merge the call grammar behind a measured gate.**
- What: `KATANA_CALL_GRAMMAR` default on after the measurement, off before.
- Measure: rejected or malformed calls per 100 (`error[args]`, `error[unknown-tool]`, calls the reader could not
  parse) on the micro-set, alternating on and off; corrections per 100 calls (`g_cg` counters); tokens per second
  during calls. Drop (keep off) if solved tasks fall, which would mean the mask steers the model into valid but
  wrong calls.

**F2. Canonical parameter order.**
- What: inside a call the grammar accepts parameters only in declaration order, required first.
- Mechanism: with a fixed order, most of the framing has exactly one legal continuation (F3), a missing required
  parameter is impossible rather than detected at the end, and the model's own distribution already follows
  declaration order in the recorded sessions (check with `katana tokens`: share of calls out of order).
- Cost: a few lines in the parameter-name state of `callgrammar.c`. Measure: corrections per 100 calls; if the model
  fights the order often (over 5 per 100), drop the order and keep the set check.

**F3. Forced tokens are fed, not sampled.**
- What: when the grammar state admits exactly one continuation of two or more tokens (after the opener:
  `\n<function=`; after a tool name with required parameters: `>\n<parameter=NAME>\n`; after the last possible
  parameter closes: `\n</function>\n</tool_call>` and the end-of-turn token), the engine appends those tokens through
  the batch path (`agent_feed`) and resumes sampling after them.
- Mechanism: P1 and P6. A forced token carries no decision; sampling it costs a full generation step (about 40 ms)
  where feeding costs about 15 ms and can be batched. With F2, an estimated 15 to 20 of the 28 framing tokens of a
  call are forced (estimate; count with the tokenizer).
- Evidence: skipping forced tokens is deployed in serving-side grammar engines [20] (not re-verified for this
  report; marked so in the landscape file).
- Cost: `cg_forced(state, out_ids)` in `callgrammar.c` and a branch in `gen_console`. Three days.
- Measure: seconds per call on a fixed set of 50 recorded calls replayed by forcing the values. Expected about 0.4 s
  per call (estimate), about 1.5 minutes on the reference session. Drop if the measured saving is under 0.15 s per
  call.

**F4. Call validity as a standing metric.** `katana tokens` prints, per session: calls, calls rejected by kind,
grammar corrections, calls cut by the token limit. This is the most sensitive behaviour metric available on few
runs (`80-evaluation.md`).

### Next

**F5. State-grounded constraints (original).**
- What: for parameters whose valid values are determined by the state of the machine, the grammar is built from
  that state at the moment the parameter opens:

  | parameter | constraint | source of truth |
  |---|---|---|
  | `Read.file_path`, `Edit.file_path` | a path that exists | a trie of the project's files (the list `Grep` already builds) plus a `stat` of the completed path for anything outside it |
  | `Edit.old_string` | a substring of the file being edited (unique unless `replace_all`) | a suffix automaton of the file, built when `file_path` closes |
  | handle names (`r17`), plan step numbers, background task ids | one of the live ones | engine tables |
  | `Grep.type`, `output_mode`, other enumerations | one of the declared values | schema |

  The check stays lazy (only the sampled token is tested; a rejected token is resampled among the allowed ones).
- Mechanism: P6. "No such file" and "string not found" stop being errors that cost a round and become tokens that
  cannot be sampled. For `old_string` there is a second effect: every continuation is a substring of a known text,
  so the copy drafts of `70-inference.md` (I3) are almost always accepted and the string is written at reading
  speed.
- Evidence: none for state-grounded masks in a shipped agent (landscape report, topic 3). The measured base rate
  here: 8 of 81 failures in the reference session, and 3 of 10 edits.
- Cost: suffix automaton (about 150 lines, linear in file size; files over 1 MB are not constrained), path trie
  reuse from `src/tools/glob.c`, hooks in `callgrammar.c` at parameter open. Two weeks with tests.
- Risks: a model that wants to edit text that is not there may be steered to a nearby wrong substring (valid,
  unique, wrong). The uniqueness requirement and the verdict of V1 (`20-tools.md`) bound the damage; measure it.
- Measure: `error[not-found]` and `error[mismatch]` per 100 calls, and the edit micro-benchmark of T7
  (`20-tools.md`): success rate with and without, plus **wrong-but-valid edits** counted by the hidden tests. Drop
  if wrong-but-valid edits exceed the mismatches removed.
- Confidence: medium to high.

**F6. The batch language under the grammar.** The `Do` tool of `20-tools.md` (B1) is defined by a grammar from the
start: step numbers ascending, references only to earlier steps, tool names and parameters checked as in single
calls. A batch that parses is a batch that runs.

### Beyond

**F7. Sampler-level control of what a turn may contain (original as a system).** The same machine that validates
calls decides, per turn, which openers are legal: no call opener on a closing turn (L2 in `10-agent-loop.md`), no
second mutating call before a verdict arrives, no `Write` to a path a `never:` rule forbids (the rule becomes a mask
on the path parameter, so the model is steered away before writing 600 tokens of file content that would be
refused). Each rule the engine enforces after the fact today can be moved before the fact, saving the refused
round. Measure per rule: refused calls per 100 before and after. Confidence: medium; each rule is cheap once F5
exists.

**Rejected:**
- One-token calls through new vocabulary entries (a learned token per tool and parameter). With F3 the framing
  already costs reading speed; new embeddings need gradient through the whole network at 245 s per window, and they
  break the native format the model was trained on.
- A learned "plan token" vocabulary, for the same reason and because plans are under 6% of written tokens in the
  session with the most plan updates; L6 (`10-agent-loop.md`) gets the saving with a line format.
- Converting Qwen3.6 to JSON calls (10% fewer envelope tokens, measured, at the cost of escaping code [7]).
- Constraining reasoning or prose (the published losses come from there [22][23]).
