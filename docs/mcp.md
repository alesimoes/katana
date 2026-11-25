# MCP client and server: protocol coverage

Basics (stdio and streamable HTTP transports, tools, prompts, resources, the card in the system prompt, `katana mcp ...`
commands, trust of project files) are in `docs/extending.md`. This document covers the rest of the protocol.

## Protocol revisions

katana speaks the stateful handshake (`initialize` / `notifications/initialized`) and offers protocol version
`2025-11-25`; a server that answers with an older version (`2025-06-18`, `2025-03-26`) is accepted and the negotiated
version is sent in `MCP-Protocol-Version` on HTTP. The specification read for this work is the current one,
revision `2026-07-28` (https://modelcontextprotocol.io/specification/2026-07-28), together with the `2025-11-25` revision
it follows. Revision `2026-07-28` is stateless (no `initialize`, no `Mcp-Session-Id`, `server/discover`, multi round-trip
requests instead of server-initiated requests) and deprecates roots, the HTTP+SSE transport and dynamic client
registration, and removes `Last-Event-ID` resumption and `notifications/roots/list_changed`. The features below that
those revisions deprecate or remove (roots, SSE, session resume, dynamic registration) are implemented for servers that
negotiate `2025-11-25` or older, which is what servers in use speak today. Not implemented: the stateless handshake,
`server/discover`, `subscriptions/listen`, multi round-trip results (`resultType: "input_required"`), and the tasks
extension. A server that only speaks `2026-07-28` is not reachable yet.

## Variables in the configuration (`mcp.json`, `.mcp.json`)

`command`, `args`, `env` values, `url` and `headers` values accept `${VAR}` and `${VAR:-default}`. The default is used when
the variable is unset or empty; `${VAR}` alone with an unset variable is an error; a variable that is set but empty is
accepted as empty when there is no default. `$${` writes a literal `${`. Names are letters, digits and `_`.

A server with a missing variable is skipped: it is listed as unavailable with the variable's name and the field
(`environment variable GH_TOKEN is not set ... (field env)`), nothing is started, and the other servers are not affected.
Values are never printed: only names appear in messages, `katana mcp list` and `mcp.log`.

```json
{"mcpServers": {
  "issues": {"command": "${TOOLS_DIR:-/opt/tools}/issues-server", "env": {"TOKEN": "${ISSUES_TOKEN}"}},
  "wiki":   {"url": "https://${WIKI_HOST}/mcp", "headers": {"Authorization": "Bearer ${WIKI_TOKEN}"}}
}}
```

Tests: `tools/tooltest/mcpx.sh` (fake servers in `tools/tooltest/mcpfake.c`, no network).

## Roots

Declared in `initialize` as `{"roots":{"listChanged":true}}`. A `roots/list` request from the server is answered with one
root: the agent's working folder as a `file://` URI (percent-encoded) and the folder name. When the working folder changes
(`/cd`, entering or leaving a worktree) every connected server gets `notifications/roots/list_changed`; the check also runs
before each tool call, so a change made another way is announced before the next call. Revision `2026-07-28` deprecates
roots and removes the list-changed notification; katana still offers roots to servers that negotiate `2025-11-25` or older.
Over HTTP the server's request arrives in the response stream of the call and katana answers with a separate POST; the
response stream is now read as it arrives (`curl -N`), which also lets a server ask something before it answers.

Hidden test aid: `katana toolrun` accepts a line `!cd <dir>` that changes the folder like `/cd` does.

## List changes: tools, resources, prompts

`notifications/tools/list_changed` reloads the tool list (already supported). `notifications/resources/list_changed` and
`notifications/prompts/list_changed` are now handled too, from stdio and from an HTTP response stream: the prompt list is
reloaded (so `/server:prompt` sees the current prompts) and the result of the call that was running says what changed,
so the model knows to list the resources again (resources are listed on each request, there is no cache to refresh).
A notification that arrives while no call is running is recorded and applied at the next call or `/server:prompt`.

## Tool annotations

`tools/list` entries may carry `title` and `annotations` (`title`, `readOnlyHint`, `destructiveHint`, `idempotentHint`,
`openWorldHint`). katana keeps them per tool. The specification says annotations are untrusted unless the server is
trusted, so they are only ever used in two ways: to make katana more careful, or to skip a question or allow a call for
servers declared in the user's own file (`~/.katana/mcp.json` or `KATANA_MCP_CONFIG`). A server that comes from a project
file (`.katana/mcp.json`, `.mcp.json`) never gains anything from its hints.

| where | effect |
|---|---|
| `katana mcp list` | `server__tool [read-only, idempotent, destructive, open-world]` and a `title:` line |
| question text (when `KATANA_CONFIRM=1` or `KATANA_MCP_CONFIRM=1` makes katana ask) | "hints from the server (not verified): ..." |
| question skipped | the tool says `readOnlyHint: true`, no `destructiveHint: true`, server from the user's file |
| `KATANA_MCP_CONFIRM_DESTRUCTIVE=1` | asks before tools with `destructiveHint: true`, even when katana would not ask |
| plan mode (`/plan`) | an MCP tool runs only under the same condition as the skipped question; every other MCP tool is refused as before |

A tool that claims both read-only and destructive is treated as neither. Before this item, `KATANA_CONFIRM` did not make MCP
calls ask; now it does (as it does for file writes and commands), and `KATANA_MCP_CONFIRM` keeps working.

## `.mcp.json` at the project root

katana also reads `.mcp.json` (same format as `.katana/mcp.json`; extra keys such as `"type": "stdio"` are ignored) from
the working folder or the nearest folder above it, stopping at the repository root (the folder that has `.git`) or the
user's home. It is a project file, so it needs the same trust confirmation as `.katana/mcp.json`: asked once per path and
content hash (`~/.katana/mcp-trust`), skipped without a terminal or the desktop, `KATANA_MCP_TRUST=1/0` as before.
`KATANA_MCP_STRICT=1` ignores it, like the other project file.

Precedence for a server name that appears more than once, weakest first: `~/.katana/mcp.json` (or `KATANA_MCP_CONFIG`),
then `.mcp.json`, then `.katana/mcp.json`. The server listing says which file each one came from. The usage file of a
server declared in `.mcp.json` is `.katana/mcp/<server>.md`. `katana mcp add --project` still writes `.katana/mcp.json`.

## Elicitation

katana declares `{"elicitation":{"form":{}}}` and answers `elicitation/create` from the server during a call (stdio, and
HTTP through the response stream). Only form mode with flat fields is handled: string (also `enum` and `oneOf` with
`const`), number, integer (with `minimum`/`maximum`), boolean, and arrays of enum values (answered as a comma-separated
list), up to 10 fields. URL mode and nested objects get a JSON-RPC error (`-32602`), which the specification allows for an
unsupported mode.

- Who asks is always named: the first question is `MCP server 'NAME' asks you for information: <message> (fields: ...)`,
  with the choices Answer and Decline, and the following questions start with `[mcp NAME]`.
- In the console the questions use the keyboard; in the desktop and the JSON/UI protocol they use the same marker as the
  `AskUserQuestion` tool (`CHOICE question<TAB>option...`, answered by `\x01ANS text`; an empty answer, or Stop, cancels).
  Optional fields skip on an empty answer, `default` is used when present, a value that does not fit the type or limits is
  asked again (three tries, then `cancel`).
- With nobody to ask (headless runs, `-p`, a pipe) the answer is `{"action":"decline"}` and a line on stderr says so.
- A field whose name, title or description looks like a password or secret (password, passphrase, token, api key,
  credential, private key, cvv, otp, ...) is never asked silently: the user must first confirm
  `send what looks like a secret ('field') to the MCP server 'NAME'`; if not confirmed the answer is `decline`.
- Typed values go only to the server. They are not written to `mcp.log` (which records only `elicitation <action>`) or
  anywhere else. The terminal does not hide typing; use the desktop for secrets.
- While the user answers, the call's timeout does not run (stdio); over HTTP the call may wait up to 10 extra minutes when a
  person can answer.

Revision `2026-07-28` replaces server-initiated requests (elicitation included) with multi round-trip results
(`resultType: "input_required"`); that form is not implemented.

## SSE: the GET event stream and the old SSE transport

Both use the `curl` the engine already uses (a child process whose output is a non-blocking pipe), so there is no new
dependency. POSIX only; on Windows the stream is simply not opened (HTTP calls keep working).

**Streamable HTTP GET stream.** After `notifications/initialized`, katana sends `GET <url>` with
`Accept: text/event-stream`, `Mcp-Session-Id` and `MCP-Protocol-Version`. A server without a stream answers 405 (or anything
else but an event stream) and everything works as before. Notifications and requests that arrive on the stream
(`list_changed`, `roots/list`, `elicitation/create`, `ping`) are handled exactly like those that arrive during a call; replies
go out as separate POSTs. The stream is read before each tool call. If it drops, katana reconnects (up to 3 times) with
`Last-Event-ID` set to the last event id seen, so the server can replay what was missed. When a POST answered with an event
stream that breaks before the response, katana resumes it with a GET and `Last-Event-ID` (the id of the last event of that
stream) and takes the response from the replay; if it cannot, the call fails as before. `KATANA_MCP_NO_STREAM=1` disables
the GET stream. Revision `2026-07-28` removes the GET stream, sessions and `Last-Event-ID` (replaced by
`subscriptions/listen`); this is for servers that negotiate `2025-11-25` or older.

**Old HTTP+SSE transport** (deprecated since `2025-03-26`): `{"mcpServers": {"x": {"type": "sse", "url": "https://host/sse"}}}`.
katana opens the stream, waits for the `endpoint` event, and POSTs every message to that endpoint; replies and notifications
come back as `message` events on the stream. The endpoint must be on the same origin as the configured url, otherwise the
server is refused. This transport does not resume: if the stream closes, the server is marked unavailable
(`/mcp reconnect NAME` opens it again). `katana mcp list` shows `[sse]`, `[http]` or `[stdio]`.

**For code that wants to wait on the stream** (the idle listening of the channel): `mcpx_stream_fd(McpSrv *)` returns the
descriptor to put in `poll()` (-1 when the server has no stream, or it is dead); when it is readable call
`mcpx_stream_pump(McpSrv *, ms)`, which reads what arrived, dispatches it (flags such as `dirty`, `rdirty`, `pdirty` are set
and requests are answered) and reconnects with `Last-Event-ID` if the stream dropped. Both are in `src/agent/mcp_extra.c`.
`katana toolrun` accepts `!streamfd NAME` in tests.
