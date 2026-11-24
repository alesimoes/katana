# katana render (planned): a fast preview of an architectural scene

Status (2026-11-24): **stages 1 to 7 are written and checked against the float32 reference; stages 8 to 10 are open.** Everything runs on the
CPU in float32 (`katana render generate`, `katana render convert`, `katana render selftest`); the FLUX.2 klein path also has a CUDA backend (see "CUDA path" at the end); Stable Diffusion 1.5 is CPU only. Decision of the
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

- **Render is not a tool of the lean list yet.** The desktop calls `katana render generate` directly from its image mode (attach an image, pick Stable/FLUX/Krea); the agent (Qwen) is not in that path and does not rewrite the prompt. The engine itself treats the request: lexical analysis, embedding for what the words do not recognise and for priority (`sdspec.c`), then the engine adapter.
- **A `Render` tool for the agent comes after FLUX is ready.** The specialized agent leaves the tools section out of its prompt because it knows the 12 tools from training (`docs/specialization.md`, "Tools known by training"); a tool created later is unknown to it. Order: (1) now: no tool, desktop direct; (2) when FLUX is stable: a one-line declaration of Render only, found through ToolSearch so the base prompt does not change; (3) at the final curriculum: Render conversations in the training data, then the declaration is dropped like the others.
- **The curriculum is not touched by this work.** It is still being built and tested; the final version is generated from zero at the END of the UIOE construction (`docs/plan.md` T5; `tools/curriculum/generate.sh`, trained once, at night). Version names stopped at v7 (2026-11-23); v6 and v7 data were deleted, and the curriculum is now updated in place with no version name. Nothing of render or embedding is in any training data yet.


## CUDA path (FLUX.2 klein, 2026-11-24)

Owner decision (2026-11-24): the engine gets a CUDA backend for the speed on NVIDIA GPUs, although the project avoids third-party software. This overrides the "nothing proprietary" rule **for the GPU layer only** (also recorded in `CLAUDE.md`). What is and is not used:

- **No SDK, no library.** `src/engine/cu_rt.c` loads the driver with `dlopen("libcuda.so.1")` (Windows: `nvcuda.dll` through `LoadLibrary`; the Windows branch is written but **untested**) and uses the driver API only (context, stream, events, `cuMemAlloc`, `cuLaunchKernel`, `cuModuleLoadDataEx`). Nothing is linked: without the driver, without a device or with compute capability below 7.5 `cu_init()` returns 0 and the CPU path runs as before. No cuBLAS, no cuDNN, no nvcc at build time.
- **Kernels are PTX text** (PTX 7.0, target `sm_80`, JIT-compiled by the driver for the actual card; the driver's message is printed if it rejects the module). The matrix products (`src/engine/cu_ptx.c`) are emitted by C code: `mma.sync.m16n8k16` (f16 in, f32 accumulate), `ldmatrix`, 3-stage `cp.async` pipeline, XOR-swizzled shared memory, Q4_K / Q5_K / BF16 weights decoded in registers right before the mma (the weights stay quantized in VRAM, as in the GGUF: 2.26 GB for the transformer). The other kernels (LayerNorm + modulation, RMSNorm + RoPE + q/k/v split, SwiGLU, fused flash-style attention with online softmax, GroupNorm coefficients, convolution as implicit GEMM with the nearest-neighbour upsample and GroupNorm + SiLU fused into the tile load) are written in CUDA C in `tools/cuda/kern.cu` and compiled to PTX **at development time** with NVRTC (`tools/cuda/gen.sh`, needs the NVRTC library from a pip/CUDA install, used only by whoever edits the kernels); the generated PTX is committed as `src/engine/cu_kern.inc`. The katana binary needs none of it.
- **Precision modes.** `KATANA_CUDA_PREC=exact` (default) keeps every activation and weight as two f16 halves (high part + residual) and the product sums three terms (hi*hi + lo*hi + hi*lo, about 22 bits), with a fresh f32 accumulator per 32-wide k tile added in f32 (the tensor core accumulator is not IEEE-exact; this alone cut the product error 32x for 4% of the time). `KATANA_CUDA_PREC=fast` is plain f16 operands (one term): about 2.4x faster, but it **fails the 1e-3 tolerance** (transformer output 2.6e-3, VAE 1.2e-3), so it is a flag, not the default.
- **Backend selection.** `KATANA_BACKEND=cuda|vulkan|cpu`; `KATANA_CPU=1` forces the CPU; `KATANA_FLUX_CUDA=0` turns this path off. Default for FLUX.2 klein on a machine with an NVIDIA driver: CUDA (exact), because it is the fastest verified backend measured here (table below); if the card is missing, the VRAM is not enough (weights plus an activation reserve are checked against `cuMemGetInfo`, never a fixed 4 GB) or any launch fails, the call returns 0 and the CPU path computes the same step (a message is printed, and the CUDA path stays off for the rest of the process). Vulkan stays the backend on non-NVIDIA GPUs (branch `feat-vulkan`; the merge has to make its `fxd_gpu_blocks` hook honour `KATANA_BACKEND`).
- Each kernel launch is short (largest: one 800 x 27648 x 3072 product, about 40 ms; attention about 2 ms per block; the largest VAE convolution about 100 ms) so a Windows display driver watchdog (TDR) should not trigger; not verified on Windows.

What runs on the GPU: the whole transformer (5 double + 20 single blocks, residual stream f32, one upload of image, text, modulations and RoPE tables per step and one download of the velocity), and the VAE decoder (convolutions, resnets, middle attention, upsampling). Still on the CPU: the text encoder (Qwen3-4B, about 5 s per prompt), the time embedding and modulation vectors (single-vector products, a few hundred ms), the VAE encoder (reference images), the sampler, PNG writing. Stable Diffusion 1.5 is CPU only. The dense LLM path (decode and prefill of the working model) has **not** been moved to CUDA yet.

Verification against the float32 references (`/mnt/dados/render-ref`), default `exact` mode, RTX 3050 Laptop: `flux-dit selftest` (fluxB) worst block 1.1e-5, final output 5.5e-5 (CPU path 1.8e-5, tolerance 1e-3); `flux-vae selftest` (fluxC) decoder 3.4e-5 to 5.2e-5 max, 1.7e-5 to 2.8e-5 mean (tolerance 1e-3 / 1e-4); `flux-gen selftest` (fluxD, 320x192, 2 steps, reference image): latents 8.7e-6 and 4.9e-5, final image 5.2e-4, mean pixel difference 0.002 of 255 (all pass; with the single-f16 `fast` mode the transformer and the VAE fail). Per-kernel (random data, double-precision CPU reference): product exact 1.9e-5 without and 5.8e-7 with the per-tile accumulator (n = 768, 9216 x 3072, Q4_K), plain f16 product 2.9e-4, attention 3.6e-4 (f16, N = 300..2800).

Measured speed (same session, `flux-dit bench`, `flux-vae bench`, shared GPU serialised with a lock):

| item | CPU (8 threads) | Vulkan (`feat-vulkan`) | CUDA exact | CUDA fast (fails tolerance) |
|---|---|---|---|---|
| transformer step, 720 image + 64 text tokens | 9.9 s | 1.62 s | 1.42 s | 0.58 s |
| transformer step, 720 image + 512 text tokens | | 2.62 s | 2.07 s | |
| VAE decode 576x320 | 4.8 s | 1.6 s (their number, not re-measured) | 0.68 s | 0.30 s |
| 2-step 576x320 image, `render generate --engine flux` | | 18.3 s (their number) | 13.9 s (text encoder on the CPU about 5 s) | |
| VRAM | | 2.1 GB weights | 2.26 GB weights (+ about 0.5 GB activations in the step, 0.6 GB in the VAE) | same |

Roofline (card limits from `cuDeviceGetAttribute`: 16 SMs, 1500 MHz, so 12.3 TFLOPS f16/f32-accumulate peak at 256 FMA/clock/SM; 192 GB/s): the plain f16 product reaches 10.4 TFLOPS (85% of that peak) on a 768 x 9216 x 3072 Q4_K product and the fused attention 9.9 TFLOPS (80%) at 2800 tokens, 8.1 TFLOPS (66%) at 784; the exact product does three mma per element, so it reaches 3.9 TFLOPS counted as useful work (32% of peak, 96% of the peak for the mma it actually issues): at this precision the products are at the hardware limit of this card, the remaining time is in the product (1.32 of 1.42 s). Laptop clocks under load may be lower than the 1500 MHz reported, so the percentages are against the nominal peak.

Not done, and why: CUDA graphs (the step has about 700 launches, measured launch overhead is small next to 1.4 s of products; would matter for the fast mode); split-K / stream-K (all shapes here have enough tiles: 720+ tokens x 3072+ rows gives at least 42 blocks on 16 SMs; thin shapes are the single-vector modulation products, left on the CPU); autotuning of tile sizes (one 128x128 tile with 8 warps was used; no second variant measured); the "scale after mma" exact variant (2 mma terms instead of 3, at most about 1.2x because of the extra ALU work) ; GPU modulation/time-embedding products; weight streaming for models that do not fit (the whole 2.26 GB is resident or the CPU runs the step); VAE encoder and text encoder on the GPU; Windows testing; the dense LLM decode/prefill path.
