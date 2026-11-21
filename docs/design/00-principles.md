# katana design: principles

Written 2026-11-21. These documents are the architecture and the roadmap the implementing agents follow. They are
design, not a record of work: nothing here is built unless a document says so with a file reference. Numbers marked
**measured** come from this machine (sources: `docs/handoff.md`, `docs/comparisons.md`, and the session accounting in
`01-token-budget.md`); everything else is an estimate or published evidence. Published evidence is cited as `[n]`;
the numbered source list is kept outside the repository, in `ref/research/2026-11-21-landscape.md`.

Reading order: this file, `01-token-budget.md`, `99-review-of-current-work.md`, `90-roadmap.md`, then the subsystem
documents as needed.

## The constraint everything follows from

| | katana on the reference machine | hosted assistants |
|---|---|---|
| generation | 23 to 27 tokens/s (measured, Qwen3.6) | 5 to 50 times faster |
| reading a prompt | 60 to 68 tokens/s (measured) | effectively free when cached |
| one tool round | seconds to minutes of model time | about a second |
| training | 245 s per 512-token window on the CPU (measured) | not the user's problem |

A hosted agent that wastes 30% of its tokens loses a little money. katana wasting 30% of its tokens loses the user's
afternoon. In the reference session of 2026-11-21 (215 model turns), about 92,000 tokens were processed and about
15,000 of them were failed rounds (`01-token-budget.md`). The unit of cost in katana is **seconds of model time per
finished piece of work**, and its two components are tokens read (about 15 ms each) and tokens written (about 40 ms
each). A token written costs about 2.6 tokens read.

## Principles

Each principle says what it rules in and what it rules out.

### P1. The engine does everything that does not need intelligence

If a step can be computed (running the tests after an edit, noticing a file is unchanged, expanding the closing tags
of a call, keeping the list of files touched), the engine computes it and the model neither writes nor decides it.

- Rules in: verification before the model reads (`20-tools.md`), the engine-kept ledger as compaction
  (`40-context-and-memory.md`), forced tokens under the call grammar (`30-call-formats.md`), plan progress inferred
  from evidence (`10-agent-loop.md`).
- Rules out: asking the model to summarise what the engine already knows; prompt rules for things the engine can
  enforce; a second model call to post-process the first.

### P2. Every token the model reads or writes is accounted for

Each subsystem has a budget in `01-token-budget.md`, and `katana tokens` (proposed there) prints the realised
numbers per session. A change that moves tokens is judged by **seconds per solved task**, never by tokens per call:
published work shows local savings that raised total cost because the agent re-read what was cut [10].

- Rules in: per-category accounting from the session file; budgets for wrappers, notes, declarations; removing fixed
  overhead that repeats every round (result wrapper, empty reasoning block, call framing).
- Rules out: "it is only 30 tokens" (it is 30 tokens times 200 rounds); lossy truncation of content the model
  needs (code, diffs) without a way back; judging a change by one category.

### P3. Rounds are worth more than tokens

A round has a fixed cost that tokens do not show: the framing of a call (about 28 tokens, measured), the result wrapper (about 39 tokens, measured), the decision itself, and the risk of failure (39% of results in the reference
session were failures). One call that does the work of three is better than three short ones.

- Rules in: several calls per turn, batch programs, results that carry what the next call would have asked for,
  verdicts attached to edits.
- Rules out: tools so fine-grained that a common task needs a chain; pagination as the default answer to large
  results; sub-agents for parallelism (one engine: no wall-clock gain, only context isolation).

### P4. Behaviour comes from training plus enforcement; prose is the last resort

A rule in the prompt costs tokens in every session and is obeyed some of the time. A habit in the weights costs
nothing at run time; a rule in the engine is obeyed always. Prompt prose is for what is neither trainable yet nor
enforceable.

- Rules in: the call grammar, `never:`/`ask:` patterns, repeat refusal, training on decisions.
- Rules out: adding a paragraph to the system prompt as the fix for an observed failure without also opening an item
  to train it or enforce it; engine notes that are not individually switchable and counted.

### P5. Measure before believing, and measure the whole task

The project rule (alternating runs in the same hour; estimate labelled as estimate) extends to agent behaviour: a
mechanism is "done" when its effect on a task set is measured with a real model, not when its scripted test passes.
Every mechanism has a switch so it can be ablated.

- Rules in: the gate ladder of `80-evaluation.md` (offline replay in minutes, micro-set in an hour, full set
  overnight); one named metric and one drop criterion per proposal.
- Rules out: stacking unmeasured mechanisms (the state on 2026-11-21: see `99-review-of-current-work.md`);
  accepting a one-task difference; changing two things between two measurements.

### P6. The state belongs to the engine, so use it

katana owns the KV cache, the recurrent state, the sampler and the expert cache. Hosted agents see none of those.
Anything that needs a snapshot, a rollback, a forced token or a logit is cheap here and impossible or expensive
there.

- Rules in: state snapshots at round boundaries and rollback instead of re-reading (`40-context-and-memory.md`),
  grammar-forced tokens, sampler-level limits on reasoning, per-phase expert statistics.
- Rules out: designs copied from hosted agents whose shape is dictated by a stateless API (resend the whole
  conversation, summarise with another call, a sub-agent that re-reads the prompt).

### P7. The training budget is spent on decisions, not on payload

One night is 100 to 150 windows, 50 to 75 thousand supervised tokens (measured). A 650-token file body teaches one
habit, the same as the 12 tokens that chose `Write` and its path. Loss goes where the model decides.

- Rules in: decision spans, hindsight-corrected failures, one behaviour per night until its metric moves.
- Rules out: mixing five sources into 20 to 30 windows each; training on content that the model already writes well;
  a night with no held-out measurement.

### P8. One base model at a time, chosen by measurement

A specialization is tied to one base file. Every night trained on a base that is later replaced is lost. The base is
chosen by experiment 1 of `90-roadmap.md` before another night is spent.

## What "three horizons" means in these documents

- **Now**: can be built this month from what exists; mostly engine work with a model-free test and one measurement.
- **Next**: needs a new mechanism (a new file, a new state, a format change) and a measurement that may fail.
- **Beyond**: research bets that, as far as the landscape report shows, nobody has shipped. They are here because
  expensive tokens make them worth more to katana than to anyone with cheap tokens. Each carries an honest
  confidence.

Every proposal states: what it is, the mechanism, the evidence, the cost in this codebase, how to measure it on this
machine, and the result that makes us drop it.
