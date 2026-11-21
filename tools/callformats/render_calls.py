# Gera as mesmas chamadas de ferramenta em cada formato (XML, JSON, Gemma 4) para contar tokens com
# `katana tokenize <modelo> <arquivo>`. Uso: python3 tools/callformats/render_calls.py <pasta>; depois count.sh.
import json, os, sys

out = sys.argv[1] if len(sys.argv) > 1 else "callformats_out"
os.makedirs(out, exist_ok=True)
code = "\n".join(f"def f{i}(x):\n    return x * {i} + \"a\\tb\"  # line {i}" if i % 2 == 0 else f"print(f'value {{f{i-1}(3)}}')" for i in range(27))
code = "\n".join(code.split("\n")[:40])
assert code.count("\n") + 1 == 40
calls = [
    ("Bash", {"command": "cd src && grep -rn \"TODO\" . | wc -l"}),
    ("Bash", {"command": "python3 -m pytest tests/ -x -q", "timeout": 300000}),
    ("Write", {"file_path": "app/main.py", "content": code}),
    ("Edit", {"file_path": "app/main.py", "old_string": "    return x * 2 + \"a\\tb\"  # line 2",
              "new_string": "    return x * 2 + \"a\\tb\"  # fixed\n    # \"quoted\" note"}),
    ("TodoWrite", {"todos": [{"content": c, "status": s} for c, s in [("Read the failing test", "completed"), ("Find the cause in parser.c", "in_progress"),
                   ("Fix the bounds check", "pending"), ("Run the test suite", "pending"), ("Report to the user", "pending")]]}),
    ("AskUserQuestion", {"questions": [{"question": "Which database should the app use?", "options": [
        {"label": "SQLite", "description": "single file, no server"}, {"label": "PostgreSQL", "description": "server, concurrent writes"},
        {"label": "None", "description": "keep JSON files"}]}]}),
    ("Read", {"file_path": "src/agent/loop.c", "offset": 320, "limit": 80}),
    ("Grep", {"pattern": "gr_tools\\(", "path": "src", "output_mode": "content", "-C": 2}),
    ("Glob", {"pattern": "**/*.py"}),
    ("WebSearch", {"query": "sqlite json1 extension syntax"}),
]
G4Q = '<|"|>'


def g4v(v):
    if isinstance(v, str): return G4Q + v + G4Q
    if isinstance(v, bool): return "true" if v else "false"
    if isinstance(v, (int, float)): return str(v)
    if isinstance(v, list): return "[" + ",".join(g4v(x) for x in v) + "]"
    return "{" + ",".join(k + ":" + g4v(x) for k, x in v.items()) + "}"


def xml(n, a):
    s = f"<tool_call>\n<function={n}>\n"
    for k, v in a.items(): s += f"<parameter={k}>\n{v if isinstance(v, str) else json.dumps(v)}\n</parameter>\n"
    return s + "</function>\n</tool_call>"


def js(n, a): return "<tool_call>\n" + json.dumps({"name": n, "arguments": a}) + "\n</tool_call>"
def g4(n, a): return f"<|tool_call>call:{n}" + g4v(a) + "<tool_call|>"


def lines(todos):
    m = {"completed": "[x] ", "in_progress": "[>] ", "pending": "[ ] "}
    return "\n".join(m[t["status"]] + t["content"] for t in todos)


fmts = (("xml", xml), ("json", js), ("g4", g4))
for i, (n, a) in enumerate(calls):
    for f, fn in fmts: open(f"{out}/c{i}_{n}_{f}.txt", "w").write(fn(n, a))
    open(f"{out}/c{i}_{n}_raw.txt", "w").write("\n".join(v if isinstance(v, str) else json.dumps(v) for v in a.values()))
plan = [{"content": c, "status": s} for c, s in [("Read the failing test", "completed"), ("Find the cause in parser.c", "completed"),
        ("Fix the bounds check", "in_progress"), ("Add a regression test", "pending"), ("Run the test suite", "pending"), ("Report to the user", "pending")]]
for f, fn in fmts:
    open(f"{out}/plan_array_{f}.txt", "w").write(fn("TodoWrite", {"todos": plan}))
    open(f"{out}/plan_lines_{f}.txt", "w").write(fn("TodoWrite", {"todos": lines(plan)}))
