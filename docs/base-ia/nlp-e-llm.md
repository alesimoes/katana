# Base de conhecimento: bibliotecas de NLP e LLM (visão para o katana)

Pesquisa feita em novembro de 2026. Convenções: o que veio de página consultada leva o link da fonte;
o que veio de conhecimento prévio, sem página consultada nesta pesquisa, leva "(verificar)". Datas de
release foram lidas nas páginas de releases do GitHub (que mostram dia e mês, sem ano); tratar como
2026 com cautela. Nada aqui foi medido no katana.

## Tabela-resumo

| Nome | Uso principal | Ativa em 2026? | Relevância para o katana |
|---|---|---|---|
| Transformers (Hugging Face) | Definição de arquiteturas, tokenizadores, chat templates, geração | Ativa (v5.23.0 em 13/nov) | Altíssima: é a referência a imitar (tokenização, template, amostragem) |
| PEFT | Ajuste fino eficiente (LoRA, DoRA, IA3...) | Ativa (docs v0.21.0) | Alta: formato do adaptador LoRA para aplicar em tempo de execução |
| Adapters (adapter-hub) | Adaptadores sobre Transformers (bottleneck, LoRA, fusão) | Ativa/baixa atividade (verificar) | Baixa: formato próprio, PEFT cobre o caso útil |
| sentence-transformers | Embeddings, rerank, busca semântica | Ativa (v6.3.0 em 11/nov) | Alta: memória semântica do agente |
| SetFit | Classificação de poucos exemplos sobre embeddings | Ativa/baixa cadência (verificar) | Média-baixa: classificar intenção sem LLM, com embeddings |
| NeMo (NVIDIA) | Fala (ASR/TTS) e LLM multimodal, treino | Ativa (NeMo Speech 3.0.0) | Baixa: foco em fala; checkpoints grandes |
| Flair | Rotulagem de sequências (NER, POS), embeddings | Ativa em ritmo lento (0.15.1) | Média-baixa: NER, mas depende de PyTorch |
| spaCy | Pipeline de NLP industrial (tokenização, NER, regras) | Ativa (v3.8.18 em 26/out) | Média: ideias (regras/EntityRuler); runtime Python não encaixa |
| fastText | Classificação de texto, vetores, detecção de idioma | Abandonada (arquivado 19/mar/2024) | Alta para detecção de idioma: lid.176 é pequeno e portável |
| BERTopic | Modelagem de tópicos sobre embeddings | Ativa | Média-baixa: agrupar memórias por tema |
| AllenNLP | Pesquisa de NLP sobre PyTorch | Abandonada (arquivado 16/dez/2022) | Nenhuma |
| Fairseq | Tradução/modelos de sequência (Meta) | Abandonada (arquivado 20/mar/2026) | Nenhuma (fairseq2 existe; verificar) |
| Stanza | Pipeline linguístico Stanford (60+ idiomas, UD, NER) | Ativa | Baixa-média: referência de NER/lematização multilíngue |
| SpanMarker | NER por spans sobre encoders | Ativa em baixa cadência (verificar) | Baixa: modelos grandes; use como referência de NER |
| PaddleNLP | NLP/LLM no ecossistema Paddle (Baidu) | Incerta, aparente transição (ver seção) | Nenhuma direta |

Resumo do que serve ao katana: (1) Transformers como especificação de comportamento; (2) PEFT como
formato de LoRA; (3) sentence-transformers e modelos de embedding em GGUF para memória semântica;
(4) fastText lid.176 e regras estilo spaCy para detecção de idioma e extração leve de entidades, sem LLM.

---

## 1. Transformers (Hugging Face)

**O que é / quem mantém.** Biblioteca Python da Hugging Face que define arquiteturas de modelos,
tokenizadores, processadores e a API `generate`. Releases recentes: v5.23.0 (13/nov), v5.22.0 (29/out,
Mistral Large 4), v5.21.0, v5.20.0 ([releases](https://github.com/huggingface/transformers/releases)).
Os docs já apontam para a série v5.

**Para que serve.** Carregar e executar modelos, gerar texto, treinar. Para o katana é principalmente a
**referência de comportamento**: se o katana tokeniza e formata a conversa de forma diferente do
Transformers, o modelo degrada.

**Estado em 2026.** Ativa, com releases quase semanais.

**Como as arquiteturas são definidas.**
- Cada modelo tem `configuration_<m>.py` (classe de config) e `modeling_<m>.py` (código autocontido);
  toda config herda de `PreTrainedConfig` e todo modelo de `PreTrainedModel`. O modelo salvo vira
  `model.safetensors` e a config `config.json`
  ([guia de novo modelo](https://huggingface.co/docs/transformers/main/en/add_new_model)).
- O nome dos tensores segue o nome dos atributos do `nn.Module` (mesma fonte). Para modelos
  estilo Llama, os nomes típicos são `model.embed_tokens.weight`, `model.layers.N.self_attn.{q,k,v,o}_proj.weight`,
  `model.layers.N.mlp.{gate,up,down}_proj.weight`, `model.layers.N.input_layernorm.weight`,
  `model.layers.N.post_attention_layernorm.weight`, `model.norm.weight`, `lm_head.weight` (conhecimento
  prévio; verificar contra o `model.safetensors.index.json` de cada modelo). A Hugging Face recomenda o
  "modular transformers" para novos modelos (mesma fonte).
- `config.json` traz `model_type`, `architectures`, `hidden_size`, `num_attention_heads`,
  `num_key_value_heads`, `rope_theta`, `rms_norm_eps`, `tie_word_embeddings` etc. (verificar a lista
  exata por modelo). O katana já lê isso ao carregar; o ponto é tratar `tie_word_embeddings`, RoPE
  scaling e sliding window como fonte de divergência numérica.
- Tolerância de validação adotada pelo projeto: saídas devem bater com 1e-3 contra a implementação
  original (mesma fonte). Boa meta de selftest para o katana: comparar logits da primeira posição.

**Chat templates (Jinja).**
- O template vem em `tokenizer_config.json` (campo `chat_template`) e `apply_chat_template` o renderiza
  a partir de uma lista de `{role, content}`; papéis comuns: `system`, `user`, `assistant`
  ([chat templates](https://huggingface.co/docs/transformers/chat_templating)). Que o campo fica em
  `tokenizer_config.json` é conhecimento prévio (verificar); recentemente alguns repositórios usam
  arquivo `chat_template.jinja` separado (verificar).
- `add_generation_prompt=True` acrescenta o cabeçalho do turno do assistente (ex.: `<|im_start|>assistant\n`);
  alguns modelos, como Llama, não têm nada ali (mesma fonte).
- `continue_final_message` remove o fim de turno da última mensagem para "prefill"; não pode ser usado
  junto com `add_generation_prompt` (mesma fonte). Modelos de raciocínio leem `reasoning_content` em
  campo separado (mesma fonte).
- Cuidado documentado: o template já inclui BOS/EOS; tokenizar o texto renderizado exige
  `add_special_tokens=False` para não duplicar (mesma fonte). Regra para o katana: renderizar o
  template, tokenizar sem especiais extras.
- Exemplos de formato: ChatML (`<|im_start|>role\n...<|im_end|>\n`), Zephyr (`<|user|>\n...</s>\n`),
  Mistral (`[INST] ... [/INST]`) (mesma fonte).
- Implicação: o katana precisa de um interpretador Jinja mínimo (for, if, filtros `trim`, `tojson`,
  `loop.index`, `namespace`, acesso a campos) ou de templates embutidos por família, validados por
  comparação byte a byte com o Transformers. Os templates com chamadas de ferramenta (`tools`) são os
  mais complexos. O GGUF carrega o mesmo template em `tokenizer.chat_template` (verificar).

**Tokenizadores.**
- `tokenizer.json` é o formato da biblioteca `tokenizers`, com os blocos: normalizador, pré-tokenizador,
  modelo (BPE, WordPiece, Unigram, WordLevel), pós-processador, decodificador
  ([componentes](https://huggingface.co/docs/tokenizers/components)).
- Pré-tokenizadores relevantes: **ByteLevel** (mapeia bytes para caracteres visíveis, ex. `Ġ` para
  espaço; alfabeto de 256), **Split** (padrão regex com comportamento `Isolated`, `MergedWithPrevious`
  etc., opção `invert`), **Metaspace** (espaço vira `▁`, U+2581), **Digits**, **Sequence** (mesma fonte).
  Os BPE modernos (Llama 3, Qwen, GPT-4 like) usam `Sequence[Split(regex), ByteLevel]`; o regex é a parte
  que mais causa divergência e usa propriedades Unicode (`\p{L}`, `\p{N}`) e lookahead (`\s+(?!\S)`)
  (conhecimento prévio; verificar o regex por modelo no próprio `tokenizer.json`).
- Normalizadores: NFC/NFD/NFKC/NFKD, Lowercase, Strip, Replace, Prepend, Sequence (mesma fonte).
  SentencePiece (Llama 2, Gemma) usa Metaspace + normalizador `Prepend("▁")`/`Replace` e
  `byte_fallback` (verificar).
- Pós-processador `TemplateProcessing` insere especiais (`[CLS] $A [SEP]`) (mesma fonte). Em chat, o
  template é quem controla BOS.
- `added_tokens` (tokens especiais como `<|im_start|>`) são casados antes do modelo e não passam pelo
  pré-tokenizador; no `tokenizer.json` aparecem em `added_tokens` com `special: true` (verificar).
- Nota de versão: o mesmo pacote `tokenizers` hoje documenta bindings novos que carregam `tokenizer.json`
  e fazem encode/decode (página de componentes acima).

**Geração.**
- Guloso é o padrão; amostragem com `do_sample=True`; beam com `num_beams>1`
  ([estratégias](https://huggingface.co/docs/transformers/generation_strategies)). Parâmetros de amostragem
  típicos: `temperature`, `top_k`, `top_p`, `min_p`, `repetition_penalty` (conhecimento prévio; a página
  consultada não listou; verificar a ordem dos processadores de logits).
- `generation_config.json` do modelo define `eos_token_id` (pode ser lista), temperatura/top_p
  recomendados. O katana deve ler esse arquivo para parar nos tokens corretos (verificar).
- Decodificação especulativa ("assisted generation") e prompt lookup existem; a página consultada
  delegava a outra seção (verificar em docs de "assisted decoding"). O mecanismo de `custom_generate`
  permite métodos de geração em repositórios do Hub, exigindo `trust_remote_code=True`
  (mesma página de estratégias).
- Cache KV: há caches quantizados (FP8 em `QuantizedCache` na v5.21.0, nas
  [releases](https://github.com/huggingface/transformers/releases)).

**Formatos que usa.** safetensors, `config.json`, `tokenizer.json`, `tokenizer_config.json`,
`generation_config.json`; GGUF é lido/escrito pela integração (verificar).

**O que o katana pode aproveitar.**
1. Conjunto de testes dourados: gerar com Transformers (Python) listas de `input_ids` para um corpus
   variado (acentos, emojis, números, código, CJK, tokens especiais) e comparar com a saída do tokenizador
   do katana. Fazer o mesmo para o texto renderizado do chat template, com e sem ferramentas.
2. Seguir a regra "template já inclui BOS/EOS".
3. Ler `generation_config.json` e `eos_token_id` múltiplo.
4. Comparar logits da primeira posição (tolerância ~1e-3) contra o Transformers em bf16/f32.

**Exemplo mínimo.**
```python
from transformers import AutoTokenizer
tok = AutoTokenizer.from_pretrained("Qwen/Qwen3-0.6B")
msgs = [{"role": "system", "content": "Seja breve."}, {"role": "user", "content": "Oi"}]
texto = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True)
ids = tok(texto, add_special_tokens=False).input_ids   # nunca duplicar BOS
print(texto); print(ids)
```

---

## 2. PEFT

**O que é / quem mantém.** Biblioteca da Hugging Face para ajuste fino eficiente; integra com
Transformers, Diffusers e Accelerate; ~21,9 mil estrelas
([repositório](https://github.com/huggingface/peft)). Docs na série v0.21.0
([formato de checkpoint](https://huggingface.co/docs/peft/developer_guides/checkpoint)).

**Para que serve.** Treinar poucos parâmetros extras (LoRA, DoRA, IA3, prompt tuning etc.) e salvar só
o adaptador.

**Estado em 2026.** Ativa.

**Formato do adaptador (fonte: [checkpoint](https://huggingface.co/docs/peft/developer_guides/checkpoint)).**
- Diretório com `adapter_model.safetensors` (ou `.bin`), `adapter_config.json` e `README.md`
  (o README não é necessário para carregar).
- Chaves do state_dict começam com `base_model.model.` seguidas do caminho do módulo do modelo base,
  depois `lora_A.weight` e `lora_B.weight` (o nome do adaptador, `default`, é removido ao salvar).
  Ex.: `base_model.model.encoder.layer.0.attention.self.query.lora_A.weight`. Para embeddings existem
  `lora_embedding_A/B`. DoRA acrescenta `lora_magnitude_vector`.
- `adapter_config.json` (LoRA) tem: `peft_type: "LORA"`, `r`, `lora_alpha`, `lora_dropout`,
  `target_modules`, `bias`, `use_rslora`, `use_dora`, `rank_pattern`, `alpha_pattern`,
  `fan_in_fan_out`, `base_model_name_or_path`, `modules_to_save`. O mínimo exigido é `target_modules` e
  `peft_type`.
- Fórmula de aplicação (conhecimento prévio do código `peft/tuners/lora/layer.py`; verificar):
  `W' = W + s * (B @ A)`, com `A` de formato `[r, in]`, `B` de formato `[out, r]`,
  `s = lora_alpha / r` (ou `lora_alpha / sqrt(r)` com `use_rslora`). Saída em tempo de execução:
  `y = W x + s * B (A x)`.
- Mesclar (`merge_and_unload`) gera um modelo único mais rápido, mas perde a troca de adaptador e não
  funciona com todo tipo de quantização (mesma fonte).

**Como aplicar um LoRA num motor de inferência (proposta para o katana).**
1. Ler `adapter_config.json` (`r`, `lora_alpha`, `use_rslora`, `target_modules`, `fan_in_fan_out`;
   recusar `use_dora` ou implementar a magnitude depois).
2. Casar cada par `lora_A/lora_B` com o tensor base pelo nome (tirar `base_model.model.` e
   `.lora_A.weight`). Atenção: o nome do modelo base no Transformers pode diferir do nome nos pesos
   GGUF do katana; precisa de um mapa de nomes.
3. Duas estratégias: (a) **mesclar na carga** em f32 e requantizar (simples, sem custo por token, mas
   perde precisão na requantização e exige memória de trabalho); (b) **caminho lateral por camada**:
   `y += s * B(Ax)` em f16/f32, custo extra de duas matmuls pequenas de posto `r` por projeção
   alvo, permite trocar adaptador sem recarregar. Para um motor em C com pesos quantizados, (b) evita
   requantizar (é o desenho do llama.cpp, que converte o adaptador para GGUF com script próprio;
   verificar o nome e formato do script).
4. Validar contra Transformers+PEFT comparando logits.

**Exemplo mínimo.**
```python
from peft import LoraConfig, get_peft_model
from transformers import AutoModelForCausalLM
m = AutoModelForCausalLM.from_pretrained("Qwen/Qwen3-0.6B")
m = get_peft_model(m, LoraConfig(r=8, lora_alpha=16, target_modules=["q_proj","v_proj"]))
m.save_pretrained("meu-lora")   # grava adapter_config.json + adapter_model.safetensors
```

---

## 3. Adapters (adapter-hub)

**O que é.** Biblioteca complementar ao Transformers com 10+ métodos e 20+ modelos; substitui o pacote
`adapter-transformers` ([repositório](https://github.com/adapter-hub/adapters)). Métodos listados:
bottleneck, AdapterFusion, MAD-X, prefix tuning, Compacter, LoRA, MTL-LoRA, IA3, VeRA, DoRA, UniPELT,
prompt tuning, QLoRA, ReFT, aritmética de adaptadores.

**Estado.** A página consultada não indica manutenção reduzida nem mostra releases; o repositório tem
189 commits em `main` (bem menos que PEFT). Considerar "ativa, pequena" (verificar cadência).

**Formato.** Próprio do projeto, com configs de adaptador por método (verificar); LoRA do Adapters não
é intercambiável por padrão com o do PEFT (verificar).

**Aproveitável.** Pouco. Ideia útil: fusão e aritmética de adaptadores (combinar vários LoRA com pesos),
que no katana seria somar caminhos laterais ponderados.

**Exemplo mínimo.**
```python
import adapters
from transformers import AutoModelForSequenceClassification
m = AutoModelForSequenceClassification.from_pretrained("bert-base-uncased")
adapters.init(m); m.add_adapter("meu", config="lora"); m.train_adapter("meu")
```

---

## 4. sentence-transformers

**O que é / quem mantém.** Biblioteca de embeddings e rerankers, mantida por Tom Aarsen (Hugging Face).
Releases: v6.3.0 (11/nov), v6.2.0 (21/out), v6.0.0 (18/ago) ([releases](https://github.com/huggingface/sentence-transformers/releases)).
A v6 exige Transformers v5 e traz quatro tipos de modelo: `SentenceTransformer`, `CrossEncoder`,
`SparseEncoder` e `MultiVectorEncoder` (estilo ColBERT) (mesma fonte).

**Estado.** Ativa.

**Para que serve.** Busca semântica, similaridade, agrupamento, deduplicação, reranking.

**Como calcular (pooling e normalização).**
- Um modelo é uma sequência de módulos: Transformer, Pooling, opcionalmente Normalize. Se termina em
  `Normalize`, use produto escalar, que equivale ao cosseno sem renormalizar; similaridade padrão é cosseno
  ([uso](https://sbert.net/docs/sentence_transformer/usage/semantic_textual_similarity.html)).
- Pooling por média (usado por EmbeddingGemma, segundo o [artigo](https://arxiv.org/html/2509.20354v3)),
  CLS (BERT clássico; verificar por modelo) ou último token (Qwen3-Embedding, ver abaixo). Média: somar os
  estados ocultos finais mascarando o padding e dividir pelo número de tokens reais; depois L2.
- Prefixos de tarefa: consultas e documentos usam prompts diferentes (`encode_query`/`encode_document`
  na v6; mesma página de releases).
- Matryoshka: truncar o vetor e renormalizar (EmbeddingGemma: 768, 512, 256, 128).

**Modelos pequenos de embedding em GGUF para a memória do agente.**

| Modelo | Parâmetros / dim / contexto | Pooling e prefixos | Fonte |
|---|---|---|---|
| EmbeddingGemma 300M | ~0,3B; 768 (Matryoshka 512/256/128); 2048 tokens; 100+ idiomas; licença Gemma | média; consulta `task: search result \| query: {texto}`; documento `title: {título ou none} \| text: {texto}` | [HF](https://huggingface.co/google/embeddinggemma-300m), [GGUF ggml-org](https://huggingface.co/ggml-org/embeddinggemma-300M-GGUF) |
| Qwen3-Embedding 0.6B | dim máxima 1024 (configurável 32 a 1024); contexto 32k; Q8_0 639 MB, F16 1,2 GB | **último token** (`--pooling last`); instrução na consulta, sem ela perde 1 a 5% | [GGUF](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF) |
| nomic-embed-text v1.5 | quantizações de 48 MiB (Q2_K) a 262 MiB (F16); contexto 2048 (8192 com YaRN); Apache-2.0 | prefixos `search_query:` e `search_document:` (obrigatórios) | [GGUF](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF) |
| all-MiniLM-L6-v2 | 22M, dim 384, contexto curto (verificar) | média + normalizar (verificar) | [sbert.net](https://sbert.net/docs/sentence_transformer/usage/semantic_textual_similarity.html) usa-o no exemplo; versão GGUF (verificar) |

Observações: nomic e Qwen3 têm GGUF oficial; para MiniLM e BGE há conversões da comunidade (verificar).
Para a memória do katana, nomic v1.5 (Apache-2.0, ~137M, ainda pequeno em Q8) e EmbeddingGemma
(multilíngue, inclui português; licença Gemma, que tem termos de uso próprios; o requisito do projeto de
"nada proprietário" deve ser avaliado antes) são os candidatos práticos; MiniLM é a opção mais leve, em
inglês principalmente (verificar suporte a português; para português prefira modelos multilíngues).
O llama.cpp normaliza por padrão (`embd_normalize=2`, L2)
([GGUF EmbeddingGemma](https://huggingface.co/ggml-org/embeddinggemma-300M-GGUF)); o katana deve fazer
o mesmo no fim.

**O que o katana pode aproveitar.**
1. Um modo de embedding no motor: rodar um modelo encoder (BERT/nomic) ou decoder pequeno (Qwen3-Embedding)
   até a última camada, aplicar pooling (média ou último token) e L2.
2. Memória do agente: guardar vetor por memória (float16 ou int8) num arquivo; busca por produto escalar
   (força bruta basta para milhares de itens); combinar com a busca por palavras (BM25/FTS) por fusão
   de ranking (RRF) para robustez (técnica comum; verificar escolha).
3. Truncagem Matryoshka (256 dims) para reduzir espaço com pouca perda.
4. Validar contra sentence-transformers: cosseno entre vetores do katana e de referência deve ser ~0,999.

**Exemplo mínimo.**
```python
from sentence_transformers import SentenceTransformer
m = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
e = m.encode(["lembrar que o usuario prefere C", "o clima esta bom"], normalize_embeddings=True)
print(m.similarity(e, e))
```

---

## 5. SetFit

**O que é.** Treino de classificadores de texto com poucos exemplos: ajusta um modelo de
sentence-transformers por aprendizado contrastivo e treina uma cabeça (logística) sobre os embeddings
([repositório](https://github.com/huggingface/setfit)). Mantido sob a organização Hugging Face
(historicamente com Tom Aarsen; verificar).

**Estado.** Repositório público com 2,9 mil estrelas e 148 issues abertas; sem aviso de arquivamento no
README; a página não mostra releases (verificar cadência real em PyPI).

**Modelos e formatos.** Corpo sentence-transformers + cabeça scikit-learn (`model_head.pkl`, pickle;
verificar).

**Aproveitável.** A ideia, não a biblioteca: classificar intenção do usuário (ex.: "pedido de busca",
"salvar memória") usando os embeddings que o katana já terá, com a cabeça como regressão logística
simples treinada offline e carregada como matriz de pesos (sem pickle).

**Exemplo mínimo** (condensado do README, via [repositório](https://github.com/huggingface/setfit)):
```python
from setfit import SetFitModel, Trainer, sample_dataset
model = SetFitModel.from_pretrained("sentence-transformers/paraphrase-mpnet-base-v2",
                                    labels=["negativo", "positivo"])
Trainer(model=model, train_dataset=treino).train()
print(model.predict(["filme delicioso", "filme chato"]))
```

---

## 6. NeMo (NVIDIA)

**O que é.** Framework da NVIDIA. O repositório foi reorientado para fala e LLM multimodal: o README
atual descreve ASR, TTS e Speech LLMs; versão **NeMo Speech 3.0.0**; selo "Project Status: Active"; a
última versão com o escopo antigo é a v2.7.3 ([repositório](https://github.com/NVIDIA-NeMo/NeMo)).

**Estado.** Ativa, com foco em fala.

**Formatos.** Checkpoints em Hugging Face; o formato `.nemo` (arquivo tar com config YAML e pesos;
verificar) não foi confirmado na página consultada. A página avisa que alguns checkpoints exigem
`torch.load(weights_only=False)` (pickle, risco de segurança).

**Aproveitável.** Quase nada para o motor de texto. Se um dia o katana tiver entrada por voz: modelos
de ASR (Parakeet/Canary; verificar) e o formato de seus pesos. Não adotar agora.

**Exemplo mínimo** (ASR; verificar na versão 3.x):
```python
import nemo.collections.asr as nemo_asr
asr = nemo_asr.models.ASRModel.from_pretrained("nvidia/parakeet-tdt-0.6b-v2")
print(asr.transcribe(["audio.wav"]))
```

---

## 7. Flair

**O que é.** Biblioteca de NLP em PyTorch (Humboldt-Universität zu Berlin, grupo flairNLP; verificar).
Versão 0.15.1, ~14,4 mil estrelas, licença MIT, Python 3.9+ ([repositório](https://github.com/flairNLP/flair)).

**Para que serve.** NER, POS, classificação, embeddings contextuais, treino de taggers.

**Estado.** Mantida, ritmo lento (a página não mostra data de release; verificar).

**Modelos.** NER pré-treinado em inglês, alemão, holandês e espanhol; inglês CoNLL-03 (F1 94,09) e
OntoNotes de 18 classes (F1 90,93); modelos no Hub (mesma fonte). Português: não listado (verificar).

**Aproveitável.** Pouco: depende de PyTorch e de modelos grandes. Serve como oráculo offline para gerar
dados de avaliação de NER.

**Exemplo mínimo.**
```python
from flair.data import Sentence
from flair.nn import Classifier
s = Sentence("I love Berlin .")
Classifier.load("ner").predict(s)
print(s)
```

---

## 8. spaCy

**O que é / quem mantém.** Biblioteca industrial de NLP da Explosion. Última release: v3.8.18 (26/out);
suporta Python 3.14; ajustes de empacotamento nas versões recentes
([releases](https://github.com/explosion/spaCy/releases)).

**Para que serve.** Tokenização baseada em regras, POS, lematização, dependências, NER, `Matcher`,
`PhraseMatcher`, `EntityRuler` (regras de entidade por padrão), pipelines treináveis, integração com
transformers e LLM (verificar; a página consultada não tratou disso).

**Estado.** Ativa, em manutenção (releases de versão 3.8.x, sem salto de major).

**Modelos e formatos.** Pacotes `pt_core_news_sm/md/lg` (português, estatísticos, CPU), `xx_ent_wiki_sm`
(multilíngue) e variantes `_trf` com transformers (verificar nomes e tamanhos atuais). Pacotes são
diretórios com `config.cfg` e pesos do Thinc.

**O que dá para fazer no agente sem LLM, inspirado em spaCy.**
- Extração de entidades por regras: datas, e-mails, URLs, números, caminhos de arquivo, com regex e
  listas, no estilo `EntityRuler`/`Matcher`, direto em C.
- NER estatístico: não vale portar; se necessário, chamar o modelo por MCP/ferramenta externa.
- Segmentação de frases por regras, útil para fatiar memórias antes de gerar embeddings.

**Exemplo mínimo.**
```python
import spacy
nlp = spacy.load("pt_core_news_sm")
for e in nlp("A Petrobras abriu uma vaga em Salvador em março.").ents:
    print(e.text, e.label_)
```

---

## 9. fastText

**O que é.** Biblioteca da Meta para vetores de palavras e classificação de texto. **Repositório
arquivado em 19/mar/2024 (somente leitura)** ([repositório](https://github.com/facebookresearch/fastText)).

**Estado.** Abandonada (sem novas versões), mas estável e amplamente usada.

**Formatos.** `.bin` (parâmetros, dicionário, hiperparâmetros), `.vec` (texto), `.ftz` (quantizado)
(mesma fonte). Treino supervisionado: `fasttext supervised -input train.txt -output model`, rótulos
`__label__x`.

**Detecção de idioma (o uso de maior valor para o katana).**
- `lid.176.bin` (126 MB, mais preciso) e `lid.176.ftz` (917 kB), 176 idiomas, licença CC BY-SA 3.0
  ([página](https://fasttext.cc/docs/en/language-identification.html)). A licença CC BY-SA exige
  atribuição e compartilhamento pela mesma licença do modelo; avaliar se isso conflita com a regra do
  projeto antes de embutir.
- Inferência é simples: soma de embeddings de n-gramas de caracteres hasheados e uma camada linear com
  softmax; dá para reimplementar em C (~200 linhas) lendo o `.ftz` (o formato quantizado usa product
  quantization; verificar a especificação do arquivo no código-fonte).
- Alternativa sem modelo: heurística por alfabeto e palavras funcionais (pt/en/es), útil como fallback.

**Exemplo mínimo.**
```python
import fasttext
m = fasttext.load_model("lid.176.ftz")
print(m.predict("onde fica a biblioteca?"))   # (('__label__pt',), array([0.9...]))
```

---

## 10. BERTopic

**O que é.** Modelagem de tópicos de Maarten Grootendorst, MIT, ~8 mil estrelas. Pipeline modular:
sentence-transformers para embeddings, UMAP para reduzir dimensão, HDBSCAN para agrupar, c-TF-IDF
para pesar termos, mais representações opcionais (inclusive com LLM)
([repositório](https://github.com/MaartenGr/BERTopic)).

**Estado.** Ativa (repositório com 251 commits e issues abertas; versão atual não mostrada; verificar).

**Aproveitável.** Conceito para organizar a memória: agrupar vetores de memórias em temas, nomear cada
grupo por termos c-TF-IDF (estatística simples, implementável em C) ou pelo próprio LLM do katana. O
passo UMAP/HDBSCAN é pesado; k-means simples sobre os vetores já dá agrupamento aceitável (escolha de
projeto; verificar com dados reais).

**Exemplo mínimo.**
```python
from bertopic import BERTopic
topicos, probs = BERTopic().fit_transform(docs)
```

---

## 11. AllenNLP

**O que é.** Biblioteca de pesquisa de NLP do Allen Institute for AI sobre PyTorch.

**Estado.** **Abandonada: arquivada em 16/dez/2022; "modo de manutenção" sem novos recursos nem
atualização de dependências** ([repositório](https://github.com/allenai/allennlp)).

**Aproveitável.** Nada. Evitar como dependência.

---

## 12. Fairseq

**O que é.** Toolkit da Meta para tradução, sumarização e modelos de linguagem; origem de modelos como
wav2vec 2.0 e MMS.

**Estado.** **Abandonada: arquivada em 20/mar/2026 (somente leitura)**; a última versão estável citada
é a 0.10.x e a última notícia é de maio de 2023 ([repositório](https://github.com/facebookresearch/fairseq)).
O sucessor fairseq2 existe (verificar; a página consultada não o cita).

**Formatos.** Checkpoints `.pt` (pickle PyTorch) com dicionários próprios; muitos modelos foram
convertidos para Transformers.

**Aproveitável.** Nada direto. Se um modelo só existir em fairseq, usar a versão convertida no Hub.

---

## 13. Stanza

**O que é / quem mantém.** Biblioteca de NLP do Stanford NLP Group, mantida principalmente por John
Bauer, licença Apache 2.0, 60+ idiomas, treinada em treebanks Universal Dependencies
([repositório](https://github.com/stanfordnlp/stanza)).

**Para que serve.** Tokenização, divisão de frases, expansão de palavras compostas (MWT), POS e
morfologia, lematização, análise de dependências, NER (para alguns idiomas), pacotes biomédicos e
clínicos.

**Estado.** Ativa (5,6 mil commits; versão atual não mostrada; verificar).

**Formatos.** Modelos PyTorch baixados por `stanza.download`; dados de treino em CoNLL-U e BIOES
(mesma fonte). Detecção de idioma: o módulo `langid` existe na biblioteca (verificar; a página
consultada não o mencionou).

**Aproveitável.** Referência de lematização multilíngue (útil para busca por palavras em português com
normalização), e conjuntos UD para testes. Não embutir.

**Exemplo mínimo.**
```python
import stanza
nlp = stanza.Pipeline("pt")
doc = nlp("O Brasil venceu a partida.")
print([(e.text, e.type) for e in doc.ents])
```

---

## 14. SpanMarker

**O que é.** NER por spans sobre encoders do Transformers (BERT, RoBERTa, XLM-R), criado por Tom Aarsen
como parte de sua tese na Argilla; integra com spaCy ([repositório](https://github.com/tomaarsen/SpanMarkerNER)).

**Estado.** Repositório público, 476 estrelas, sem aviso de arquivamento; releases não mostradas na página
(consultar CHANGELOG; verificar atividade em 2026).

**Modelos.** `span-marker-bert-base-fewnerd-fine-super`, `span-marker-roberta-large-ontonotes5` (F1 91,54),
`span-marker-xlm-roberta-large-conll03`, `span-marker-xlm-roberta-base-multinerd`,
`span-marker-mbert-base-multinerd` (multilíngue) (mesma fonte). Formato: safetensors no Hub.

**Aproveitável.** Pouco; o modelo `mbert-base-multinerd` seria o mais próximo de NER multilíngue. Rodar
um encoder BERT no katana (ver sentence-transformers) abriria a porta para NER por encoder no futuro.

**Exemplo mínimo.**
```python
from span_marker import SpanMarkerModel
m = SpanMarkerModel.from_pretrained("tomaarsen/span-marker-bert-base-fewnerd-fine-super")
print(m.predict("Amelia Earhart flew across the Atlantic to Paris."))
```

---

## 15. PaddleNLP

**O que é / quem mantém.** Biblioteca de NLP e LLM da Baidu sobre PaddlePaddle, Apache-2.0
([repositório](https://github.com/PaddlePaddle/PaddleNLP)). A versão citada na página é
`paddlenlp==3.0.0b4`, e a notícia mais recente é de 29/abr/2025 (suporte a Qwen3).

**Estado.** Incerta. O README consultado não traz aviso de arquivamento nem de migração. Existe um
projeto separado, PaddleFormers (PyPI 1.0.0), com imports `paddleformers.transformers`; um espelho de
terceiros o lista como arquivado em 27/abr/2026, **não confirmado** na página oficial
(busca: [PaddleFormers 1.0.0](https://pypi.org/project/paddleformers/1.0.0/)). Não há anúncio oficial
encontrado sobre PaddleNLP ser substituído (verificar antes de qualquer dependência).

**Aproveitável.** Nada. É um ecossistema paralelo ao PyTorch; seus pesos de LLM geralmente são
conversões de checkpoints do Hugging Face.

**Exemplo mínimo** (do README):
```python
from paddlenlp.transformers import AutoTokenizer, AutoModelForCausalLM
tok = AutoTokenizer.from_pretrained("Qwen/Qwen2-0.5B")
model = AutoModelForCausalLM.from_pretrained("Qwen/Qwen2-0.5B", dtype="float16")
```

---

## Plano sugerido para o katana (por ordem de valor)

1. **Teste dourado de tokenização e chat template** gerado com Transformers (Python, offline) e guardado
   no repositório como arquivos de dados; o motor compara byte a byte. Inclui conversas com ferramentas.
2. **Modo de embedding** com um modelo pequeno em GGUF (nomic-embed-text v1.5, Apache-2.0, ou
   EmbeddingGemma, com licença Gemma a avaliar): pooling correto, L2, prefixos de consulta e documento.
   Memória do agente passa a ter busca híbrida (vetor + palavras).
3. **Aplicar LoRA** no formato PEFT (`adapter_model.safetensors` + `adapter_config.json`), caminho
   lateral por camada, com mapa de nomes e verificação de logits.
4. **Detecção de idioma** sem LLM: heurística em C, e opcionalmente leitor de `lid.176.ftz` (atenção à
   licença CC BY-SA 3.0).
5. **Extração leve de entidades** por regras (datas, e-mails, URLs, caminhos), inspirada em
   `EntityRuler`/`Matcher` do spaCy.

## Fontes

- Transformers: [chat templates](https://huggingface.co/docs/transformers/chat_templating),
  [estratégias de geração](https://huggingface.co/docs/transformers/generation_strategies),
  [adicionar modelo](https://huggingface.co/docs/transformers/main/en/add_new_model),
  [releases](https://github.com/huggingface/transformers/releases)
- Tokenizers: [componentes](https://huggingface.co/docs/tokenizers/components)
- PEFT: [repositório](https://github.com/huggingface/peft),
  [formato de checkpoint](https://huggingface.co/docs/peft/developer_guides/checkpoint)
- Adapters: [repositório](https://github.com/adapter-hub/adapters)
- sentence-transformers: [uso](https://sbert.net/docs/sentence_transformer/usage/semantic_textual_similarity.html),
  [releases](https://github.com/huggingface/sentence-transformers/releases)
- Embeddings: [EmbeddingGemma](https://huggingface.co/google/embeddinggemma-300m),
  [GGUF EmbeddingGemma](https://huggingface.co/ggml-org/embeddinggemma-300M-GGUF),
  [artigo](https://arxiv.org/html/2509.20354v3),
  [Qwen3-Embedding GGUF](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF),
  [nomic GGUF](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF)
- [SetFit](https://github.com/huggingface/setfit), [NeMo](https://github.com/NVIDIA-NeMo/NeMo),
  [Flair](https://github.com/flairNLP/flair), [spaCy](https://github.com/explosion/spaCy/releases),
  [fastText](https://github.com/facebookresearch/fastText),
  [fastText lid](https://fasttext.cc/docs/en/language-identification.html),
  [BERTopic](https://github.com/MaartenGr/BERTopic), [AllenNLP](https://github.com/allenai/allennlp),
  [Fairseq](https://github.com/facebookresearch/fairseq), [Stanza](https://github.com/stanfordnlp/stanza),
  [SpanMarker](https://github.com/tomaarsen/SpanMarkerNER), [PaddleNLP](https://github.com/PaddlePaddle/PaddleNLP)
