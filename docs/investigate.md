# Code investigation: evaluation bench and training data factory

Problem (owner, 2026-11-22): the local agent investigates code badly. It reads whole files, forms no hypothesis,
repeats the same read in a loop and does not reach the cause. A strong investigator works differently: hypothesis, a
SHORT query (`Grep -C 2`, `Read` with `limit` of at most 40, `Glob`), "what I learned", the next hypothesis following
references (who registers this, who lists it, where the data is stored), then an answer with `file:line`.

`tools/investigate/` holds (A) a bench that measures this behavior and (B) a factory of training examples with it. These
are research tools in Python; the "everything in C" rule applies to the engine, not to them. They never change `src/`.

## (A) Evaluation

- `questions.json`: 76 questions in the owner's style (Portuguese) over three local repositories (greymod 18, katana 22,
  sonora-test 36). Each has `id`, `repo`, `base` (the commit to check out), `question`, `answer_files` (answer key),
  `min_files` (how many key files the answer must cite), `keywords` (regexes, `|` separates alternatives; the cause must
  match all of them, or `min_keywords` of them), a `note` saying what the cause is, `split` (`train` or `val`) and, for
  the 64 added questions, `fix_commit` (audit only) and `area` (folder of the cause). Except for the mandatory first
  question (greymod, why `gkatana` does not show up in the Grey phone), every case comes from a fix commit: the commit
  message is the symptom, the commit's files are where the cause is, the base is the PARENT commit, and the question is
  chosen so that the cause is NOT in the files where the question's words appear (several hops).
- Split: `train` = 52 (greymod 13, katana 14, sonora 25; includes the 10 pilot questions) and `val` = 24 (greymod 5,
  katana 8, sonora 11; the 2 original questions not used in the pilot plus 22 new ones). Validation questions come from
  commits and folders that no training question uses, except where the original 12 already overlapped (sonora
  `ui/qt/qml` and `plugins/host`, which have train questions that were frozen before the split). Several pairs of
  questions that touch the same code (callgrammar, MCP, the harness) were put on the same side.
- Target was about 85 questions; 76 survived. The histories are thin: most katana commits are features, not fixes, and
  many sonora commits are tests, refactors or merges. Candidates with a trivial single obvious file, a duplicate of
  another question's code, more than 5 code files, or a cause that does not exist in the parent were discarded.
- `verify_questions.py [--file F] [--split S] [id...]`: mandatory check, scripted. For each question: (1) every key file
  exists in BASE (`git show BASE:path`); (2) the keywords match in at least one key file AT BASE; (3) the question does
  not contain a key file's basename; (4) a detached worktree of BASE can be created (tested without checkout and
  removed). It also checks `fix_commit`'s parent is the base and that the commit changes the key files, and warns when the
  question text already matches a cause keyword. The 3 original questions that fail on purpose-built checks
  (`grey-gkatana`, `grey-vpn-loop`, `sonora-bounce-loop`) are kept as they were and reported as legacy warnings.
- `setup_repos.py [--split train|val|all] [--base-dir DIR] [--du] [id...]`: creates only the missing copies, as detached
  worktrees at the base commit under `--base-dir` / `$INVEST_REPOS` (default `/mnt/dados/investigate-repos`). The source
  repositories are only used to register the worktrees. All 76 copies together take 3.6 GB (full checkouts; a sonora copy
  is about 80 MB of files plus git metadata). To free space: `git -C <repo> worktree remove --force <dir>`; they are
  recreated on demand (`eval.sh` calls `setup_repos.py` for each question).
- `eval.sh [--split train|val|all] [id...]` (`--split val` measures only the validation questions): runs `katana -p "<question>" --cwd <copy> --allowed-tools "$TOOLS" --max-turns 30 --no-session` with
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
export INVEST_REPOS=/mnt/dados/investigate-repos INVEST_TRACES=/some/traces
python3 tools/investigate/verify_questions.py
python3 tools/investigate/setup_repos.py --split all --du
# validation only: tools/investigate/eval.sh --split val
# ... investigate with runtool.py ...
python3 tools/investigate/build_dataset.py --model models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf --out pilot.jsonl $INVEST_TRACES/*.jsonl
katana agentsys > sys.txt
katana import models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf pilot.jsonl pilot_data.jsonl --system file:sys.txt
katana curriculum windows models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf pilot_data.jsonl
```

`pilot.jsonl` is the pilot: 10 real investigations done by a strong model under this protocol (traces in `traces/`).
