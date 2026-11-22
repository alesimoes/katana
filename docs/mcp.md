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
