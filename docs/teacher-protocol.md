# Teacher protocol

Teacher mode lets a larger model answer in place of the local one, inside the same agent. The katana binary knows no
vendor, URL or model name: it runs an **external program** (the teacher plugin) and talks to it with the protocol
below. Everything that does not depend on the vendor stays in the binary (`src/agent/teacher.c`): rewriting the answer
in the local model's syntax, writing it to the history and the KV cache, noting it in `<session>.teacher`,
`katana export --teacher`, switching the teacher on and off in the middle of a conversation.

## Turning it on

| What | How |
|---|---|
| On / off | `/teacher on`, `/teacher off`, `/teacher` (toggle), `KATANA_TEACHER=1` at start |
| Status | `/teacher status` (model option and token counts of this run) |
| Model option | `/teacher <name>` (turns it on and sends `options.model = <name>`), or `KATANA_TEACHER_MODEL` |
| Effort option | `KATANA_TEACHER_EFFORT` (sent as `options.effort`; the values mean whatever the plugin says) |
| Token limit | `KATANA_TEACHER_MAX_TOKENS` (default 32000, sent as `options.max_tokens`) |

Which program runs (first match wins):

1. `KATANA_TEACHER_CMD`: a command line, run with `sh -c`.
2. The first line of `~/.katana/teacher.cmd`: same.
3. The executable `~/.katana/plugins/teacher/run`.

With none of them, `/teacher on` refuses and says how to configure one. The program inherits the environment of the
agent, so plugin settings (address, key) are plain environment variables of the plugin; katana does not read them.
What leaves the machine depends on the plugin: katana hands it the agent's system prompt (with the saved memory and
the folder map), the whole conversation and the tool results.

`KATANA_TEACHER_DUMP=<file>` writes the last request line to a file (debugging).

## One turn

For every turn of the agent, katana starts the program once (working directory: a fresh temporary folder), writes
**one JSON line** to its stdin, closes stdin, and reads its stdout line by line until it exits (limit: 15 minutes).
Stderr is kept and shown only when the program fails. One program run = one agent turn; the program keeps no state
between turns (katana sends the whole conversation each time).

### Request (stdin, one line)

```json
{"protocol": 1,
 "system": "text: the agent's system prompt, without the student's own call syntax",
 "tools": [{"name": "write_file", "description": "...", "parameters": {"type": "object", "properties": {...}, "required": [...]}}],
 "messages": [
   {"role": "user", "content": [{"type": "text", "text": "write a note"}]},
   {"role": "assistant", "content": [
       {"type": "opaque", "data": <whatever the plugin returned as "opaque" for this turn>},
       {"type": "text", "text": "I will write it."},
       {"type": "tool_call", "id": "kt_1_0", "name": "write_file", "arguments": {"path": "nota.txt", "content": "..."}}]},
   {"role": "user", "content": [{"type": "tool_result", "id": "kt_1_0", "content": "ok: wrote 17 bytes"}]}],
 "options": {"max_tokens": 32000, "model": "optional", "effort": "optional"}}
```

Guarantees about `messages`: the first and the last message are `user`; roles alternate; every `tool_call` is answered
by a `tool_result` with the same `id` in the next message (a missing result is filled with a short placeholder text);
no message is empty. `tool_result.content` is always a string. `tool_call.id` values are assigned by katana
(`kt_<message index>_<n>`), never taken from the plugin. The conversation may contain turns written by the local
model. `tools` lists the tools the agent allows right now (including MCP tools); the local sub-agent tool is left out.
`parameters` is a JSON Schema object. `options.model` and `options.effort` are present only when set.

An `opaque` block is present only in assistant messages of the current run for which the plugin returned `opaque` in
its `done` event; it is returned unchanged (use it for data the vendor wants echoed back, such as signed reasoning).
After a restart of the agent it is gone; a plugin must work without it.

### Response (stdout, JSON Lines)

One JSON object per line; lines that are not JSON objects are ignored. Field `event` selects the kind:

| event | fields | meaning |
|---|---|---|
| `thinking` | `text` | a piece of the teacher's reasoning (optional). Printed as it arrives |
| `text` | `text` | a piece of the answer text. Printed as it arrives |
| `progress` | `bytes` | optional: how much of a long tool call has been written so far; the desktop shows it (at most one per second is used) |
| `done` | see below | the whole turn. **Required** exactly once, last |
| `error` | `message` | the turn failed; the program should then exit non-zero. Shown to the user; nothing is done in this step |

`done`:

```json
{"event": "done",
 "text": "the full answer text (may be empty when only calling tools)",
 "thinking": "the full reasoning summary (optional)",
 "tool_calls": [{"id": "ignored", "name": "write_file", "arguments": {"path": "nota.txt", "content": "..."}}],
 "stop": "end_turn",
 "usage": {"input": 100, "output": 20, "cached": 50},
 "opaque": null}
```

- `text` and `thinking` in `done` are the truth; if absent, katana uses the concatenation of the streamed pieces.
  Streaming is optional: a program may send only `done`. If pieces were already printed, katana prints only what is
  left (the tool calls).
- `tool_calls[].arguments` must be a JSON object. Entries without `name` are ignored. `id` is ignored.
- `stop`: `end_turn` (default), `max_tokens` (the last tool call may be incomplete: katana drops it and marks the
  answer as cut), or `refusal`. A turn with no text and no tool calls counts as no answer.
- `usage`: all optional; `input` counts everything sent, `cached` the part served from a cache.
- `opaque`: any JSON value, kept in memory for this run and sent back in the next requests (see above).

If the program exits without a `done` event, katana reports `error.message`, or the stderr text, or "ended without a
done event", and the agent step does nothing (the user can send the message again or turn the teacher off).

### Minimal program (sh)

```sh
#!/bin/sh
req=$(cat)
case "$req" in
  *'"type": "tool_result"'*) echo '{"event":"done","text":"Done.","tool_calls":[]}' ;;
  *) echo '{"event":"done","text":"Writing.","tool_calls":[{"name":"write_file","arguments":{"path":"a.txt","content":"hi\n"}}]}' ;;
esac
```

## Training data

Each teacher answer is written to the session as if the local model had produced it (reasoning block, text, calls in
the local syntax) and noted in `<session>.teacher` (`message index`, `length`, `model option or "teacher"`).
`katana export out.jsonl <sessions> --teacher` and `katana specialize <model> teacher` use only those answers.

## Plugins

- `plugins/teacher-anthropic/`: a plugin for the Anthropic Messages API and, optionally, the `claude` command line
  program. See its README for installation and for the mapping of the old `KATANA_TEACHER_*` settings.

Tests: `tools/traintest/mock_teacher_prog.py` is a scripted teacher speaking this protocol (used by
`tools/traintest/run.sh`, `tools/tooltest/*` and `tools/servertest`); the plugin has its own mock server in
`plugins/teacher-anthropic/test/`.
