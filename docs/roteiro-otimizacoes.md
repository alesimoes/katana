# Roteiro de otimizações do katana

Técnicas conhecidas de motores de inferência que o katana ainda não usa (ou usa em parte), com o
que cada uma resolve, o ganho esperado e o esforço. Os ganhos marcados como *estimativa* não foram
medidos aqui; só entram como fato depois de medidos com `compare.sh` ou `katana bench`.

Situação medida (RTX 3050 4 GB, i7-11800H, Qwen3-30B-A3B Q4_K_M): geração empatada com o motor de
referência quando ele usa cache de especialistas na GPU (12,5–14,9 contra 13,2–14,4 tok/s); prompt
curto a frio ~1,8x mais rápido no katana.

## 1. Menos lançamentos e menos esperas por token (GPU)

O piso de ~9–11 ms por token vem do custo fixo por lançamento de kernel (~40 µs), não da conta.

| Técnica | O que faz | Ganho | Esforço |
|---|---|---|---|
| Roteador do MoE na GPU | logits, top-k e pesos num kernel pequeno; o kernel dos especialistas lê os índices de um buffer. Token em que todos os especialistas estão na VRAM não precisa de leitura bloqueante por camada | estimativa: o maior ganho em MoE com cache quase cheio (OLMoE) | médio |
| q, k e v num lançamento para qualquer mistura de formatos | hoje só quando os três são K-quants; a matriz é escolhida pelo índice da grade | 2 lançamentos por camada | médio |
| Norma RMS dentro do consumidor | um kernel calcula 1/rms; os produtos multiplicam pela escala ao ler a ativação | 2 lançamentos por camada | médio |
| Soma residual e bias na gravação do produto | já feito em parte (modo "soma") | pequeno | pequeno |
| RoPE gravando direto no cache KV | já feito | — | — |
| Espera ativa na leitura do token | sondar o evento depois de `clFlush` em vez de leitura bloqueante (o despertar da thread pode custar parte do piso) | a medir | pequeno |
| Kernels com argumentos já presos por camada | um objeto de kernel por camada, sem `SetKernelArg` por token | a medir | pequeno |
| Opções de compilação de matemática rápida no OpenCL | `-cl-mad-enable -cl-fast-relaxed-math` | pequeno | pequeno |
| Kernels especializados por constantes (`-D` de colunas, palavras por linha) | laços com limites conhecidos na compilação | estimativa 5–15% do tempo de kernel | pequeno |

Medido em 2026-11-15 (RTX 3050, driver NVIDIA OpenCL):

- cada `clEnqueueNDRangeKernel` custa 22–25 µs no host; `clSetKernelArg` custa ~0,08 µs. Num modelo denso de 28 camadas são ~226 lançamentos por token, ~5,6 ms dos 10,8 ms. É esse o piso.
- espera ativa no lugar da leitura bloqueante: sem efeito. Kernels com argumentos presos também não ajudariam (o custo não está nos argumentos).
- `-cl-fast-relaxed-math`: sem efeito (fica opcional, `KATANA_FASTMATH=1`).
- só restam dois caminhos: fundir kernels (cada lançamento a menos por camada vale ~0,7 ms por token em 28 camadas) ou trocar para uma API com buffer de comandos gravado uma vez e submetido por token (Vulkan), que elimina o custo por lançamento. O segundo é uma reescrita do backend.

Correção depois de fundir a atenção inteira num lançamento (um a menos por camada, conferido em cinco modelos): a geração não mudou em nenhum modelo de 1 B ou mais (Qwen 1,5B 10,9 ms, Llama 3B 18,7 ms, OLMoE ~10 ms por token, com e sem a fusão). Conclusão: nesses modelos o host termina de enfileirar em ~5,6 ms e fica esperando a GPU; o tempo por token é o trabalho da GPU nos produtos (o Qwen 1,5B lê ~0,85 GB de pesos por token, ~78 GB/s), não o custo por lançamento. Menos lançamentos (ou um buffer de comandos gravado) só rende em modelos minúsculos, como o de 135 M. Para os modelos que interessam, o que conta é a vazão dos kernels de produto e a parte que roda na CPU.

Protótipo de buffer de comandos (`tools/vkprobe`, Vulkan, mesma placa): 226 despachos de computação com barreira entre eles, gravados uma vez e submetidos por token, levam 0,44 ms no total (2 µs por despacho); regravando a cada token, ~1 ms. No OpenCL só o enfileiramento desses 226 custa ~5,6 ms. Despachar a fila do OpenCL a cada camada (`KATANA_FLUSH`) não muda nada, o que confirma que a GPU já executa em paralelo com o enfileiramento. Portanto um backend Vulkan tiraria ~5 ms de host por token, o que só aparece onde o host é o limite: modelos pequenos (o de 135 M gasta ~7 ms por token quase só enfileirando). Nos modelos de 1 B ou mais o limite é a conta na GPU e o ganho esperado é perto de zero.

## 2. Lotes (prompt)

| Técnica | O que faz | Ganho | Esforço |
|---|---|---|---|
| Decodificar o bloco de pesos uma vez por lote | na GPU e na CPU: desempacotar um ladrilho de pesos para bytes e aplicá-lo a todos os tokens do lote, em vez de decodificar por token | estimativa: várias vezes no prompt | grande |
| Lotes maiores (128–512 tokens) | depende do item acima | junto | grande |
| Última camada só para o último token | o feed-forward da última camada, a norma final e a projeção só interessam ao token que gera logits | pequeno | pequeno |
| Agrupar tokens por especialista no lote MoE | cada especialista processa de uma vez todos os tokens roteados para ele | junto com o primeiro | grande |

## 3. Produtos na CPU

Token a token com pesos frios estamos no limite de banda de memória (~27–33 G pesos/s); o ganho
está no que cabe no cache (especialistas pequenos, lotes).

| Técnica | O que faz | Ganho | Esforço |
|---|---|---|---|
| Ativações em 8 bits com escala por bloco de 256 e somas parciais | o dobro de elementos por instrução; o termo do mínimo (Q4_K) e o deslocamento (Q6_K) saem das somas parciais. Nosso teste antigo de 8 bits usava uma escala para o vetor inteiro e trocava o token; com escala por bloco precisa ser refeito | estimativa 1,5–2x no que cabe no cache | médio |
| 8 linhas em paralelo por passo | uma carga da ativação multiplica 8 linhas; 8 acumuladores independentes | estimativa 10–30% em cache | médio |
| AVX-512 VNNI (`vpdpbusd`) com escolha em tempo de execução | funde três instruções e dobra a largura | estimativa até 2x em cache | médio |
| Threads = núcleos físicos, fixadas, sem limitação de energia do Windows | menos disputa e menos variação | medido: 8 threads ficam ~20% mais lentas que 12 neste motor (com ou sem fixar); desligar a limitação de energia não mudou nada | feito |
| Número de threads diferente para geração e para lote | geração é limitada por memória; lote por conta | a medir | pequeno |
| `exp` vetorizado para SiLU e softmax | polinômio de grau 5 | pequeno | pequeno |

## 4. Cache de especialistas (MoE)

Já temos: vagas por camada, calor com decaimento, especialista frio na CPU, carga em segundo
plano, pré-carga pelo calor da sessão anterior.

| Técnica | O que faz | Esforço |
|---|---|---|
| Classes de frequência no despejo | classe = log2 do uso decaído; uma fila por classe; sai quem tem a maior idade relativa à classe. O uso acumulado sobrevive ao despejo | pequeno |
| Lote grande não despeja | o prompt só ocupa vagas livres, para não expulsar os especialistas da geração | feito |
| Subidas agrupadas | faltas vizinhas ocupam vagas vizinhas e sobem numa cópia só | pequeno |
| Vagas compartilhadas entre camadas de mesmo formato | medimos que vagas iguais por camada já ficam a 1 ponto do teto; baixa prioridade | médio |
| Registro das escolhas para simulação fora do motor | `katana routing` já simula; falta gravar o traço de uma sessão real | pequeno |

## 5. Carga e partida

| Técnica | O que faz | Esforço |
|---|---|---|
| Pedir as páginas ao sistema em um chamado só, com faixas alinhadas e fundidas | cobre só os tensores que ficam na CPU; substitui as threads que tocam página por página | pequeno |
| Ler direto para um buffer fixo o que vai para a GPU | leitura sem cache do sistema, um buffer reaproveitado, tensores maiores primeiro | médio |
| Aquecimento | rodar dois tokens depois da carga e limpar o estado | pequeno |
| Calculadora de memória sem alocar | pesos por dispositivo + KV + rascunho, para escolher o tamanho do cache antes de alocar | pequeno |
| Margem de VRAM | medido: acima de ~92% a placa passa a usar memória compartilhada e a geração cai de 13,5 para 7,9 tok/s | feito |

## 6. Cache KV e contexto

| Técnica | O que faz | Esforço |
|---|---|---|
| Atender só às posições usadas (arredondadas para 256) | independe do tamanho máximo do contexto | pequeno |
| KV sempre em 16 bits | metade da VRAM e da banda | pequeno |
| Reuso de prefixo | guardar os tokens da conversa anterior e manter o prefixo comum | pequeno |
| Deslocamento de contexto | descartar o trecho antigo regirando K pela diferença de posição, sem reprocessar | médio |
| Salvar e restaurar o KV de um prompt de sistema fixo | partida instantânea do agente | pequeno |
| Atenção num kernel só com softmax em linha, lendo K e V uma vez por grupo de cabeças | já temos a atenção fundida para um token; falta o lote | médio |

## 7. Amostragem, tokenizador, formatos

| Técnica | O que faz | Esforço |
|---|---|---|
| Cadeia de amostragem | penalidade de repetição (anel dos últimos N tokens + contagem), top-k, top-p, min-p, temperatura, nessa ordem; min-p sem ordenar | pequeno |
| Top-k por histograma | 128 faixas de logit acham a faixa do k-ésimo; só ela é ordenada | pequeno |
| Gramática preguiçosa | amostra primeiro e confere só o token escolhido; refaz com a máscara se for rejeitado (útil para chamadas de ferramenta em JSON) | médio |
| Pré-tokenizador exato | máquina de estados por tipo declarado no GGUF (`tokenizer.ggml.pre`): agrupamento de dígitos (1 ou 1–3), último espaço antes da palavra, quebras de linha, contrações, classes Unicode de letra e número; separar os tokens especiais antes | médio |
| Formato de conversa por detecção no template | já fazemos por trechos do template; faltam Gemma, Phi, Mistral, DeepSeek, Kimi | pequeno |

## 8. Arquiteturas e formatos novos

| Item | O que exige | Esforço |
|---|---|---|
| Atenção de posto baixo com KV comprimido (família DeepSeek-V3/Kimi) | q em dois estágios, K e V comprimidos e compartilhados entre cabeças, RoPE só numa faixa, descompressão por cabeça, roteador sigmoide com viés, especialista compartilhado, camadas densas iniciais | grande |
| Q2_K e Q3_K nativos | sobre o kernel de Q4_K | pequeno a médio |
| Formatos com tabela (IQ) | não escrever produto direto: desempacotar para bytes num ladrilho e usar o kernel genérico de 8 bits (mesma base dos lotes) | grande |

## Ordem sugerida

1. Experimentos baratos: threads, energia, espera ativa, matemática rápida, pedido de páginas em bloco, aquecimento.
2. Política do cache: classes de frequência, lote não despeja, subidas agrupadas.
3. Roteador na GPU.
4. Ativações de 8 bits por bloco e AVX-512 VNNI na CPU.
5. Ladrilho decodificado uma vez por lote (CPU e GPU).
6. KV: posições usadas, reuso de prefixo, deslocamento.
7. Amostragem e pré-tokenizador exato.
8. Atenção de posto baixo (DeepSeek/Kimi).
