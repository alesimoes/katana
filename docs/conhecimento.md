# katana: o que sabemos

Documento de conhecimento do projeto, para quem (pessoa ou sessão de IA) retomar o trabalho.
Escrito em 2026-11-15. Tudo o que está marcado como **medido** foi medido nesta máquina; o resto
é estimativa ou leitura de código e está marcado assim. Planos estão em `plano.md`; o catálogo de
técnicas ainda não usadas está em `roteiro-otimizacoes.md`.

## 1. Objetivo

Infraestrutura de IA feita do zero, em C de baixo nível, capaz de servir modelos muito grandes
("trilhão de parâmetros") em hardware pequeno, com os pesos fluindo de disco para RAM para GPU,
e no futuro com o trabalho dividido entre máquinas (cada uma guarda e resolve uma parte).

Ideia central do dono do projeto: **só entra na GPU o que interessa ao assunto da conversa**. No
começo é lento; à medida que o assunto fica claro, só as partes do modelo ligadas a ele ocupam a
VRAM. Isso se mostrou falso para modelos densos e verdadeiro para modelos de mistura de
especialistas (seção 4).

Regras do projeto:

- Tudo local, sem serviços de terceiros. Nada proprietário: usar o que é publicado e escrever o nosso.
- O código não cita outros motores de inferência. A palavra "llama" no código é o nome de uma
  arquitetura de modelo. Comparações usam "motor de referência".
- Repositório privado em github.com/alesimoes/katana. Commits com push; não tornar público sem pedido.
- Toda afirmação de velocidade precisa de medição alternada (seção 7).

## 2. Máquina de desenvolvimento

- i7-11800H (8 núcleos, 16 threads, AVX2 e AVX-512 com VNNI), 64 GB de RAM, Windows 11.
- NVIDIA RTX 3050 Laptop 4 GB + Intel UHD. A NVIDIA fica com teto de potência por software em
  60 W e chega a ~75–85 °C sob carga. Não existe "trava" de computação.
- Disco: ~45 GB livres (os modelos ocupam o resto).
- Compilador: MSVC BuildTools 2022 (`build.bat`). Sem SDK de OpenCL nem de Vulkan: as duas
  bibliotecas são carregadas em tempo de execução.
- A máquina costuma ter 60–75% de CPU ocupada por outros processos (WSL/Docker, antivírus).

## 3. O motor hoje (`katana.c`, um arquivo, ~5.000 linhas)

**Entrada.** GGUF por arquivo mapeado. Arquiteturas `llama`, `qwen2`, `qwen3`, `olmoe`,
`qwen3moe`. Tensores F32, F16, Q8_0, Q4_0, Q3_K, Q4_K, Q5_K, Q6_K. Tokenizadores BPE (com
pré-tokenização aproximada) e SentencePiece. Formatos de conversa ChatML, Llama 3, Zephyr,
`[INST]` e texto simples, escolhidos pelo template do modelo.

**GPU (OpenCL carregado em runtime).** Pesos quantizados ficam na VRAM num layout próprio: linhas
entrelaçadas de 32 em 32 em palavras de 32 bits, para que um grupo de 32 threads leia palavras
vizinhas. Conjuntos de kernels por tamanho de lote (16, 4, 1). Kernels fundidos na geração: q, k e
v num lançamento; par gate/up com SwiGLU; atenção inteira de um token (bias, norma, RoPE, cache
KV, softmax e mistura) num lançamento. Se o modelo não cabe, as primeiras camadas ficam na GPU e
o resto roda na CPU. Margem de VRAM de 92%.

**CPU.** Produtos inteiros com ativações de 16 bits direto sobre os blocos Q4_K/Q5_K/Q6_K/Q3_K.
Pool de threads próprio (12 threads, blocos por contador atômico). Caminho de ponto flutuante
exato como referência para o `selftest`.

**Mistura de especialistas (MoE).** Atenção de todas as camadas fixa na VRAM; o resto da VRAM é
um cache de especialistas com vagas por camada. Cada especialista tem um "calor" (uso recente com
decaimento). Os que estão na VRAM rodam juntos na GPU, um lançamento por camada, inclusive em
lote. Os que estão fora rodam na CPU na hora (o token nunca espera disco) e uma thread carregadora
os traz se estiverem quentes. `pack` grava os especialistas já no layout da GPU. O calor é gravado
entre sessões e os mais quentes são pré-carregados.

**Uso.** `run`, `chat` (uma pergunta ou conversa interativa de vários turnos), `agent` (conversa
com ferramentas: ler e listar na pasta atual; gravar arquivo e executar comando só com
confirmação), `bench`, `selftest`, `micro`, `tune`, `pack`/`verify`, `sparsity`, `routing`.
`compare.sh` alterna o katana e o motor de referência.

## 4. O que aprendemos sobre a ideia central (medido)

**Modelo denso não se deixa cortar por assunto** (`katana sparsity`, Qwen 1,5B). Mantendo só os
50% de neurônios do feed-forward mais ativos num texto do mesmo assunto, 45% das previsões mudam.
Mesmo escolhendo os melhores neurônios a cada token, 6% mudam. A ativação é espalhada: os 10% mais
ativos respondem por 19% dela.

**Modelo MoE se deixa** (`katana routing`). Com 25% dos especialistas na VRAM:

| | OLMoE-1B-7B (64 por camada) | Qwen3-30B-A3B (128 por camada) |
|---|---|---|
| conjunto calibrado no mesmo assunto | 63% dos usos | 79% |
| teto (melhor conjunto possível) | 64% | 83% |
| calibrado em outro assunto (OLMoE) / acaso | 34% / 25% | — / 25% |

No Qwen3-30B, 15% dos especialistas cobrem até 65% dos usos e 35% cobrem 93%: a curva é íngreme,
cada ponto percentual de VRAM a mais conta. Distribuir as vagas de forma desigual entre camadas
não ajuda (teto 64,4% com vagas iguais contra 65,1%).

## 5. Fatos medidos sobre desempenho

**Custos básicos nesta máquina**

| O quê | Valor |
|---|---|
| Subir um especialista (2,8–3,6 MB) para a VRAM | ~0,75 ms |
| Ler um especialista do disco (em cache do sistema) | ~1,75 ms |
| Converter um especialista do GGUF para o layout da GPU | ~4,6 ms |
| Rodar um especialista na CPU | 0,15–0,5 ms |
| Primeira leitura de uma página de arquivo mapeado (Windows) | ~2,5 µs por página de 4 KB |
| Um `clEnqueueNDRangeKernel` (host) | 22–25 µs |
| Um `clSetKernelArg` | ~0,08 µs |
| Um despacho Vulkan gravado e resubmetido | ~2 µs |
| CPU, produto com pesos frios | 27–33 G pesos/s (limite de banda de memória) |

Consequências: o token não deve esperar carga de especialista (rodar na CPU é mais barato); a
partida a frio de um MoE é dominada por páginas tocadas pela primeira vez; acima de 92% da VRAM a
placa usa memória compartilhada e a geração cai de 13,5 para 7,9 tok/s.

**Onde está o tempo**

- Geração, modelos de 1 B ou mais: o limite é a conta na GPU (o Qwen 1,5B lê ~0,85 GB de pesos por
  token). O host termina de enfileirar em ~5,6 ms e espera. Tirar um lançamento por camada não
  mudou nada; despachar a fila a cada camada também não.
- Geração, modelo minúsculo (135 M): o limite é o custo por lançamento (~7 ms por token quase só
  enfileirando).
- Prompt em lote no OpenCL: lote de 4 e de 16 rendem o mesmo (~0,32 T multiplicações/s). O driver
  OpenCL da NVIDIA não oferece produto inteiro de 8 bits nem meia precisão.
- MoE maior que a VRAM (Qwen3-30B): quase metade do tempo por token são os especialistas na CPU.
  Por camada o tempo é atenção na GPU + o maior entre especialistas na GPU e na CPU, em série
  entre camadas.

**Coisas testadas que não deram ganho**: 8 threads em vez de 12 (20% pior), fixar threads em
núcleos, desligar a limitação de energia do Windows, matemática rápida no OpenCL, espera ativa na
leitura, despacho da fila por camada, atenção fundida (correta, mas sem ganho acima de 1 B),
vagas desiguais por camada, converter só especialistas usados duas vezes (piorou), ativações de 8
bits com uma escala para o vetor inteiro (trocava o token escolhido no modelo de 7B).

## 6. Como estamos em relação ao motor de referência (medido, mesma máquina)

Motor de referência: binário oficial b12418 com CUDA e com Vulkan.

**Modelos densos, benchmark 64+64 (tok/s)**

| Modelo | | katana (OpenCL) | referência CUDA | referência Vulkan |
|---|---|---|---|---|
| SmolLM2 135M | geração | 127 | 402 | 224 |
| | prompt | 1.222 | 6.708 | 8.638 |
| Qwen2.5 1,5B | geração | 96 | 117 | 90 |
| | prompt | 212 | 2.776 | 2.343 |
| Llama 3.2 3B | geração | 57 | 69 | 63 |
| | prompt | 108 | 1.685 | 1.375 |

**Qwen3-30B-A3B (18,6 GB em 4 GB de VRAM), texto real**

| | katana | referência com cache de especialistas | referência, especialistas só na CPU |
|---|---|---|---|
| geração | 12,5–14,9 | 13,2–14,4 | 6,7–8,6 |
| prompt curto a frio | 9–11 | ~5,5 | ~5,5 |

Leitura honesta:

- **Estamos iguais ou perto** na geração: empate no MoE grande, ~20% atrás do CUDA nos densos de
  1 B ou mais, iguais ao Vulkan.
- **Estamos na frente** só no prompt curto a frio do MoE grande (~1,8x) e na partida (o token não
  espera disco, pré-carga pelo calor).
- **Estamos muito atrás** no prompt: 11 a 16 vezes nos densos. A causa é o acesso aos núcleos
  tensoriais, que CUDA e Vulkan dão e o OpenCL da NVIDIA não.
- **Estamos atrás** em modelo minúsculo (3x do CUDA) pelo custo por lançamento.
- O motor de referência já tem cache de especialistas na GPU com a mesma ideia do nosso. A
  primeira comparação que fizemos (1,6x a nosso favor) era contra a configuração deles sem cache.

## 7. Como medir sem se enganar

- A máquina deriva: a mesma medida variou de 15,5 para 11,5 tok/s ao longo de horas, nos dois
  motores. Só vale comparação alternada (A, B, A, B) na mesma hora: `compare.sh` ou dois binários
  lado a lado (`ref/prev`).
- `bench` usa tokens sintéticos e várias rodadas; `chat` com texto real inclui partida a frio. Os
  dois números diferem bastante e devem ser citados separados.
- Diferenças abaixo de ~10% não são conclusivas com uma rodada.
- Antes de afirmar onde está um gargalo, medir as partes (`KATANA_DEBUG=1`, `KATANA_TIME=1`). A
  hipótese "o piso é o custo por lançamento" parecia certa e estava errada para modelos de 1 B ou mais.
- Conferir a configuração do concorrente: a melhor deles pode usar um recurso que não conhecemos.
- `selftest` compara a GPU com a nossa CPU exata; um erro comum aos dois caminhos passa. A
  conferência externa feita até agora: a saída gulosa do Qwen3-30B bate com a do motor de
  referência por ~60 tokens (um teste).

## 8. Pontos fortes

- Código pequeno, um arquivo, sem dependências; dá para experimentar rápido.
- Layout próprio na VRAM e kernels próprios, com pesos sempre quantizados.
- Cache de especialistas por calor com CPU para os frios, carga em segundo plano, formato em disco
  já no layout da GPU e memória de assunto entre sessões.
- Ferramentas de medição próprias: `sparsity`, `routing`, `micro`, `compare.sh`.
- Conversa de vários turnos e agente com ferramentas e confirmação.

## 9. Pontos fracos

- Prompt em lote 11–16x mais lento que a referência (falta o backend com núcleos tensoriais).
- Poucas arquiteturas; falta a família DeepSeek/Kimi (atenção de posto baixo), Gemma, Phi.
- Pré-tokenizador BPE aproximado; pode dividir texto diferente do tokenizador oficial.
- Contexto fixo de 4.096 tokens, cache KV alocado inteiro, sem reuso de prefixo nem deslocamento.
- Amostragem só com temperatura e top-k.
- Sem quantizações abaixo de 3 bits (Q2_K, família IQ).
- CPU só com AVX2 (a máquina tem AVX-512 VNNI); ativações de 16 bits.
- Testado numa máquina só; o caminho Linux compila em teoria e nunca foi rodado.
- Sem testes automatizados além do `selftest` manual por modelo.
- Rede entre máquinas e roteador entre modelos não começados.

## 10. Trabalho em andamento: backend Vulkan

Motivo: é o único jeito, nesta placa, de usar os núcleos tensoriais no lote sem depender de CUDA.

Medido com os protótipos em `tools/`:

| Versão do produto em lote | T multiplicações/s |
|---|---|
| nosso lote OpenCL | ~0,32 |
| Vulkan, um ladrilho por subgrupo, cargas da memória global | 0,51–0,78 |
| + ladrilho 64x64 na memória compartilhada, 4 acumuladores por subgrupo | 1,17–1,36 |
| + cargas de 4 valores, passo de 64 colunas | 1,88 (lote 64), 2,4–2,6 (lote 256) |
| referência Vulkan (estimado pelo prompt) | ~3,5 |

Com os pesos reais, quantizados no layout do katana e decodificados ao encher o ladrilho
(`tools/vkq4k`, todas as saídas conferidas com a CPU, placa livre):

| Pesos | 8960x1536, lote 64 | 8960x1536, lote 256 | 1536x8960, lote 256 |
|---|---|---|---|
| Q4_K | 1,96 T/s | 2,65–2,69 T/s | 2,34 (2,79 dividindo as colunas em 4) |
| Q6_K | 2,17 T/s | 3,34 T/s | 3,84 dividindo as colunas em 4 |

Decodificar não custa vazão: lê-se menos memória. Lote pequeno ainda desperdiça (lote de 16 é
completado até 64: ~0,25 T/s úteis); falta um ladrilho estreito. Acumulador em meia precisão não
acelera e erra 2–6%: descartado.

Produto por token em Vulkan (`tools/vkmv`, 106 verificações sem falha; vazão relatada com a placa
dividida, não refeita): Q4_K 136–231 G pesos/s e Q6_K 150–202 conforme o formato, contra ~93 e
~180 do perfil do OpenCL (métodos diferentes; indica o mesmo nível). Organização alternativa com
uma linha por grupo de 32 threads é 2,5x mais rápida em matrizes de poucas linhas (69 contra 27).

Lote generalizado (`tools/vkq4k/mm.comp`, harness `vkmm`; 1.400 + 1.650 casos conferidos, incluindo
tamanhos quebrados): tipos Q4_K, Q5_K, Q6_K, Q8_0 e F32; modos gravar, somar e par gate/up com
SwiGLU; ladrilhos estreitos para lotes pequenos. Refeito com a placa livre em 8960x1536: Q4_K
1,40 T/s com lote de 16 (era 0,57 completando até 64), 2,50 com 64 e 2,83 com 256; Q6_K 2,10,
3,27 e 3,23. Regra: até 16 tokens ladrilho 64x16, até 32 64x32, acima 64x64; dividir as colunas só
com poucas linhas e muitas colunas. Descartados por medição: tabela de escalas do Q4_K na memória
compartilhada (mais lenta, pela barreira extra), ladrilhos de 2 subgrupos, ler fp32 direto
(2–6% mais lento que converter uma vez quando vários produtos usam a mesma entrada).

Kernels que não são produto (`tools/vkatt`, 1.720 casos conferidos): normas, RoPE com cache KV,
atenção de um token, atenção em lote, soma dos especialistas. O seno e cosseno do GLSL perdem
precisão em ângulos grandes (erro ~2e-4 na posição 600): é preciso reduzir o ângulo antes. Norma
RMS com soma por subgrupo: de 35 para 3 µs (lote de 16: de ~1.000 para 6–25 µs). Ponto fraco: a
atenção em lote em três kernels custa 1,2–2,6 ms por lote de 16 na posição 600, mais que os
produtos; precisa virar um lançamento só.

Integração (etapa 3): `KATANA_BACKEND=vulkan` sobe os pesos e faz o produto por token pelo Vulkan
(`selftest` de produtos passa em 5 modelos densos); o resto do grafo ainda roda na CPU.

Camada de runtime (`vk/kvk.h`, 9 testes passando na NVIDIA e na Intel): despacho esperando a cada
um custa ~156 µs; gravando a fila, ~4,7 µs; grafo gravado e resubmetido, 1,5–2,4 µs.

A placa aceita matrizes cooperativas fp16 x fp16 (16x16x16, resultado fp16 ou fp32) e
int8 x int8 -> int32 (16x16x32), além do produto inteiro empacotado. Ladrilho de 128 com 16
acumuladores por subgrupo é pior (0,4–0,7 T/s).

Decisão de projeto: Vulkan e OpenCL não compartilham VRAM, então o backend Vulkan precisa ser
completo (pesos, produto por token, lote, atenção, especialistas). O OpenCL continua para placas
sem esses recursos.

Peças prontas e conferidas (pastas separadas; falta integrar em `katana.c`):

- `vk/kvk.h`: camada de runtime (carregar a biblioteca, dispositivo, buffers, pipelines, fila de
  despachos, grafo gravado).
- `tools/vkq4k`: produto em lote com Q4_K/Q6_K no layout do katana.
- `tools/vkmv`: produto por token (porte dos kernels `mv_*`).

Para compilar shaders: `ref/vk/glslang/bin/glslang.exe` (liberado em `.claude/settings.local.json`).
Cabeçalhos Vulkan em `ref/vk/include` (fora do git; decidir se entram no repositório).

## 11. Onde ficam as coisas

| Caminho | O quê |
|---|---|
| `katana.c`, `build.bat` | o motor e como compilar |
| `compare.sh` | comparação alternada com o motor de referência |
| `docs/` | este documento, o plano e o roteiro de técnicas |
| `tools/vkprobe`, `tools/vkgemm` | protótipos Vulkan |
| `modelos/` (fora do git) | GGUFs e pastas `.ktn` com shards |
| `ref/` (fora do git) | binários e código do motor de referência, cabeçalhos Vulkan, compilador de shaders, cópias antigas do katana para comparação |

Modelos em disco: SmolLM2 135M, TinyLlama (vários formatos), Qwen2.5 1,5B/7B/14B, Llama 3.2 3B,
Mistral 7B, OLMoE-1B-7B, Qwen3-30B-A3B.

## 12. Dicas de trabalho no código

- `katana.c` é grande: editar com scripts Python de substituição com `assert` de contagem, gravados
  em arquivo. Comandos embutidos no shell estragam `\n` e aspas.
- Compilar: `cmd //c "cd /d C:\source\katana && .\build.bat"` (o aviso sobre `vswhere.exe` é inofensivo).
- Depois de qualquer mudança em kernel: `selftest` em um modelo denso, no OLMoE e no Qwen3-30B
  (este com `KATANA_KV16=0 KATANA_CTX=512` para caber e comparar em 32 bits).
- Downloads grandes: em faixas paralelas com retomada e conferência de tamanho antes de juntar.
- Não rodar medições com download, empacotamento ou outro benchmark em paralelo.
