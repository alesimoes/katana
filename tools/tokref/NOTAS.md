# Teste dourado de tokenizacao e de template de conversa

Referencia: biblioteca `tokenizers` (wheel publico do PyPI, so descompactado em `vendor/`, nada instalado), `jinja2` +
`markupsafe` para renderizar o `chat_template` do proprio modelo, e os `tokenizer.json` / `tokenizer_config.json` publicos
dos repositorios abertos. Nenhum repositorio pediu login ou licenca.

Como rodar (da raiz): `sh tools/tokref/dl.sh` (uma vez), `sh build.sh`, `sh tools/tokref/run.sh [modelo ...]`.
Pecas: `ref.py` (corpus + ids de referencia + texto do template), `katana toktest <gguf> <casos.hex> [--chat]`
(so le metadados e vocabulario, nao carrega pesos), `compare.py` (percentual e primeira divergencia de cada caso),
`gen_unitab.py` e `gen_nfc.py` (geram `unitab.h` e `nfc.h`, tabelas Unicode usadas pelo motor).

Corpus: 1898 casos = 173 escritos a mao (portugues com acentos, ingles, C/Python/JSON/HTML/SQL, numeros longos, emojis,
CJK/kana/hangul/arabe/hebraico/tailandes/hindi, espacos e quebras `\n` `\r\n` multiplos, NBSP/U+3000, acentos
decompostos, tokens especiais de todas as familias, contracoes) + 225 trechos reais (docs do repositorio, katana.c) +
1500 casos aleatorios de fragmentos (letras, digitos, marcas combinantes, jamo, contracoes, especiais). Conversa: 8
conversas (sistema opcional, usuario, assistente, usuario 2) x 2 variantes (1 turno e 2 turnos, ambos com
`add_generation_prompt`) = 16 casos por modelo, comparados em texto e em ids.

## Resultado (casos identicos)

| modelo | tokenizacao antes | tokenizacao depois | conversa texto antes | conversa depois | ids da conversa antes -> depois |
|---|---|---|---|---|---|
| qwen2.5-1.5b | 1030/1898 (54,3%) | 1898/1898 (100%) | 10/16 | 16/16 | 6 -> 16 |
| Qwen3-30B-A3B-Instruct-2507 | 1105/1898 (58,2%) | 1898/1898 (100%) | 16/16 | 16/16 | 13 -> 16 |
| Qwen3.6-35B-A3B | 1137/1898 (59,9%) | 1898/1898 (100%) | 14/16 (*) | 16/16 (*) | 12 -> 16 |
| DeepSeek-V2-Lite-Chat | 1721/1898 (90,7%) | 1898/1898 (100%) | 0/16 | 16/16 | 8 -> 16 |
| Moonlight-16B-A3B | 1156/1898 (60,9%) | 1898/1898 (100%) | 16/16 | 16/16 | 10 -> 16 |
| OLMoE-1B-7B (0924) | 1075/1898 (56,6%) | 1898/1898 (100%) | 0/16 | 16/16 | 0 -> 16 |
| Llama-3.2-3B (autoteste, ver abaixo) | 826/1898 (43,5%) | 1898/1898 (100%) | 0/16 | 16/16 | 0 -> 16 |

"Antes" = o katana.c do commit d16b634 com so o comando `toktest` acrescentado (mesmo corpus, mesmos scripts).
(*) Qwen3.6, 2 turnos: ver "O que sobrou".

## O que foi corrigido

Tokenizador (`tokenizer.ggml.pre` agora escolhe o pre-tokenizador; antes era uma aproximacao unica do regex GPT-2):
- Pre-tokenizadores exatos, com propriedades Unicode de verdade (`\p{L}` `\p{N}` `\p{M}` `\p{Lu}` `\p{Ll}` `\p{Han}`, `\s` =
  White_Space) em vez de "byte >= 0x80 e letra": `qwen2` (Qwen2.5/Qwen3), `qwen35` (acrescenta `\p{M}` as letras),
  `llama-bpe` (digitos em grupos de 3), `olmo` e `gpt-2` (regex GPT-2: digitos em grupo, contracoes so minusculas),
  `kimi-k2` (maiusculas/minusculas separadas, Han a parte, sufixo de contracao, grupos de 3 digitos) e `deepseek-llm`
  (sequencia de divisoes: quebras de linha isoladas, letras/pontuacao com `\s?`, `\s+$`, CJK, um digito por peca; a classe
  de letras vem do tokenizer.json oficial).
- Regras de `\s*[\r\n]+` e `\s+(?!\S)` com o retrocesso certo (o ultimo espaco antes de uma palavra, `\r\n`, quebras de
  linha depois de pontuacao); contracoes sem diferenca de maiuscula nas familias Qwen/Llama/Kimi.
- Normalizacao NFC (normalizer dos vocabularios Qwen e OLMo): texto com acentos decompostos (e + U+0301, jamo hangul) agora
  vira o mesmo caractere composto antes do BPE. Tabelas geradas em `nfc.h`.
- Tokens especiais reconhecidos em qualquer primeiro byte (antes so os que comecam por `<`): ex. os tokens de
  espacos e `|||EMAIL_ADDRESS|||` do OLMoE. Tokens especiais do GGUF continuam sendo os de tipo 3 e 4.
- Qualquer `tokenizer.ggml.pre` desconhecido continua no caminho antigo (aproximado), sem regressao.

Formatos de conversa (`turn_text`, usado por `chat`, console, `serve`; o `chat` de uma pergunta tambem passou a usa-lo):
- Qwen2.5: sem prompt de sistema do usuario, o template poe "You are Qwen, created by Alibaba Cloud. You are a helpful
  assistant." (antes faltava).
- DeepSeek: a resposta anterior e fechada com o token de fim de frase (antes `\n\n`).
- OLMoE: formato proprio (era tratado como Zephyr, errado): BOS no inicio, `<|user|>\n...\n<|assistant|>\n`, resposta
  fechada com eos + `\n`.
- Llama 3.1/3.2: o template sempre abre com o bloco de sistema "Cutting Knowledge Date: December 2023 / Today Date: <data>"
  (data do dia; `KATANA_DATE` fixa a data para teste).
- Modelos cujo template usa `|trim` (Qwen3.6, Llama 3.x): espacos nas pontas das mensagens sao removidos.
- Qwen3.6: antes falhava so no `|trim` das mensagens.

## O que sobrou (e por que)

- Qwen3.6, conversa de 2 turnos: o template oficial descarta o raciocinio (`<think>...</think>`) das respostas antigas,
  mas o cache KV do katana guarda o que o modelo gerou (o prompt termina em `<think>\n`). O `compare.py` compara essa
  variante com o trecho `<think>\n` retirado (e disso vem o "16/16 (*)"). Para bater de verdade seria preciso reprocessar o
  historico sem o raciocinio, o que desfaz a vantagem do cache incremental: deixado como esta (decisao de projeto).
- Byte NUL no texto: o motor trabalha com strings C. A referencia nao tem caso com NUL.
- Referencias que nao sao o `tokenizer.json` oficial: Moonlight so publica `tiktoken.model`; reconstrui o BPE (conversao padrao
  tiktoken -> fusoes, `ignore_merges`) com o regex de `tokenization_moonshot.py`. Llama 3.2 so tem tokenizer.json gated
  (nao usado): o "autoteste" usa o BPE da biblioteca com o vocabulario e as fusoes do proprio GGUF e o regex publicado do
  Llama 3; valida o pre-tokenizador, os especiais e o formato de conversa, nao o vocabulario.
- OLMoE: o GGUF e o 0924; o repositorio 0125 troca os ids de `<|endoftext|>` e `|||IP_ADDRESS|||`. A referencia usa o 0924.
- Unicode: tabelas geradas do `unicodedata` do Python 3.14 (Unicode 16/17); a biblioteca de referencia pode usar versao
  diferente (letras recem-criadas poderiam divergir; nao apareceu em 1898 casos). A lista de Han e escrita a mao.
- Prompt de sistema vazio (`""`) e tratado como ausente (no Qwen2.5 vira o prompt padrao); nao esta nos casos.
- Bytes UTF-8 invalidos: passam sem normalizacao como U+FFFD de 1 byte no pre-tokenizador; sem referencia (a biblioteca
  exige texto valido).
- Familias nao mapeadas (smollm, tinyllama/SentencePiece, deepseek-coder/v3, qwen3 antigo etc.): sem referencia aqui; o
  SentencePiece nao foi tocado.
- A resposta simulada do DeepSeek no teste de 2 turnos comeca por um espaco (o template escreve "Assistant: " + resposta e o
  modelo gera o espaco depois de "Assistant:").
