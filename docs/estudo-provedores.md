# Estudo: provedores de inferência e o que serve ao katana (nov/2026)

Pesquisa feita em 16/11/2026. A pesquisa foi rápida, e a maior parte das fontes encontradas são
blogs de fornecedores ou textos de SEO, que discordam entre si sobre versões e números. O que está
marcado como **(verificar)** não foi confirmado numa fonte primária. Tamanhos e licenças vêm dos
cartões de modelo, quando foram encontrados; os demais vêm do conhecimento prévio até meados de 2026.

## 1. Panorama dos modelos abertos

- Os laboratórios chineses lideram os pesos abertos: DeepSeek V4 (Pro/Flash, abr/2026), Kimi K2.6 e
  K3 (jul/2026), GLM-5.x (5.1, 5.2 em jun/2026, 5.3 e 5.3-Flash), Qwen3.6/3.7/3.8 e MiniMax M3.
  Fontes: https://deepinfra.com/blog/kimi-k3-vs-deepseek-v4-pro-vs-glm-5-2 ,
  https://ai2.work/blog/chinese-labs-own-the-open-weight-leaderboard-glm-deepseek-qwen ,
  https://agentic-up.pages.dev/posts/open-source-ai-model-landscape-june-2026/ ,
  https://www.contextstudios.ai/blog/deepseek-v4-the-open-source-pricing-earthquake
- Índice da Artificial Analysis no fim de ago/2026, segundo a AI2Work: Kimi K3 (60), GLM-5.3 (60),
  Qwen3.8 2.4T (58) e GLM-5.3-Flash (57) **(verificar)**.
- O gpt-oss (20b/120b, Apache 2.0, MXFP4 nativo) continua sendo a opção americana barata e fácil de
  hospedar. A Meta passou a priorizar modelos fechados, e o Llama perdeu relevância.
- Licenças: Apache 2.0 virou o padrão (Qwen, gpt-oss, Gemma 4). DeepSeek, Kimi e GLM usam MIT ou MIT
  modificado.

## 2. Provedores

A tabela resume o que se sabe de cada um. Os catálogos mudam toda semana; para conferir, use a página
de modelos de cada provedor e o seletor de provedores do HF (https://huggingface.co/docs/inference-providers).

| Provedor | Destaques do catálogo | Técnicas divulgadas | Velocidade/preço (contexto) |
|---|---|---|---|
| DeepInfra | DeepSeek V4, Kimi K3, GLM-5.x, Qwen3.x, gpt-oss | GPUs H100/B200, FP8/FP4, batching contínuo, cache de prefixo | Preços entre os mais baixos; fez rodada de 107 mi de dólares (https://deepinfra.com/blog/best-open-source-llm-api-providers) |
| Novita | DeepSeek V4 Pro/Flash, Kimi K2.6, Qwen3.6, GLM-5.1, MiniMax M3 (https://blogs.novita.ai/open-source-llm-guide-2026/) | API compatível com OpenAI, quantização FP8 | DeepSeek V4 Pro citado a cerca de US$ 0,44 / 0,87 por milhão de tokens |
| Groq | gpt-oss, Llama, Qwen, Kimi | Chip próprio (LPU) com SRAM no chip, execução determinística compilada; agora sob a NVIDIA (Groq 3 LPX gera tokens candidatos e GPUs Rubin verificam: https://futurumgroup.com/insights/nvidia-groq-3-lpxs-promise-of-worlds-fastest-inference-enters-full-production/) | Cerca de 476 tok/s no gpt-oss-120B (https://costbench.com/best/fastest-llm-inference/) |
| Cerebras | gpt-oss-120B, Qwen3, GLM | Chip do tamanho do wafer, pesos em SRAM, especulativa | Cerca de 3000 tok/s no gpt-oss-120B (mesma fonte) |
| Baseten | Sob medida (DeepSeek, Kimi, Qwen) | TensorRT-LLM, EAGLE/MTP, prefill e decode separados, FP8/FP4 | Dedicado; por GPU |
| Featherless AI | Milhares de ajustes finos (Llama/Qwen/Mistral) | Troca rápida de modelos sem servidor, LoRA e multi-modelo | Assinatura mensal fixa |
| Z.ai | GLM-4.5/4.6/4.7, GLM-5.x, GLM-Flash | MTP nativo nos GLM, FP8 | Plano de código barato |
| Nscale | Llama, Qwen, DeepSeek (via HF) | Nuvem europeia de GPUs | – |
| OVHcloud AI Endpoints | gpt-oss, Llama, Qwen, Mistral | Hospedagem soberana na UE | Por token |
| fal | Foco em imagem e vídeo; poucos LLMs | Kernels próprios para difusão | – |
| Cohere | Command A e família (pesos sob licença não comercial) | Function calling e RAG próprios | – |
| Public AI | Modelos públicos ou de governos (Apertus, SEA-LION, Olmo) | Sem fins lucrativos | – |
| WaveSpeed | Imagem e vídeo | Kernels de difusão e cache de passos | – |
| Fireworks | DeepSeek V4, Kimi, GLM, Qwen, gpt-oss | Motor FireAttention, especulativa adaptativa (https://metavert.io/compare/fireworks-ai-vs-groq), FP8/FP4, LoRA multi-tenant, JSON e gramáticas | – |
| Together AI | Todos os grandes | Kernels próprios (FlashAttention é de lá), ATLAS (especulador que aprende em tempo de execução), afirma inferência sem servidor até 2x mais rápida (https://together.ai/blog/fastest-inference-for-the-top-open-source-models) | – |
| Scaleway | gpt-oss, Llama, Qwen, Mistral, Gemma | Nuvem europeia | Por token |
| Replicate | Modelos da comunidade, com foco em imagem; LLMs populares | Contêineres Cog | Por segundo de uso |
| HF Inference Providers | Roteia para os provedores acima | Roteamento e cobrança unificados | – |

Técnicas comuns a todos: batching contínuo, KV paginado, cache de prefixo, FP8 e, cada vez mais,
NVFP4 no Blackwell, além de especulativa. O EAGLE-3 virou o padrão de fato na indústria
(https://arxiv.org/html/2603.18567), e o MTP se usa quando o modelo foi treinado com ele (DeepSeek,
GLM, Qwen3.6, MiniMax). Saída estruturada por gramática e function calling compatível com OpenAI
viraram obrigatórios.

## A. Os 10 modelos mais relevantes para um assistente diário

"Cabe" quer dizer: cabe em 64 GB de RAM + 4 GB de VRAM em Q4, com até cerca de 12B ativos, numa
velocidade utilizável.

| # | Modelo | Total / ativos | Arquitetura (GGUF) | Licença | GGUF no HF | Cabe? |
|---|---|---|---|---|---|---|
| 1 | Qwen3.6-35B-A3B | 35B / 3B | qwen3moe (ou qwen3next, **verificar**); tem pesos MTP | Apache 2.0 | Sim (unsloth, ggml-org: https://huggingface.co/ggml-org/Qwen3.6-35B-A3B-GGUF) | **Sim, a melhor aposta** |
| 2 | Gemma 4 26B-A4B | 25,2B / 3,8B | gemma4 | Gemma/Apache **(verificar)** | Sim (https://huggingface.co/unsloth/gemma-4-26B-A4B-it-GGUF); cerca de 18 GB em 4 bits | **Sim** |
| 3 | gpt-oss-20b | 21B / 3,6B | gpt-oss, MXFP4 | Apache 2.0 | Sim | **Sim (precisa de MXFP4)** |
| 4 | GLM-4.7-Flash / GLM-5.x-Flash | cerca de 30B / 3B | glm4moe (ou deepseek2-like) | MIT | Sim | **Sim**; forte em agente (τ²-Bench 79,5: https://www.datalearner.com/en/ai-models/compare/google-gemma-4-26b-a4b/vs/glm-4-7-flash) |
| 5 | Qwen3.6-27B (denso) | 27B | qwen3 | Apache 2.0 | Sim | Cabe, mas é lento (denso, cerca de 2–4 tok/s) |
| 6 | gpt-oss-120b | 117B / 5,1B | gpt-oss MXFP4 | Apache 2.0 | Sim (cerca de 63 GB) | **No limite**; especialistas do disco ou mmap |
| 7 | Qwen3-30B-A3B / Qwen3-Coder-30B-A3B | 30B / 3B | qwen3moe | Apache 2.0 | Sim | **Sim (já roda no katana)** |
| 8 | MiniMax M2.x/M3 | cerca de 230B / 10B | minimax-m2 | MIT | Sim | Só com especialistas do disco e Q2/IQ; lento |
| 9 | GLM-5.x (completo) | 355B+ / 32B+ | glm4moe ou glm5 | MIT | Sim | Não |
| 10 | DeepSeek V4 / Kimi K2.6–K3 | cerca de 1T / 32–50B | deepseek2/kimi (MLA) | MIT ou MIT modificado | Sim | Não (referência de qualidade apenas) |

Mistral Small 3.x (24B denso, Apache 2.0) fica como alternativa europeia; com 4 GB de VRAM é lento.

## B. Técnicas aplicáveis a um motor local de um usuário só

| Técnica | Ganho esperado | Esforço | Prioridade |
|---|---|---|---|
| Saída estruturada (gramática GBNF e esquema JSON no amostrador) | Function calling confiável no agente; elimina JSON quebrado | Médio | **Alta** |
| Cache de prefixo persistente (KV do prompt de sistema e das ferramentas salvo em disco e reaproveitado entre turnos) | Primeiro token de vários segundos para quase zero no agente | Médio | **Alta** |
| MTP nativo (Qwen3.6, GLM, DeepSeek) como rascunho | 1,5–2x no decode quando o modelo traz a cabeça MTP | Médio a alto | **Alta** |
| MXFP4 (CPU AVX-512 e Vulkan) | Habilita o gpt-oss-20b/120b no formato original, sem reconverter | Médio | **Alta** |
| Especulativa por modelo-rascunho pequeno ou EAGLE-3 | 1,5–2,5x em modelo denso; menos em MoE de 3B ativos | Alto | Média |
| N-gram ou repetição (já existe no katana) | Bom para código e edição | – | Feito |
| Prever e pré-carregar especialistas (pegar os especialistas da próxima camada antes do roteador decidir) | Esconde a leitura do disco | Médio | Média |
| Quantizar o KV em Q8/Q4 | Mais contexto na VRAM | Baixo a médio | Média |
| Prefill e decode separados entre GPU e CPU | O prefill fica na GPU (já é assim, em parte) | – | Baixa |
| NVFP4 | Só ganha com hardware Blackwell; na RTX 3050 é emulado | Médio | Baixa (ler o formato sim, otimizar não) |
| Batching contínuo, KV paginado, LoRA multi-tenant | Servem muitos usuários; aqui não compensam | – | Não fazer |

## C. Formatos de quantização nos GGUF de 2026

- **MXFP4 / MXFP4_MOE**: blocos de 32 valores FP4 (E2M1) com escala E8M0. É nativo do gpt-oss; o
  MXFP4_MOE quantiza só os especialistas. A decodificação é simples, então vale suportar.
- **IQ2_XXS…IQ4_XS (i-quants)**: grade lattice com tabelas; dominam abaixo de 4 bits. O IQ4_XS e o
  IQ3/IQ2 são o que permite rodar MoE grandes em 64 GB. Vale suportar o IQ4_XS primeiro, depois o
  IQ3_S/IQ2_XXS.
- **UD-* (Unsloth Dynamic)**: não é um tipo novo. É uma mistura por tensor de tipos existentes
  (Q4_K, Q6_K, IQ*, Q8_0) escolhida por camada. Funciona automaticamente se os tipos de base
  forem suportados.
- **TQ1_0/TQ2_0** (ternários), **Q4_0_x_x** (repacote em tempo de carga) e variantes de terceiros
  (TQ3_4S etc.): nicho, não vale.
- **NVFP4**: começa a aparecer em checkpoints safetensors; em GGUF ainda é raro **(verificar)**.
  Prioridade baixa.

Ordem sugerida: MXFP4, IQ4_XS, IQ3_S/IQ2_XXS e, por último, NVFP4 (só leitura).
