# Code investigation: evaluation bench and training data factory

Problem (owner, 2026-11-22): the local agent investigates code badly. It reads whole files, forms no hypothesis,
repeats the same read in a loop and does not reach the cause. A strong investigator works differently: hypothesis, a
SHORT query (`Grep -C 2`, `Read` with `limit` of at most 40, `Glob`), "what I learned", the next hypothesis following
references (who registers this, who lists it, where the data is stored), then an answer with `file:line`.

`tools/investigate/` holds (A) a bench that measures this behavior and (B) a factory of training examples with it. These
are research tools in Python; the "everything in C" rule applies to the engine, not to them. They never change `src/`.

## (A) Evaluation

- `questions.json`: 12 questions in the owner's style (Portuguese) over three local repositories (greymod, katana,
  sonora-test). Each has `id`, `repo`, `base` (the commit to check out), `question`, `answer_files` (answer key),
  `min_files` (how many key files the answer must cite), `keywords` (regexes, `|` separates alternatives; the cause must
  match all of them, or `min_keywords` of them) and a `note` saying what the cause is. Except for the mandatory first
  question (greymod, why `gkatana` does not show up in the Grey phone), every case comes from a fix commit: the commit
  message is the symptom, the commit's files are where the cause is, the base is the PARENT commit, and the question is
  chosen so that the cause is NOT in the files where the question's words appear (several hops).
- `setup_repos.py [id...]`: creates each copy as a detached worktree at the base commit under `$INVEST_REPOS`
  (default `/tmp/katana-investigate`). The source repositories are only used to register the worktrees.
- `eval.sh [id...]`: runs `katana -p "<question>" --cwd <copy> --allowed-tools "$TOOLS" --max-turns 30 --no-session` with
  its own `KATANA_LOG` and a timeout (`TIMEOUT`, default 900 s), one question after another, and prints one line per
  question: calls, summed input tokens (`entrada: N tokens` of stderr), largest context, identical repeated calls,
  verdict (`PASSOU` when the answer cites at least `min_files` key files and matches the keywords, `FALHOU`, `SEM RESPOSTA`),
  files cited, keywords matched and seconds. `TOOLS` defaults to `Read,Grep,Glob` (set `TOOLS=Read,Grep,Glob,Find` when
  the engine has `Find`). Raw files (`<id>.out`, `.err`, `.log`) stay in `EVAL_OUT` (default `./eval-out/<date>`).
  No `pkill`/`pgrep` by pattern is used anywhere: the timeout owns its child.
- `score.py`: the measuring part of `eval.sh`; it can score a run made by hand: `score.py ID LOG ERR OUT SECONDS`.

Baseline (2026-11-22, Qwen3.6-35B-A3B, greymod question, `Read,Grep,Glob`): 8 calls, 11914 input tokens summed, largest
context 13176, 0 repeated calls, FALHOU (cites 1 of 3 key files, none of the cause), 238 s. The agent globbed for the two
command names, read the Windows installer, assumed the problem was a missing Windows script and never looked at how the
bridge lists sessions. It is a measurement of the agent as it is, not something the bench tries to improve.

## (B) Training data factory

Flow: an investigator (a strong model, or anyone) answers a question using ONLY `runtool.py`; the trace becomes one
training conversation. The owner lifted the old restriction on where training data comes from: transcripts and a
teacher model may be used, as long as the result works.

- `runtool.py QID call <Tool> '<json>'`: runs the engine's real tool (`katana tool <Name> '<json>'`, no model) with the
  working folder set to the question's copy, records call and result in a JSONL trace and prints the result. It refuses
  an identical repeated call, a `Read` without `limit` (or above 80) and arguments outside the short tool schema.
  `hyp "..."`, `learned "..."` and `answer "..."` record the investigator's text in the same trace.
- `protocolo.md`: the instructions for the investigator (method, hard rules).
- `build_dataset.py`: validates traces and writes `{"messages": [...]}` lines for `katana import`. Each assistant turn
  before a call carries the short text "what I learned from the previous result + the hypothesis for this call" (each
  part at most 140 characters, Portuguese); the call is a `tool_calls` entry; the result is a `tool` message; the last
  assistant message is the answer. The system prompt is omitted: `katana import ... --system file:F` puts the agent's
  (`katana agentsys > F`). The copy's path becomes `/work/<repo>`. A trace enters only if: the answer cites the key (same
  rule as `score.py`) and has a `file:line`; no call repeats; every call has a hypothesis and a "learned"; the summed
  tokens are at most 12000 (katana's tokenizer with `--model`, else characters / 3.3); every `Read` has a `limit` of at
  most 80; arguments fit the schema.

```
export INVEST_REPOS=/some/dir INVEST_TRACES=/some/traces
python3 tools/investigate/setup_repos.py
# ... investigate with runtool.py ...
python3 tools/investigate/build_dataset.py --model models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf --out pilot.jsonl $INVEST_TRACES/*.jsonl
katana agentsys > sys.txt
katana import models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf pilot.jsonl pilot_data.jsonl --system file:sys.txt
katana curriculum windows models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf pilot_data.jsonl
```

`pilot.jsonl` is the pilot: 10 real investigations done by a strong model under this protocol (traces in `traces/`).
