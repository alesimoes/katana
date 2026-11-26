# katana: making games (survey, 2026-11-26)

Written from reading the code and the docs. Nothing here was run with a model and nothing was measured. "Exists" means the code is in the repository; it does not mean it was tried on games.

## 1. What katana already gives a game project

| Need | What exists | Where |
|---|---|---|
| Write and edit many files, run commands, read errors | the agent's tools (`Write`, `Edit`, `Bash`, `Find`, `Symbols`, `Grep`), no confirmation prompts | `docs/tools.md` |
| A plan for a multi-file job | `TodoWrite`, flow `plan`, flow `code` (read, edit, verify, repair, report) | `docs/flows.md` |
| Project conventions the model must follow | `KATANA.md` "brief" (`test:`, `done:`, `never:` rules), skills, MCP cards | `docs/extending.md` |
| Finding code in a growing project | code index (`Find`, `Symbols`) | `docs/coder.md` |
| Images (sprites, backgrounds, concept art) | FLUX.2 klein, Stable Diffusion, ControlNet; `Render` tool; flow `image` (brief, show, deliver, wait, final) | `docs/architecture.md` L7, `src/render/` |
| Show a file to the owner | `SendUserFile`, desktop file card | `docs/tools.md` |
| Long jobs | background tasks, `jobs.c`, durable work state (`.kstate`) so a restart resumes the job | `docs/flow-resume.md` |
| Judging whether the agent solved a coding task | `katana codebench` (hidden tests per task) | `docs/handoff.md` section 0t |

Tools on this machine (checked): `python3` (with PIL), `node`, `gcc`, `google-chrome`, `firefox`, `ffmpeg`, `xvfb-run`. Not installed: pygame, a game engine, ImageMagick.

## 2. Hardware limits that shape the choice

- Notebook: i7-11800H (8 cores), RTX 3050 with 4 GB, 61 GB RAM.
- The language model uses the GPU (Vulkan graph, expert slots). FLUX also wants the GPU; sharing 4 GB between the two is an open problem (`architecture.md`). Practical rule: generate art in a separate step while the model is idle, or run FLUX on the CPU.
- Old figures (not re-measured): Qwen3.6 generates about 25 tokens/s and reads a prompt at about 70 to 85 tokens/s. A game of 1,000 lines is roughly 12,000 tokens, so about 8 minutes of pure generation. Games must be built in small files, not one big file.

## 3. Which kind of game fits

Ranked by how well the agent can verify its own work without a person looking at the screen:

1. **Browser games in plain JavaScript on a canvas, no dependencies.** One folder, no build step, runs anywhere, the owner opens `index.html`. The agent can test logic with `node` and take a screenshot with headless Chrome (`google-chrome --headless --screenshot`). Best fit.
2. **Terminal games in Python or C** (roguelike, snake, text adventure). Fully testable with scripted input through the existing `Bash`/`SendInput` tools. Very cheap, but narrow audience.
3. **Python with pygame / C with SDL2.** Needs a package install and a window; testable only with a virtual display (`xvfb-run`). Possible, more steps.
4. **Godot, Unity, Unreal.** The model has to write engine-specific formats and the editor is a person's tool. Not recommended before the first three work.

## 4. What is missing (gaps, in order of value)

1. **A verification loop for games.** The `code` flow knows "verify" only through a build file or `test:` in `KATANA.md`. A game needs three checks the engine does not know about: (a) the logic runs headless (`node tests.js`), (b) the page loads with no console error, (c) a screenshot is taken and looked at. Build as a skill (`game-check`) plus a small helper command in C: `katana game check <folder>` that starts headless Chrome on `index.html`, collects console errors, saves `shot.png`, and prints one result line. The model does not see images today (vision exists only as a test command), so the result must be text: errors, canvas size, count of non-background pixels, and a frame counter that advances.
2. **A game-shaped project template and rules.** A `KATANA.md` for games: logic in pure functions separate from drawing, a fixed-step loop, a seeded random generator, input mapped through one table, no external assets needed to start (shapes first, art later). This keeps files small and testable, which is what a 35B model with a short context does well.
3. **A `game` flow.** States: design (one paragraph and a list of mechanics), skeleton, mechanic by mechanic (write, check, fix), art, polish, deliver. The existing flow table in `src/agent/flow.c` takes a playbook as a row, so this is a table entry plus its contract, not new machinery.
4. **Art pipeline for the image engine.** Sprites need a transparent background, a fixed size and a consistent style. FLUX gives none of that by default. Needed: a post step in C or Python (PIL exists) that crops, resizes to the grid, removes the background colour and packs a sprite sheet; a fixed style prefix in the prompt; a seed kept per character so the model can ask for variants. Open question: quality of FLUX klein at 32 to 64 pixel sprites is unknown and should be judged by the owner on three samples before building more.
5. **Sound and music.** Nothing local exists. Options that need no model: a small synth written in JavaScript (Web Audio) or a tone generator in C, with the agent writing the note tables. A music model is out of scope for this machine.
6. **Codebench tasks for games.** Add 6 to 8 tasks with hidden tests: pure-logic games (tic-tac-toe with an AI, snake step function, a tile-collision function, a level parser, a card-game scorer). They separate models and show whether the game flow helps. Same format as `write-*`.
7. **Training data.** The curriculum generator (`tools/curriculum`) can get a family "build a small game from a one-line request" with real file trees and a policy in C, as the composed scenarios do for other stacks. Only worth doing after items 1 to 3 exist, because the data must show the verification loop.

## 5. Proposed order

| Step | Content | Effort (estimate) | Done when |
|---|---|---|---|
| G1 | `katana game check <folder>`: headless Chrome, console errors, screenshot, pixel and frame counters, text result | 1 to 2 days | a hand-written 40-line canvas game and a broken copy give different results |
| G2 | Game `KATANA.md` template and skill `game-new <name>` that writes the skeleton | 1 day | the skeleton passes `game check` untouched |
| G3 | Flow `game` in `flow.c` plus contract; `Render` allowed in it | 1 day | `katana flow sim` walks design to deliver |
| G4 | Sprite post-processing helper and sprite-sheet packer | 1 to 2 days | one FLUX image becomes a transparent sprite at a fixed size |
| G5 | 6 to 8 game codebench tasks | 2 days | `katana codebench verify` passes them |
| G6 | Curriculum family for games | 2 to 3 days | `katana curriculum check` 0 problems |
| G7 | Sound: tone table and Web Audio helper in the skeleton | 1 day | skeleton plays a beep on key press |

Everything is behind existing conventions (new things on by default, `=0` off; names in English; code in C for anything in the binary, Python allowed for helpers humans run).

## 6. Risks and open questions

- A 35B model with 3 billion active parameters writes game loops well but loses track in files over a few hundred lines; the "small files, pure logic" rule is not optional.
- Without vision the agent cannot judge how a game looks or feels. It can only check that it runs, draws something and responds to input. The owner remains the judge of fun and art.
- GPU sharing between the model and FLUX can fail or stall on 4 GB; art generation should be an explicit, separate step.
- Licensing of generated art and of any downloaded asset is the owner's decision.
- Game design (is it fun) cannot be measured by the engine; keep the scope to what can be checked.

## 7. Decisions needed from the owner

1. Target: browser games first (recommended) or something else?
2. Art: simple shapes first with art added later (recommended), or art from the start?
3. Whether to spend training time on games before the verification loop exists (recommended: no).
