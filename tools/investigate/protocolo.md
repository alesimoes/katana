# Investigator protocol

You answer a question about a code repository (the copy lives in `$INVEST_REPOS/<id>`) the way an experienced
engineer does: with hypotheses and short queries, never by reading everything. Every query goes through `runtool.py`,
which runs the engine's REAL tool and records the trace. Do not use cat, sed, grep, ls or any other way to read files:
the training data is only valid if every result is the engine's own.

## Commands (QID = question id in questions.json; run from tools/investigate)

    python3 runtool.py QID hyp "..."        # BEFORE each query: what you expect to find and where
    python3 runtool.py QID call Grep '{"pattern":"x","output_mode":"content","-C":2}'
    python3 runtool.py QID call Read '{"file_path":"src/a.c","offset":100,"limit":40}'
    python3 runtool.py QID call Glob '{"pattern":"bridge/*.mjs"}'
    python3 runtool.py QID learned "..."    # AFTER each query: the fact the result gave you
    python3 runtool.py QID answer "..."     # final answer (or answer @file.txt)

Set `INVEST_REPOS` (copies) and `INVEST_TRACES` (traces) first. `Find` and map-aware `Read` join this set when the
engine has them (add them to `TOOLS` in runtool.py and to `SCHEMA` in common.py).

## Method

0. **Map first (one or two queries).** Projects often keep a map of themselves: `KATANA.md`, `AGENTS.md`, `CLAUDE.md`, `README.md`,
   `docs/`. Before searching the code, look for the map with one `Glob` (a narrow pattern at the repository root, for
   example `*.md`) and read the part of it that describes the area of the question (a `Grep -C 2` for a word of the question
   inside that file, or a `Read` of at most 60 lines). Write what the map told you in `learned`. If the project has no
   map, say so in one `learned` line and go on. The map names folders and files; use it to choose the first real hypothesis.
1. Read the question. Separate the symptom from the cause. The cause is rarely where the words of the question appear.
2. **Hypothesis**: one short sentence (at most 140 characters, in Portuguese, the owner's language) saying what you expect
   to find and where. Record it with `hyp`.
3. **One short query**:
   - Grep with `"output_mode":"content"` and `-C` 1 or 2 (at most 3); use `glob` or `path` to limit the files. Start with
     `files_with_matches` or `count` when you do not know where a term lives. Line numbers come by default (do not pass `-n`).
   - Read always with `offset` and `limit` (at most 80; normally 20 to 40), centered on a line a Grep showed.
   - Glob with a narrow pattern (a folder prefix), never `**/*word*` over the whole repository.
4. **Learned**: one short sentence (at most 140 characters) with the fact the result brought. Record it with `learned`.
5. **Next hypothesis** following references: who registers this, who lists it, who calls it, where the data is stored,
   where the condition is decided. Every jump comes from what you just learned.
6. Stop when the cause is confirmed in code you actually read (not assumed). Usually 4 to 10 queries.
7. **Final answer** (Portuguese, addressed to the project owner): the cause in 2 to 6 sentences with `file:line` for each
   claim, and what has to be done. Say "provavelmente" for anything the queries did not show. Change nothing in the repository.

## Hard rules

- Never repeat an identical query (runtool refuses it). For another slice of the same file, change offset/limit.
- Never read a whole file (a Read without `limit`, or with `limit` above 80, is refused).
- Arguments must match the short tool schema (Read: file_path, offset, limit; Grep: pattern, path, glob, output_mode,
  -i, -C, literal; Glob: pattern, path). Anything else is refused.
- Keep the total trace (question, queries, results, answer) under 12000 tokens: prefer queries that return little.
- The answer must cite the files holding the cause (the answer key in questions.json is for checking AFTERWARDS).
- No magic jumps: every hypothesis follows from an earlier "learned" (only the first comes from the question).

## Afterwards

    python3 build_dataset.py --model <qwen.gguf> --out pilot.jsonl traces/*.jsonl

validates (answer key cited, no repeated call, at most 12000 tokens, Read limit at most 80, arguments against the
schema, `file:line` in the answer) and converts to training lines. See `docs/investigate.md`.
