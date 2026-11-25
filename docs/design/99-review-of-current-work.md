# Review of the work of 2026-11-21

A critical reading of what was decided and built that day, in order of importance. "Measured" means on this
machine; the session numbers are from `01-token-budget.md`. The owner's decisions (everything in C, knowledge and
tool use learned by training, the engine drives behaviour) are not reopened; the review is about how they were
carried out.

## The state in one paragraph

In one day the project gained a tool set of 44 tools, a default switch to it, six loop mechanisms, three
document formats, a call grammar, a curriculum generator, a knowledge extractor, a five-source training command, a
network API with an agent endpoint and a desktop plan view, with thousands of model-free checks. **None of it has
been measured with a real model**, and the only real-model evidence of the day is a session in which the agent
failed 39% of its calls, hit the round limit in four of six requests and wrote files in two. The volume of
unmeasured mechanism is now the main risk: when the first measurement comes back bad, nobody will know which of
twenty changes caused it.

## 1. The everyday model is not an agent model, and a night was trained on it

The model the owner's desktop ran that afternoon, and the base of the training run that finished at 19:45, is the
"AgentWorld" variant. Its authors describe it as a language *world model*: it is trained to predict the
environment's next state (the output of a terminal, of a tool) given an agent's action and the history. It was
built from the previous generation's base model, not from Qwen3.6, and its card documents no tool-call format and
no use as an assistant [26]. The plan treats it as "Qwen3.6 and its AgentWorld variant".

This fits what the session shows: a model that emits action after action, rarely addresses the user (0 text turns
in two requests), and repeats probes. It does not prove it; the same harness has not been run on Qwen3.6 that day.
But the prior is strong enough to act on:

- Stop training on it until experiment 1 (`90-roadmap.md`) compares the three bases on the same tasks. A
  specialization is tied to its base file by size and parameter count; `tools.kspec` (75 steps, 7.9 hours) is
  usable only on AgentWorld.
- If AgentWorld loses, several of the day's loop mechanisms were built to correct a model that should not have
  been in the loop, and each must be re-justified on Qwen3.6 (which is why L7 in `10-agent-loop.md` makes every
  note switchable).
- Plan item B3 (the comparison) was listed fifth in block B. It is the first thing to do.

## 2. Twenty mechanisms, zero ablations

Plan-first notes, the step reminder, repeat notes, refusal, the blocked-prerequisite note, the streak note, the
round-limit note, the newest-message note, the open-steps reminder, reading notes, kept reading positions, the
closing rule in the prompt: each is reasonable, each was checked with a scripted stand-in, and they interact (three
different notes can land on one result). There is one switch for some and none for most.

- **Sound:** identical call plus identical result as the definition of a repeat matches the published definition
  [89]; refusing the third identical failure is enforcement, not prose (P4). Keep both.
- **Likely to fail:** notes as English paragraphs appended to results. A model that ignores the system prompt will
  ignore a paragraph inside a tool result; in the reference session 5 notes fired in 209 results and the behaviour
  did not visibly change. Where the engine can act, it should act: forbid the call opener for the closing turn
  (L2), refuse the mutating call until a plan exists (L3), instead of asking.
- **Wrong trigger:** "plan at the third call" taxes questions and exploration. The plan belongs before the first
  change (L3).
- **Missing:** the reasoning switch. The desktop runs with reasoning off; the model's authors trained it to reason
  by default and to reuse earlier reasoning in agent use [25]. The one tool a model has to change course after an
  error is turned off, and plan item B1 that would measure it is unstarted.
- **Change:** every note named, switchable and counted in the session file before any more are added.

## 3. Compaction at 6,000 tokens gives the agent amnesia

`cp_trig` compacts when the conversation passes the system prompt plus 6,000 tokens, replacing everything by a
summary of up to 600 tokens that the model writes, plus the last call. In the reference session about 86,000
tokens passed through that window: six files were read 24 times, commands were repeated three times. The constant
was chosen for speed; the speed at depth on the GPU path has never been measured (plan item C5), and the effect on
tasks has never been measured either. Each compaction also costs up to 24 s of generation.

- Published comparisons find that dropping old tool output by rule solves as many tasks as model-written
  summaries at about half the cost, and that summaries lengthen trajectories [13][78].
- What to change: measure the depth curve (C1 in `40-context-and-memory.md`); replace the summary by the engine's
  ledger and masked results (C2, C3); then scopes (C4). Experiment 3.

## 4. The training recipe spends the night in the wrong places

Measured from the run that finished today (`~/.katana/training/tools/train.log`): 75 steps, 381 s per step with
the machine busy, a moving average of the training loss from 2.6 to 1.8, no validation, no final loss, no task
result.

- **No held-out number at all** (`--val 0 --final 0`). Nobody can say whether those 7.9 hours did anything.
  A fixed 20-window validation file and the replay probe cost under an hour (R3, E1).
- **Windows are not chosen for what they teach.** 23 of the 75 windows had fewer than 150 targets and one had 12,
  at the same cost per window. Whole turns are targets, so a file body counts as much as the decision to write it
  (R2).
- **The schedule fights the budget.** Ten warm-up steps and a cosine decay to a tenth: the last third of the run
  was below a quarter of the peak rate. Every resumed night restarts the optimizer and repeats this (R4).
- **Attention-only adapters** are the placement that published work found weakest, on a model of this family [34].
  The shared expert is the available feed-forward target that keeps serving on the GPU (R5).
- **Five sources in one night** (`--mix tools=30,code=30,teacher=20,knowledge=20`) puts 20 to 45 windows on each.
  Published thresholds for the easiest goal (a format) are in the hundreds of examples [71]. One target per night
  (R1).
- **The serving cost of a specialization is unknown.** Adapted matrices become float at load; if they are larger in
  VRAM they displace expert slots. And `katana merge` re-encodes the sum into 4 bits, which can round the update
  away; it was checked on tiny models only for giving the same three answers [47] (R6).
- **Data:** 80% of transcript calls are shell commands on this repository; 96% of reasoning blocks are empty; the
  hindsight plans include dead ends (the specialization document says so itself). The transcripts are a hosted
  provider's output: the terms question is noted three times in the docs and answered nowhere.

## 5. Removing the tools section optimises a cost the cache already removed

The owner's reason is sound in general (declarations should not cost context in every conversation). On this
engine the cost is already gone: the system prompt's state is loaded from disk in 0.05 s (measured) and is 2.7% of
a session's tokens. What the removal buys is about 1,200 tokens of context room. What it risks is the thing that
is failing: 39% of calls. And it makes the prompt depend on a flag inside a trained file, so a specialization that
half-learned the tools silently runs without their descriptions.

- The call grammar already guarantees names and shapes, with or without declarations.
- The middle path costs no training: declare katana's own tools as signature cards (X6 in
  `50-instructions-skills-mcp.md`), a quarter of the size, meaning kept.
- Keep the flag as an experiment with its stated gate (plan item D4), run with the grammar on, after the model can
  be shown to call correctly *with* declarations.

## 6. The knowledge corpus teaches a quiz the agent never takes

`katana knowledge corpus` writes question-answer pairs whose questions name the project ("In project X, what was
done about ..."). Three problems, in order:

1. **Evidence.** No published result puts facts into a model of this size from thousands of examples; the positive
   results use hundreds of millions of synthetic tokens, paraphrase alone saturates, and learning new facts by
   fine-tuning raises hallucination on other questions [38][39][40]. At 20% of a night the corpus gets 20 to 30
   windows; 715 windows is a month of that share.
2. **Form.** The agent does not answer quizzes about projects; it acts in them. A fact is useful as the first
   call it implies (R11). Facts are also only available in the direction trained [42]: the corpus has no reverse
   forms.
3. **Quality and staleness.** The answer is the last message of an episode, whatever it was; commands and
   decisions go stale; a merged model cannot forget.

The extraction itself (notes per session, masked, incremental, 22 s) is good and worth keeping as the source.
The held-out phrasing is the right instinct. What is missing is a control: unseen facts of the same shape, to count
invented answers before and after (experiment 2b).

## 7. The three formats are right in principle and stop one step short

The brief, the ability and the card move meaning from prose to things the engine parses, which is P1 and P4.
Weaknesses:

- The brief still sends the whole file to the model, including `layout:`, which is the repository overview that a
  controlled study found useless and costly, and `/init` asks the model to write more of it [17]. Send one line
  per enforced key; drop `layout:`; stop at detection (X1, X2).
- The end-of-request check exists and is not wired, and as designed it is a reminder. It should be a verdict the
  engine computes (L8, V1).
- Abilities turn steps into a task list the model then executes by typing commands the file already contains.
  Steps that are commands should be run by the engine (X4).
- The card is the best of the three and is used only for external servers. Use it for katana's own tools (X6).
- None of the three was seen by a model. The conventions curriculum will teach formats whose value is unmeasured.

## 8. The tool set became the default on faith

44 tools, 1,244 checks, default "before any measurement" (the tools document's own words). Guidance and
small-model practice point to fewer, consolidated tools [1][67][78]. Its names and arguments were chosen to match
the transcripts of the hosted agent used as training data, which is a real advantage for import and a reason the
set is larger than a local model needs. An 8-tool arm should be measured (T6). The result wrapper around every result is two Portuguese lines in an otherwise English prompt: 9% of the
session's tokens (T1).

## 9. The measuring instruments are not trustworthy yet

- `katana agentlog` reports the tool tests as agent behaviour (3,551 `Grep` calls). Plan items A3 and A4 depend on
  it. Use the session files (`katana tokens`), and close A5 for every test script.
- The session file has no times, so the 86 minutes between model time and wall time in the reference session
  cannot be attributed (tools, compaction, slow generation).
- Codebench limits tasks by wall seconds on a machine that drifts by 30% and runs seven other jobs.
- The first 21 tasks have never separated two configurations.

## 10. Smaller points

- **Web search with passages**: the right shape (one round, bounded bytes), measured on the network side only.
  The passage picker is a word count; the risk is confident answers from the wrong passage. One measurement closes
  it: ten questions with known answers, passages against full pages.
- **Agent API**: two modes are sound. The agent endpoint runs tools on the server's machine for a remote client;
  with confirmations off by default this is remote command execution guarded by one bearer key. Before any use
  beyond loopback: the brief's `never:`/`ask:` rules must apply to API sessions, and the default for API sessions
  should be confirmation on.
- **Round limit of 40 with extension on progress**: fine; "progress" should count a passing verdict, not a file
  written (a model that rewrites the same file forever makes progress by this rule).
- **`Bash` description**: 10 tokens per call for the interface's benefit; let the engine write it (T5).
- **Teacher mode** sends the system prompt, memory, folder map and tool results to a hosted service. The handoff
  says so; the desktop button should say so too.

## What is sound and should not be touched

The single binary. Native call formats with raw strings. The prefix cache with its two levels and the recurrent
state inside it. The decision to enforce rules in the dispatcher. The session file that stores the exact text
(it made this review possible). The model-free test habit. The honesty of the documents about what was not
measured: every gap listed here was already admitted somewhere in `docs/`. What is missing is not candour but
the measurements themselves, and an order in which to take them (`90-roadmap.md`).
