# katana network API

`katana serve` speaks an OpenAI-compatible HTTP API, so client applications that already talk to a hosted assistant
through that API shape can point at katana instead. It has two modes:

| Mode | Command | What it offers |
|---|---|---|
| Model | `katana serve <model.gguf> [port]` | `GET /v1/models`, `POST /v1/chat/completions` (text, streaming, JSON mode, client-side tools), the legacy `POST /api/chat` and the built-in page at `/` |
| Agent | `katana serve <model.gguf> [port] --agent` | `GET /v1/models` and `/v1/agent/sessions`: a remote client holds a conversation with katana's own agent, which runs its tools **on the machine where the server runs** |

Default port 8080. Code: `src/app/server.c` (HTTP, queue, network rules), `src/app/openai.c` (chat completions),
`src/app/agentapi.c` (agent sessions). Tests: `tools/servertest/oa_unit.c` and `tools/servertest/test_server.py --all ./katana`.

## Network and keys

- The server listens on `127.0.0.1` unless `KATANA_HOST` says otherwise (for example `KATANA_HOST=0.0.0.0`).
- Reachable from the network, it refuses to start without `KATANA_API_KEY` (16 characters or more). Remote clients
  send `Authorization: Bearer <key>` on every path except `/health`.
- When `KATANA_API_KEY` is set, `/v1/...` asks for the key even from the same machine.
- Local requests without a key are refused when their `Host` or `Origin` is not local (a web page from another site
  cannot drive the server).
- Errors on `/v1` come as `{"error": {"message": "...", "type": "invalid_request_error", "code": null}}` with the HTTP
  status: 400 bad request (the message says why), 401 key, 404 unknown path or session, 405 method, 409 conflict
  (agent busy or already active), 413 body too large (8 MB), 503 busy server or wrong mode.

## Model mode

### `GET /v1/models`

```json
{"object": "list", "data": [{"id": "Qwen3.6-35B-A3B-Q4_K_M", "object": "model", "created": 1795299985, "owned_by": "katana"}]}
```

The id is the model file name without `.gguf`. The `model` field of a request is echoed back and otherwise ignored:
the server has one model.

### `POST /v1/chat/completions`

One generation at a time; other requests wait in arrival order. The KV cache of the previous request is reused when
the new prompt starts with the same tokens (a client that resends the whole conversation each turn only pays for the
new part).

Request fields:

| Field | Supported |
|---|---|
| `messages` | roles `system`, `developer` (same as system), `user`, `assistant` (with optional `tool_calls`), `tool` (`tool_call_id`, `content`); `content` as a string or a list of `{"type": "text"}` parts. The last message must be `user` or `tool` |
| `stream`, `stream_options.include_usage` | yes (server-sent events, `data: [DONE]` at the end) |
| `max_tokens`, `max_completion_tokens` | yes (default: the rest of the context) |
| `temperature` (0 to 2), `top_p`, `seed` | yes |
| `stop` | string or list, up to 4; held back across token boundaries, never sent |
| `response_format` | `{"type": "json_object"}` and `{"type": "json_schema", "json_schema": {"schema": {...}}}`, enforced by a grammar |
| `tools`, `tool_choice`, `parallel_tool_calls` | yes, see below |
| `n` | only 1 (anything else is 400) |
| images, audio, files in `content` | no (400) |
| `logprobs`, `top_logprobs`, `logit_bias`, `presence_penalty`, `frequency_penalty`, `user`, `metadata` | ignored |

Plain answer:

```json
{"model": "katana", "messages": [{"role": "user", "content": "What is 2+2?"}], "max_tokens": 200}
```

```json
{"id": "chatcmpl-1a2b3c4d5e6f7a8b", "object": "chat.completion", "created": 1795299985, "model": "katana",
 "choices": [{"index": 0, "message": {"role": "assistant", "content": "4.", "reasoning_content": "Simple sum."},
              "finish_reason": "stop"}],
 "usage": {"prompt_tokens": 14, "completion_tokens": 9, "total_tokens": 23}}
```

`finish_reason` is `stop` (end of turn or a stop string), `length` (token limit) or `tool_calls`.

**Reasoning.** Models that think before answering get their reasoning in `reasoning_content`, apart from `content`:
the `<think> ... </think>` block of ChatML and GLM models, and the thinking channel of Gemma 4
(`<|channel>thought ... <channel|>`, on with `KATANA_THINK=1`). In streaming it comes as `delta.reasoning_content`.
Earlier assistant turns in the history are sent to the model with an empty reasoning block, as the models expect.

Streaming chunks:

```
data: {"id":"chatcmpl-...","object":"chat.completion.chunk","created":1795299985,"model":"katana","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}
data: {"id":"chatcmpl-...", ... "choices":[{"index":0,"delta":{"reasoning_content":"Simple"},"finish_reason":null}]}
data: {"id":"chatcmpl-...", ... "choices":[{"index":0,"delta":{"content":"4."},"finish_reason":null}]}
data: {"id":"chatcmpl-...", ... "choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}
data: [DONE]
```

### Client-side tools

The client declares functions, the model asks for calls, the client runs them and sends the results back. katana
writes the declarations and the whole history in the model's **native** format, with the same writers the agent and
`katana import` use, and reads the calls with the agent's readers:

| Model chat format | Declarations | Call the model writes |
|---|---|---|
| ChatML with XML calls (Qwen3.6) | `# Tools` block with `<tools>` JSON lines in the system turn | `<tool_call><function=name><parameter=k>v</parameter></function></tool_call>` |
| ChatML with JSON calls | same block | `<tool_call>{"name": ..., "arguments": {...}}</tool_call>` |
| Gemma 4 | `<|tool>declaration:name{...}<tool|>` at the end of the system turn | `<|tool_call>call:name{k:<|"|>v<|"|>}<tool_call|>` |
| GLM 4.5+ | `# Tools` block | `<tool_call>name<arg_key>k</arg_key><arg_value>v</arg_value></tool_call>` |

Other chat formats answer 400 when the request carries tools or tool history.

Tool results go back to the model wrapped the way katana's agent wraps them (marked as external data, with the tool
name), so a model specialized on katana's sessions sees the same text.

Request:

```json
{"model": "katana",
 "messages": [
   {"role": "user", "content": "Weather in Rio?"},
   {"role": "assistant", "content": null, "tool_calls": [{"id": "call_1", "type": "function",
     "function": {"name": "get_weather", "arguments": "{\"city\": \"Rio\"}"}}]},
   {"role": "tool", "tool_call_id": "call_1", "content": "sunny, 30 C"},
   {"role": "user", "content": "And in 3 days?"}],
 "tools": [{"type": "function", "function": {"name": "get_weather", "description": "Weather now",
   "parameters": {"type": "object", "properties": {"city": {"type": "string"}, "days": {"type": "integer"}}, "required": ["city"]}}}]}
```

Response:

```json
{"id": "chatcmpl-...", "object": "chat.completion", "created": 1795299985, "model": "katana",
 "choices": [{"index": 0, "message": {"role": "assistant", "content": "I'll check.",
   "tool_calls": [{"id": "call_6b8b4567327b23c6643c9869", "type": "function",
                   "function": {"name": "get_weather", "arguments": "{\"city\": \"Rio\", \"days\": 3}"}}]},
   "finish_reason": "tool_calls"}],
 "usage": {"prompt_tokens": 412, "completion_tokens": 48, "total_tokens": 460}}
```

- Text the model writes before the first call stays in `content` (`null` when there is none); reasoning stays in
  `reasoning_content`.
- `arguments` is a JSON **string**. The XML and GLM formats write every value as text; katana converts a value to the
  type its schema declares (`integer`, `number`, `boolean`, `null`, `array`, `object`) when the text parses as that
  type, and leaves it a string otherwise (also when the schema allows `string`).
- Streaming: content deltas first; then, for each call, one delta with `index`, `id`, `type` and `function.name`
  (with `arguments: ""`) and one delta with the `function.arguments` text; then `finish_reason: "tool_calls"`. The
  calls are sent after the model has finished writing them (they are parsed whole, to type the arguments).
- `tool_choice`: `auto` (default); `none` (the tools are not declared to the model and no `tool_calls` come back);
  `required` and `{"type": "function", "function": {"name": "..."}}` are enforced as far as possible without a
  grammar: the assistant turn already starts with the call opener (and the function name), with reasoning closed.
  The arguments are not constrained by the schema.
- `parallel_tool_calls: false` returns only the first call.
- 400 with the reason: unknown role, a `tool` message whose `tool_call_id` matches no earlier call (or has none),
  `tool_calls` on a non-assistant message, malformed `tools` (not a list, not `type: function`, no or bad
  `function.name`, `parameters` not an object, duplicate names), `tool_choice` naming a function not in `tools`.

## Agent mode: `/v1/agent/sessions`

> **Work in progress.** The web page that uses this mode (`katana serve ... --agent`) is marked WIP: it was checked only against a mock server, not with a real model.

> **Security warning.** This endpoint gives whoever can reach it, or holds the key, the power to run commands, read
> and write files and use the network on the machine where the server runs, with the rights of the user who started
> `katana serve`. The agent does not ask for confirmation (the owner's decision for katana's agent; `KATANA_CONFIRM=1`
> brings confirmations back, and they then arrive as `permission` events). The server refuses to start in this mode
> when it is reachable from the network without `KATANA_API_KEY`. Treat the key like an SSH key.

The built-in web page (`GET /`) uses this endpoint by itself: on load it calls `GET /v1/agent/sessions`; if it answers,
the page drives the agent (tool cards, plan, questions, permissions, context meter), otherwise it stays a plain chat.
A 401 makes it ask for the key (kept in `sessionStorage` only, sent as `Authorization: Bearer`). `tools/web-mock/`
has a stand-in server to look at it without a model.

Server-side tools: the client sends messages; katana's own agent (the loop the desktop app drives) works on the
server machine with its tools and streams what it does.

**How it runs.** Each session is a child process of the server: the same binary, `katana agent <model> --sessao
<file>` with `KATANA_UI=1`, started in the requested folder. The server translates the line protocol the desktop
reads on its pipe into server-sent events. In agent mode the server process does **not** load the model (only the
child does), so `/v1/chat/completions` and `/api/chat` answer 503 "agent mode". One model in memory means **one
session at a time**: creating a second one, or sending a message while the session is working, answers 409.
Linux only (501 on Windows).

Sessions are saved in `~/.katana/api-sessions/<id>.ksess` (the agent's own resumable session file) with the agent's
stderr in `<id>.log` next to it. Creating a session with an existing `session_id` resumes it.

### `POST /v1/agent/sessions`

```json
{"cwd": "/home/me/project", "session_id": "work-1"}
```

Both optional: `cwd` defaults to the server's folder, `session_id` (letters, digits, `_`, `-`, up to 64) to a new
random id. The call returns when the agent is ready (model loaded; minutes for a large model the first time):

```json
{"id": "work-1", "object": "agent.session", "state": "ready", "cwd": "/home/me/project",
 "session_file": "/home/me/.katana/api-sessions/work-1.ksess", "resumed": false, "created": 1795299985,
 "tokens": {"used": 0, "context": 0}, "question": null, "todo": []}
```

### `POST /v1/agent/sessions/<id>/messages`

```json
{"content": "Write notes.txt with today's tasks and show it"}
```

The text is sent as one line (line breaks become spaces, as the desktop does). Text starting with `/` is refused
(those are agent commands). The response is a stream of server-sent events until the request ends:

| Event | Data |
|---|---|
| `text` | `{"text": "..."}`: a piece of the agent's answer |
| `tool_call` | `{"name": "Write", "arguments": "{...}", "description": "..."}`: a tool started (arguments cut at 160 characters, as the desktop shows them) |
| `tool_output` | `{"text": "..."}`: live output of a running command |
| `tool_result` | `{"name": "Write", "output": "..."}`: what the tool returned (long output keeps its start and end) |
| `todo` | `{"items": ["[x] step 1", "[ ] step 2"]}`: the agent's task list |
| `question` | `{"question": "...", "options": ["a", "b"]}`: the agent asks the user; answer with `/answer` |
| `permission` | `{"kind": "...", "what": "...", "detail": "..."}`: only with `KATANA_CONFIRM=1`; answer `yes` or `no` |
| `password_refused` | a sudo password was requested: not supported over the API, the command is refused at once |
| `tokens` | `{"used": 5120, "context": 131072}` |
| `cwd`, `mode`, `notify`, `file` | working folder changes, plan mode, `PushNotification`, `SendUserFile` |
| `status` | `{"marker": "PREP", "args": "..."}`: any other marker of the agent (preparing, reading long input, compacting, teacher) |
| `exit` | `{"exit_code": N}`: the agent process ended (create a new session) |
| `done` | `{}`: the request is over; the session is ready for the next message |

```
event: tool_call
data: {"name":"Write","arguments":"\"arguments\": {\"file_path\": \"notes.txt\", ...","description":""}

event: tool_result
data: {"name":"Write","output":"File created successfully at: notes.txt"}

event: text
data: {"text":"Done: notes.txt has three tasks.\n"}

event: done
data: {}
```

If the client disconnects, the request still runs to its end (use `/interrupt` to stop it).

### `POST /v1/agent/sessions/<id>/answer`

```json
{"answer": "b"}
```

Answers the pending `question` (free text or one of the options) or `permission` (`yes`/`no`, or `true`/`false`).
409 when nothing is waiting. **Passwords cannot be answered remotely**: the server refuses sudo prompts itself.

### `POST /v1/agent/sessions/<id>/interrupt`

Stops the current request, like Ctrl+C in the terminal or Stop in the desktop. 409 when the session is idle.

### `GET /v1/agent/sessions/<id>` and `GET /v1/agent/sessions`

The session object above, with `state` one of `ready`, `busy`, `waiting_answer`, `exited`; the list has zero or one
session.

### `DELETE /v1/agent/sessions/<id>`

Ends the agent (`/exit`, then a kill after 5 s). The session file stays on disk and can be resumed with the same
`session_id`. 409 while busy.

## Not supported

- `n` > 1, images and audio, logprobs, penalties, `logit_bias`; the Responses and Assistants style endpoints;
  embeddings; several models in one server.
- True parallel generation: one request generates at a time.
- Tool arguments constrained by the schema (no grammar for calls yet).
- Several agent sessions at once; passwords over the API; the agent endpoint on Windows.

## Status (2026-11-21)

Checked with tiny random-weight models only (`tools/tinymodel/make_tiny_gguf.py`), unit tests and an
AddressSanitizer build; the tool tests use `KATANA_TEST_RAW_OUTPUT` (a fixed raw model output, since a random model
cannot call a tool) and the agent test uses the scripted stand-in teacher. Not yet run with Qwen3.6 or Gemma 4: a
real client library against each model, a tool round trip with the model choosing the call, the reasoning split with
thinking on, and the agent endpoint driving a real session.
