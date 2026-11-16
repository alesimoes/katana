# katana: handoff

Escrito em 2026-11-15, ao fim da sessão que criou o motor, o suporte a mistura de especialistas e o
backend Vulkan. Para retomar: leia este arquivo, depois `conhecimento.md` (tudo o que foi medido),
`comparacoes.md` (todas as tabelas de comparação) e `plano.md` (prioridades). Último commit desta sessão: `54ac6b0` (mais o commit deste arquivo).

## 1. Estado em uma página

**O que roda**

- Modelos densos (`llama`, `qwen2`) e de mistura de especialistas (`olmoe`, `qwen3moe`) a partir de GGUF.
- Dois backends de GPU, escolhidos por `KATANA_BACKEND`:
  - `opencl` (padrão): tudo, inclusive MoE com cache de especialistas e modelo maior que a VRAM.
  - `vulkan`: só modelos densos que cabem inteiros na VRAM; geração com grafo gravado e prompt em
    lote com núcleos tensoriais. MoE pedido em Vulkan cai para o OpenCL; modelo que não cabe cai
    para a CPU.
- `chat` de uma pergunta ou interativo, `agent` com ferramentas (gravar e executar pedem confirmação).

**Números de referência** (RTX 3050 Laptop 4 GB; benchmark 64+64; medidos alternando os motores)

| Modelo | | katana OpenCL | katana Vulkan | referência Vulkan | referência CUDA |
|---|---|---|---|---|---|
| SmolLM2 135M | prompt | 1.227 | 5.972–8.478 | 7.995–8.408 | 6.708 |
| | geração | 119–139 | 385–409 | 198–223 | 402 |
| Qwen2.5 1,5B | prompt | 212 | 1.349–1.364 | 2.362–2.374 | 2.776 |
| | geração | 98 | 98–100 | 113–115 | 117 |
| Llama 3.2 3B | prompt | 108 | 753–761 | 1.357–1.364 | 1.685 |
| | geração | 57 | 56 | 63–64 | 69 |

Os valores de CUDA são de uma medida anterior no mesmo dia, não alternada com as demais.

Qwen3-30B-A3B (MoE, 18,6 GB), texto real, katana OpenCL contra a referência com cache de
especialistas: geração 12,5–14,9 contra 13,2–14,4 tok/s (empate); prompt curto a frio 9–11 contra ~5,5.

**Leitura**

- Prompt de modelo denso: saímos de 11–16x atrás para ~1,7–1,8x atrás (e empate no modelo de 135 M).
- Geração de modelo denso de 1 B ou mais: ~13–15% atrás do Vulkan da referência, ~20% atrás do CUDA.
- Geração do modelo de 135 M: ~2x à frente do Vulkan da referência, empate com o CUDA.
- MoE grande: empate na geração, à frente no prompt curto a frio.

## 2. Próximos passos, em ordem

### 2.1 Fechar o que falta do Vulkan em modelos densos

| Passo | O quê | Pronto quando |
|---|---|---|
| a | Testar pelo Vulkan o que nunca rodou: sessão interativa, `agent`, `run`, um modelo denso com norma q/k (Qwen3 denso), Mistral 7B (não cabe: hoje cai para a CPU) | cada um roda ou tem a falha registrada |
| b | Modelo denso maior que a VRAM em Vulkan: primeiras camadas na placa, resto na CPU, como no OpenCL | Qwen2.5 7B e 14B rodam pelo Vulkan e conferem no `selftest` |
| c | Produto em lote mais rápido (hoje ~2,7 T mult/s; a referência é estimada em ~3,5 T). Ideias não testadas: carga do ladrilho em dois buffers alternados para sobrepor decodificação e multiplicação, menos barreiras por passo. No lote o produto é ~85% do tempo (gate/up 71 ms e down 34 ms de 149 ms num lote de 256 do Qwen 1,5B) | prompt do Qwen 1,5B acima de 1.800 tok/s |
| d | Atenção em lote mais barata (19 ms por lote de 256; 14 ms são reduções por subgrupo) | abaixo de 8 ms |
| e | Geração depois de prompt longo: no Vulkan fica ~5% abaixo do OpenCL (81 contra 86 tok/s com 512 posições); a atenção de um token precisa dividir o contexto entre grupos | igual ou melhor que o OpenCL |
| f | Investigar a diferença de logit do lote no Llama 3B (0,0255, acima do limite de 0,02 que definimos; argmax confere e 600 tokens batem) | causa conhecida; aceitar ou corrigir |
| g | Decidir se o Vulkan vira o backend padrão quando disponível | depois de (a) e (b) |

### 2.2 Mistura de especialistas em Vulkan (etapa 6 do plano)

- Cache de especialistas com vagas por camada, especialistas quentes num lançamento, frios na CPU,
  thread carregadora, pré-carga pelo calor: tudo existe no OpenCL (`gpu_moe_ffn`, `expert_load`,
  `loader_*`, `heat_*`) e precisa do equivalente em Vulkan.
- O modo de várias matrizes num lançamento (`MOEPRE` nos kernels OpenCL) não foi portado para os
  shaders `mv_*`; `moe_sum` já está portado e conferido (`tools/vkatt`).
- No lote, usar o produto com núcleos tensoriais por especialista (tokens agrupados por especialista).
- O runtime faz uma alocação por buffer; com milhares de especialistas usar poucos buffers grandes
  (vagas dentro de um buffer por camada, como já é no OpenCL).
- Pronto quando: `selftest` do OLMoE e do Qwen3-30B passa pelo Vulkan e a geração do Qwen3-30B é
  igual ou melhor que no OpenCL (12,5–14,9 tok/s), medida com `compare.sh`.

**Primeira etapa feita (geração token a token, branch do worktree)**: `vk_forward_moe` roda a atenção de cada
camada no grafo Vulkan (`vk_emit_attn`, ao vivo), devolve a entrada normalizada do feed-forward, a CPU roteia
(`moe_route`) e aplica o mesmo calor/cache/carregador do OpenCL (`moe_tables`, `loader_*`, `exp_write` com ramo
Vulkan). Quentes: gate/up e down por especialista, um despacho cada, vaga por deslocamento em palavras (`woff`/`ooff`
novos nos `mv_*`), dois buffers por camada (`vk_sg`, `vk_sd`); frios na CPU, resultado sobe para `vkm.bt`;
`moe_sum` soma no resíduo. Prompt cai token a token. `selftest` do OLMoE e do Qwen3-30B passa pelo Vulkan e pelo
OpenCL. Falta: medir com `compare.sh` (indicativo curto: Qwen3-30B 12,6 tok/s Vulkan contra 21 OpenCL, frio),
um lançamento por camada (várias matrizes por despacho, `MOEPRE`), grafo gravado por camada, leitura da entrada
sem parar a fila, prompt em lote MoE com núcleos tensoriais, e MoE que não cabe o grafo todo (hoje cai para "grafo na CPU").

**Segunda etapa feita (2026-11-16, seção 7)**: várias matrizes num lançamento nos `mv_*`, roteador na GPU, soma do
MoE fundida com a norma seguinte, cache de especialistas Vulkan no modo híbrido (MLA). Vulkan é o padrão para
qwen3moe e deepseek2; olmoe continua no OpenCL por padrão (2–4% mais lento em Vulkan). Falta: olmoe acima do OpenCL.

**Terceira etapa feita (seção 8)**: prompt MoE em lote no Vulkan (qwen3moe, olmoe): Qwen3-30B 48 → 141–149 tok/s
(OpenCL 58), OLMoE 140 → 362–375 (OpenCL 209–212). Falta o lote no modo híbrido (MLA: Moonlight, DeepSeek).

### 2.3 Arquitetura DeepSeek/Kimi (prioridade 2 do plano)

**Cache de especialistas generalizado (branch do worktree)**: camadas densas iniciais convivem com as MoE (sem vagas
nelas, feed-forward normal no grafo OpenCL), especialista compartilhado inteiro na VRAM fora das vagas (portao sigmoide
escalar opcional `ffn_gate_inp_shexp` na CPU; no grafo completo ainda nao), roteador unico `moe_route` (softmax ou
sigmoide com vies so na escolha, normalizacao, escala; sem limite fixo de especialistas, ate 64 ativos). No deepseek2
(modo `mv_host`) `host_moe_ffn` usa as vagas/calor/carregador do OpenCL: entrada sobe, compartilhado e quentes na GPU,
frios na CPU, uma leitura por camada. Q5_0 passou a subir para a GPU (layout de 8 bits; antes virava lixo: refazer
`katana pack` de shards antigos com Q5_0). Moonlight: geracao 11 -> 24-33 tok/s (medida alternada curta). Falta: Vulkan
(`vk_moe_init` recusa densas/compartilhado/sigmoide), portao do compartilhado no grafo completo, atencao MLA na GPU.

O alvo "trilhão de parâmetros" é dessa família. O Kimi não cabe no disco (≈600 GB em 4 bits; há
~45 GB livres). Validar no DeepSeek-V2-Lite (16 B, ~10 GB): atenção de posto baixo com K e V
comprimidos, roteador sigmoide com viés, especialista compartilhado, camadas densas iniciais.
Detalhes em `roteiro-otimizacoes.md` seção 8 e `plano.md`.

### 2.4 Depois

- CPU dos especialistas frios: ativações de 8 bits por bloco de 256, AVX-512 VNNI (a máquina tem).
- Assistente: pré-tokenizador exato, contexto maior que 4.096 com reuso de prefixo, amostragem
  completa, compactação de contexto, saída JSON garantida para o agente.
- Rede entre máquinas.
- Linux: fora do plano por decisão do dono do projeto ("não vamos testar Linux ainda").

## 3. Pendências e riscos conhecidos

- **Não testado em Vulkan**: sessão interativa, agente, `run`; placa Intel no motor (o runtime
  passou nos testes dela, o motor não foi rodado); caminho sem matrizes cooperativas (só revisado);
  queda para OpenCL quando o Vulkan falha ao iniciar; modelos com norma q/k no lote.
- **Precisão do lote**: os núcleos tensoriais trabalham em meia precisão; a diferença de logit no
  lote é 0,008–0,026 contra ~3e-5 no caminho por token. O argmax bate em todos os testes e 600
  tokens de prompt dão os mesmos 40 tokens seguintes, mas não há medida de qualidade em texto longo.
- **Conferência externa fraca**: o `selftest` compara GPU com a nossa CPU. Contra o motor de
  referência só há um teste de ~60 tokens (Qwen3-30B) e nenhum com o backend Vulkan.
- **Tokenizador BPE aproximado**: pode tokenizar diferente do oficial; nunca foi medido.
- **Medições derivam com a temperatura da máquina**: só valem alternadas (ver `conhecimento.md` seção 7).
- **Trabalho feito por agentes**: as etapas 2 a 5 do Vulkan foram escritas por agentes e conferidas
  rodando os testes e os benchmarks; o código não foi lido linha a linha. Um comentário antigo em
  `katana.c` ainda diz que o Vulkan só faz o produto de matriz por vetor.
- **Decisões em aberto**: Vulkan como padrão; SPIR-V versionado como cabeçalho (é o que está feito)
  ou gerado no build; `compare.sh` cita o executável do motor de referência (único lugar do
  repositório que o nomeia).

## 4. Como retomar

**Compilar e conferir**

```
cmd //c "cd /d C:\source\katana && .\build.bat"
./katana.exe selftest modelos/qwen2.5-1.5b-q4km.gguf                       # OpenCL: 4 linhas OK
KATANA_BACKEND=vulkan ./katana.exe selftest modelos/qwen2.5-1.5b-q4km.gguf  # Vulkan: lote e token a token OK
KATANA_KV16=0 ./katana.exe selftest modelos/olmoe-1b-7b-q4km.gguf           # MoE (OpenCL)
KATANA_KV16=0 KATANA_CTX=512 ./katana.exe selftest modelos/qwen3-30b-a3b-q4km.gguf
```

**Medir**

```
./katana.exe bench <modelo> 64 64                    # terceira rodada; alternar backends
KATANA_BACKEND=vulkan ./katana.exe vkcheck <modelo> <texto.txt> 40 600   # lote contra token a token
bash compare.sh modelos/qwen3-30b-a3b-q4km.gguf ref/llama 3 200          # contra a referência
```

**Shaders**: fontes em `vk/shaders`, cabeçalhos SPIR-V gerados por `vk/shaders/gen.sh` com
`ref/vk/glslang/bin/glslang.exe` (execução liberada em `.claude/settings.local.json`). Quem só
compila o motor não precisa do compilador de shaders.

**Variáveis úteis do Vulkan**: `KATANA_VK_REC=0` (fila ao vivo em vez de grafo gravado),
`KATANA_VK_BATCH=0` (prompt token a token), `KATANA_VK_PASS` (tokens por passada; padrão 256),
`KATANA_VK_ATT=3` (atenção em três kernels), `KATANA_VK_DEVICE` (escolher a placa).

**Onde está cada coisa**

| Caminho | O quê |
|---|---|
| `katana.c` | o motor; código Vulkan nas funções `vk_*` |
| `vk/kvk.h`, `vk/LEIAME.md` | runtime Vulkan e sua documentação |
| `vk/shaders` | shaders usados pelo motor |
| `tools/vkmv`, `tools/vkatt`, `tools/vkq4k` | bancadas de teste dos kernels (produto por token, atenção e auxiliares, lote), com `NOTAS.md` |
| `tools/vkprobe`, `tools/vkgemm` | protótipos iniciais |
| `ref/` (fora do git) | motor de referência (binários CUDA e Vulkan, código-fonte), compilador de shaders |
| `modelos/` (fora do git) | GGUFs e shards `.ktn` |

**Regras do projeto**: em `CLAUDE.md`. As principais: português; o código não cita outros motores;
commit e push a cada passo no repositório privado; nada de publicar; comparar velocidade só com
medidas alternadas e dizer o que é medido e o que é estimativa.

## 5. Linux (2026-11-15, segunda sessão)

A máquina passou a rodar Ubuntu (kernel 7.0, mesma RTX 3050 4 GB). Sem root, o compilador foi
montado com `apt-get download` + `dpkg -x` em `~/.local/tc` (gcc 15, binutils, libc6-dev,
cabeçalhos OpenCL). `build.sh` compila com o `gcc` do sistema ou, sem ele, com esse diretório.
Única mudança no código: `_putenv` mapeado para `putenv` fora do Windows.

- `selftest` passa no Linux: Qwen 1,5B pelo OpenCL e pelo Vulkan (lote 0,0098), OLMoE e Qwen3-30B
  (`KATANA_KV16=0 KATANA_CTX=512`) pelo OpenCL.
- Bench Qwen2.5 1,5B 64+64, alternado, rodadas 2 e 3: OpenCL prompt 217–220 / geração 93–97;
  Vulkan prompt 922–942 / geração 86–88 tok/s. No Windows o Vulkan dava 1.349–1.364 / 98–100
  (outra hora, outro sistema: não comparável diretamente). A queda do Vulkan no Linux é um ponto a
  investigar (driver, clocks ou caminho de submissão).

### 5.1 Passo 2.1a feito no Linux (Vulkan, Qwen2.5 1,5B)

- `run`, `chat` de uma pergunta e `chat` interativo (dois turnos, lembra o nome): ok, grafo
  gravado em Vulkan, geração 83–97 tok/s.
- `agent`: `list_dir` não existia fora do Windows; portado com `opendir`/`readdir`. Listagem ok;
  `run_command` pede confirmação e respeita a recusa.
- Mistral 7B Q5_K_M: carrega 25 de 32 camadas na VRAM ("Vulkan: grafo na CPU"), responde certo,
  geração 6,3 tok/s. Ou seja, o resto cai na CPU; o caminho misto de verdade é o passo 2.1b.
- Não testado ainda: Qwen3 denso (não há GGUF em `modelos/`).

### 5.2 Lentidão do Vulkan no Linux: causa e correção

- Causa: o driver Linux compila mal ladrilhos em que um subgrupo tem 4 acumuladores 16x16; o 64x64 que
  o motor usava acima de 64 tokens (e todo o gate/up) rendia ~1,3 T contra ~2,8 T no Windows.
  Detalhes em `tools/vkq4k/NOTAS.md`, seção Linux.
- Correção: novos ladrilhos (64x64 em 4x2 subgrupos, glu 64x32 em 2x2). Prompt do Qwen 1,5B 851 →
  1.515 tok/s, Llama 3B 411 → 750 (alternado, `comparacoes.md` seção 11). Acima do Windows antigo.
- Diferença de logit do lote no Llama 3B no `vkcheck` de 600 tokens: 0,006 (era 0,0255; passo 2.1f).
  No `selftest` de 23 tokens continua 0,025 (lote pequeno usa o 64x16, que não mudou).
- Geração: a "lentidão" era deriva térmica; 98–99 tok/s no Vulkan contra 101 no OpenCL.
- `vkmm` compila no Linux. glslang Linux em `~/.local/tc/gl` (`gen.sh` acha sozinho).
- Orçamento de VRAM fora do Windows: 88% (era 92%; o Qwen3-30B estourava com erro -4 no OpenCL).
- Pendente: medir os ladrilhos novos no Windows (podem mudar lá; não medido).

## 6. Sessão de 2026-11-15/16 (Linux, agentes em paralelo)

Entrou no main (detalhes nos commits e em `docs/modelos-novos.md`):
- Vulkan: ladrilhos com até 2 acumuladores por subgrupo (driver Linux), B em [k][n] no `mm.comp`
  (bancada 3,8–4,1 T), atenção em lote por blocos e atenção de um token dividida em pedaços,
  grafo parcial (modelo maior que a VRAM), MoE em Vulkan (`KATANA_VK_MOE=1`, ainda mais lento que o OpenCL).
- CPU: camadas em lote no grafo parcial, produto VNNI (Q4_K/Q6_K).
- Arquiteturas novas: `deepseek2` (DeepSeek-V2-Lite, Moonlight: MLA absorvida, roteador sigmoide,
  compartilhados) e `qwen35moe` (Qwen3.6-35B-A3B: DeltaNet + atenção com portão, 256 especialistas).
  Cache de especialistas generalizado (camadas densas, compartilhado, roteador único, modo híbrido).
- Formatos: MXFP4, IQ4_NL/XS, Q2_K, Q4_1, Q5_1, BF16, IQ1/2/3, TQ1/2 (`katana qtest`).
- Agente: busca web (meta-buscador embutido), web_fetch, search_files, memória, log, cliente MCP,
  formato XML de ferramentas, `KATANA_THINK=0`. Interface: console e `katana serve` (página local).
- Bancada `tools/vkdelta`: kernels Vulkan do DeltaNet (geração 0,025 ms/camada; lote 8,5x), ainda
  não ligados ao motor.
- Estudos: `docs/estudo-provedores.md` e `docs/base-ia/`.

Medidas desta sessão são indicativas (máquina compartilhada com outra sessão compilando).
Moonlight geração ~11 → 30–33 tok/s; Qwen3.6 ~25–27 tok/s (bench 16+48, alternado).

Próximos: grafo completo do qwen35moe na GPU com `tools/vkdelta`; prompt do DeltaNet em blocos;
saída JSON garantida; cache persistente do prompt do agente; MTP; teste dourado de tokenização;
KATANA_CONFIRM (bloqueado pelo classificador de permissões, decisão do dono).

## 7. MoE em Vulkan mais rápido e cache de especialistas no modo híbrido (2026-11-16)

O que mudou (`katana.c`, `vk/shaders`):
- `mv_*` ganharam o modo de várias matrizes num lançamento (porte do `MOEPRE`): push constants `mtp, mxrow, morow, mcol`
  (44 bytes), a tabela de 6 uint por matriz vem no binding do bias (`vk_gmv_multi`). Por camada MoE: gate/up+SiLU de todos
  os quentes num despacho, down noutro (antes 2 por especialista: 16 para K=8).
- Roteador na GPU (`moe_router.comp`, pesos em float32 em linhas simples, um grupo por especialista): os logits saem logo
  depois da entrada normalizada em `vkg.xn` e uma leitura traz os dois; a escolha top-K continua na CPU (`moe_lg_ext` em
  `moe_route`), porque os frios e o calor precisam dela. `KATANA_VK_ROUTER=0` volta ao roteador na CPU.
- `moe_sum_rms.comp`: soma pesada do MoE + norma de entrada da camada seguinte num despacho (`vk_moe_ffn_x`,
  `vk_emit_attn_x` com `pre_norm`); q/k/v sem barreira entre si; no grafo ao vivo a atenção de um token só lança os
  pedaços de contexto já ocupados (`vk_pos_live`).
- Modo híbrido (MLA, deepseek2) em Vulkan: `vk_host_moe_setup`/`vk_host_moe_ffn` (vagas só nas camadas MoE, compartilhado na
  GPU, roteador comum `moe_route` com softmax ou sigmoide+viés, frios na CPU, uma leitura por camada). `gx_host_layer`
  escolhe OpenCL ou Vulkan. O qwen35moe não usa (o grafo próprio cria as vagas).
- Padrões: `vk_ok` não exclui mais MLA; MoE em Vulkan ligado por padrão exceto olmoe (`KATANA_VK_MOE=1` liga no olmoe,
  `KATANA_VK_MOE=0` desliga em todos).

Medidas (geração tok/s, `bench 64 64`, rodadas 2 e 3, alternadas na mesma hora; máquina às vezes com outra compilação
rodando, então só valem as comparações dentro de cada linha):

| Modelo | Vulkan antes | Vulkan agora | OpenCL |
|---|---|---|---|
| Qwen3-30B-A3B (`KATANA_KV16=0 KATANA_CTX=512`) | 34,4–35,9 | 40,0–43,0 (média 41,4) | 40,6–42,4 (média 41,6) |
| OLMoE (`KATANA_KV16=0`) | 105,6–105,9 | 127,5–132,7 | 132,6–136,1 |
| Moonlight (`KATANA_CTX=512`) | 10,2–10,3 (especialistas na CPU) | 22,9–23,1 | 21,5–21,8 |
| DeepSeek-V2-Lite (`KATANA_CTX=512`) | — | 21,6–23,9 | 21,2–22,3 |
| Qwen3.6-35B-A3B (`KATANA_CTX=512`, grafo próprio) | 28,6–29,3 | 31,9–32,7 | — |

- Qwen3-30B: empate dentro do ruído (numa série anterior 42,0–42,6 contra 41,0–42,3). Prompt no Vulkan continua token a
  token: 44 contra 57 tok/s no OpenCL.
- OLMoE: com prompt de 16 o Vulkan dava 133 contra 128–129; com 64+64 fica 2–4% atrás, por isso continua no OpenCL por
  padrão. Medido por partes (temporizadores provisórios): ~5,2 ms/token esperando a GPU (atenção + quentes), ~1,4 ms
  na CPU (frios), roteamento na CPU custava 0,4 ms (agora ~0). Tentativas sem ganho: alvo de segmentos 8k–64k, submeter
  os quentes antes da CPU (flush), soma separada da norma.
- Moonlight: o indicativo de 30 tok/s da sessão anterior não se repetiu nesta hora em nenhum dos dois backends.

Validação (`selftest`, padrão e `KATANA_BACKEND=opencl`): qwen2.5-1.5b, OLMoE (`KATANA_VK_MOE=1` também), Qwen3-30B,
Moonlight, Qwen3.6 OK. DeepSeek-V2-Lite: a diferença do token a token / lote fica entre 0,025 e 0,055 conforme quais
especialistas estão quentes (os frios usam ativações de 8 bits na CPU; o modelo é sensível), nos dois backends; numa
rodada Vulkan passou do limite de 0,05 (0,0555) e na seguinte deu 0,035. Os quentes do Vulkan conferidos contra a CPU
(diferença < 1%, teste provisório). Um `selftest` do Qwen3.6 terminou com falha de segmentação uma vez na bateria e não
se repetiu em três rodadas seguidas (investigar se reaparecer). A diferença do lote do Qwen3.6 (0,06–0,15, tolerância de
3% do maior logit) varia de rodada a rodada com o estado das vagas, no binário antigo e no novo; numa rodada da bateria o
"lote 16" trocou o argmax (148622 x 532, diferença 0,116: dois logits quase empatados) e na seguinte passou. É a precisão
do lote em fp16 que outro agente está tratando (`mm.comp`, `dn_*`), caminho que esta mudança não toca.

Observação: as ferramentas `tools/vkmv` e afins que usam os `mv_*` empurram 28 bytes de push constants; com o layout novo
precisam mandar 44 (os 4 campos novos em zero).

## 8. Prompt MoE em lote no Vulkan (2026-11-16)

O que mudou (`katana.c`, `vk/shaders/moe_router.comp`):
- `vk_plan`/`vk_batch_build` aceitam MoE no grafo completo (qwen3moe; olmoe com `KATANA_VK_MOE=1`). A atenção do lote é a
  mesma dos densos (`vk_emit_n`); no lugar do feed-forward entra `vk_moe_n`, por camada:
  norma em fp32 (`rms_wg32`) e logits do roteador no `mm.comp` preciso (pesos float32 do roteador agora no layout da GPU,
  grupos de 32 linhas; o `moe_router.comp` de um token passou a ler esse layout, um grupo por 32 especialistas);
  uma leitura traz entrada normalizada e logits; a CPU escolhe top-K de cada token (`moe_route` com `moe_lg_ext`), atualiza
  calor/vagas como no caminho de um token e agrupa os pares por vaga; na GPU cada vaga roda sobre as colunas dos seus
  tokens (`moe_gath32`, `mm` glu e down com a vaga por deslocamento, `moe_scat` soma pesada no residual).
- Frios do lote: os que têm pelo menos `KATANA_VK_COLDMIN` (padrão 2) tokens no lote sobem para até `KATANA_VK_COLDT`
  (padrão 16) vagas temporárias (`vkbm.tg/td`), lidos do shard por um arquivo próprio (a thread carregadora usa o dela),
  do blob morno, ou convertidos na hora; o resto roda na CPU enquanto a GPU trabalha e sobe somado. As vagas do cache
  não mudam por causa disso (a thread carregadora continua só com vagas livres no lote).
- Precisão: os produtos dos especialistas usam o modo preciso (alta + resto, como o qwen35moe). Em fp16
  (`KATANA_VK_MOEP=0`) o OLMoE falha o `selftest` no "lote 16" (1,29 de diferença: roteamento quase empatado que vira);
  os kernels foram conferidos contra o modo preciso (erro relativo ≤ 8e-4). O modo preciso não custou velocidade
  medível (o gargalo é CPU/subidas).
- `KATANA_VK_MOEPROF=1`: tempo por passada (leitura, roteamento, frios na CPU, subidas temporárias, pares).
  Exemplo (Qwen3-30B, lote 256): ~1,7–2,2 s, frios na CPU ~0,9 s, subidas ~0,5–1,0 s, leitura ~0,17 s.

Medidas (`bench`, rodadas 2 e 3, binário anterior / novo / OpenCL alternados na mesma hora, GPU ≤ 72 °C no início de
cada rodada; detalhes em `comparacoes.md` seção 12):

| Modelo | | anterior (Vulkan) | novo (Vulkan) | OpenCL |
|---|---|---|---|---|
| Qwen3-30B (`KATANA_KV16=0 KATANA_CTX=1024`), 256+16 | prompt | 48,1–49,0 | 140,7–149,4 | 57,6–58,1 |
| Qwen3-30B, 64+64 | prompt | 44,6–45,2 | 66,4–68,1 | 60,3–60,6 |
| | geração | 41,8–42,6 | 42,3–42,9 | 41,6–41,8 |
| OLMoE (`KATANA_KV16=0 KATANA_VK_MOE=1`), 256+16 | prompt | 139,8–141,3 | 362,5–375,5 | 209,0–212,0 |
| | geração | 121,6–122,4 | 118,4–122,5 | 126,6–127,0 |

- A geração de 16 tokens depois do lote de 256 no Qwen3-30B variou muito no binário novo (23,5–41,8); com 64+64 empata.
  Provável: o lote mexe no calor e nos pedidos da carregadora, e 16 tokens são curtos para medir. Não investigado.
- Tentado sem ganho: 32 ou 48 vagas temporárias (120 e ~100 tok/s contra 144–146 com 16; mais VRAM nelas tira vagas do
  cache e mais subidas por camada), `KATANA_VK_COLDMIN=4` empata com 2.

Validação (`selftest`): qwen2.5-1.5b (Vulkan, lote 0,0078), OLMoE padrão (OpenCL) e com `KATANA_VK_MOE=1` (lote 0,0042,
lote 16 0,0044), Qwen3-30B (`KATANA_KV16=0 KATANA_CTX=512`; lote 0,003–0,017 conforme quais especialistas estão quentes,
lote 16 0,0039), Moonlight (0,0002, caminho não mudou), Qwen3.6 (lote 0,0006). Todos OK.

Pendências: lote MoE no modo híbrido (MLA: Moonlight e DeepSeek-V2-Lite continuam com prompt token a token: atenção na
CPU por token + `vk_host_moe_ffn`); subidas temporárias em paralelo com os frios da CPU (hoje uma depois da outra na
thread principal); roteamento da CPU (~0,1 s por passada de 256 no Qwen3-30B) poderia ir para a GPU (top-K no shader).

**Correção (revisão do coordenador)**: no `selftest` do Qwen3-30B o "lote 16" falhou com 0,115 em 2 de 3 rodadas no main. Causa: precisão, não
estado entre passadas. Medido: a saída de cada vaga (quente e temporária) confere com a CPU (erro relativo ≤ 1,4e-3, int8 na CPU); o que
mudava era a atenção do lote em fp16 (norma fp16, q/k/v/o em fp16, saída da atenção em fp16), que desloca o residual o bastante para virar
escolhas quase empatadas do roteador. Agora, no MoE com o modo preciso (padrão), a atenção do lote também é fp32 (`rms_wg32`, `mm` alta +
resto, `att_flash32`), como no qwen35moe. Diferença do lote no Qwen3-30B caiu de 0,003–0,017 para ~0,0005; 6 `selftest` seguidos OK;
OLMoE (`KATANA_VK_MOE=1`) lote 0,0002 / lote 16 0,0001; Qwen3.6 e qwen2.5 OK. Velocidade alternada (256+16, rodadas 2–3): Qwen3-30B
125–146 antes contra 138–139 depois; OLMoE 352–375 contra 362–364 (empate dentro do ruído).

## 9. Prompt em lote para MLA (deepseek2) no Vulkan (2026-11-16)

O que mudou (`katana.c`, `vk/kvk.h`, `vk/shaders/mla_att.comp`):
- `vk_mla_build` (chamado no `mla_gpu_upload` antes das vagas, que ficam com o resto da VRAM; ~184 MB com lote de 256)
  e `vk_mla_n` (usado pelo `forward_seq`). Residual do lote na VRAM (`vkb.x`, transposto). Por camada: norma fp32,
  q e kv_a no `mm` preciso; uma leitura traz os dois; a CPU faz a norma do latente, RoPE e grava `m->ckv` (o mesmo cache
  do token a token, que continua igual); a atenção absorvida roda na GPU (`mla_att.comp`, fp32, um grupo por
  token×cabeça, softmax online; sobem por camada o q, as linhas do cache até `pos0+B` e `k_b`/`v_b` em float32, sem
  ficar residentes para não tirar vagas); produto da saída soma no residual. Feed-forward: camada densa com par
  gate/up próprio do lote (`vkml.dg`), MoE pelo `vk_moe_n` (roteador float32 na GPU, compartilhado na GPU, frios em
  vagas temporárias ou na CPU). Cabeça na CPU (como no modo híbrido).
- `KATANA_VK_MLA=0` desliga o lote; `KATANA_VK_MLAATT=0` põe a atenção do lote na CPU (`mla_n_job`, blocos de 8 tokens).
- `vk_moe_n` (vale também para qwen3moe/olmoe): os frios temporários são lidos do shard por uma thread direto numa área
  mapeada (`kvk_host_create`/`kvk_copy_q`, novos no `kvk.h`), enquanto a principal enfileira os quentes e roda os frios
  da CPU (`vk_moe_cold_cpu`); antes era leitura + cópia para o staging + espera, em série. `KATANA_VK_TMPTHR=0` volta.
- `KATANA_VK_MOEPROF=1` mostra também o tempo do MLA por passada.

Medido por partes (Moonlight, 256 tokens, `MOEPROF`): só o lote com atenção na CPU ~84 tok/s (atenção 0,85 s, frios
CPU 0,95 s, subidas temporárias 1,1 s); + thread de leitura ~95; + atenção na GPU ~121.
Tentado: `KATANA_VK_COLDT=32` dá prompt 126 mas geração ~30 (menos vagas); `COLDMIN=1` igual a 2. Ficou 16/2.

Medidas (`bench`, `KATANA_CTX=1024`, rodadas 2–3, main anterior / novo / OpenCL alternados na mesma hora, GPU ≤ 76 °C no
início de cada execução; outra sessão do katana estava aberta na máquina):

| Modelo | | anterior (Vulkan) | novo (Vulkan) | OpenCL |
|---|---|---|---|---|
| Moonlight, 256+16 | prompt | 30,2–32,5 | 108–123 | 27,9–30,3 |
| | geração | 23,0–24,9 | 28,6–32,1 | 21,1–23,3 |
| Moonlight, 64+64 | prompt | 24,9–27,4 | 58,4–59,1 | 24,1 |
| | geração | 20,9–25,3 | 24,1–24,4 | 21,2–22,4 |
| DeepSeek-V2-Lite, 256+16 | prompt | 27,0–27,7 | 76,8–81,8 | 24,9–25,4 |
| | geração | 13,3–13,8 | 22,4–23,2 | 12,6–12,8 |
| DeepSeek-V2-Lite, 64+64 | prompt | 18,8–21,0 | 46,1–48,6 | — |
| | geração | 23,6–23,8 | 27,4–28,4 | — |

A geração de 16 tokens logo após 256 de prompt é curta e sensível ao estado das vagas (o lote aquece especialistas);
com 64+64 a geração do novo empata ou fica um pouco à frente. O caminho de geração não mudou.

Validação (`selftest`, Vulkan): Moonlight 4 vezes seguidas, lote 0,00018 (antes 0,00016 com atenção na CPU);
DeepSeek-V2-Lite 2 vezes, lote 0,00034, token a token 0,0245 (frios int8 na CPU, como antes); Qwen3-30B
(`KATANA_KV16=0 KATANA_CTX=512`) lote 0,0005 / lote 16 0,0005; qwen2.5-1.5b lote 0,0078. Todos OK.

Pendências:
- **Falha de segmentação intermitente**: 2 em ~7 execuções de `bench 64 64` do Moonlight no binário novo (Vulkan), com a
  GPU a 82–83 °C e outra sessão do katana aberta; depois disso 10 execuções seguidas (4 sob gdb, 6 sem) sem falha. Não
  reproduzida, causa desconhecida. Suspeitos: a thread de leitura dos temporários (`vk_trd`) e o caminho de fallback;
  `KATANA_VK_TMPTHR=0` desliga a thread se reaparecer.
- Atenção MLA de um token na GPU (geração) não feita: exigiria `k_b`/`v_b` residentes (~216 MB em float32, tira vagas)
  e o grafo da camada na GPU para cortar as idas e voltas (q, kv_a, saída, MoE: 4 leituras por camada). É onde está o
  ganho da geração, não na conta da atenção.
- O cache latente sobe inteiro (até `pos0+B`) a cada camada e passada; com contexto longo vale manter uma cópia na VRAM.
- Velocidade do Qwen3-30B/OLMoE com a thread de leitura no `vk_moe_n` não medida (só o `selftest`).
