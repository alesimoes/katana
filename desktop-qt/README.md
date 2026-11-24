# katana-desktop (Qt6)

Native chat application for the katana agent (a layer above the engine, a separate executable).
Own window in Qt Quick; no browser and no HTTP server. The backend (C++17) launches
`katana agent <model>` directly with QProcess.

- Build: `desktop-qt/build.sh` (CMake; only Qt modules: Core, Gui, Qml, Quick, QuickControls2, Test).
  Produces `desktop-qt/build/katana-desktop-qt`. To install in place of the old app:
  `cp desktop-qt/build/katana-desktop-qt katana-desktop`.
- Run: `./katana-desktop-qt` (or the copied binary). Options: `--katana PATH`, `--models FOLDER`.
  `katana` and `models/` are searched next to the binary, in `..` and in the current directory.
- Model: selector below the text box, next to send (the list opens upward). Default Qwen3.6-35B (starts on its own); also usable: Gemma 4.
  The others appear disabled ("ainda nao esta utilizavel no agente", i.e. "not yet usable in the agent").
- Collapsible left sidebar (on a narrow window it becomes a drawer): "Nova conversa" (New conversation) and the saved conversations
  (title, relative time, model); trash icon on mouse hover, with confirmation in the interface itself.
- Sessions: one file per conversation in `~/.katana/desktop/sessoes/<id>.json` (id, titulo, modelo, criada,
  atualizada, mensagens[{papel, texto, nome?}]), written on send and at the end of each turn (atomic write).
  `KATANA_DESKTOP_HOME` changes the base folder (`~/.katana/desktop`). The agent's cache is not restored: when
  continuing an old session, the first new message goes to the agent preceded by a text summary
  (up to ~6000 characters, keeping the end); the screen shows only what was typed. The pending summary survives a
  model switch.
- Tool calls become cards (name + readable arguments); the model's raw XML does not appear.
- Large input box (minimum ~2 lines) that grows with the text up to ~240 px and then scrolls. Enter
  sends, Shift+Enter breaks the line (the agent reads one line per turn: the breaks become spaces, and the
  interface warns about it).
- Discreet statistics under the box: tok/s (stderr) and a context indicator (`ContextMeter.qml`): progress
  ring + "X mil / Y mil tokens · Z%" (X thousand / Y thousand tokens), with a tooltip on hover (used, total, free). Normal color
  below 70%, amber from 70%, red from 85%. Fed by the `TOKENS` marker
  (and by the statistics line from stderr). The `COMPACT` marker shows "Contexto compactado: N -> M
  tokens" (Context compacted: N -> M tokens). Unknown markers are ignored (extension point: `Backend::handleMarker`).

Looking at an image (chat): attaching an image shows one dropdown (`imageEngineBox`) with three options; the first, "Só olhar (conversar)", is the default and is remembered in `ui.ini` (`imagemAcao`: `look`, `stable` or `flux`) across attachments. With it the send goes to the running agent, not to `render generate`: the backend writes the single protocol line `@image(/abs/path) <text>` (an empty text becomes "Descreva a imagem."). The engine resolves `@image(PATH)` in any user line (several per line allowed, see `docs/vision.md`), so the model can describe, read text or compare. The user bubble shows a thumbnail; the session stores the typed text and the image path (`nome` of the user message), never pixels. If no agent is loaded the composer says "Escolha um modelo para iniciar o agente." and keeps the attachment; a path containing `)` is refused with a hint to rename the file. A model without a projector answers with a plain "no vision loaded" note.

Image generation: the composer is a plain text box like other chat apps. The "+" button inside the box (bottom left) attaches an image (file dialog, drag a file onto the box, or paste an image from the clipboard with Ctrl+V); the attachment shows as a small thumbnail chip with an X above the text. There is no separate image mode: attaching an image shows the action dropdown, and then one compact dropdown near the send button picks the action: "Só olhar (conversar)" (default, see above), "Stable (rápido)" or "FLUX (qualidade)". Sending with Stable or FLUX does not go to the agent: the backend runs `katana render generate --engine <stable|flux> --init <image> --strength 0.35 --size WxH --out ~/.katana/desktop/imagens/<stamp>-<engine>.png` (`--lcm` for Stable only; the size keeps the image ratio, multiples of 64, area close to 576x320) and shows the result as an image card with "Abrir" and "Abrir pasta". The text is the briefing: a short text goes as `--nl <text>`; a text that looks like a briefing (contains MATERIAIS or REGRAS, has lines starting with `- "`, or is longer than 600 characters; `Backend::looksLikeBrief`) is written to a temporary `.md` file and goes as `--brief <file>` (removed when the run ends); an empty text uses a default architectural prompt. The engine CLI still has `--brief`, `--logo` etc., but the desktop no longer exposes briefing or logo buttons. Exit code 3 of katana (engine not available) gives a friendly card, not a red error; any other failure becomes the usual red error card with the last output lines. The session file keeps the typed text and the image path (role `image`), never the pixels.

Render by the agent: when the agent calls the `Render` tool (the model writes the briefing and engine), the tool result carries `rendered with <engine>: <path> (<n> bytes)` and a `[flux|stable] usado: ...` line; the backend turns it into the same image card ("Abrir" / "Abrir pasta", label with the engine and processor) right under the tool action, and the session keeps it as a role `image` entry. Covered by `--selftest-standin` (request "renderize pelo agente", capture `22`).

Image processing (CUDA/Vulkan/CPU) is in the settings menu (gear), section "Processamento da imagem": Auto / CUDA / Vulkan / CPU, kept in `ui.ini` (`imagemBackend`) and sent to the engine as `KATANA_BACKEND` for FLUX and Stable (the card says who processed: `[flux|stable] usado:` line; Auto sends nothing and lets the engine choose). Covered by `--selftest-standin` (the stand-in engine answers `render generate` and writes the arguments to `$HOME/.standin-render-args` and the `--brief` file content to `$HOME/.standin-render-brief` for the test to read; captures `60` to `66`, including a 715 px wide window).

Second tool set markers: `NOTIFY <title>\t<message>` (PushNotification) becomes a highlighted line and, if the window is not active, a taskbar alert (the engine itself already calls `notify-send`; the app adds no second desktop notification); `FILE <mode>\t<path>\t<caption>` (SendUserFile) becomes a card with the file name and the buttons "Abrir" (default program, `QDesktopServices`) and "Abrir pasta"; `FINDING <file>\t<line>\t<category>\t<summary>` (ReportFindings) become one compact card, and each item opens its file (a relative path is resolved against the working folder). These items are screen-only: they are not saved in the session file.

Stopped without answering: when the engine signals the end of a request (`READY` after a working turn) and the last visible item is a tool card (or nothing but the user message), the conversation gets the line "o agente parou sem responder" with a "pedir resumo" button that sends a fixed message asking what was done and what is left. Not shown after the Stop button, nor when the last turn had text.

Specializations: the engine writes `especializacao: <path> (<n> tensores)`, `especializacao pulada (<reason>): <path>` and `especializacoes de <dir>: N aplicadas de M` on stderr at load; the settings popup (gear) lists the names in use for the current model and the skipped ones with the reason, and "Abrir a pasta" opens `~/.katana/specs` (created if missing). The folder is the source of truth: the app has no on/off switch.

Request context block: the engine appends a `[katana: context for this request ...]` block after the user's text (UIOE). The desktop shows what the user typed, never the block (it keeps its own sessions, with the typed text only); when the engine reports on stderr that it attached one (`[context: about N tokens from the project index, M verified step(s) ...]`) the conversation gets a discreet notice line "Contexto do katana: ~N tokens do índice do projeto, M passo(s) verificado(s)". Engine notes appended to tool results use the dim caption described above.

Agent start and end failures: the desktop never stays silent when the agent does not start, cannot load the model or dies. The last 8 stderr lines of the engine are kept; when the process ends (exit code, crash, or never started) while loading or answering, the conversation gets a red, selectable error card (kind `error`, `errorC` in `qml/MessageView.qml`) with the cause ("O agente nao conseguiu carregar o modelo", "O agente terminou (codigo N) no meio do pedido", "O agente travou...", "Nao foi possivel iniciar o katana (...)") and those last lines; running actions are closed as failed, the queue is dropped and the status line asks for a model to start again. The card is not saved in the session. Covered by `--selftest-standin` (`morra agora` in `tools/standin-engine.sh`, screenshot `50`).

Stand-in test (no model, no GPU, does not touch `~/.katana`): `QT_QPA_PLATFORM=offscreen desktop-qt/build/katana-desktop-qt --selftest-standin --katana $PWD/desktop-qt/tools/standin-engine.sh [--capturas DIR]` (absolute `--katana` path). It sets a temporary `HOME` with three `.kspec` files and a fake model, drives the window with real events against `tools/standin-engine.sh` (same marker protocol), and checks the three items above, the plan and activities, and the Find/Symbols cards, the engine notes and the index status (146 checks in all; the plan screenshots are `capturas/30` to `35`, dark and light; the Find/Symbols ones are `40` to `43`: dark, note open, light, IDE). Sample tool outputs are real ones copied from `katana tool Find`, `katana tool Symbols` and `katana coder status` in `tools/samples/` (the `links:` line is written by hand from the format in `src/coder/symtool.c`: no real example was found); prints PASSOU/FALHOU.
Code-index tools and engine notes: `Find` and `Symbols` have their own verbs ("Procurando no projeto", "Consultando os símbolos", in `qml/Verbs.js`; the verb table only lists the tools the engine declares: Bash, Read, Edit, Write, Find, Symbols, Grep, TodoWrite, AskUserQuestion, WebSearch, WebFetch, ToolSearch). The one-line summary of the arguments (`Verbs.resumo`) shows the query and mode for Find (`"zzqqnotthere" (refs)`), action plus name or file for Symbols (`callers ct_sb_puts`, `def x em tools/glob.c`, `flow ct_glob`, `tree src`; every Symbols action, tree included, shows as action plus its target). The opened card of Find and Symbols shows the result with the theme colors (`Verbs.realce`, Qt rich text, selectable): `[confidence N level; ...]` (high: `Theme.ty`, medium: `Theme.fn`, low: `Theme.str`), the labels `called by:`, `calls:`, `links:`, `also referenced in:` (`Theme.kw`) and `NOT FOUND` (`Theme.err`); other tools stay plain text. Engine notes (`[katana: ...]`, `[plan: ...]`) are cut out of the result wherever they are in it (start, middle or end; a note may span several lines until the line that ends with `]`) and shown below it as a dim caption ("nota do motor (para o modelo): ..."); a long note (over 130 characters or several lines) is collapsed to one line and opens on click.

Index state: while the agent works (and once when it stops) the backend runs `katana coder status <folder>` with QProcess every 3 s (never blocking the interface; killed after 10 s) and shows a discreet "índice: pronto" or "índice: indexando N%" next to the speed text in the bottom bar, with a tooltip (files, levels N1/N2/N3 as percentages, ignored by rule, crawler running or stopped). Parsed fields (`Backend::parseIndexStatus`): `arquivos vigentes: N`, `N0 arvore ... (M sem leitura`, `N1 texto`, `N2 simbolos`, `N3 ligacoes` (percentages), `ignorados por regra ...: N entradas`, `N discovered` (folders still to list) and `rastreador: em execucao`; "indexando" when the crawler runs, folders are still discovered or N1/N2 is below 100% (shown as the lower of the two, at most 99). Nothing is shown when the engine has no such command, the folder has no index (no `arquivos vigentes` line) or the process cannot start. Not read: the `estado:` lines of the background crawler (format not final).

- Icon: purple gradient squircle with a thick pixel-art K (white to lilac, 1-pixel purple shadow). `icons/katana.svg`
  and PNGs 16 to 512 are generated by `tools/gerar-icone-k.py` (PIL), embedded in the executable (Qt resources) and used as the icon
  of the window, the .desktop and the dock. The samurai appears only on the welcome screen.
- Font: embedded JetBrains Mono (OFL license in `fonts/`), interface and code.
- Purple identity: the accent (`Theme.ac`: send button, context ring, text selection, avatar, focus,
  title) is `#820AD1` in the light theme and `#B66CF2` in the dark one (text over the accent: white / `#1A1022`); the
  neutrals have a slight lilac tone. All in `qml/Theme.qml`.
- Welcome screen (`emptyState` in `MessageView.qml`): appears only with an empty conversation (start, 'Nova
  conversa'); disappears when the first message is sent and does not appear when opening a session with messages. It shows
  the samurai (`icons/samurai.png`, 16x16 logical, 5 tones: outline, shadow, purple base, light, white) enlarged at an
  integer scale of 5x (80 px; 4x in a short window) with `smooth: false`, the title "Katana" as an 80s retro image
  (`icons/titulo-claro.png` / `titulo-escuro.png`: blocky 5x7 pixel letters, purple gradient and 1 px shadow,
  enlarged at an integer scale), "Em que posso ajudar hoje?" (How can I help today?) and 3 clickable suggestions that fill in and send.
  Sprite and title come from `tools/gerar-samurai.py` (the icon, from `tools/gerar-icone-k.py`); run it and then `build.sh`.
- Appearance (gear in the custom title bar, frameless window): theme Escuro (dark, default), Claro (light) or IDE escuro (editor style, code with syntax highlighting) and font JetBrains Mono (default) or IBM Plex; saved in ui.ini. `KATANA_THEME=light|dark`, `KATANA_TEMA` and `KATANA_FONTE` force one.

Embedded terminal: `>_` button in the title bar (or Ctrl+`) opens a bottom panel with a real bash shell in a pty (via `script`, Linux only): colors, cursor, line editing, history (arrows), Tab, Ctrl+C/D, copy Ctrl+Shift+C, paste Ctrl+Shift+V, Limpar (Clear), Reiniciar (Restart). Own emulator in `src/terminal.cpp` (SGR 16/256/truecolor, cursor and erase CSI); full-screen programs (vim, top) do not work. Sudo and passwords are typed in the terminal itself. The user's terminal is separate from the agent's `terminal`.

Awake screen (like caffeinate): while the agent loads or responds, the desktop asks the system not to lock the screen or suspend (Linux: `gnome-session-inhibit` or `systemd-inhibit` running `cat`, which ends when the input closes); toggled in the gear ("Nao bloquear enquanto trabalha", i.e. "Do not lock while working", default on, `manterAcordado` in ui.ini). Not done on Windows/macOS.

Working folder: the button next to the model selector shows the agent's folder and opens a folder chooser (the agent switches with `/cd`, without reloading the model). The agent can read and write anywhere; there are no per-folder prompts. The only dialog is the sudo password (marker `NEEDPASS`). Markers `CWD <folder>\t<1|0>` (current folder) and `ASK <kind>\t<what>\t<detail>` (answer `\x01ANS 1|0`; only used with `KATANA_CONFIRM=1`) are handled by the app.

Sudo: the agent runs as your normal user; root only enters when a command asks for `sudo` (in a `Bash` command) (`KATANA_SUDO=1` makes everything run as root). When it needs the password, the engine emits the marker `NEEDPASS` (`NEEDPASS retry` after a wrong one); the desktop opens a password dialog and answers with `\x01PASS <password>` (empty = cancel). The password stays only in the agent's memory and is not sent to the model or the history. In the terminal (`katana agent`) it is asked on screen, without echo.

Plan and activities in the conversation: the marker `TOOL <name>\t<arguments>[\t<description>]` carries the short description the model gave the call; the app shows it as the main line of the action (with a spinner and the elapsed time while it runs, the duration and an ok or failed mark after) and keeps the command as a second line that opens on click; without a description the line is the verb plus the target, as before. A result that starts with `error:` or reports an exit code other than 0 counts as failed (the engine's own rule). When the model writes a task list (`TODO`), the request gets a plan card (`PlanView.qml`, backed by `MessageModel`): the actions are grouped under the step that was in progress when they ran (the marker has no status, so the first undone step is the one in progress); done steps collapse to one line ("N ações · time", click to open), the step in progress is open, pending ones are dimmed. When the list changes, a step with the same text is the same step, a different text at the same position is a reworded step (it keeps its actions), a new text is a new step and a step that left the list stays as "descartado" with its actions. Calls that only manage the plan (`TodoWrite`) are hidden and not counted. Notes the engine appends to a result (`[katana: ...]`, `[plan: ...]`) are for the model: they are cut out of the output and shown dimmed, only in the opened action. The status line under the box shows "passo 2 de 6: text", the elapsed time and the number of actions while the request runs, and "6 de 6 passos, 23 ações, 4 min" at the end; a request with no plan shows only the time and the count. Sessions keep, per call, the description, mark, duration and step, plus the plan as an extra `plan` message (JSON: steps with id, text, status, start and end of the request), so an opened session regroups the same way; the tool output is not stored and a plan that was only carried over from an earlier request is not regrouped. Covered by `--selftest-standin` (`execute o plano` in `tools/standin-engine.sh`).

Task list, plan mode and undo: the marker `TODO <steps>` fills the plan card in the conversation (the list is shown only there, not in a second card above the message box); `MODE plan` shows the plan-mode card above the box, with the plan-mode line with the "Aprovar e executar" button (`/go`); the "plano" and "desfazer" buttons next to the folder button send `/plan` and `/undo`, and `UNDO <n>\t<files>` becomes a notice.

Teacher mode: not exposed in the interface (no button, menu section or dialog). The backend still understands the `TEACHER`/`TEACHERR` markers, but nothing from `ui.ini` turns it on and `KATANA_TEACHER` is not passed on to the agent.

Questions from the agent: when the model calls `AskUserQuestion`, the engine emits `CHOICE <question>\t<option>...` and waits. The desktop shows the question in a card above the message box with one button per option (not a dialog); a button, or any text typed in the box, answers with `\x01ANS <text>`. Stop cancels the question (empty answer).

Remembered model: the last model started is kept in ui.ini (`ultimoModelo`) and is the one that opens next time; when opening a saved conversation, the desktop switches to the model it was in (if usable) and resumes the session.

Environment: the desktop discards performance KATANA_* variables inherited from the terminal (KATANA_CPU, KATANA_CTX, KATANA_KV16, KATANA_VK_*...) so the engine uses the fastest defaults; `KATANA_DESKTOP_ENV=1` preserves the inherited environment.

Protocol: the agent runs with `KATANA_UI=1 NO_COLOR=1 KATANA_THINK=0`. Stdout lines that start with
the byte 0x01 are markers (`READY`, `TOOL nome<TAB>args`, `TOKENS pos ctx`, `COMPACT antes depois`); the
rest is response text, filtered of tool blocks. Stop = SIGINT to the agent's group (on
Windows, kill and restart). New conversation = `/new`; switching models = `/exit` and, if it does not exit within 3 s, kill.

Test: `QT_QPA_PLATFORM=offscreen desktop-qt/build/katana-desktop-qt --selftest --capturas desktop-qt/capturas`
(run at the repo root; uses the reference model `models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf`, slow to load when cold; `--selftest-model <file>` picks another, e.g. the Gemma 4 file; and a temporary data folder). It types and
clicks in the window with real events (QTest), checks the `Bash` tool card, sessions, dropdown, delete, stop
(SIGINT), model switch with a pending summary, and prints PASSOU/FALHOU (PASSED/FAILED). `capturas/` holds images (`12-boasvindas-claro.png` and `12-boasvindas-escuro.png`: the dark one comes from a
second run with `KATANA_THEME=dark`). It also checks the welcome screen (visible with an empty
conversation, disappears after sending, does not appear in an old session, returns on 'Nova conversa').

Limitations: reasoning off (`KATANA_THINK=0`); the agent reads one line per turn (line breaks
become spaces); on Windows, stop kills the agent; the Windows version was neither compiled nor tested.

## Models screen

The download button in the top bar opens the models screen; it opens by itself when the models folder has no
usable model. It shows the machine (RAM, GPU, free disk), the catalog with the recommended model, and downloads
with progress, pause and resume (`katana hw`, `katana catalog`, `katana pull`). After the first download the app
starts that model. An installed app keeps models in `~/.katana/models` (or `KATANA_MODELS`).

## Packaging

`packaging/appimage.sh` builds `packaging/out/katana-x86_64.AppImage` (engine, app and Qt inside). An AppImage
only runs on systems with a glibc at least as new as the build machine, so the workflow
`.github/workflows/pacotes.yml` (manual trigger) builds it on Ubuntu 22.04. Not yet executed.

## Install at the repository root

Do not copy the binary: it needs the `Katana/` folder (QML module) that sits next to it in `desktop-qt/build/`.
Use a symbolic link (Qt resolves the real path): `ln -sf desktop-qt/build/katana-desktop-qt katana-desktop`.
Copying the binary alone gives the error `Module "Katana" contains no type named "Main"`.

## Dock and application menu (GNOME/Ubuntu)

`desktop-qt/instalar-dock.sh` installs `katana-desktop.desktop` in `~/.local/share/applications/`, the icons in
`~/.local/share/icons/hicolor/<size>/apps/katana-desktop.png`, updates the caches and adds
`katana-desktop.desktop` to `org.gnome.shell favorite-apps` (without removing the favorites or duplicating). In other
environments it installs only the menu shortcut. The `Exec` of the .desktop uses the link `/mnt/dados/source/katana/katana-desktop`
(see above); the script does not touch the binary.
