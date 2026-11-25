# katana web: the desktop interface in a browser tab

`katana web` (same as `katana serve --agent`) serves a page that looks and behaves like katana-desktop (`desktop-qt/`), from a local server, with no
external service, no CDN and no third-party library. The page drives the same agent process the desktop drives. Linux only (the agent runs as a child
process, the terminal uses a pty).

```sh
katana web                       # port 8080, the page picks the model (last used, else the default Qwen3.6)
katana web 9000                  # another port
katana web model.gguf 9000       # model chosen on the command line (same as: katana serve model.gguf 9000 --agent)
```

It prints `katana web: abra no navegador  http://127.0.0.1:PORT/?token=...`. Open that exact address. The key in it is random, new on every start.

## Architecture

| piece | where |
|---|---|
| page source | `web/index.html`, `web/css/app.css`, `web/js/*.js` (plain scripts, no framework, no build step) |
| code shared with the desktop | `desktop-qt/qml/Markdown.js`, `Highlight.js`, `Verbs.js` are served as they are (the `.pragma library` line is cut and each file is wrapped in a function by `tools/genweb.sh`), so markdown, code colouring, verbs, durations and the Find/Symbols output colouring are the same code, not a rewrite |
| fonts and icons | `desktop-qt/fonts/*.ttf` (JetBrains Mono, IBM Plex) and `desktop-qt/icons/` (K, title images), served from the binary |
| embedding | `tools/genweb.sh` (called by `build.sh`) writes `src/app/web.inc` (base64 of every file, git-ignored, like `changes.inc`); `katana` stays one file. Without `web.inc` (Windows build, or `gcc` run by hand) the server answers with a page that says to run `./build.sh` |
| server | `src/app/webui.c` (included after `agentapi.c`; hooked in `srv_conn` of `server.c`) |
| colours | `web/js/theme.js` has the three palettes of `Theme.qml` (dark, light, IDE) and the two font choices; CSS variables do the rest |

### Protocol (page to server)

The desktop reads the agent's stdout and stderr and writes lines to its stdin (`desktop-qt/src/backend.cpp`). The web page does the same through the server:

* The server starts `katana agent <model>` (`KATANA_UI=1`, `NO_COLOR=1`, the same environment filter as the desktop, own process group) and turns what it writes into
  numbered events in a ring buffer (4096 events or 24 MB), sent as Server-Sent Events by `GET /web/events` (reconnection resumes with `Last-Event-ID`):
  `t` text, `m` a marker line (`READY`, `TOOL`, `TOOLOUT`, `TOOLLIVE`, `TOKENS`, `TODO`, `CHOICE`, `ASK`, `MODE`, `CWD`, `NOTIFY`, `FILE`, `FINDING`, `PREP`, `FEED`, `RESUME`, `STEERED`...), `e` a stderr line, `s` started, `x` exited.
* `web/js/backend.js` is a port of the desktop backend: the same marker handling, tag filter, `cleanArgs`, tool states, notes split out of results, plan reconciliation
  (`web/js/model.js` is `MessageModel`), request progress, queue, session save format (`~/.katana/desktop/sessoes/<id>.json`, the desktop's own files) and the `/session`, `/resume`, `/new`, `/cd`, `/plan`, `/go`, `/undo`, `/teacher` lines.
* The page writes to the agent with `POST /web/line` (one line, newlines and `\x01` become spaces), `POST /web/ans` (`\x01ANS`), `POST /web/pass` (`\x01PASS`) and stops it with `POST /web/stop` (SIGINT to the group, like Ctrl+C / Esc).
* Message during a cycle: the desktop also writes `\x01STEER <text>` while the agent works; the engine appends it to the next tool result as a user note and answers `\x01STEERED<TAB><text>`, or, if the request ends first, reads it as the next user message (also with `STEERED`). Details in `desktop-qt/README.md`. The web page does not send STEER yet (no endpoint).
* Other endpoints: `GET /web/state` (models, engine version, preferences, agent state), `POST /web/start` (start or switch model, with the working folder), `GET/PUT/DELETE /web/sessions[/id]`,
  `GET /web/dirs` (folder browser), `POST /web/ui` (preferences in the desktop's `ui.ini`: theme, font, folder, last model, collapsed sidebar, keep awake), `POST /web/open` (xdg-open on the server machine),
  `GET /web/sys` and `GET /web/index` (the bottom bar: CPU, RAM, GPU, agent activity, code-index state), `POST /web/teacher`, `/web/term/*` (embedded terminal).
* `/v1/agent/sessions` (docs/api.md) is unchanged and independent. The two use different child processes: do not drive both at once.

Test hook: `KATANA_AGENT_EXE=path` makes the server run another executable in place of itself as the agent (for example `desktop-qt/tools/standin-engine.sh`, which speaks the marker protocol without a model). Not for normal use.

## Security

The page commands an agent that runs commands and writes files on this machine, so:

* only loopback connections are served (`/web/*` and the page answer 403 to any other peer, even with `KATANA_HOST` opened);
* `Host` must be exactly `127.0.0.1:PORT` or `localhost:PORT` (anti DNS rebinding) and `Origin`, when present, must be the same host;
* a random 128-bit key is printed in the opening URL; the first request (`/?token=...`) is answered with a `HttpOnly; SameSite=Strict` cookie and a redirect to `/`, so the key leaves the address bar and the history; every page file and every endpoint needs the cookie (constant-time comparison);
* every non-GET request must carry `X-Katana: web`, which a page on another site cannot send without a CORS preflight, and the server never answers preflights;
* the page has a Content-Security-Policy (`script-src 'self'`; no inline script, no external host), and the page builds user and tool text with `textContent`; the only HTML comes from `Markdown.js`, `Highlight.js` and `Verbs.js`, which escape `& < >` first;
* the agent child does not receive `KATANA_API_KEY` or `KATANA_HOST`;
* sudo passwords: the page shows the same dialog as the desktop and writes `\x01PASS` straight to the agent (not logged, not sent to the model); the `/v1/agent` API keeps refusing them.

## Parity with the desktop

Checked by screenshots (below). Same: layout, colours and fonts of `Theme.qml` in the three themes and both fonts; sidebar with the saved conversations (the same files as the desktop) and inline delete confirmation; welcome screen with suggestions and engine version/changes;
user bubble, assistant body with markdown and code blocks with copy; activity groups, action rows (verb and target, description, spinner and time, command, output, engine notes, Find/Symbols colouring); plan view with steps, discarded steps and per-step actions;
notice, notify, file card (Abrir / Abrir pasta run `xdg-open` on the server machine) and findings; "o agente parou sem responder" with "pedir resumo"; red error card when the agent dies (with the last engine lines);
the task card above the box (steps, "ver todas", plan mode with "Aprovar e executar"), the question card (`CHOICE`: option buttons or typing), the permission dialog (`ASK`), the sudo dialog;
the message box (grows to 330 px then scrolls, Enter sends, Shift+Enter breaks, Esc stops), queue with pause/resume after Stop, send/stop button, model picker that opens upward, working-folder warning, plan and teacher buttons, status line (progress, activity, errors), index state, speed, context meter with its colours and tip, CPU/RAM/GPU line;
settings popup (theme, keep awake, teacher key and model, specializations, working folder, font); resume of saved conversations (`/resume` with the engine's `.ksess`, text summary as fallback) and model switch when the conversation was in another model; embedded terminal (own emulator, a port of `terminal.cpp`).

### Differences (honest list)

* No window frame buttons (minimise, maximise, close): the browser has them.
* The folder chooser is a list from the server (`/web/dirs`), not the system dialog.
* "Keep awake" uses the browser Wake Lock API (works on `127.0.0.1`), not the desktop inhibitor; it depends on the browser.
* No download of models from the page yet: the Models panel lists installed models and says to use `katana catalog` / `katana pull`.
* File "Abrir" and "Abrir pasta" open on the machine that runs `katana web`, not in the browser (same machine in the supported setup).
* The terminal has no full-screen programs (same limit as the desktop) and its columns follow the panel width at start.
* The weak points of a web page: the browser's own text rendering differs from Qt by about a pixel; the desktop's overlapped status text when the bar is crowded is an ellipsis here; the settings popup clips long labels instead of overflowing.
* One agent per server, shared by every tab: a second tab attaches to the running agent and shows its state, but it only restores the conversation that this browser last opened.
* The page needs a modern browser (CSS `color-mix` is not used; relies on `EventSource`, `fetch`, ES2015, `ResizeObserver`).

## What was verified (and what was not)

Built with `gcc -O0`, served from a scratch `HOME`, driven by headless Chrome (DevTools protocol) against the desktop's stand-in engine (`desktop-qt/tools/standin-engine.sh`) and a second stand-in written for this test (questions, permission, plan mode, slow request, password, code answer).
Screenshots at the same scene as `desktop-qt/capturas/` (30, 31, 32, 33, 34, 40, 41, 42, 43, 20, 21, 22, 50: dark, light and IDE) were compared with the desktop's own offscreen self-test captures (92 desktop checks pass) after scaling the 1180 px page to the 1000 px captures.
Also exercised: question, permission, plan approval, queue and Stop, sudo dialog, folder browser, embedded terminal with colours, model picker, reopening a saved conversation, page reload while the agent is up, security headers (`curl`: no cookie 401, wrong Host/Origin 403, POST without `X-Katana` 403).

Not verified: a real session with Qwen3.6 (GPU busy; plan T6 stays open), Windows (not supported), browsers other than Chrome, narrow-window drawer in screenshots, IBM Plex in screenshots, the teacher flow with a real plugin, `xdg-open`, the Wake Lock.
