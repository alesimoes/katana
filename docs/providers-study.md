# Study: inference providers and what serves katana (Nov/2026)

Research done on 16/11/2026. The research was quick, and most of the sources found are vendor
blogs or SEO texts, which disagree with one another on versions and numbers. What is marked
**(verify)** was not confirmed in a primary source. Sizes and licenses come from the model cards, when
they were found; the rest comes from prior knowledge up to mid-2026.

## 1. Overview of open models

- Chinese labs lead open weights: DeepSeek V4 (Pro/Flash, Apr/2026), Kimi K2.6 and
  K3 (Jul/2026), GLM-5.x (5.1, 5.2 in Jun/2026, 5.3 and 5.3-Flash), Qwen3.6/3.7/3.8 and MiniMax M3.
  Sources: https://deepinfra.com/blog/kimi-k3-vs-deepseek-v4-pro-vs-glm-5-2 ,
  https://ai2.work/blog/chinese-labs-own-the-open-weight-leaderboard-glm-deepseek-qwen ,
  https://agentic-up.pages.dev/posts/open-source-ai-model-landscape-june-2026/ ,
  https://www.contextstudios.ai/blog/deepseek-v4-the-open-source-pricing-earthquake
- Artificial Analysis index at the end of Aug/2026, according to AI2Work: Kimi K3 (60), GLM-5.3 (60),
  Qwen3.8 2.4T (58) and GLM-5.3-Flash (57) **(verify)**.
- gpt-oss (20b/120b, Apache 2.0, native MXFP4) remains the cheap and easy-to-host American option.
  Meta began prioritizing closed models, and Llama lost relevance.
- Licenses: Apache 2.0 became the standard (Qwen, gpt-oss, Gemma 4). DeepSeek, Kimi and GLM use MIT
  or modified MIT.

## 2. Providers

The table summarizes what is known about each one. Catalogs change every week; to check, use the
models page of each provider and the HF provider selector (https://huggingface.co/docs/inference-providers).

| Provider | Catalog highlights | Disclosed techniques | Speed/price (context) |
|---|---|---|---|
| DeepInfra | DeepSeek V4, Kimi K3, GLM-5.x, Qwen3.x, gpt-oss | H100/B200 GPUs, FP8/FP4, continuous batching, prefix cache | Prices among the lowest; did a 107 million dollar round (https://deepinfra.com/blog/best-open-source-llm-api-providers) |
| Novita | DeepSeek V4 Pro/Flash, Kimi K2.6, Qwen3.6, GLM-5.1, MiniMax M3 (https://blogs.novita.ai/open-source-llm-guide-2026/) | OpenAI-compatible API, FP8 quantization | DeepSeek V4 Pro quoted at about US$ 0.44 / 0.87 per million tokens |
| Groq | gpt-oss, Llama, Qwen, Kimi | Own chip (LPU) with on-chip SRAM, compiled deterministic execution; now under NVIDIA (Groq 3 LPX generates candidate tokens and Rubin GPUs verify: https://futurumgroup.com/insights/nvidia-groq-3-lpxs-promise-of-worlds-fastest-inference-enters-full-production/) | About 476 tok/s on gpt-oss-120B (https://costbench.com/best/fastest-llm-inference/) |
| Cerebras | gpt-oss-120B, Qwen3, GLM | Wafer-sized chip, weights in SRAM, speculative decoding | About 3000 tok/s on gpt-oss-120B (same source) |
| Baseten | Custom (DeepSeek, Kimi, Qwen) | TensorRT-LLM, EAGLE/MTP, separate prefill and decode, FP8/FP4 | Dedicated; per GPU |
| Featherless AI | Thousands of fine-tunes (Llama/Qwen/Mistral) | Fast serverless model swapping, LoRA and multi-model | Fixed monthly subscription |
| Z.ai | GLM-4.5/4.6/4.7, GLM-5.x, GLM-Flash | Native MTP in the GLMs, FP8 | Cheap coding plan |
| Nscale | Llama, Qwen, DeepSeek (via HF) | European GPU cloud | – |
| OVHcloud AI Endpoints | gpt-oss, Llama, Qwen, Mistral | Sovereign hosting in the EU | Per token |
| fal | Focus on image and video; few LLMs | Own kernels for diffusion | – |
| Cohere | Command A and family (weights under non-commercial license) | Own function calling and RAG | – |
| Public AI | Public or government models (Apertus, SEA-LION, Olmo) | Non-profit | – |
| WaveSpeed | Image and video | Diffusion kernels and step caching | – |
| Fireworks | DeepSeek V4, Kimi, GLM, Qwen, gpt-oss | FireAttention engine, adaptive speculative decoding (https://metavert.io/compare/fireworks-ai-vs-groq), FP8/FP4, multi-tenant LoRA, JSON and grammars | – |
| Together AI | All the major ones | Own kernels (FlashAttention comes from there), ATLAS (a speculator that learns at run time), claims serverless inference up to 2x faster (https://together.ai/blog/fastest-inference-for-the-top-open-source-models) | – |
| Scaleway | gpt-oss, Llama, Qwen, Mistral, Gemma | European cloud | Per token |
| Replicate | Community models, with a focus on image; popular LLMs | Cog containers | Per second of use |
| HF Inference Providers | Routes to the providers above | Unified routing and billing | – |

Techniques common to all: continuous batching, paged KV, prefix cache, FP8 and, increasingly,
NVFP4 on Blackwell, plus speculative decoding. EAGLE-3 became the de facto industry standard
(https://arxiv.org/html/2603.18567), and MTP is used when the model was trained with it (DeepSeek,
GLM, Qwen3.6, MiniMax). Grammar-based structured output and OpenAI-compatible function calling
became mandatory.

## A. The 10 most relevant models for a daily assistant

"Fits" means: fits in 64 GB of RAM + 4 GB of VRAM at Q4, with up to about 12B active, at a
usable speed.

| # | Model | Total / active | Architecture (GGUF) | License | GGUF on HF | Fits? |
|---|---|---|---|---|---|---|
| 1 | Qwen3.6-35B-A3B | 35B / 3B | qwen3moe (or qwen3next, **verify**); has MTP weights | Apache 2.0 | Yes (unsloth, ggml-org: https://huggingface.co/ggml-org/Qwen3.6-35B-A3B-GGUF) | **Yes, the best bet** |
| 2 | Gemma 4 26B-A4B | 25.2B / 3.8B | gemma4 | Gemma/Apache **(verify)** | Yes (https://huggingface.co/unsloth/gemma-4-26B-A4B-it-GGUF); about 18 GB in 4 bits | **Yes** |
| 3 | gpt-oss-20b | 21B / 3.6B | gpt-oss, MXFP4 | Apache 2.0 | Yes | **Yes (needs MXFP4)** |
| 4 | GLM-4.7-Flash / GLM-5.x-Flash | about 30B / 3B | glm4moe (or deepseek2-like) | MIT | Yes | **Yes**; strong in agent use (τ²-Bench 79.5: https://www.datalearner.com/en/ai-models/compare/google-gemma-4-26b-a4b/vs/glm-4-7-flash) |
| 5 | Qwen3.6-27B (dense) | 27B | qwen3 | Apache 2.0 | Yes | Fits, but slow (dense, about 2–4 tok/s) |
| 6 | gpt-oss-120b | 117B / 5.1B | gpt-oss MXFP4 | Apache 2.0 | Yes (about 63 GB) | **At the limit**; experts from disk or mmap |
| 7 | Qwen3-30B-A3B / Qwen3-Coder-30B-A3B | 30B / 3B | qwen3moe | Apache 2.0 | Yes | **Yes (already runs on katana)** |
| 8 | MiniMax M2.x/M3 | about 230B / 10B | minimax-m2 | MIT | Yes | Only with experts from disk and Q2/IQ; slow |
| 9 | GLM-5.x (full) | 355B+ / 32B+ | glm4moe or glm5 | MIT | Yes | No |
| 10 | DeepSeek V4 / Kimi K2.6–K3 | about 1T / 32–50B | deepseek2/kimi (MLA) | MIT or modified MIT | Yes | No (quality reference only) |

Mistral Small 3.x (24B dense, Apache 2.0) remains as a European alternative; with 4 GB of VRAM it is slow.

## B. Techniques applicable to a single-user local engine

| Technique | Expected gain | Effort | Priority |
|---|---|---|---|
| Structured output (GBNF grammar and JSON schema in the sampler) | Reliable function calling in the agent; eliminates broken JSON | Medium | **High** |
| Persistent prefix cache (KV of the system prompt and the tools saved to disk and reused across turns) | First token from several seconds to almost zero in the agent | Medium | **High** |
| Native MTP (Qwen3.6, GLM, DeepSeek) as a draft | 1.5–2x in decode when the model ships the MTP head | Medium to high | **High** |
| MXFP4 (CPU AVX-512 and Vulkan) | Enables gpt-oss-20b/120b in the original format, without reconverting | Medium | **High** |
| Speculative decoding with a small draft model or EAGLE-3 | 1.5–2.5x on a dense model; less on an MoE with 3B active | High | Medium |
| N-gram or repetition (already exists in katana) | Good for code and editing | – | Done |
| Predict and preload experts (fetch the next layer's experts before the router decides) | Hides the disk read | Medium | Medium |
| Quantize the KV to Q8/Q4 | More context in VRAM | Low to medium | Medium |
| Separate prefill and decode between GPU and CPU | The prefill stays on the GPU (already so, in part) | – | Low |
| NVFP4 | Only pays off with Blackwell hardware; on the RTX 3050 it is emulated | Medium | Low (reading the format yes, optimizing no) |
| Continuous batching, paged KV, multi-tenant LoRA | They serve many users; not worth it here | – | Do not do |

## C. Quantization formats in the 2026 GGUFs

- **MXFP4 / MXFP4_MOE**: blocks of 32 FP4 (E2M1) values with an E8M0 scale. It is native to gpt-oss; 
  MXFP4_MOE quantizes only the experts. Decoding is simple, so it is worth supporting.
- **IQ2_XXS…IQ4_XS (i-quants)**: lattice grid with tables; they dominate below 4 bits. IQ4_XS and
  IQ3/IQ2 are what allow running large MoEs in 64 GB. It is worth supporting IQ4_XS first, then
  IQ3_S/IQ2_XXS.
- **UD-* (Unsloth Dynamic)**: not a new type. It is a per-tensor mix of existing types
  (Q4_K, Q6_K, IQ*, Q8_0) chosen per layer. It works automatically if the base types
  are supported.
- **TQ1_0/TQ2_0** (ternary), **Q4_0_x_x** (repacking at load time) and third-party variants
  (TQ3_4S etc.): niche, not worth it.
- **NVFP4**: starting to appear in safetensors checkpoints; in GGUF it is still rare **(verify)**.
  Low priority.

Suggested order: MXFP4, IQ4_XS, IQ3_S/IQ2_XXS and, last, NVFP4 (read only).
