# Vision: images in the conversation

Models with a vision projector (Qwen3.6 with `mmproj-*.gguf` next to the model file) can look at images. Without a projector
every image request gets a plain message saying the model has no vision loaded; nothing crashes.

## Showing an image in any message

Write `@image(PATH)` anywhere in a user message, in the terminal REPL (`agent` and `chat`) and in the desktop line protocol
(a normal user line). Several images per message are fine: `compare @image(a.png) with @image(~/b.jpg)`.

- PATH may be absolute, relative to the working folder, or start with `~/`. Quotes and spaces around it are ignored.
  The path cannot contain `)`.
- Each use is replaced before tokenization by `[image /abs/path: WxH pixels, N tokens]` plus the image mark, so the model
  sees the path, the size and the picture, in the place where you wrote it. The image costs at most
  `KATANA_IMAGE_TOKENS` tokens (default 1000) and counts in the `TOKENS` line like any text.
- A file that is missing, unreadable, too large, or not a supported format (PNG, JPEG, ...) is not attached: the message
  keeps `[image PATH not attached: reason]` (the model is told) and the same line goes to stderr. The turn still runs.
  An empty `@image()` or one without a closing `)` stays plain text.
- `--image FILE` at startup still works; those images go in front of the first message.
- The Read tool also attaches an image file when vision is on, so "look at this file" works without the syntax. Without a
  projector Read says the model has no vision input.

## Sessions, prefix cache, regrow

The session stores the text (path, size, mark with the SHA-256 of the file), never pixels. The original file is copied into
`~/.katana/cache/img-HASH.bin` (the 64 most recent are kept), so resuming a session re-encodes the picture even if the
original moved. If the copy is gone too, the mark is dropped and the `[image PATH: ...]` line stays as a note. The prefix
cache keys on the file hash, and `agent_regrow` reloads the same history, so both behave as before.

Tests without the 22 GB model: `sh tools/visiontest/run.sh ./katana` (the unit part covers the `@image` resolution).
