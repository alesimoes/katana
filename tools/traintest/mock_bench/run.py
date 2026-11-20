#!/usr/bin/env python3
"""Stand-in for tools/codebench/run.py, to test katana learn without a real model (tools/traintest/run.sh).

Same command line as the real harness for what katana learn uses. Each run starts the real katana agent on the tiny
model (so the sessions are real .ksess files), but "passed" is decided here: in collection, every other attempt
passes; in evaluation, 1 run passes on the base model and 2 when a specialization (KATANA_SPEC) is loaded. So round 1
must be accepted and round 2 must not (2 is not more than 2)."""
import argparse, json, os, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("--model"); ap.add_argument("--out"); ap.add_argument("--tasks"); ap.add_argument("--katana")
ap.add_argument("--timeout", type=int, default=120); ap.add_argument("--label", default="run")
ap.add_argument("--env", action="append"); ap.add_argument("--sessions"); ap.add_argument("--repeat", type=int, default=1)
a = ap.parse_args()
env = dict(os.environ)
for kv in a.env or []:
    k, _, v = kv.partition("=")
    env[k] = v
spec = env.get("KATANA_SPEC", "")
names = a.tasks.split(",")
n = 0
for k in range(a.repeat):
    for t in names:
        rec = dict(label=a.label, task=t, attempt=k)
        if a.sessions:
            os.makedirs(a.sessions, exist_ok=True)
            sess = os.path.abspath(os.path.join(a.sessions, "%s.%d.ksess" % (t, k)))
            subprocess.run([a.katana, "agent", a.model, "--sessao", sess], input="task %s, say ok\n/exit\n" % t,
                           capture_output=True, text=True, errors="replace", timeout=a.timeout, env=env)
            rec["session"] = sess if os.path.exists(sess) else None
            rec["passed"] = (n % 2) == 0
        else:
            rec["passed"] = n < (2 if spec else 1)
        n += 1
        open(a.out, "a").write(json.dumps(rec) + "\n")
print("%s: %d runs" % (a.label, n))
