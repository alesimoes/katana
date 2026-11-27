---
license: apache-2.0
base_model: black-forest-labs/FLUX.2-klein-4B
pipeline_tag: text-to-image
tags:
- gguf
- katana
- flux
- text-to-image
- image-editing
---

# katana-flux2-klein

GGUF files of FLUX.2 [klein] 4B for [katana](https://github.com/alesimoes/katana), a from-scratch inference engine written in C.

These are **format conversions only**. No retraining, no fine-tuning, no merging. The weights are the upstream weights, re-encoded into GGUF (the transformer is quantized to Q4_K_M; the other parts are kept at higher precision).

## Files

| File | What it is | Converted from |
|---|---|---|
| `flux-2-klein-4b-Q4_K_M.gguf` | the diffusion transformer, Q4_K_M | `transformer/` of the upstream repo |
| `flux2-vae.gguf` | the image autoencoder (VAE) | `vae/` of the upstream repo |
| `qwen3-4b-text.gguf` | the text encoder (Qwen3 4B) | `text_encoder/` of the upstream repo |

## Upstream and license

- Upstream model: [black-forest-labs/FLUX.2-klein-4B](https://huggingface.co/black-forest-labs/FLUX.2-klein-4B), by Black Forest Labs.
- License as written in the upstream model card: `license: apache-2.0` ("This model is licensed under the https://www.apache.org/licenses/LICENSE-2.0."). The upstream repository ships the full Apache License 2.0 text as `LICENSE.md`.
- The upstream card also says: "Nothing contained in this Model Card should be interpreted as or deemed a restriction or modification to the license the model is released under." The FLUX.2 [klein] 9B model has a different, non-commercial license; it is **not** part of this repo.

These converted files are distributed under the same license, Apache-2.0. Please read the upstream model card, including its safety and acceptable-use notes, before use.

## Use with katana

```
katana pull flux2-klein
```

katana downloads the files, checks the sha256 of each one against its built-in catalog, and uses them for image generation and editing. (The catalog id is the one planned in the katana repository; see `docs/distribution.md` there.)

The files are plain GGUF, but they follow katana's own tensor naming, so do not expect them to load in other tools.
