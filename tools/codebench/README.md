# codebench

A small local coding benchmark for the katana agent. It exists to answer one question with numbers: does a change
(a model, a prompt, a sampling preset, an adapter) make the agent solve more real coding tasks?

- 15 tasks in `tasks/`, Python standard library only: 12 bug fixes (one spans two modules) and 3 features written from
  a stub (a calculator, a persistent to-do list, a duration parser).
- Each task has `instruction.md` (the user's request), `repo/` (the starting project), `hidden/` (unit tests the agent
  never sees) and `solution/` (a reference fix used only by `--verify`).
- The agent starts in a temporary copy of `repo/`, gets the instruction as its first message, and works with its normal
  tools. Then `hidden/` is copied in and run with `python3 -m unittest`. Pass or fail, wall time, tool calls and
  generated tokens are appended to a JSONL file.

```
python3 tools/codebench/run.py --verify                      # every task must fail before and pass after the reference fix
python3 tools/codebench/run.py --model models/X.gguf --out a.jsonl --label base
python3 tools/codebench/run.py --model models/X.gguf --out b.jsonl --label changed --env KATANA_THINK=1
python3 tools/codebench/run.py compare a.jsonl b.jsonl      # newly solved / lost, exact sign test, mean cost
```

`--tasks a,b` runs a subset, `--timeout` is per task (default 900 s), `--keep DIR` keeps each task's agent log and final
files. Run one model at a time: the GPU and the memory are shared.

Limits to keep in mind: 15 tasks is a small sample, so only large differences mean something (the sign test prints the
p-value); the tasks are small and in one language; the agent can read the visible code and write its own tests, but
never sees `hidden/`.
