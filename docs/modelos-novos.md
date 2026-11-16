# Modelos novos baixados (arquitetura DeepSeek / Kimi)

Origem dos dados: leitura do cabecalho GGUF por script Python sem bibliotecas (v3 do formato).
Tamanhos conferidos contra a API do Hugging Face (batem byte a byte). Os arquivos ficam em
`modelos/` (fora do git).

## Arquivos

| Arquivo | Tamanho (bytes) | Repositorio |
|---|---|---|
| `modelos/deepseek-v2-lite-chat-q4k.gguf` | 10.364.416.576 (9,65 GiB) | legraphista/DeepSeek-V2-Lite-Chat-IMat-GGUF (`DeepSeek-V2-Lite-Chat.Q4_K.gguf`, com imatrix) |
| `modelos/moonlight-16b-a3b-instruct-q4km.gguf` | 10.540.747.360 (9,82 GiB) | gabriellarson/Moonlight-16B-A3B-Instruct-GGUF (`...-Q4_K_M.gguf`) |

Observacoes:
- Nao existe DeepSeek-V2-Lite-Chat Q4_K_M em bartowski/lmstudio. O `Q4_K` do legraphista e o
  mesmo esquema (mistura Q4_K_M, `file_type = 15`) do modelo Chat verdadeiro, com 1 arquivo.
- DeepSeek-Coder-V2-Lite-Instruct (bartowski, mesma arquitetura) e alternativa, nao baixada.

## Por que Moonlight como segundo modelo

- E o unico modelo pequeno com a arquitetura DeepSeek-V3/Kimi (a Moonshot treinou o Moonlight
  na arquitetura do DeepSeek-V3): roteador sigmoide (`expert_gating_func = 2`) com vies
  (`exp_probs_b.bias`), pesos normalizados e escalados (2,446), 2 especialistas compartilhados,
  1 camada densa, MLA. Cobre exatamente o que o V2-Lite nao tem (softmax sem vies).
- Cabe com folga (9,8 GiB), roda na maquina, permite validar logits/qualidade rapido, e e o
  caminho mais curto para Kimi K2 / DeepSeek-V3 (mesmo codigo, so escala).
- Ressalva: NAO e um MoE "grande". Se for preciso testar escala (leitura de especialistas do
  disco), o candidato seguinte e GLM-4.5-Air (arquitetura `glm4moe`, sem MLA): unsloth/GLM-4.5-Air-GGUF,
  pasta `Q4_K_M/`, 2 partes, 50.000.746.752 + 22.975.001.632 = ~73 GB. Nao baixado (escolha de UM).
  Kimi K2 e DeepSeek-V3 completos nao foram baixados, como pedido.

## DeepSeek-V2-Lite-Chat (`general.architecture = deepseek2`)

- blocos: 27; embedding 2048; vocab 102400; contexto 163840 (yarn fator 40, original 4096,
  `yarn_log_multiplier` 0,0707); rope base 10000; rms eps 1e-6.
- atencao: 16 cabecas, `head_count_kv` 16; `key_length` 192 (= 128 nope + 64 rope),
  `value_length` 128; `kv_lora_rank` 512; **sem `q_lora_rank`** (Q e uma matriz direta
  `attn_q` [2048, 3072]); `rope.dimension_count` 64.
- MoE: 64 especialistas, 6 usados, 2 compartilhados, `expert_feed_forward_length` 1408;
  `leading_dense_block_count` 1 (camada 0 densa, `feed_forward_length` 10944);
  `expert_weights_scale` 1,0; sem `expert_weights_norm` nem `gating_func` (softmax, sem vies).
- tokenizador: gpt2 / pre `deepseek-llm`, bos 100000, eos 100001.
- tensores globais: `token_embd.weight` [2048,102400] Q4_K; `output.weight` Q6_K;
  `output_norm.weight` F32.
- camada 0 (densa): `attn_norm` F32; `attn_q` [2048,3072] Q4_K; `attn_kv_a_mqa` [2048,576] Q4_K
  (576 = 512 latente + 64 rope); `attn_kv_a_norm` [512] F32; `attn_kv_b` [512,4096] Q4_K
  (4096 = 16 x (128 k_nope + 128 v)); `attn_output` [2048,2048] Q4_K; `ffn_norm` F32;
  `ffn_gate`/`ffn_up` [2048,10944] Q4_K; `ffn_down` [10944,2048] Q8_0.
- camada 1 (MoE): mesma atencao; `ffn_gate_inp` [2048,64] F32 (roteador);
  `ffn_gate_exps`/`ffn_up_exps` [2048,1408,64] Q4_K; `ffn_down_exps` [1408,2048,64] Q8_0;
  `ffn_gate_shexp`/`ffn_up_shexp` [2048,2816] Q4_K; `ffn_down_shexp` [2816,2048] Q6_K
  (2816 = 2 x 1408, os 2 compartilhados fundidos).

## Moonlight-16B-A3B-Instruct (`general.architecture = deepseek2`)

- blocos: 27; embedding 2048; vocab 163840; contexto 8192; rope base 50000; rms eps 1e-5;
  `feed_forward_length` 11264; licenca MIT.
- atencao: 16 cabecas, `head_count_kv` 1 (forma "absorvida" do MLA); `key_length` 576,
  `value_length` 512 (= latente + rope / latente); `key_length_mla` 192, `value_length_mla` 128;
  `kv_lora_rank` 512; sem `q_lora_rank`; rope dims 64; sem yarn nos metadados.
- MoE: 64 especialistas, 6 usados, 2 compartilhados, ffn do especialista 1408, 1 camada densa;
  `expert_gating_func = 2` (sigmoide), `expert_weights_norm = true`,
  `expert_weights_scale = 2,446`.
- Diferenca de tensores em relacao ao V2-Lite: em vez de `attn_kv_b` unico, vem separado
  `attn_k_b` [128,512,16] Q5_0 e `attn_v_b` [512,128,16] Q4_K (por cabeca), e a camada MoE
  tem `exp_probs_b.bias` [64] F32 (viés do roteador).
- tensores globais: `token_embd` [2048,163840] Q4_K; `output` Q6_K; `output_norm` F32.
- camada 0 (densa): `attn_norm`, `attn_q` [2048,3072] Q4_K, `attn_kv_a_mqa` [2048,576] Q4_K,
  `attn_kv_a_norm` [512], `attn_k_b`, `attn_v_b`, `attn_output` [2048,2048] Q4_K, `ffn_norm`,
  `ffn_gate`/`ffn_up` [2048,11264] Q4_K, `ffn_down` [11264,2048] Q6_K.
- camada 1 (MoE): atencao igual; `ffn_gate_inp` [2048,64] F32; `exp_probs_b.bias` [64] F32;
  `ffn_gate_exps`/`ffn_up_exps` [2048,1408,64] Q4_K; `ffn_down_exps` [1408,2048,64] Q8_0;
  `ffn_*_shexp` (2816): gate/up Q4_K, down Q6_K.

## Pontos para o motor

- Chaves novas a ler: `kv_lora_rank`, `key_length_mla`/`value_length_mla`, `expert_shared_count`,
  `leading_dense_block_count`, `expert_gating_func`, `expert_weights_norm`, `expert_weights_scale`,
  `rope.scaling.*` (yarn, so no V2-Lite).
- Os dois modelos tem nomes de tensor diferentes para o MLA (`attn_kv_b` vs `attn_k_b`+`attn_v_b`).
- Tipos de quantizacao presentes: Q4_K, Q6_K, Q8_0, Q5_0, F32.

## Estado no katana (suporte `deepseek2`)

O que funciona (CPU, caminho de referencia):
- Leitura dos metadados `deepseek2` (kv_lora_rank, key/value_length e *_mla, rope.dimension_count,
  leading_dense_block_count, expert_shared_count, expert_gating_func, expert_weights_norm/scale,
  rope.scaling yarn) e dos tensores das duas formas de MLA (`attn_kv_b` unico ou `attn_k_b` +
  `attn_v_b`). Tipo Q5_0 decodificado (so usado no `attn_k_b` do Moonlight).
- Atencao MLA na forma absorvida para os dois modelos: o cache guarda por posicao so o latente
  normalizado (512) e a parte rope de k (64) = 576 floats por camada (contra 16 x 320 = 5120 da
  forma descomprimida). q_nope e multiplicada por k_b (vira vetor no latente), a pontuacao e
  q_lat . c_kv + q_rope . k_rope, a media fica no latente e so no fim passa por v_b. k_b/v_b ficam
  em float na RAM (~8 MB por camada). RoPE em pares adjacentes (convencao "normal").
- Yarn do V2-Lite: frequencias com rampa entre interpolacao (fator 40) e extrapolacao (beta 32/1,
  contexto original 4096) e escala q.k multiplicada por mscale^2 (mscale = 1 + 0,0707 ln 40).
- MoE: camada 0 densa; roteador softmax (V2) ou sigmoide com vies `exp_probs_b` so na escolha,
  pesos normalizados e escalados por 2,446 (Moonlight); compartilhados (`*_shexp`) sempre somados.
- Tokenizador BPE carrega os dois vocabularios (102.400 e 163.840). Pre-tokenizador: digitos
  agrupados (`deepseek-llm`: sem limite; `kimi-k2`: ate 3); o resto usa a aproximacao do GPT-2 do
  katana (as regex reais diferem em detalhes: CJK, quebras de linha). Formatos de conversa novos:
  5 = DeepSeek (`User: ...\n\nAssistant:`), 6 = Kimi (`<|im_user|>user<|im_middle|>...`).
- GPU (OpenCL e Vulkan): por enquanto "camadas na CPU, produtos densos na GPU": q, kv_a, saida da
  atencao, feed-forward denso, compartilhados e vocabulario sobem; atencao MLA e especialistas
  roteados ficam na CPU. `selftest` confere esses produtos e o fim a fim contra a CPU.

Testes (KATANA_CPU=1, KATANA_CTX=512, Linux, greedy):
- V2-Lite `run "The capital of France is" 20`: " Paris.\n\nThe official language is French.\n\nThe
  currency is the Euro (€)." (~2 tok/s com paginas frias; 10 tok/s em 116 tokens com o arquivo em cache).
- V2-Lite `chat "Quanto e 17 vezes 23?"`: " 17 x 23 = 391".
- Moonlight `run`: " Paris, which is located in the northern part of the country. It is one of the most visited" (4,9 tok/s).
- Moonlight `chat`: "O resultado de 17 vezes 23 é 391." (7,3 tok/s).

O que falta:
- Grafo completo da MLA na GPU (kernel de atencao no latente, cache de 576 por posicao,
  absorcao de k_b/v_b na GPU) e especialistas no cache de VRAM (reaproveitar `gx`/`vk_moe`, que
  hoje assumem todas as camadas MoE e sem compartilhados).
- Caminho em lote na CPU para o prompt (hoje token a token, como os outros MoE).
- Kimi K2 / DeepSeek-V3 completos: `q_lora_rank` (attn_q_a + attn_q_a_norm + attn_q_b), roteamento
  em grupos (n_group/topk_group; Moonlight usa 1 grupo), 256/384 especialistas, 61 camadas com 3
  densas, e leitura de especialistas do disco (nao cabem na RAM desta maquina).

GPU (RTX 3050 4 GB, OpenCL, KATANA_CTX=512): `selftest` OK nos dois (Moonlight: maior diferenca de
logit 0,00026; V2-Lite: 0,035, abaixo do limite 0,05, com argmax igual); Vulkan (`KATANA_BACKEND=vulkan`)
tambem OK no Moonlight. Chat "17 vezes 23" igual ao da CPU, ~7 tok/s nos dois, isto e, sem ganho
sobre a CPU (6,7 tok/s; medida isolada, nao alternada): a ida e volta por produto come o ganho.
`q_lora_rank` (attn_q_a/attn_q_a_norm/attn_q_b) ja e lido e usado na CPU, mas sem modelo para testar.
Regressao: `selftest` OK em qwen2.5-1.5b e olmoe (KATANA_KV16=0).

## Qwen3.6-35B-A3B (`general.architecture = qwen35moe`)

Arquivo: `modelos/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf` (22.134.528.992 bytes, Unsloth, imatrix).
40 camadas, dim 2048, vocab 248320, 16 cabecas (kv 2) de 256, MoE 256 especialistas / 8 ativos
(ffn 512) + 1 compartilhado (ffn 512) com portao sigmoide escalar (`ffn_gate_inp_shexp`).
Camadas com (l+1) % 4 == 0 (3, 7, ..., 39) sao atencao completa com portao; as outras 30 sao Gated
DeltaNet. Tipos: Q8_0 (projecoes da atencao/DeltaNet, compartilhado, embedding), Q4_K (gate/up dos
especialistas), Q5_K/Q6_K (down), Q6_K (output), F32 (normas, roteador, ssm_alpha/beta, conv1d).
Tokenizador gpt2, pre `qwen35` (carregou com o BPE existente; pre-tokenizacao igual a do qwen2, sem
regra propria registrada - texto comum tokeniza e gera certo).

### Convencoes confirmadas rodando o modelo real (referencia da CPU)

1. Mapa grupo q/k -> cabeca de v: **h % 16** (intercalado). Com h / 2 a saida vira lixo
   ("The capital of France is诚聘英才IES..."); com h % 16 sai "Paris, a city renowned for...".
   (`KATANA_DN_TILE=0` liga o outro mapa, so para teste.)
2. Pesos das normas RMS (attn_norm, post_attention_norm, ssm_norm, attn_q_norm, attn_k_norm,
   output_norm): **w direto** (o arquivo ja traz o 1+w onde o modelo original usa). ssm_norm e
   norma RMS por cabeca de v (128) seguida de multiplicacao por SiLU(z).
3. `ssm_a` ja vem pronto como **-exp(A_log)** (negativo): g = ssm_a * softplus(alpha + dt_bias),
   decaimento = exp(g).
4. `attn_qkv` [2048 -> 8192]: blocos contiguos **q (16x128) | k (16x128) | v (32x128)**; a
   convolucao [4][8192] (dims [4, 8192], 4 taps contiguos por canal, tap 3 = token atual) roda sobre
   os 8192 canais antes da divisao; depois SiLU, L2 (eps 1e-6) por grupo em q e k, q * 1/sqrt(128).
   `attn_q` da atencao completa [2048 -> 8192]: **intercalado por cabeca**, [q 256 | portao 256] x 16;
   saida de cada cabeca * sigmoide(portao) antes de attn_output.
5. RoPE parcial neox: giram as **64 primeiras dims de cada cabeca, pares (i, i+32)**, frequencia
   1e7^(-2i/64); as dims 64..255 nao giram. mRoPE [11,11,10] com texto puro = RoPE comum.
Roteador: softmax sobre os 256, top-8, pesos renormalizados. Formato: ChatML, assistente comeca
com `<think>\n`; ferramentas em XML (`<function=..><parameter=..>`).

### Estado no katana

- CPU exata: etapas por camada em funcoes com buffers (`dn_proj`, `dn_conv`, `dn_recur`,
  `dn_gate`, `qn_core`, `ffn_moe`, `qn_layer`). Estado do DeltaNet contiguo em `m->ssm`: por camada
  linear, S [32][128][128] e (4-1) x 8192 da convolucao (`dn_S`, `dn_CV`); zera em pos 0. O
  servidor recomeca do zero se o prefixo reaproveitado nao for o cache inteiro.
- GPU (OpenCL, modo `gpu.mv_host`): camadas na CPU, 251 produtos densos na GPU (qkv, z, ssm_out,
  q/k/v/o, compartilhado, vocabulario; 1,9 GB) e os especialistas pelo mesmo cache de vagas do
  qwen3moe (`gx_setup`, extraido do `gpu_setup`; calor, thread carregadora, `heat_*`), via
  `gpu_moe_host` (sobe xb, `gpu_moe_ffn`, le a soma). Na RTX 3050 4 GB: 19 de 256 vagas por camada.
- Prompt em lote na CPU (`qn_layers_n`, ate 16 tokens): projecoes, saidas, compartilhado e pares
  (token, especialista) em lote; conv/recorrencia/atencao token a token (`dn_recur` e o laco a
  trocar pela forma em blocos). Padrao: lote so sem GPU (`KATANA_QN_BATCH=1/0` forca).
- Medidas (indicativas, maquina compartilhada, alternadas na mesma hora, 46 tokens de prompt,
  60 gerados): GPU+cache de especialistas: prompt token a token 15,2 tok/s, geracao 14,6-18,8 tok/s;
  prompt em lote na CPU 15,4-17,6 tok/s. So CPU (`KATANA_CPU=1`): ~8-10 tok/s de geracao, prompt
  10-14 tok/s. Especialistas: ~46% dos usos na VRAM.
- Falta (OpenCL): grafo completo na GPU; `katana pack` nao testado para este modelo; cache KV da
  CPU aloca as 40 camadas (so 10 usam).

### Grafo completo em Vulkan (2026-11-16)

Com `KATANA_BACKEND=vulkan` o qwen35moe roda inteiro na GPU (`vkq_setup`, `vk_qn_forward_1`,
`vk_qn_forward_n`); o OpenCL continua no modo hibrido. `KATANA_VK_QN=0` volta ao hibrido no Vulkan.
- Pesos densos sobem pelo mesmo `mla_gpu_upload` (attn_q fica com q e portao intercalados: os
  kernels leem q com passo 2*hd por cabeca). alpha/beta viram uma matriz float [a | b] por camada.
- Kernels novos em `vk/shaders/` (vindos de `tools/vkdelta/`): `dn_step` (posicao do meta; posicao
  0 trata S e anel como zero, nao precisa zerar), `dn_prep`, `dn_cstate`, `dn_chunk`, `dn_state`
  (com `pos0`; S como operando em duas partes fp16, alta + resto: o erro do estado no lote caiu ~10x
  nas camadas do meio), `dn_norm`; `qn_gate` (portao da atencao no lote), `moe_gath`/`moe_scat`
  (MoE em lote). `att1f` e `qkv_post` ganharam `nrot` (RoPE parcial neox: pares (i, i+nrot/2), dims
  >= nrot nao giram) e `qst` (passo de q por cabeca); `att1f` tambem `gate` (saida * sigmoide do
  portao, aplicado por pedaco, pois e linear). `mm.comp` ganhou `woff` (deslocamento dos pesos:
  especialista numa vaga). Mapa grupo -> cabeca `kmap` = `KATANA_DN_TILE` (padrao h % 16).
- Um token: atencao/DeltaNet na GPU; MoE como o `vk_forward_moe` (roteador na CPU, quentes nas
  vagas, frios na CPU), agora com o compartilhado na GPU (linha K de `vkm.bt`, peso = portao
  sigmoide). Uma leitura de 8 KB por camada.
- Lote (ate 256 por passada): produtos em `mm.comp`, DeltaNet em blocos de 64, atencao
  `qkv_post` + `att_flash` (hd 256, grupo 8) + `qn_gate`. MoE: o residual desce (norma e roteamento
  exatos na CPU); compartilhado e cada vaga quente rodam na GPU sobre as colunas dos seus tokens;
  frios na CPU (blocos de 16 tokens) e sobem somados. `KATANA_QN_PROF=1` mostra o perfil;
  `KATANA_QN_DNSTEP=1` faz a recorrencia token a token (fp32) dentro do lote (conferencia).
- VRAM (4 GB, ctx 4096): densos 1,9 GB + estado/cache/lote ~0,2 GB; o resto vira 18-20 vagas por
  camada.
- Correcao: `qn_batch` nao pode mandar o prompt para a CPU com o grafo Vulkan ligado (o estado vive
  na VRAM; o `vkcheck` token a token caia nesse caminho).
- Validacao: `selftest` (ctx 512): token a token x CPU exata 0,0003-0,0004; lote 0,06-0,23 conforme a
  rodada (maior |logit| 9,8; fp16 nos produtos de 40 camadas e roteamento top-8 sensivel a isso; com
  todos os especialistas do lote na CPU fica 0,15, entao nao e o caminho das vagas). O teste tolera 3%
  do maior logit no lote deste modelo (o limite fixo de 0,05 valia para modelos densos). Medido camada a
  camada (lote x token a token, ultimo token): erro relativo ~4e-4 na camada 0 (arredondamento fp16
  da ativacao) crescendo devagar, sem salto em camada nenhuma. `vkcheck` 200 tokens: 20 proximos
  gulosos iguais (diferenca 0,15); 600 tokens: diferenca 0,42-0,46 (maior |logit| 15,6), divergencia
  no 8o token gerado; com `KATANA_QN_DNSTEP=1` a diferenca fica 0,42 (o DeltaNet em blocos nao e a
  fonte principal; o resto e fp16 dos produtos e a mistura GPU/CPU dos especialistas).
  `chat` "17 vezes 23" -> 391.
- Medida indicativa (bench 256+64, alternado, rodada sem carga externa): Vulkan prompt 82,7 tok/s,
  geracao 25,2-25,5; OpenCL hibrido prompt 27,3-27,5, geracao 18,0-18,7. No lote, 2,7 de 3,1 s
  por passada sao os especialistas frios na CPU (~65% dos pares com prompt aleatorio).
- Proximos: frios do lote na GPU (subir os especialistas usados por varios tokens do lote numa
  area temporaria), agrupar pares por especialista na CPU, roteador na GPU para gravar o grafo de
  um token.
- Agente validado: pergunta sobre o kernel Linux -> `web_search` e `web_fetch` chamados em XML,
  convertidos para o JSON de argumentos, resposta "7.3.4" citando kernel.org. Velocidade no agente
  8-11 tok/s, caindo a ~2 tok/s numa rodada (11535 trocas de especialistas: o calor troca demais
  com 19 vagas; investigar `KATANA_SWAP`/margem para este modelo).
