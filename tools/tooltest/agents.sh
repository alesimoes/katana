#!/bin/sh
# testes de ListAgents, SendMessage, SubagentHandback e Workflow sem modelo (KATANA_AGENT_FAKE=1)
# uso: sh tools/tooltest/agents.sh ./katana
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
export HOME="$T/home" KATANA_AGENT_FAKE=1; mkdir -p "$HOME"
unset KATANA_AGENT_NAME KATANA_WORKFLOW_MAX_AGENTS
N=0; F=0
has() { N=$((N+1)); if printf '%s' "$2" | grep -qF -- "$3"; then echo "ok   $1"; else echo "FAIL $1: wanted [$3] in:"; printf '%s\n' "$2" | sed 's/^/     /'; F=$((F+1)); fi; }
hasnt() { N=$((N+1)); if printf '%s' "$2" | grep -qF -- "$3"; then echo "FAIL $1: unexpected [$3]"; F=$((F+1)); else echo "ok   $1"; fi; }
run() { "$K" tool "$@" 2>/dev/null; }

o=$(run ListAgents '{}'); has "empty list" "$o" "No agents"
W='{"script":"{\"name\":\"w\",\"phases\":[{\"title\":\"a\",\"agents\":[{\"label\":\"x\",\"prompt\":\"look at {{args}}\"},{\"label\":\"y\",\"prompt\":\"FAIL now\"}],\"then\":\"sum {{results}}\"},{\"title\":\"b\",\"agents\":[{\"prompt\":\"next {{results}}\"}]}]}","args":"foo"}'
o=$(run Workflow "$W" ListAgents '{}' SendMessage '{"to":"agent-1","message":"more please","summary":"s"}' ListAgents '{}' SendMessage '{"to":"agent-9","message":"x"}' SendMessage '{"message":"x"}' SendMessage '{"to":"agent-1"}')
has "wf header" "$o" "[workflow w: 2 phases, 4 sub-agent runs in sequence, 3 ok, 1 failed"
has "wf agent ok line" "$o" "- a / x: ok"
has "wf failure recorded" "$o" "- a / y: failed, 0 s (error: fake sub-agent failure)"
has "wf then line" "$o" "- a / then: ok"
has "wf default label" "$o" "- b / phase2-agent1: ok"
has "wf args + then results + next-phase results" "$o" "fake: next fake: sum ### x fake: look at foo"
has "registry lists agent-1" "$o" "- agent-1  finished  turns: 1  task: look at foo"
has "registry failed" "$o" "- agent-2  failed  turns: 1  task: FAIL now"
has "registry replay note" "$o" "context: replay only (task + last report)"
has "send continues" "$o" "[agent-1 continued from its previous report, not from its full context;"
has "send replay prompt" "$o" "fake: [You are continuing an earlier sub-agent task. Original task"
has "turns grow" "$o" "- agent-1  finished  turns: 2"
has "unknown id" "$o" "error: no agent named agent-9."
has "missing to" "$o" "error: missing to"
has "missing message" "$o" "error: missing message"

o=$(run Workflow '{"script":"export const meta = {name: \"x\"}\nawait agent(\"hi\")"}'); has "js rejected" "$o" "error: this is JavaScript; katana has no JavaScript interpreter"
has "js example" "$o" '{"name": "review", "phases": ['
o=$(run Workflow '{"script":"const x = 1"}'); has "not json" "$o" "error: script is not a JSON object"
o=$(run Workflow '{}'); has "no source" "$o" "error: give script (inline JSON), scriptPath or name"
o=$(run Workflow '{"script":"{\"name\":\"n\"}"}'); has "no phases" "$o" 'error: the workflow needs a "phases" array'
o=$(run Workflow '{"script":"{\"phases\":[{\"agents\":[{\"label\":\"a\"}]}]}"}'); has "no prompt" "$o" 'error: agent 1 of phase 1 has no "prompt" text'
o=$(run Workflow '{"script":"{\"phases\":[{\"agents\":[]}]}"}'); has "empty agents" "$o" "error: phase 1 has no agents"
o=$(run Workflow '{"script":"{\"phases\":[]}"}'); has "empty phases" "$o" "error: phases is empty"
o=$(run Workflow '{"scriptPath":"nope.json"}'); has "missing file" "$o" "error: cannot read workflow nope.json"
A='{\"prompt\":\"p\"}'
o=$(run Workflow "{\"script\":\"{\\\"phases\\\":[{\\\"agents\\\":[$A,$A,$A,$A,$A,$A,$A,$A,$A,$A,$A]}]}\"}"); has "cap 10" "$o" "needs 11 sub-agent runs (agents plus then steps); the limit is 10"
hasnt "cap runs nothing" "$o" "fake:"
o=$(KATANA_WORKFLOW_MAX_AGENTS=2 "$K" tool Workflow "{\"script\":\"{\\\"phases\\\":[{\\\"agents\\\":[$A,$A],\\\"then\\\":\\\"t\\\"}]}\"}" 2>/dev/null); has "cap env" "$o" "the limit is 2"
mkdir -p .katana/workflows
printf '{"name":"saved","phases":[{"agents":[{"prompt":"from file {{args}}"}]}]}' > .katana/workflows/saved.json
o=$(run Workflow '{"name":"saved","args":{"k":1}}'); has "by name, args object" "$o" 'fake: from file {"k":1}'
o=$(run Workflow '{"scriptPath":".katana/workflows/saved.json","args":"z"}'); has "scriptPath" "$o" "fake: from file z"
o=$(run Workflow '{"script":{"phases":[{"agents":[{"prompt":"inline object"}]}]}}'); has "script as object" "$o" "fake: inline object"

o=$(run SubagentHandback '{"message":"r"}'); has "handback outside sub" "$o" "error: SubagentHandback is only for sub-agents"
o=$(run Workflow '{"script":"{\"phases\":[{\"agents\":[{\"prompt\":\"HANDBACK please\"}]}]}"}'); has "handback inside sub is the answer" "$o" "handed back: HANDBACK please"

# caixa de correio entre dois processos
KATANA_AGENT_NAME=bob "$K" tool ListAgents '{"_test":"serve","_wait":10}' > srv.out 2>/dev/null &
SP=$!
i=0; while [ $i -lt 50 ] && ! grep -q registered srv.out 2>/dev/null; do sleep 0.1; i=$((i+1)); done
o=$(run ListAgents '{}'); has "lists other session" "$o" "name: bob  folder: $T"
o=$(run SendMessage '{"to":"bob","message":"hello bob","summary":"greeting"}'); has "send to session" "$o" "Message sent to session $SP (bob): greeting"
wait $SP
o=$(cat srv.out); has "target took the message" "$o" "[message from katana-"
has "message body" "$o" "hello bob"
o=$(run ListAgents '{}'); hasnt "unregistered on exit" "$o" "bob"
o=$(run SendMessage "{\"to\":\"$SP\",\"message\":\"x\"}"); has "gone session" "$o" "error: no agent named $SP"
# registro de pid morto e apagado
mkdir -p "$HOME/.katana/mail/agents" "$HOME/.katana/mail/999999"; printf 'ghost\n/x\n' > "$HOME/.katana/mail/agents/999999"; touch "$HOME/.katana/mail/999999/a.msg"
o=$(run ListAgents '{}'); hasnt "stale not listed" "$o" "ghost"
N=$((N+1)); if [ ! -e "$HOME/.katana/mail/agents/999999" ] && [ ! -e "$HOME/.katana/mail/999999" ]; then echo "ok   stale cleaned"; else echo "FAIL stale cleaned"; F=$((F+1)); fi
o=$(run ListAgents '{"_test":"take"}'); has "empty mailbox" "$o" "no message"

if [ $F -gt 0 ]; then echo "FAILED $F"; exit 1; fi
echo "OK $N checks"
