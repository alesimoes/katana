# Contributing to katana

katana is an inference engine written from scratch in C, with an agent and a desktop app on top. This page is what you need to build it, find your way around and send a change. It is a personal project, so small, focused changes are easiest to review.

## Build and run

```
sh build.sh              # Linux x86_64, gcc; produces ./katana (about 85 s)
./katana setup           # downloads a model and the optional extras
./katana                 # agent console in the current folder
```

The desktop app (`desktop-qt/`) needs Qt 6.5+ and CMake 3.21+ (`sh desktop-qt/build.sh`). Windows (`build.bat`) is not compiled or tested here.

## Where things are

`src/katana.c` is the only compilation unit; it includes the parts:

| Folder | What it holds |
|---|---|
| `src/engine/` | inference: GGUF, tokenizer, CPU kernels, Vulkan and CUDA graphs, sampler, expert cache |
| `src/agent/` | the layers above the model: tools, prompt, context, sessions, flows |
| `src/uioe/` | the layer before the model: interpret, classify, route, retrieve, build context |
| `src/app/` | console, server, `main`, catalog and setup |
| `src/render/`, `src/vision/` | image generation and image input |
| `desktop-qt/` | the native desktop app |
| `tools/` | test and bench scripts, release helpers |
| `docs/` | documentation; start with `docs/handoff.md` (current state) and `docs/architecture.md` |

## Style

- Code comments are in Portuguese, without accents. Documentation (`README`, `docs/`) is in English.
- File, command and environment-variable names are in English. A new feature is on by default and has a `KATANA_X=0` switch to turn it off, except anything that sends data off the machine, which is off by default.
- Do not name other inference engines or products in code or docs; call them "the reference engine" when a comparison is needed.
- No legacy: when a format changes, the old data is rebuilt or dropped. Do not add aliases for old names, migration code or switches that only restore old behavior.
- The engine runs without any external service. Only the agent's tools may reach out (web, MCP), and anything that sends data off the machine must be explicit.
- Edit `src/` with care for the single compilation unit: a new `.c` file is added with an `#include` in `src/katana.c`.

## Checks

Compiling must pass with no new errors. After changing the engine, run `./katana selftest <model.gguf>` (GPU and CPU products against exact float math). Smaller checks that need no model: `./katana samplertest`, `./katana toktest <gguf> --spm-synth`, and the shell tests in `tools/tooltest/`. Say in your change what you ran and what you did not; do not present an estimate as a measurement.

## What never goes into git

`models/` and `ref/`, model weights of any kind, third-party binaries, build outputs, keys and tokens, and personal or company e-mail addresses in commits (use your GitHub `noreply` address).

## License

By contributing you agree that your change is released under the Apache License 2.0 (`LICENSE`).
