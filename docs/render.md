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

## LoRA training of the FLUX.2 klein DiT, in C on the CPU (2026-11-24, branch feat-lora)

Owner decision: training happens inside katana. The quantized base (Q4_K/Q5_K/BF16 GGUF) stays frozen; only low-rank adapters train, in float32, with the same conventions as the `.kspec` adapters of `docs/specialization.md` (A random in +-1/sqrt(in), B zero so a new adapter equals the base, scale s = alpha/rank, AdamW with bias correction and global-norm clipping, optimizer state in a `.kopt` next to the file). Code: `src/render/fxlora.c` (modules, `.fxlora`), `fxback.c` (gradients of each piece), `fxblocks.c` (stored forward and backward of each block, the whole training step), `fxgrad.c` (gradient tests), `fxtrain.c` (the `katana flux-train` command).

**What is trained.** Rank-r adapters (default 16, alpha 16) on 80 matrices: in each of the 5 double blocks `{img,txt}_attn.qkv`, `{img,txt}_attn.proj`, `{img,txt}_mlp.0`, `{img,txt}_mlp.2` (8 each), in each of the 20 single blocks `linear1` and `linear2`. 23.1 M trainable weights at rank 16 (88 MB `.fxlora`). Modulation, time embedding, input/output projections and norms are frozen (the modulation vectors depend only on the timestep, a constant, so no gradient flows to them; the adaLN backward still computes d/shift and d/scale and is tested).

**Backward pass.** Per block, in float32: modulated LayerNorm, per-head RMSNorm (QK-norm), RoPE on 4 axes (inverse rotation), SwiGLU, gated residuals, and non-causal attention without ever storing the n x n matrix: the backward recomputes the scores in blocks of 48 queries/keys in two parallel passes (queries give dQ, keys give dK and dV, sharing the row log-sum-exp), so memory is linear in the token count. A frozen quantized linear returns only dX = dY W (the weight is dequantized row by row straight into transposed panels for each call, no second cached copy); a linear with an adapter also returns gA and gB. Gradient checkpointing per block: the forward keeps only the input of each of the 25 blocks, the backward redoes each block's forward with its intermediates, runs its backward and frees them.

**Dataset and loop.** `katana flux-train --pairs <dir> --out file.fxlora [--rank 16] [--alpha 16] [--steps 100] [--lr 1e-4] [--accum 1] [--size WxH | --long 512] [--txt-len 512] [--t-mode shifted|steps] [--wd 0] [--clip 1] [--warmup 0] [--save-every 20] [--eval-every 0] [--seed 1] [--resume] [--rebuild-cache]`. A pair is `<name>_cond.png` (the SketchUp view), `<name>_target.png` (the approved render), `<name>.txt` (caption) and optionally `<name>_lines.png` (line drawing, a second reference). The caption goes through `fxt_embed` and the images through `fxv_encode` once; the result is cached in `<out>.cache/` (invalidated by size, text length, reference count and caption hash; `--rebuild-cache` forces it). Loss: flow matching, x_t = (1-s) x0 + s e, target v = e - x0 (the same convention as the sampler of `fxgen.c`), mean squared error on the target tokens only; the condition image enters as reference tokens with time ids 10 and 20 exactly as at inference. s is `shifted` (default: u uniform in (0,1), s = e^mu / (e^mu + 1/u - 1) with the dynamic `fxg_mu` of the 4-step schedule) or `steps` (one of the 4 inference sigmas at random, which matches the distilled model's actual operating points; which of the two trains better is not measured). `--accum N` averages N samples per optimizer step. Checkpoints (`.fxlora` + `.kopt`, written atomically) every `--save-every` steps and at the end; `--resume` continues from them. A loss CSV is written next to the output. Text length defaults to 512 as at inference; a shorter `--txt-len` is only for tests.

**Using it.** `katana render generate --engine flux ... --lora file.fxlora [--lora-scale 1.0]`. The adapter is a separate low-rank path added to the output of each covered linear, Y += scale * s * (X A^T) B^T; the quantized weights are never touched or requantized, and without `--lora` the cost is one integer test per linear. The file carries the size and an FNV hash of the whole base GGUF and is refused for another base.

**What was checked (all on this machine, CPU, nothing on client images):**

| Check | Result |
|---|---|
| Finite differences (central, direction random, `katana render flux-train selftest-grad`), real Q4_K/Q5_K/BF16 weights, 6 image + 5 text tokens | attention dQ/dK/dV 2e-5 / 9e-6 / 1e-4; RMSNorm 7e-5; RoPE 4e-8; adaLN d/X, d/shift, d/scale 4e-5 / 2e-6 / 2e-6; SwiGLU 3e-4; double block (inputs and all 8 adapters, A and B) 1e-4 to 2.6e-3; single block 1e-4 to 2e-4; the whole 25-block model with checkpointing 4e-4 to 7.5e-3 (relative error of the directional derivative; the floor is float32 round-off in the finite difference, not the analytic gradient) |
| Gradients against PyTorch autograd in float32 (`tools/ref_lora_grad.py`: the same GGUF dequantized, diffusers `Flux2Transformer2DModel`, adapters hooked on the linears) | loss 1.59846532 (C) vs 1.59846568 (torch), 2e-7; prediction 1.4e-5 absolute; all 160 adapter tensors (A and B of 80 matrices) worst 4.3e-5, median 1.6e-5 of the largest gradient value; global norm ratio 1.000004 |
| Stored forward equals `fxd_double`, `fxd_single`, `fxd_forward` (with the adapter) | max difference 0 |
| Adapter path equals an explicit merged weight W + s B A (`double_blocks.0.img_attn.qkv`, 7 rows, W dequantized) | max error 5.8e-6 on outputs up to 8.5 (the adapter changed outputs by 0.64) |
| `.fxlora` save and load | identical, base hash checked, scale applied at load |
| Overfit on one synthetic pair (`katana render flux-train selftest-overfit 40`: 128x128, caption of 26 tokens, lr 1e-3, 3 warmup steps) | evaluation loss (fixed noise, sigmas 0.55 and 0.85) 0.2238 at step 0, 0.1523 (5), 0.1076 (10), 0.0676 (15), 0.0545 (20), 0.0389 (25), 0.0344 (30), 0.0252 (35), 0.0220 (40); the training loss is noisy because the sigma changes every step |
| That adapter in `render generate --engine flux --init cond.png --size 128x128 --steps 4 --seed 3` | mean absolute pixel difference to the training target 55.0/255 without the adapter, 8.3/255 with it (text length 512 at inference against 32 in training) |

**Cost, measured.** One optimizer step at 512x512 with one reference and a 512-position caption (1024 target + 1024 reference + 512 text = 2560 tokens, rank 16, micro-batch 1, 16 cores shared with other jobs): 140 s and 131 s (2 steps), peak RSS 24.4 GB (about 15 GB of that is the float32 weight panels; checkpoints, block intermediates and the activations of one block make up the rest). One step at 128x128 with a 32-position caption (160 tokens): about 10 to 12 s, dominated by dequantizing and repacking the weights for the backward, not by arithmetic (the 40-step overfit run took 477 s including 8 evaluations). A 100-step run at 512x512 is therefore about 3.8 hours; with a second reference (line drawing) add about 40% tokens. Measured with other agents compiling on the same machine, so treat as an upper bound.

**Limits, stated plainly.**
- Only one real-size configuration was timed and no real dataset was trained: whether a LoRA trained on SketchUp/render pairs improves renders of unseen scenes is not known. The overfit test shows the machinery learns and that the learned adapter changes inference in the right direction; it says nothing about generalization.
- The modulation, gate vectors, time embedding and the input/output projections have no adapters and receive no gradient (design decision, not a limitation of the backward: the adaLN and gate gradients are implemented only where the tests use them).
- Float32 finite differences have a floor near 1e-3 relative; the stronger evidence is the PyTorch comparison, which was run on 6 + 5 tokens, one timestep and one random adapter, not on a trained one.
- The weights are cached as float32 panels for the forward (about 15 GB, as in inference); the backward dequantizes each matrix again per call. Peak RSS is in the cost line above. The two parts of the pair of text lengths (training vs inference) should match for best results; the default is 512 for both.
- CPU only; no GPU code was touched. A step at 512x512 takes minutes, so a serious run is a night job (`--save-every`, `--resume`).

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
