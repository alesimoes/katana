# katana render (planned): a fast preview of an architectural scene

Status (2026-11-24): **stages 1 to 7 are written and checked against the float32 reference; stages 8 to 10 are open.** Everything runs on the
CPU in float32 (`katana render generate`, `katana render convert`, `katana render selftest`); there is no GPU path yet. Decision of the
owner: put an image-generation pipeline inside katana.

| Stage | What | Check against the reference (max error / max value) |
|---|---|---|
| 1 | weights downloaded, reference tensors from diffusers (`/mnt/dados/render-ref/`, outside the repository) | 28 tensors, no NaN |
| 2 | `katana render convert`: UNet, CLIP, VAE, tokenizer to `sd15-realistic-vision.gguf`; ControlNet to `sd15-controlnet-canny.gguf` | 1470 tensors identical value by value; 686 UNet names and shapes identical to diffusers; 48894 merges and 49408 tokens identical |
| 3 | convolution (im2col in blocks), GroupNorm, SiLU, nearest upsampling (`sdops.c`) | 7e-7 or better in five layer tests |
| 4 | CLIP tokenizer and text encoder (`sdclip.c`) | 13 of 13 prompts identical to CLIPTokenizer; encoder 2e-7 |
| 5 | UNet and ControlNet (`sdunet.c`) | 12 residuals + mid, UNet step, UNet with residuals: 4.3e-6 or better |
| 6 | VAE decoder and encoder (`sdvae.c`) | 1.4e-6 and 5.4e-6 |
| 7 | Euler sampler, PNG writer, Canny, `generate` (`sdgen.c`) | sampler 7e-7 against EulerDiscreteScheduler; text to image works end to end (a 512x512 living room, 354 s) |
| 8 | preview from a SketchUp PNG with the Canny ControlNet (`--control`, `--init`) | works: a SketchUp reception render became a photographic preview in 317 s (576x320). Geometry kept (arch, desk, console, armchair, ceiling light); colours and materials drift (a burgundy wall came out slate blue at strength 0.75) and text on walls is garbled, which Stable Diffusion 1.5 cannot write. Judged by eye on one scene; whether the preview predicts the outside service is still unknown |
| 9 | Vulkan kernels | not started; the training job holds the GPU |
| 10 | agent tool and the package for the outside service | not started |

Speed (one measurement, with the training job running on the same machine): one UNet step about 9 s, one ControlNet step about 4 s, VAE
decode 512x512 about 12 s; a 512x512 image in 20 steps with guidance takes about 6 minutes on the CPU.

## Goal

The owner models a project in SketchUp and wants a quick, private, local **preview** of the realistic image. The final client image comes
from an outside service (Nano Banana); katana is the simulation. So the preview must keep the geometry of the model and give a fair idea
of composition, light and materials. It does not have to match the final image. Whether the preview predicts the final image is **not
known**: it is the first thing to check with real scenes (five scenes, preview against the service's result).

## Chosen pipeline (Stable Diffusion 1.5 family)

- Checkpoint: Realistic Vision V6.0 B1 (a Stable Diffusion 1.5 fine-tune; licence on its official page: creativeml-openrail-m; read the full
  text before client use).
- Control: ControlNet v1.1 for SD 1.5. First `canny` (the edge filter is classical code, no extra model); `depth` and `mlsd` need a
  preprocessing model or a depth pass exported from SketchUp, so they come later.
- Why this one: it is the only candidate that can run on the development laptop (RTX 3050 4 GB, 64 GB RAM). Larger and newer
  models (FLUX.2 klein 4B, Qwen-Image-Edit 20B) stay as a later quality step.

## What katana already has

`src/vision/`: PNG, JPEG and BMP decoders (`png.c`, `jpeg.c`), resize (`resize.c`), CPU transformer pieces (`tensor.c`: packed matrix
product, layer norm, softmax, GELU, attention). GGUF reading, tokenizers (BPE), `katana convert` (safetensors to GGUF), the Vulkan layer.

## What is missing

2D convolution, group normalisation, SiLU, nearest-neighbour upsampling, a CLIP tokenizer variant (lower-case, end-of-word marker), the
UNet, the VAE (encoder and decoder), the ControlNet branch, the sampler, a PNG encoder, the Canny edge filter, a converter for the SD 1.5
checkpoint layout, and later Vulkan kernels for convolution and attention.

## Stages (each one is checked against a float32 reference before the next)

1. Weights and reference: download the checkpoint and the ControlNet; build a numeric reference outside the repository (as was done for
   `katana embed`), saving intermediate tensors.
2. `katana convert` for the SD 1.5 layout: UNet, VAE and CLIP in one GGUF, ControlNet in another. Keep fp16 for convolution weights.
3. CPU ops: conv2d (im2col plus the existing packed product), group norm, SiLU, upsample. Each compared with the reference.
4. CLIP text encoder (12 layers, 77 tokens): embeddings against the reference.
5. UNet forward, one step: output against the reference.
6. VAE decoder (then encoder, for image-to-image): image against the reference.
7. Sampler (Euler or DDIM) with classifier-free guidance, PNG encoder: first text-to-image picture.
8. ControlNet branch and Canny: preview from a SketchUp PNG.
9. Vulkan kernels for convolution and attention, so one image fits the laptop GPU in a reasonable time (the speed is not known yet).
10. Integration: `katana render` command and an agent tool; the agent writes the prompt from the scene's materials and lighting and also
    writes a package (clay image, edges, prompt) for the outside service.

## Rules and limits

- The engine is C, and everything it uses is C. Python is used only for the reference outside the repository.
- Correctness is checked against the reference (cosine or maximum error per tensor), not by looking at pictures alone.
- The development GPU is shared with the training job; stages 1 to 8 run on the CPU, so they do not disturb it.
- Open questions: speed on the laptop GPU (not measured), quality against the final service (not measured), licence reading for client
  work, and what SketchUp can export (clay view, edges, a depth pass) to feed the control image.

## Engines FLUX and Krea in C (WIP, planned 2026-11-24)

The desktop has three engine selectors (stable, flux, krea); `katana render generate --engine flux|krea` exits with code 3 ("WIP, not available") until each engine exists in C. Same discipline as Stable: every stage is compared with float32 reference tensors from the diffusers pipeline (`/mnt/dados/render-ref/`) before the next one starts.

FLUX.2 klein 4B (distilled, 4 steps, guidance 1.0, Apache-2.0):

| Stage | What | Status |
|---|---|---|
| F1 | GGUF reader for the Q4_K_M transformer, tensor names, converter for the VAE and the rest | done 2026-11-24 (`flux-convert-text`, `flux-vae convert`; the transformer GGUF is read as is) |
| F2 | text: Qwen3-4B hidden states of layers 9/18/27 concatenated (7680) | done, verified against float32 (`flux-text --selftest`, error 4e-7 to 8e-6) |
| F3 | transformer: 5 double + 20 single blocks, RoPE on 4 axes | done, verified (`flux-dit selftest`, final error 1.8e-5); weights cached as F32 panels (~15 GB per process), blocked attention (`KATANA_FLUX_WEIGHTS=none` / `KATANA_FLUX_ATT=simple` go back to the plain path) |
| F4 | VAE (32 latent channels, 2x2 patchify, batch-norm statistics) | done, verified (`flux-vae selftest`, 8e-7 to 2.5e-6) |
| F5 | flow-match Euler sampler, reference images as extra tokens, `--engine flux` | done, verified end to end against the pipeline (`flux-gen selftest`: 1.8e-4 on the final image, 0 of 255 mean pixel difference); `--init` and `--init2` are the references, the request goes in as prose (`sds_nl_prose`, no negative prompt: the model is distilled); two references written, not yet exercised |

Krea 2 Turbo (12.8B DiT, Qwen3-VL text encoder, Q4_K_S 6.84 GB, community licence): starts after FLUX, reusing the DiT pieces; image-to-image by SDEdit (latents + sigmas). Not started.

## Prompt engine (UIIE): request -> image spec -> engine prompt (src/render/sdspec.c)

`katana render spec "<request>" [--ref preview.png] [--lcm] [--final [--final-engine flux|krea]] [--json] [--no-rag]` shows the canonical image spec and what each engine receives; `katana render generate --nl "<request>"` generates from it (`--prompt` stays the raw path; weights `(term:1.3)` work in both).

Layers, each replaceable on its own:
1. **Router + scene compiler** (code, no model call besides the embedding): classifies the domain (architecture exterior/interior, product, person, landscape, general) and fills only the visual gaps the request left (framing, lighting, camera, negatives, aspect) from a preset table. Concept words (style, light, composition, camera, aspect, purpose) are matched in PT and EN; the rest is content, translated by a small PT->EN lexicon. A clause no word recognises goes to the embedding model (`es_rank`) against the concept descriptions (threshold `KATANA_SPEC_RAG`, default 0.82, gap `KATANA_SPEC_GAP` 0.05); `--no-rag` skips it. Words without a translation are reported: CLIP only reads English.
2. **Priority** (embedding, only when the prompt exceeds the 75-token CLIP budget): items are ranked by similarity to the request; subject and style are always kept, the rest is cut by rank.
3. **Configuration engine** (code): aspect -> width x height (multiples of 64), steps, guidance, strength; the user never asks for "30 steps".
4. **Engine adapters**: Stable gets weighted CLIP chunks (subject `(x:1.2)`, no automatic "8k" keywords); the final engine gets prose, and the approved preview as reference (`--ref`, strength 0.40) so the architecture does not change between preview and final.

Two modes with the same spec: **preview** (Stable + LCM, composition first) and **final**. The final engine is FLUX klein or Krea 2 Turbo, still to be tested against each other (both WIP, see above); `--final-engine` picks one. Inpaint/outpaint requests ("remove", "change the background") are recognised and reported; there is no mask yet, so they run as image-to-image. Geometry control stays with the existing ControlNet canny / line drawing and image-to-image; when dimensional fidelity matters the geometry source is the 3D model.

FLUX.2 klein GGUF (`flux-2-klein-4b-Q4_K_M.gguf`, architecture `flux`, 149 tensors): only the transformer is in it, in the BFL native names, not the diffusers ones: `double_blocks.N.{img,txt}_attn.{qkv,proj}.weight`, `{img,txt}_attn.norm.{query,key}_norm.scale`, `{img,txt}_mlp.{0,2}.weight` (5 blocks); `single_blocks.N.{linear1,linear2}.weight` + `norm.{query,key}_norm.scale` (20 blocks); `double_stream_modulation_{img,txt}.lin`, `single_stream_modulation.lin`, `time_in.{in,out}_layer`, `img_in`, `txt_in`, `final_layer.{adaLN_modulation.1,linear}`. Types: F32 (60: the norm scales), Q4_K (50), Q5_K (30), BF16 (9). The engine already has Q4_K/Q5_K dot kernels (`engine/cpu_kernels.c`); BF16 and the 3072-wide shapes need the image path. The text encoder (Qwen3-4B) and the VAE are not in the GGUF: they come from the safetensors in `models/flux2klein/FLUX.2-klein-4B/` (text_encoder bf16, vae) through a converter like `render convert`.

## Agent integration and training (decision 2026-11-24)

- **Update 2026-11-24 (branch feat-brief): the `Render` tool exists.** It is in the tool table (`src/tools/render.c`), found by `ToolSearch select:Render`, not in the short prompt declarations and not in any training data, so `katana.no_tool_decl` models are unaffected (a model never trained on it will only use it if told). It runs `katana render generate` with `image`, `request`, `engine` (`stable` default, or `flux`), `out` (default: next to the image, `-render-<engine>.png`) and `brief`, and returns the path written. It blocks for the minutes the render takes. `tools/tooltest/render.sh` (21 checks, fake engine through `KATANA_RENDER_BIN`) covers it; no real render was run. The final curriculum must include Render conversations (`docs/curriculum.md`). The paragraph below is the earlier plan.
- **(earlier plan) Render is not a tool of the lean list yet.** The desktop calls `katana render generate` directly from its image mode (attach an image, pick Stable/FLUX/Krea); the agent (Qwen) is not in that path and does not rewrite the prompt. The engine itself treats the request: lexical analysis, embedding for what the words do not recognise and for priority (`sdspec.c`), then the engine adapter.
- **A `Render` tool for the agent comes after FLUX is ready.** The specialized agent leaves the tools section out of its prompt because it knows the 12 tools from training (`docs/specialization.md`, "Tools known by training"); a tool created later is unknown to it. Order: (1) now: no tool, desktop direct; (2) when FLUX is stable: a one-line declaration of Render only, found through ToolSearch so the base prompt does not change; (3) at the final curriculum: Render conversations in the training data, then the declaration is dropped like the others.
- **The curriculum is not touched by this work.** It is still being built and tested; the final version is generated from zero at the END of the UIOE construction (`docs/plan.md` T5; `tools/curriculum/generate.sh`, trained once, at night). Version names stopped at v7 (2026-11-23); v6 and v7 data were deleted, and the curriculum is now updated in place with no version name. Nothing of render or embedding is in any training data yet.

## Briefing to prompt, and the logo (2026-11-24, branch feat-brief)

**FLUX prompt from the briefing, by rule.** `katana render spec --brief briefing.md --engine flux [--lines]` prints the prompt and generates nothing;
`katana render generate --engine flux --brief briefing.md --init view.png [--init2 lines.png] [--nl "extra request"] --out out.png` uses it
(`--engine stable --brief` uses the condensed Stable prompt of `katana render brief`). The text comes from `sdb_flux_prose` in `src/render/sdbrief.c`, no
hand summary: fidelity rules (exact camera, lens, framing, perspective; nothing added, removed, moved or resized; no extra decoration, plants, people,
lamps, artwork or written text when the briefing forbids them), each material with its colour (RGB to a name by HSV: burgundy, slate blue-grey, off-white...;
the RGB numbers stay only for painted walls), its pattern scale in cm and its share of the frame, the main objects (counter, chair), the sun elevation, the
colour temperature and the types of lights that are on, and the style requests of the briefing. Materials of the same phrase add their share; they run from the largest
to the smallest; items under 1% ("~0%") are left out.

Material type comes only from what the briefing writes after the name, never from the name. `"Arenito": material, repetition of the pattern 275x184 cm` becomes
`surface material "Arenito", pattern 275x184 cm, 7%`: no stone, no sandstone (the name is only the SketchUp label; the Stable builder was fixed the same way and
its "stone veining" style phrase was removed). `AK_PISO CONCRETO 0x` typed concrete stay `concrete floor tiles, pattern 90x90 cm` (the four entries add to 11%);
the one typed porcelain, whose name says concrete, is `concrete-look porcelain floor tiles, pattern 100x100 cm`. Names without a known type (`ANGRA_21_PRATA`,
`HIBISCO`) stay neutral surfaces: a colour word inside a name is not used.

The text encoder has 512 positions. The recepcao briefing with every material came to 654 positions (measured with `katana render flux-text`), so the last
positions (style, closing) would have been cut. The prose now drops the smallest materials until it is at most 1740 characters (about 500 positions; the final
prompt of the recepcao briefing measured 474 positions with 4 of the 13 materials left out, and says so on stderr). Prompt printed for
`/mnt/dados/render-ref/briefs/recepcao_v1.md` (client briefing, not committed):

> Photorealistic architectural interior photograph of a reception, made from the reference view (a 3D model export). Keep the exact camera position, lens, framing, perspective and proportions. Every wall, opening, ceiling, floor, piece of furniture and object keeps its position, size and shape: nothing is added, removed, moved or resized, no extra decoration, plants, people, lamps, artwork or written text. Every edge of the line drawing stays in place. Each material sits exactly on its surfaces in the view, with its real colour and pattern scale (share of the frame in %): slate blue-grey painted wall (RGB 99,112,123), 25%; burgundy painted wall (RGB 119,79,81), 23%; concrete floor tiles, pattern 90x90 cm, 11%; off-white painted wall (RGB 228,222,204), 10%; surface material "Arenito", pattern 275x184 cm, 7%; fabric "H10248", pattern 40x40 cm, 4%; surface material "ANGRA_21_PRATA", pattern 187x281 cm, 4%; surface material "HIBISCO", pattern 10x10 cm, 4%; concrete-look porcelain floor tiles, pattern 100x100 cm, 3%. Main objects: reception counter, chair. Lighting: bright daylight from a high sun (solar elevation 66.5 degrees) matching the shadows of the view; warm artificial light at 3000 K from recessed spotlights, linear LED strip, round ceiling lamp, organic pendant lamp, mirror light, sphere lamp, support light; unlisted luminaires stay off. Style: natural realistic interior photography, full-frame camera, no computer graphics look, soft light with real falloff, deep contact shadows, soft light gradients on the walls, visible wood grain and fabric weave, faithful colours, never oversaturated, neutral to slightly warm white balance, clean, elegant, authentic. 16:9, no borders, no watermark.

Limits, stated plainly: the quoted name `"Arenito"` is still in the prompt as a label, and a text encoder that reads Portuguese may still link it to the
stone (not tested: nothing was generated); FLUX cannot match an RGB number, only a colour word, so how close the colours come is unknown; the date and
place of the "Luz do dia" line are not used; the main-object list only knows the words of `sdb_build` (counter, chair, armchair, sofa, table, mirror, bed); `--image3`
(material samples) is still not used. `katana render brief selftest [briefing.md]` checks the rules (15 checks, on an embedded text with the same material
lines and, when a path is given, on the real file): no `stone`/`sandstone`/`pedra`, floor says concrete with the pattern size, wall colours present,
neutral names kept, 0% items left out, fidelity rules, lights, objects and the length limit.

**Logo compositing (`src/render/sdlogo.c`).** No image model writes a brand; the real logo goes in afterwards, on the CPU.
`katana render logo --image render.png --logo logo.png --box x,y,w,h [--perspective x1,y1,x2,y2,x3,y3,x4,y4] [--blend] [--out out.png]`.
The logo is a PNG with alpha (the PNG decoder returns the alpha channel on request now). `--box` fits the logo inside the box without distortion, centred;
four numbers between 0 and 1 with a decimal point are fractions of the image. `--perspective` gives the four destination corners (top-left, top-right,
bottom-right, bottom-left): a homography solved by Gauss elimination, 2x2 bilinear samples per pixel on premultiplied alpha, a logo much larger than its target is
halved first. `--blend` is not Poisson blending: the logo luminance is pulled toward the mean luminance of a ring around the box (exponent 0.35, clamped to 0.55..1.5),
tinted 35% toward the ring colour, and modulated by the low-frequency luminance of the render under the box (blurred window of a quarter of the box, 60%, clamped).
Without `--blend` it is plain alpha. `render generate` takes `--logo`, `--logo-box`, `--logo-perspective`, `--logo-blend` and keeps the render without the logo as
`<out>-nologo.png`; the options are checked before generating. `katana render logo selftest` (20 checks on synthetic images, nothing from a client): PNG alpha round
trip, box placement, nothing outside the box changes, transparent areas leave the wall, the four corners of a perspective quad land on the right colours, blend
darkens a white logo on a dark wall without a colour cast, fractions, degenerate quads. Limits: the logo is flat (no relief, no reflection, no wall texture or
noise over it), the box or corners are given by the user (nothing detects the wall plane), and it was not tried on a real render.

**Desktop.** In image mode the "briefing" and "logo" buttons attach the files; the backend passes `--brief` and `--logo --logo-box --logo-blend` (`desktop-qt/README.md`).
