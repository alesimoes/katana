---
license: apache-2.0
base_model: google/embeddinggemma-2
pipeline_tag: feature-extraction
tags:
- gguf
- katana
- embedding
- feature-extraction
---

# katana-embeddinggemma

A GGUF file of EmbeddingGemma 2 for [katana](https://github.com/alesimoes/katana), a from-scratch inference engine written in C. katana uses it only to compute embeddings (an optional search filter); it is not a chat model.

This is a **format conversion only**. No retraining, no fine-tuning. The weights are the upstream weights, re-encoded into GGUF and quantized to Q8_0.

## Files

| File | What it is |
|---|---|
| `embeddinggemma-2-q8_0.gguf` | the embedding model, Q8_0 |

## Upstream and license

- Upstream model: [google/embeddinggemma-2](https://huggingface.co/google/embeddinggemma-2), by Google DeepMind.
- License as written in the upstream model card front matter: `license: apache-2.0`. The card header reads: "License: Apache 2.0" and links to https://ai.google.dev/gemma/docs/gemma_4_license.

Please read the upstream card and the license page it links to before use.

## Use with katana

```
katana pull embeddinggemma-2
```

katana downloads the file and checks its sha256 against its built-in catalog. (The catalog id is the one planned in the katana repository; see `docs/distribution.md` there.)

The file follows katana's own tensor naming, so do not expect it to load in other tools.
