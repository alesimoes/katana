#!/usr/bin/env python3
"""Stand-in for the `claude` program in print mode, to test the teacher through the logged-in session without it.

The agent calls: claude -p --system-prompt-file F --tools '' --json-schema S --output-format json --model M ...
with the conversation on stdin. This script checks the options, the system prompt and the conversation, appends what
it received to $MOCK_CLAUDE_LOG, and prints the result object the real program prints. Scripted teacher: write a
file, read it back, final answer."""
import json, os, sys

a = sys.argv[1:]
log = os.environ.get("MOCK_CLAUDE_LOG")


def fail(why):
    if log:
        open(log, "a").write(json.dumps({"ok": False, "why": why}) + "\n")
    print(json.dumps({"type": "result", "is_error": True, "result": why}))
    sys.exit(1)


def opt(name):
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else None


if "-p" not in a or opt("--output-format") != "json" or opt("--tools") != "" or "--no-session-persistence" not in a:
    fail("options: %s" % a)
schema = json.loads(opt("--json-schema") or "null")
if not schema or set(schema.get("required", [])) != {"text", "tool_calls"}:
    fail("schema")
system = open(opt("--system-prompt-file")).read()
if "<tool_call>" in system or '"name": "write_file"' not in system or '"input_schema"' not in system:
    fail("system prompt")
prompt = sys.stdin.read()
try:
    ms = json.loads(prompt[prompt.index("\n[") + 1:prompt.rindex("]") + 1])
except Exception as e:
    fail("conversation is not JSON: %s" % e)
if not ms or ms[0]["role"] != "user" or ms[-1]["role"] != "user" or any(i and m["role"] == ms[i - 1]["role"] for i, m in enumerate(ms)):
    fail("roles")
last_user_text = max(i for i, m in enumerate(ms) if m["role"] == "user" and any(x["type"] == "text" for x in m["content"]))
step = sum(1 for m in ms[last_user_text + 1:] if m["role"] == "assistant")
if step == 0:
    out = {"text": "Vou gravar o arquivo.", "tool_calls": [{"name": "write_file", "arguments": {"path": "nota.txt", "content": "linha 1\nação \"citada\" <ok>\n"}}]}
elif step == 1:
    out = {"text": "", "tool_calls": [{"name": "run_command", "arguments": {"command": "cat nota.txt", "timeout_s": 20}}]}
else:
    out = {"text": "Pronto: nota.txt gravado e conferido.", "tool_calls": []}
if log:
    open(log, "a").write(json.dumps({"ok": True, "step": step, "model": opt("--model"), "messages": len(ms)}) + "\n")
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": json.dumps(out), "structured_output": out,
                  "usage": {"input_tokens": 10, "cache_read_input_tokens": 5, "cache_creation_input_tokens": 0, "output_tokens": 7}}))
