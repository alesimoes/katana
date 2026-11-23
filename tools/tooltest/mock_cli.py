#!/usr/bin/env python3
"""Stand-in for the teacher program, only to see what the agent sends as the user's message (tools/tooltest/extend.sh).
Appends the text of the last user message to $MOCK_LOG and answers 'seen' (teacher protocol, docs/teacher-protocol.md)."""
import json, os, sys

try:
    ms = json.loads(sys.stdin.readline())["messages"]
    txt = " ".join(x.get("text", "") for x in ms[-1]["content"] if x.get("type") == "text")
except Exception as e:
    txt = "PARSE ERROR %s" % e
open(os.environ["MOCK_LOG"], "a").write(json.dumps({"user": txt}) + "\n")
print(json.dumps({"event": "done", "text": "seen", "tool_calls": [], "stop": "end_turn", "usage": {"input": 1, "output": 1, "cached": 0}}))
