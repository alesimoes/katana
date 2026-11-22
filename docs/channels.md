# Channels: an MCP server that talks to a running console

Code: `src/agent/mcp_channel.c` (hooks in `mcp.c`, `tools.c`, `loop.c`, `launch.c`, `table.c`). Tests:
`tools/tooltest/channel.sh` (the fake server, the stand-in for the model and the pseudo-terminal scripts are one C program,
`channel_test.c`).

A connected MCP server can push a message into the console that started it: the owner writes on his phone, the message
becomes the next request, and the model answers through a tool of that server. The server can also answer the
confirmations katana asks for.

## Turn it on

A channel is off unless the owner turns it on for that server:

- in his own `~/.katana/mcp.json` (or the file of `KATANA_MCP_CONFIG` / `--mcp-config`):
  `{"mcpServers": {"phone": {"command": "node", "args": ["/path/server.mjs"], "channel": true}}}`
- or for one run: `katana --channel phone` (repeatable; `KATANA_MCP_CHANNEL=a,b` does the same).

`"channel"` in a project file (`.katana/mcp.json`) is ignored even when that file is trusted. A project file that replaces
the owner's server of the same name turns its channel off. `--channel NAME` does enable a trusted project's server.
`/mcp` and `katana mcp list` show the state of each channel.

## Protocol

- The server announces `capabilities.experimental` with a key `<segment>/channel`; katana takes the segment from it.
  `<segment>/channel/permission` adds confirmations. A server that announces nothing gets no channel, and katana says so.
- katana declares `capabilities.experimental.channel` and `channel/permission`, only to servers that are turned on.
- message, server to katana: `notifications/<segment>/channel`, `params: {content: "text", meta: {key: "value"}}`.
  An optional `id` / `message_id` / `messageId` (in `params` or `meta`) makes a repeated delivery be ignored (last 128 ids).
- confirmation, katana to server: `notifications/<segment>/channel/permission_request`,
  `params: {request_id, tool_name, description, input_preview}`; answer: `notifications/<segment>/channel/permission`,
  `params: {request_id, behavior: "allow" | "deny"}`.
- The server's `instructions` go into the system prompt, whole up to 8 KB (about 2,000 tokens), in a `# Channels` section
  before the environment block, so they are part of the stable prefix the cache keeps. Servers without a channel keep the
  120-character card. If the server only came up later, its instructions (4 KB) go with its first message.
- Transports: stdio, and HTTP through a `GET` event stream (`Last-Event-ID` is sent when it reconnects).

## What the model sees

```
<channel source="phone" via="mobile">
run the tests and tell me
</channel>
```

`source` is the name the owner gave the server, whatever the server says in `meta`; other `meta` text values become attributes
(8 at most, 200 characters each). Control characters, terminal escapes and the model's turn marks are neutralized, and
the text cannot close the mark or be taken for a slash command.

## Delivery

- One message at a time, in arrival order, when the console is idle. **A message that arrives while the agent works waits for
  the request to end** (it is not inserted between rounds: that would change a request half done, and a tool call already
  approved). It is read and queued meanwhile, and the console says how many wait.
- While the owner is typing, a message waits until 5 s pass without a key (`KATANA_CHANNEL_QUIET`); then it runs and the
  half-typed line comes back at the next prompt. A line already sent from the keyboard goes before the next message.
- 8,000 bytes per message (cut with a note), 20 requests per minute per server (`KATANA_CHANNEL_RATE`; the rest waits).
  With 64 waiting katana stops reading the server (the server blocks, nothing is lost); messages read in the middle of a
  call still queue, up to 512, then are dropped and logged. The queue is memory only: quitting katana loses what waits.
- Every message and confirmation, accepted or refused, is in `~/.katana/mcp.log`.
- A channel server that dies while idle is reported once and restarted with a 1 to 60 s backoff. The other servers are also
  read while idle (pings answered, death reported); they restart on their next call, as before.
- Tool calls of a channel server wait up to 15 minutes (a tool may wait for a person); `KATANA_MCP_TIMEOUT` changes it.
- The desktop (`KATANA_UI`) gets the marker `CHANNEL <server>\t<text>`. Headless runs (`-p`) take no messages (logged as
  ignored), but may ask for confirmations. A model reload (`/model`) is refused while messages wait.

## Confirmations

When katana asks (an `ask:` rule of `KATANA.md`, `KATANA_CONFIRM=1`, `KATANA_TRUST=0`, `KATANA_MCP_CONFIRM=1`), the question
also goes to every channel that announced the permission capability. The first answer wins, keyboard or channel (the desktop gets
`ASKDONE <0|1>` when the channel decided). No answer in 600 s (`KATANA_CHANNEL_ASK_TIMEOUT`) denies. An answer counts
only from a channel that is on, with the id of the pending request. A `never:` rule refuses before anything is asked. A
question whose detail does not fit in the 4,000-byte preview is asked only at the keyboard. There is no message to withdraw a
question: after a keyboard answer, the phone keeps showing it until its own limit, and a late answer is ignored.

Anyone who can write through the channel gives orders as the owner does: turn it on only for a server that authenticates
its sender.
