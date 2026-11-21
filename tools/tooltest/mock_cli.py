#!/usr/bin/env python3
"""Stand-in for the teacher program, only to see what the agent sends as the user's message (tools/tooltest/extend.sh).
Appends the text of the last user message to $MOCK_LOG and answers 'seen'."""
import json, os, sys

prompt = sys.stdin.read()
try:
    ms = json.loads(prompt[prompt.index("\n[") + 1:prompt.rindex("]") + 1])
    txt = " ".join(x.get("text", "") for x in ms[-1]["content"] if x.get("type") == "text")
except Exception as e:
    txt = "PARSE ERROR %s" % e
open(os.environ["MOCK_LOG"], "a").write(json.dumps({"user": txt}) + "\n")
out = {"text": "seen", "tool_calls": []}
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": json.dumps(out), "structured_output": out,
                  "usage": {"input_tokens": 1, "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0, "output_tokens": 1}}))
