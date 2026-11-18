# katana: todas as comparações medidas

Tudo medido em 2026-11-15 na mesma máquina: i7-11800H, 64 GB, RTX 3050 Laptop 4 GB, Windows 11.
Motor de referência: binário oficial b12418, nas versões CUDA e Vulkan. Velocidades em tokens por
segundo (tok/s), salvo indicação. A máquina deriva ao longo do dia; onde diz "alternado", os
motores foram medidos em sequência A, B, A, B na mesma hora.

## 1. Modelos densos: situação final do dia (alternado, benchmark 64+64)

| Modelo | | katana OpenCL | katana Vulkan | referência Vulkan | referência CUDA* |
|---|---|---|---|---|---|
| SmolLM2 135M Q8 | prompt | 1.227 | 5.972–8.478 | 7.995–8.408 | 6.708 |
| | geração | 119–139 | 385–409 | 198–223 | 402 |
| Qwen2.5 1,5B Q4_K_M | prompt | 212 | 1.349–1.364 | 2.362–2.374 | 2.776 |
| | geração | 98 | 98–100 | 113–115 | 117 |
| Llama 3.2 3B Q4_K_M | prompt | 108 | 753–761 | 1.357–1.364 | 1.685 |
| | geração | 57 | 56 | 63–64 | 69 |

\* CUDA medido mais cedo no mesmo dia, sem alternar com os demais.

Quem ganha onde:

| | Resultado |
|---|---|
| Geração, modelo de 135 M | katana Vulkan ~2x a referência Vulkan; empate com o CUDA |
| Geração, 1,5 B e 3 B | referência à frente: ~13–15% (Vulkan), ~20% (CUDA) |
| Prompt, modelo de 135 M | empate com a referência Vulkan |
| Prompt, 1,5 B e 3 B | referência à frente: ~1,7–1,8x (Vulkan), ~2,1–2,2x (CUDA) |

## 2. Modelos densos: como era antes do backend Vulkan (benchmark 64+64)

| Modelo | | katana OpenCL | referência CUDA | referência Vulkan | distância no prompt |
|---|---|---|---|---|---|
| SmolLM2 135M | geração | 127 | 402 | 224 | |
| | prompt | 1.222 | 6.708 | 8.638 | 5–7x |
| Qwen2.5 1,5B | geração | 96 | 117 | 90 | |
| | prompt | 212 | 2.776 | 2.343 | 11–13x |
| Llama 3.2 3B | geração | 57 | 69 | 63 | |
| | prompt | 108 | 1.685 | 1.375 | 13–16x |

Observação: a geração da referência Vulkan no Qwen 1,5B foi 90 nesta medida e 113–115 na medida
alternada posterior. Vale a posterior; a diferença mostra o quanto uma medida isolada engana.

## 3. Evolução do katana no prompt (Qwen2.5 1,5B, benchmark 64+64)

| Versão | Prompt |
|---|---|
| OpenCL, lote de 16 (float) | 212 |
| Vulkan, um token por vez (etapa 4) | 121 |
| Vulkan, lote com núcleos tensoriais (etapa 5) | 1.349–1.364 |
| Vulkan, prompt de 512 tokens (medida do agente) | 1.544–1.563 |
| referência Vulkan | 2.362–2.374 |

## 4. Qwen3-30B-A3B (mistura de especialistas, 18,6 GB numa placa de 4 GB)

Texto real, 200 tokens gerados, três rodadas alternadas (`compare.sh`), katana no OpenCL:

| | katana | referência com cache de especialistas na GPU | referência com especialistas só na CPU |
|---|---|---|---|
| geração | 12,5–14,9 | 13,2–14,4 | 6,7–8,6 |
| prompt curto a frio | 9–11 | ~5,5 | ~5,5 |

Benchmark sintético 64+64 (referência com especialistas só na CPU; katana antes e depois do lote):

| | katana antes | katana depois | referência |
|---|---|---|---|
| prompt | ~22 | 28–33 | 26,7 ± 8,5 |
| geração | 16–17 | 15–18 | 14,0 |

Outras configurações da referência no mesmo modelo: só CPU 5,2 de geração; 9 camadas na GPU 7,7.

Histórico da comparação: primeiro medimos contra a referência sem cache de especialistas e
concluímos "1,6x a nosso favor na geração". Depois descobrimos que ela tem cache próprio
(`--moe-cache-mib`, `--moe-cache-cpu-share`); com ele ligado a geração empata.

Conferência de saída: o texto guloso do katana bate com o da referência por ~60 tokens (um teste).

## 5. Produto em lote isolado (trilhões de multiplicações por segundo)

| Versão | lote 64 | lote 256 |
|---|---|---|
| katana OpenCL (qualquer lote) | ~0,32 | ~0,32 |
| Vulkan ingênuo (um ladrilho por subgrupo) | 0,51 | — |
| + ladrilho na memória compartilhada, 4 acumuladores | 1,17 | 1,36 |
| + cargas de 4 valores, ladrilho 64x64x64 (pesos fp16) | 1,88 | 2,4–2,6 |
| pesos Q4_K no layout do katana | 1,96–2,50 | 2,65–2,83 |
| pesos Q6_K no layout do katana | 2,17–3,27 | 3,23–3,34 |
| referência Vulkan (estimado a partir do prompt) | ~3,5 | — |

Lote de 16 em Q4_K: 0,57 completando até 64; 1,40 com ladrilho estreito.

## 6. Custo por despacho de kernel

| | µs por despacho |
|---|---|
| OpenCL, enfileirar um kernel (host) | 22–25 |
| Vulkan esperando a cada despacho | ~156 |
| Vulkan gravando a fila ao vivo | ~4,7 |
| Vulkan com grafo gravado e resubmetido | 1,5–2,4 |

Efeito no motor: modelo de 135 M, geração 119–139 (OpenCL) → 385–409 (Vulkan gravado). Nos
modelos de 1 B ou mais não muda nada, porque o limite é a conta na GPU.

## 7. Evolução do katana em mistura de especialistas (OpenCL)

| Mudança | Antes | Depois |
|---|---|---|
| OLMoE: um kernel para os especialistas do token | 20 | ~88 |
| OLMoE só CPU: especialistas num despacho | 20 | 31 |
| OLMoE com 25% dos especialistas na VRAM: frio roda na CPU | 16 | 35 |
| OLMoE, prompt em lote | ~90 | 110–155 |
| OLMoE, prompt, norma dentro do kernel | ~145 | ~174 |
| Qwen3-30B, texto real: aquecimento de páginas e troca menos agitada | 8,4 | 13,9–14,4 |

OLMoE com o cache de VRAM reduzido de propósito:

| Especialistas na VRAM por camada | Usos atendidos pela VRAM | Geração |
|---|---|---|
| 51 de 64 (padrão) | 95% | 55–90 |
| 32 de 64 | 83% | 42 |
| 16 de 64 | 62% | 35 |
| 8 de 64 | 45% | 32 |
| só CPU | — | 20–31 |

## 8. A ideia "só entra na GPU o que interessa ao assunto"

Modelo denso (Qwen 1,5B, `katana sparsity`): previsões que continuam iguais mantendo só parte dos
neurônios do feed-forward.

| Neurônios mantidos | mesmo assunto | outro assunto | ao acaso | escolha ideal por token |
|---|---|---|---|---|
| 10% | 6% | 4% | 0% | 49% |
| 30% | 30% | 17% | 1% | 84% |
| 50% | 55% | 38% | 8% | 94% |
| 70% | 72% | 49% | 21% | — |

Modelo MoE (`katana routing`): usos atendidos com parte dos especialistas na VRAM.

| Na VRAM | OLMoE: teto | OLMoE: calibrado no mesmo assunto | OLMoE: outro assunto | Qwen3-30B: teto | Qwen3-30B: mesmo assunto | acaso |
|---|---|---|---|---|---|---|
| 5% | 28% | 28% | 5% | 34% | 31% | 5% |
| 10% | 40% | 39% | 13% | 52% | 47% | 10% |
| 15% | 49% | 48% | 21% | 65% | 60% | 15% |
| 25% | 64% | 63% | 34% | 83% | 79% | 25% |
| 35% | 76% | 74% | 46% | 93% | 89% | 35% |
| 50% | 89% | 87% | 59% | 99% | 96% | 50% |

Qwen3-30B com vagas iguais por camada: teto de 64,4% com 15% na VRAM, contra 65,1% com vagas livres.

## 9. Primeiros números do motor (OpenCL, início do dia, sem alternar)

| Modelo | Situação | Prompt | Geração |
|---|---|---|---|
| Qwen2.5 1,5B | 28/28 camadas na GPU | ~212 | ~94 |
| TinyLlama 1,1B | GPU | ~287 | ~115 |
| SmolLM2 135M | GPU | ~1.150 | ~105 |
| Llama 3.2 3B | 28/28 na GPU | 107 | 55 |
| Qwen2.5 7B | 26/28 na GPU | ~32 | ~15 |
| Mistral 7B Q5_K_M | 23/32 na GPU | 12 | 8,5 |
| Qwen2.5 14B (9 GB) | 21/48 na GPU | 4,5 | 3,3–3,6 |
| Qwen2.5 1,5B | só CPU, 12 threads | 26 | ~22 |

## 10. O que foi testado e não deu ganho

| Tentativa | Resultado |
|---|---|
| 8 threads em vez de 12 | ~20% pior |
| Fixar threads em núcleos; desligar limitação de energia do Windows | sem efeito |
| Matemática rápida no OpenCL; espera ativa na leitura; despacho da fila por camada | sem efeito |
| Atenção de um token fundida num lançamento (OpenCL) | correta, sem ganho acima de 1 B |
| Usar 97% da VRAM em vez de 92% | geração cai de 13,5 para 7,9 |
| Vagas de VRAM desiguais por camada | 0,7 ponto abaixo do teto |
| Converter só especialistas usados duas vezes | piorou (50 contra 60) |
| Ladrilho de lote 128x128 com 16 acumuladores | 0,4–0,7 T/s contra 2,5 |
| Acumulador em meia precisão no lote | sem ganho e erro de 2–6% |
| Tabela de escalas do Q4_K na memória compartilhada | mais lenta (2,54 contra 2,79 T/s) |
| Ler ativações fp32 direto no lote | 2–6% mais lento que converter uma vez |

## 11. Linux (Ubuntu, driver NVIDIA 595.99, 2026-11-15)

Vulkan, binário anterior contra o novo (ladrilhos com no máximo 2 acumuladores por subgrupo),
alternados, rodada 3, tok/s:

| Modelo, benchmark | | anterior | novo |
|---|---|---|---|
| Qwen2.5 1,5B, 512+128 | prompt | 851–854 | 1.512–1.518 |
| | geração | 80,5–81,0 | 80,8–81,1 |
| Llama 3.2 3B, 512+128 | prompt | 411–412 | 749–752 |
| | geração | 47,2–47,4 | 47,2–47,4 |
| Qwen2.5 1,5B, 64+64 | prompt | 900–903 | 1.561–1.562 |
| | geração | 98,0–98,8 | 98,4–99,0 |

Na mesma hora, OpenCL no Qwen2.5 1,5B 64+64: prompt 211–212, geração 100,7–101,5.
O "Vulkan mais lento na geração" visto no começo (86–88 tok/s) era deriva térmica: medido de novo,
fica 2–3% abaixo do OpenCL, como no Windows. Espera do fence em laço (`KVK_SPIN`, padrão ligado):
geração +2–3% num teste 512+256 (13,1 → 12,8 ms/token), dentro do que a deriva também explica.

## 12. Prompt MoE em lote no Vulkan (Linux, 2026-11-16)

`bench`, binário anterior (Vulkan token a token no prompt MoE), novo e OpenCL, alternados na mesma hora, duas séries,
rodadas 2 e 3, tok/s. Espera até a GPU ficar em 72 °C ou menos antes de cada execução.

| Modelo, benchmark | | anterior | novo | OpenCL |
|---|---|---|---|---|
| Qwen3-30B-A3B, 256+16 (`KATANA_KV16=0 KATANA_CTX=1024`) | prompt | 48,1 / 48,5 / 48,7 / 49,0 | 140,7 / 144,2 / 145,5 / 149,4 | 57,6 / 57,9 / 57,9 / 58,1 |
| | geração | 26,4–26,7 | 23,5–41,8 | 34,3–34,8 |
| Qwen3-30B-A3B, 64+64 | prompt | 44,6–45,2 | 66,4–68,1 | 60,3–60,6 |
| | geração | 41,8–42,6 | 42,3–42,9 | 41,6–41,8 |
| OLMoE 1B-7B, 256+16 (`KATANA_KV16=0`; Vulkan com `KATANA_VK_MOE=1`) | prompt | 139,8–141,3 | 362,5–375,5 | 209,0–212,0 |
| | geração | 121,6–122,4 | 118,4–122,5 | 126,6–127,0 |

Varredura das vagas temporárias (Qwen3-30B 256+4, sem alternar, GPU esquentando de 81 a 85 °C ao longo da série: só
indicativo): 16 vagas 144–146, 32 vagas 113–121, 48 vagas 97–108 tok/s.


## gemma4 26B-A4B: prompt em lote no Vulkan (2026-11-17, worktree agent-abec)

`katana bench <gemma4> 128 32`, RTX 3050 4 GB, alternado na mesma hora (duas rodadas de cada; rodadas 2 e 3 do
bench). Antes = `KATANA_VK_G4=0` (lote so na CPU), depois = lote hibrido (densos na GPU, MoE na CPU). Medido:

| caso | prompt tok/s | geracao tok/s |
|---|---|---|
| antes, r1 | 4,8 / 3,2 | 7,7 / 7,8 |
| depois, r1 | 3,4 / 3,2 | 8,2 / 8,0 |
| antes, r2 | 3,3 / 3,3 | 7,9 / 7,9 |
| depois, r2 | 3,3 / 3,3 | 7,9 / 6,6 |

Sem diferenca alem do ruido. Uma serie anterior com 256+32 (uma rodada de cada, sequencial, nao alternada:
so indicativo) deu prompt 4,2 (antes) x 5,7 (depois, MoE na CPU) x 6,1 tok/s (MoE nas vagas, inexato).
A geracao nao muda (o lote so afeta o prompt).

## 13. qwen35moe no Vulkan com ctx 131072: atencao completa na CPU (Linux, 2026-11-17)

bench 512+32, Qwen3.6-35B-A3B Q4_K_M, `KATANA_CTX=131072`, binario do main e novo alternados na mesma hora
(GPU livre no inicio de cada execucao; CPU com outros agentes).

| | main (modo hibrido) | novo (grafo + atencao na CPU) |
|---|---|---|
| prompt (rodadas 2-3) | 18,8-18,9 tok/s | 83,6-85,0 tok/s |
| geracao (rodadas 2-3) | 17,3 tok/s | 27,0-27,3 tok/s |

Primeira serie do main deu 3,7-5,3 / 3,1-17,8 (fria). DeltaNet em blocos x exato: inconclusivo (carga alta), ver handoff 0d.

## 14. Roteador do MoE na GPU na geracao (Linux, 2026-11-18, worktree agent-a516)

Perfil da geracao (`KATANA_VK_GENPROF=1`, bench 16+128, 3 rodadas, ctx 4096, GPU livre, load 4-9 da maquina
compartilhada: indicativo). Medias por camada MoE, so geracao:

| | Qwen3.6-35B-A3B (40 camadas, grafo Vulkan) | gemma4 26B-A4B (30 camadas, modo hibrido) |
|---|---|---|
| vagas por camada | 17 de 256 | 9 de 128 |
| usos dos especialistas na VRAM | 36% | 74% |
| camadas com os 8 escolhidos todos nas vagas | **0,3%** | 25% |
| leitura bloqueante por camada (inclui a conta da GPU) | 0,43 ms | (nao ha: camadas na CPU) |
| roteador na CPU | 0,053 ms (~2,1 ms/token) | 0,038 ms (~1,1 ms/token) |
| frios na CPU | 0,27-0,35 ms (~11-14 ms/token) | 0,64 ms (~19 ms/token) |
| geracao | ~34 ms/token | ~46-48 ms/token |

Roteador de um token na GPU no qwen35moe (`KATANA_VK_GPUROUTER=1`), alternado 0/1 na mesma hora, 3 series,
rodadas 2-3 (ms/token):

| serie | desligado | ligado |
|---|---|---|
| 1 | 33,56 / 33,80 | 33,22 / 33,36 |
| 2 | 34,27 / 34,27 | 33,50 / 33,76 |
| 3 | 34,68 / 34,84 | 34,65 / 34,55 |
| media | 34,24 | 33,84 (-1,2%, dentro do ruido) |

O roteador sai da CPU (0,053 -> 0,002 ms) mas a leitura cresce 0,03 ms (mais um despacho e 1 KB a mais).
Geracao gulosa de 80 tokens identica. Fica desligado por padrao.
