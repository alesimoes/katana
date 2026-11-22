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
