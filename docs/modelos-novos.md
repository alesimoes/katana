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
