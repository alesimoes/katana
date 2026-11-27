# Licenses of the files to be hosted

Quotes are copied from the local upstream copies under `models/` (checked 2026-11-27). "Redistribution" is my reading of what the local files say; it is not legal advice. Anything not settled by the local text is marked unclear or BLOCKED, and no model card is written for it.

| File (hosted repo) | Upstream | License as written locally | Redistribution |
|---|---|---|---|
| `flux-2-klein-4b-Q4_K_M.gguf` (katana-flux2-klein) | black-forest-labs/FLUX.2-klein-4B (transformer) | `license: apache-2.0`; "This model is licensed under the https://www.apache.org/licenses/LICENSE-2.0."; full Apache 2.0 text in `LICENSE.md` | yes |
| `flux2-vae.gguf` (katana-flux2-klein) | same repo (`vae/`) | covered by the same repo license, apache-2.0 | yes |
| `qwen3-4b-text.gguf` (katana-flux2-klein) | same repo (`text_encoder/`, Qwen3ForCausalLM) | covered by the same repo license, apache-2.0; the local files do not carry a separate Qwen license | yes (see note 1) |
| `embeddinggemma-2-q8_0.gguf` (katana-embeddinggemma) | google/embeddinggemma-2 | `license: apache-2.0`; header: "License: Apache 2.0" linking to https://ai.google.dev/gemma/docs/gemma_4_license | yes (see note 2) |
| `sd15-realistic-vision.gguf`, `sd15-realistic-vision-lcm.gguf` (katana-sd15) | Realistic Vision V6.0 (SG161222; the local README gives no upstream repo link, only a CivitAI page) | `license: creativeml-openrail-m` (no license text in the folder) | BLOCKED (see note 3) |
| `sd15-realistic-vision-lcm.gguf` (katana-sd15) | latent-consistency/lcm-lora-sdv1-5 merged in | `license: openrail++` (no license text in the folder) | BLOCKED (see note 3) |
| `sd15-controlnet-canny.gguf` (katana-sd15) | lllyasviel/control_v11p_sd15_canny (local copy "duplicated_from: ControlNet-1-1-preview/control_v11p_sd15_canny") | `license: openrail`; "The CreativeML OpenRAIL M license is an Open RAIL M license, adapted from the work that BigScience and the RAIL Initiative are jointly carrying" | BLOCKED (see note 3) |
| VAE inside the sd15 files | stabilityai/sd-vae-ft-mse | `license: mit` (the MIT text is not in the folder) | unclear (see note 4) |

## Notes

1. The text encoder is Qwen3 4B as shipped inside the FLUX.2-klein-4B repo. I rely on that repo's Apache-2.0 declaration; the local copy has no separate Qwen notice.
2. The embeddinggemma front matter says `apache-2.0`, but the license link in the header points to a Gemma license page (`gemma_4_license`). The identifier is unambiguous, yet the link target could not be checked offline. The owner should open the link before publishing and confirm that it matches Apache 2.0. If it turns out to be the Gemma terms, the card must carry the Gemma notice and use "license: gemma" instead.
3. BLOCKED: the OpenRAIL family licenses allow redistribution only with conditions (the use restrictions must be passed on to recipients and the license text provided). The local folders contain only the one-line identifier or a link, not the license text, and the Stable Diffusion 1.5 base weights (`runwayml/stable-diffusion-v1-5`, named as `base_model` by the ControlNet and LCM-LoRA cards) have no license text locally either. Because the conditions cannot be confirmed from the local files, no card is written and `hf-upload.sh` skips the group. To unblock: add the full CreativeML OpenRAIL-M / OpenRAIL++ texts and the use restrictions to the card, confirm the Realistic Vision upstream terms (its card says only `creativeml-openrail-m`), and add a `cards/katana-sd15.md`.
4. MIT requires keeping the copyright notice; the folder does not include it. Only matters if the VAE is hosted on its own; here it is part of the BLOCKED sd15 group.
