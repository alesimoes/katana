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
| F1 | GGUF reader for the Q4_K_M transformer, tensor names, converter for the VAE and the rest | not started |
| F2 | text: Qwen3-4B hidden states of layers 9/18/27 concatenated (7680), through the existing Qwen3 code | not started |
| F3 | transformer: 5 double + 20 single blocks, 24 heads x 128, RoPE on 4 axes (32 each), modulation, compared with the reference | not started |
| F4 | VAE (32 latent channels, 2x2 patchify, batch-norm statistics), encoder for the reference image(s) | not started |
| F5 | flow-match Euler sampler (distilled, 2 to 4 steps), image reference as extra tokens, PNG out | not started |

Krea 2 Turbo (12.8B DiT, Qwen3-VL text encoder, Q4_K_S 6.84 GB, community licence): starts after FLUX, reusing the DiT pieces; image-to-image by SDEdit (latents + sigmas). Not started.
