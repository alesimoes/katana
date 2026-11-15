# katana: plano

Escrito em 2026-11-15. Cada etapa tem um critério de pronto mensurável. O contexto está em
`conhecimento.md`; as técnicas, em `roteiro-otimizacoes.md`.

## Prioridade 1: backend Vulkan (fecha o buraco de 11–16x no prompt)

| Etapa | O quê | Pronto quando |
|---|---|---|
| 1 | Lote com Q4_K (e Q6_K) no layout do katana, decodificando na carga do ladrilho (`tools/vkq4k`) | **feito**: 1,96–2,69 T mult/s em Q4_K e 2,17–3,84 em Q6_K, conferido. Falta ladrilho estreito para lotes pequenos |
| 2 | Camada de runtime (`vk/kvk.h`) e porte dos kernels por token (`tools/vkmv`) | **feito**: testes passam (runtime nas duas placas; kernels na NVIDIA). Vazão no mesmo nível do OpenCL, ainda sem medida idêntica. Faltam lote por token, vários especialistas por lançamento e kernels fundidos |
| 3 | Integração em `katana.c`: escolha de backend (`KATANA_BACKEND`), carga dos pesos, produto por token | `selftest` passa em modelo denso pelo Vulkan |
| 4 | Grafo denso completo em Vulkan (normas, atenção, projeção final), geração | geração do Qwen 1,5B igual ou melhor que o OpenCL (96 tok/s) |
| 5 | Prompt em lote com núcleos tensoriais | prompt do Qwen 1,5B acima de 1.000 tok/s no benchmark 64+64 (hoje 212; referência 2.343) |
| 6 | MoE em Vulkan: cache de especialistas, especialistas frios na CPU | Qwen3-30B confere no `selftest`; geração igual ou melhor que hoje |
| 7 | Grafo gravado e resubmetido por token | geração do modelo de 135 M acima de 250 tok/s (hoje 127; referência Vulkan 224) |

Decisões pendentes: cabeçalhos Vulkan dentro do repositório ou declarados à mão; SPIR-V gerado
no build ou versionado como cabeçalho C (versionado dispensa o compilador de shaders para quem só
compila o motor).

## Prioridade 2: arquitetura DeepSeek/Kimi

O alvo "modelo de trilhão" é da família Kimi (MoE com atenção de posto baixo). O Kimi K2 tem
~600 GB em 4 bits e não cabe nesta máquina; o código se valida num irmão pequeno.

| Etapa | O quê | Pronto quando |
|---|---|---|
| 1 | Baixar DeepSeek-V2-Lite (16 B, ~10 GB) e ler os metadados | `katana info` mostra a arquitetura e os tensores |
| 2 | Atenção de posto baixo na CPU: q em dois estágios, K e V comprimidos e compartilhados entre cabeças, RoPE numa faixa, descompressão por cabeça | saída gulosa bate com o motor de referência por dezenas de tokens |
| 3 | Roteador sigmoide com viés, especialista compartilhado, camadas densas iniciais | idem |
| 4 | Caminho de GPU e cache de especialistas | `selftest` passa; medir contra a referência |
| 5 | Teste com Kimi, se houver armazenamento (SSD externo ≥ 1 TB) | roda, com a velocidade que der |

## Prioridade 3: CPU para os especialistas frios

No Qwen3-30B quase metade do tempo por token é CPU.

- Ativações de 8 bits com escala por bloco de 256 e somas parciais. Critério: mesmo token escolhido
  que o caminho de 16 bits em 1.000 tokens de texto real, e mais rápido.
- AVX-512 VNNI com escolha em tempo de execução.
- 8 linhas por passo.

Ganho estimado (não medido): até 1,5–2x no que cabe em cache.

## Prioridade 4: o que faz do motor um assistente

- Pré-tokenizador exato por tipo declarado no GGUF. Critério: mesma tokenização que o motor de
  referência num conjunto de textos (código, números, acentos, quebras de linha).
- Cache KV: atender só às posições usadas, reuso de prefixo, deslocamento de contexto, contexto
  maior que 4.096.
- Amostragem: penalidade de repetição, top-p, min-p.
- Compactação de contexto: resumir turnos antigos quando o contexto enche, e medir o que se perde.
- Agente: saída em JSON garantida por gramática, mais ferramentas, limites configuráveis.

## Prioridade 5: rede entre máquinas

- Cada máquina guarda parte dos especialistas e responde por eles; a que pediu soma os resultados.
- Primeiro experimento: duas instâncias na mesma máquina trocando especialistas por soquete, para
  medir o custo de ida e volta por camada.
- Depende de uma segunda máquina para o teste real.

## Fora de prioridade (com motivo)

- Fundir mais kernels no OpenCL: sem ganho medido acima de 1 B.
- Vagas de VRAM desiguais por camada: 1 ponto percentual abaixo do teto.
- Usar mais de 92% da VRAM: piora.
- Especialistas com menos bits só na VRAM: muda a saída do modelo; só como experimento medido.

## Como registrar progresso

Ao concluir uma etapa: número medido (alternado quando for comparação), atualização da tabela em
`conhecimento.md`, commit e push. Hipótese que não se confirmou também entra em `conhecimento.md`,
na lista do que não deu ganho.
