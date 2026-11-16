# Frameworks e bibliotecas de ML do Hugging Face Hub (visão para o katana)

Pesquisa feita em 16/11/2026. Fontes primárias: documentação do Hub (`huggingface.co/docs/hub`), a lista de
bibliotecas do Hub no código-fonte do Hub (`huggingface.js/packages/tasks/src/model-libraries.ts` e
`model-libraries-snippets.ts`), repositórios no GitHub, PyPI e crates.io (versões e datas consultadas pelas
APIs em 16/11/2026). O que não tem fonte está marcado "(verificar)".

Contexto: o katana é um motor de inferência em C, local, que lê GGUF. Nenhuma dessas bibliotecas é usada em
tempo de execução do katana. Elas importam por três motivos: (1) são de onde os modelos vêm e em que formato
(quase sempre safetensors) antes de virarem GGUF; (2) trazem técnicas de kernel, quantização e layout de
memória que servem de referência; (3) servem de gabarito numérico para validar o katana (logits de referência).

## Tabela-resumo

| Biblioteca | Categoria | Linguagem | Ativa? (nov/2026) | Relevância para o katana |
|---|---|---|---|---|
| PyTorch | framework de DL (autograd, GPU) | Python / C++ (CUDA, etc.) | Sim, v2.15.0 (28/10/2026), ciclo de 2 meses | Alta (origem de quase todos os checkpoints; gabarito numérico) |
| TensorFlow | framework de DL (grafo/XLA) | Python / C++ | Sim, mas em manutenção relativa: v2.21.0 (mar/2026) | Baixa |
| JAX | computação numérica com transformações, XLA | Python | Sim, v0.12.0.1 (21/10/2026) | Média (checkpoints Flax/Orbax; ideias de compilação) |
| Keras (3.x) | API de alto nível multi-backend | Python | Sim, v3.15.1 (29/07/2026) | Baixa |
| TF-Keras | Keras 2 legado (só TensorFlow) | Python | Legado, v2.21.0 (mar/2026) | Baixa |
| KerasHub | modelos pré-treinados em Keras 3 (ex-KerasNLP) | Python | Sim, v0.33.0 (03/11/2026) | Baixa a média |
| PaddlePaddle | framework de DL (Baidu) | Python / C++ | Sim, v3.3.1 (26/03/2026) | Média (modelos de OCR/LLM da Baidu, em safetensors) |
| MLX | framework de arrays para Apple silicon | Python / C++ / C / Swift | Sim, v0.32.4 (14/10/2026) | Média (memória unificada, quantização em grupos, mlx-lm) |
| fastai | treino de alto nível sobre PyTorch | Python | Em manutenção lenta, v2.8.12 (09/09/2026) | Baixa |
| Habana (Gaudi / optimum-habana) | ponte Transformers para aceleradores Intel Gaudi | Python | Encerrada: última versão v1.21.2 (01/10/2026) | Baixa |
| Graphcore (IPU / optimum-graphcore) | ponte Transformers para IPUs Graphcore | Python | Em declínio: última versão v0.7.1 (jul/2023) | Baixa |
| Rust (candle e afins) | frameworks de ML em Rust | Rust | Sim, candle 0.11.1 (31/10/2026) | Média a alta (lê GGUF; kernels em Rust; comparação de arquitetura) |
| Microduck | robô bípede + política RL (não é framework de ML geral) | Rust (runtime) + Python (treino) | Sim (lançado ago/2026) | Baixa (apenas ONNX pequeno) |
| Asimov | robô humanoide da Menlo Research; visualizador de políticas | TypeScript (web) | Sim (verificar versão) | Baixa |
| univa | código do UniWorld (modelo unificado visão-linguagem, Qwen2.5-VL) | Python | Pesquisa; UniWorld-V2 (out/2025) é mais novo | Baixa a média (base Qwen2.5-VL) |

Observação sobre a lista do Hub: na tabela atual de bibliotecas da documentação, aparecem fastai, Keras,
KerasNLP/KerasHub, TF-Keras (legacy), MLX, PaddleNLP, Transformers etc. Habana, Graphcore e "Rust" não são
linhas da tabela de integrações; existem como pacotes `optimum-*` e como etiquetas de biblioteca em modelos
(a API do Hub devolveu modelos com a etiqueta `optimum_habana`, por exemplo). Microduck, Asimov e univa estão
em `model-libraries.ts` (seção própria abaixo).

---

## PyTorch

**O que é e quem mantém.** Framework de deep learning com tensores, autograd e execução "eager". Mantido pela
PyTorch Foundation (Linux Foundation) com forte contribuição da Meta. Licença BSD-3 (a API do GitHub devolve
"NOASSERTION", mas o texto é BSD-style; verificar o texto exato se for citar).

**Para que serve.** Pesquisa e treino de modelos; é o formato de origem da imensa maioria dos LLMs abertos
(via biblioteca Transformers).

**Estado em 2026.** Versão atual v2.15.0 (28/10/2026, PyPI; exige Python >= 3.11). Em 2026 o ciclo passou de
trimestral para um release a cada dois meses (2.10 em 21/01, 2.11 em 23/03, 2.12 em 13/05, 2.14 em 02/09,
2.15 em 28/10). O 2.11 tornou CUDA 13 o padrão. Muito ativa (repo com push no dia da pesquisa).

**Formatos.** `.safetensors` (padrão do Hub), `.bin`/`.pt` (pickle; inseguro), `torch.save`, `torch.export`
(ExportedProgram), TorchScript (legado), ONNX (exportador), ExecuTorch `.pte` (borda) (verificar detalhes).

**Caminho até o katana.** Checkpoint safetensors + `config.json` + tokenizador, convertido para GGUF por um
script de conversão do motor de referência (mapeia nomes de tensores, reordena RoPE se preciso, quantiza).
Transformers também consegue carregar GGUF de volta (desquantiza para fp32/bf16, exceto caminho empacotado
para Qwen3.5 com o kernel do Hub), o que serve de gabarito para comparar logits.

**Conceitos que valem para o katana.**
- `torch.compile` (TorchDynamo + Inductor): fusão de operações e geração de kernels Triton/C++; referência
  para pensar em fusão (RMSNorm+quant, rope+KV).
- SDPA / FlashAttention como kernel fundido de atenção; atenção em blocos com softmax online.
- Quantização (torchao: int8/int4 por grupo, fp8) e layout dos pesos (empacotamento de int4).
- KV cache e atenção paginada vivem nas bibliotecas acima do PyTorch, não nele.
- Autograd não se aplica ao katana (só inferência).

**Exemplo mínimo.**
```python
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer
m = AutoModelForCausalLM.from_pretrained("Qwen/Qwen3-0.6B", dtype=torch.bfloat16)
t = AutoTokenizer.from_pretrained("Qwen/Qwen3-0.6B")
x = t("Ola", return_tensors="pt")
print(m(**x).logits[0, -1].topk(5))   # gabarito de logits para o selftest
```

**Fontes.** https://pytorch.org/ , https://github.com/pytorch/pytorch , https://pypi.org/project/torch/ ,
https://pytorch.org/blog/pytorch-2.11-release/ , https://dev-discuss.pytorch.org/t/pytorch-2-15-0-general-availability/3470 ,
https://huggingface.co/docs/transformers/gguf

---

## TensorFlow

**O que é e quem mantém.** Framework de ML do Google (grafos, XLA, TFLite, TF Serving). Apache-2.0.

**Para que serve.** Treino e produção de modelos, historicamente visão e recomendação; hoje o foco do Google
em pesquisa de LLM é JAX, e na borda é LiteRT.

**Estado em 2026.** PyPI: v2.21.0 (06/03/2026; Python >= 3.10). O branch principal já prepara a 2.23
(arquivo RELEASE.md). Repositório ativo, mas é uma biblioteca madura em ritmo mais lento; é "maduro/
manutenção relativa" (julgamento meu, não declaração oficial). O TFLite virou LiteRT (anunciado como
runtime de borda principal na 2.21); o diretório `tensorflow/lite` está em modo de manutenção (só correções
críticas), segundo o README do LiteRT.

**Formatos.** SavedModel (`saved_model.pb` + variables), checkpoints TF (`.ckpt`), HDF5 (`.h5`, legado),
`.tflite`/LiteRT (FlatBuffers), pesos em safetensors via integrações (Transformers).

**Caminho até o katana.** Raro hoje. Checkpoints TF de LLMs antigos (BERT, T5) são carregáveis em PyTorch
pelo Transformers (`from_tf`) e daí para safetensors/GGUF. Não há conversão direta SavedModel para GGUF
(verificar).

**Conceitos que valem.** Quantização em blocos no TFLite 2.23 em desenvolvimento (INT2/INT4 com `block_shape`
em `fully_connected`, `embedding_lookup`, `batch_matmul`): é um bom paralelo dos blocos Q4_K do GGUF; modelo
de grafo estático + XLA para fusão.

**Exemplo mínimo.**
```python
import tensorflow as tf
a = tf.random.normal((2, 3)); b = tf.random.normal((3, 4))
print(tf.linalg.matmul(a, b).shape)   # (2, 4)
```

**Fontes.** https://github.com/tensorflow/tensorflow , https://pypi.org/project/tensorflow/ ,
https://developers.googleblog.com/whats-new-in-tensorflow-221/ , https://github.com/google-ai-edge/litert

---

## JAX

**O que é e quem mantém.** Biblioteca de computação numérica com API estilo NumPy e transformações
composáveis; mantida pelo Google (o README diz "projeto de pesquisa, não produto oficial do Google").
Apache-2.0.

**Para que serve.** Pesquisa em larga escala em TPU/GPU; base de Flax, Optax, Orbax, MaxText e dos modelos
Gemma originais.

**Estado em 2026.** v0.12.0.1 (21/10/2026; Python >= 3.12). Muito ativa.

**Formatos.** Pytrees em checkpoints Orbax (OCDBT/Zarr), `.msgpack` do Flax, safetensors via Transformers
(Flax) ou `safetensors.flax`; StableHLO exportado por `jax.export`.

**Caminho até o katana.** Converter checkpoint Orbax/Flax para safetensors (ou carregar via classes Flax
do Transformers, se ainda disponíveis; verificar no v5), depois para GGUF. Modelos Gemma distribuídos
em formato JAX/Orbax têm normalmente cópia em safetensors/GGUF no Hub.

**Conceitos que valem.** `jit` (compilação via XLA), `vmap` (batching), `shard_map` (paralelismo por
dispositivo), Pallas (kernels GPU/TPU escritos à mão, equivalente conceitual a shaders/OpenCL do katana),
formas estáticas e tiling. Autograd (`grad`) não se aplica.

**Exemplo mínimo.**
```python
import jax, jax.numpy as jnp
f = jax.jit(lambda x, w: jnp.tanh(x @ w))
print(f(jnp.ones((2, 3)), jnp.ones((3, 4))).shape)
```

**Fontes.** https://github.com/jax-ml/jax , https://docs.jax.dev/en/latest/ , https://pypi.org/project/jax/

---

## Keras (3.x)

**O que é e quem mantém.** API de alto nível multi-backend (JAX, TensorFlow, PyTorch); Google (François
Chollet e equipe), repositório `keras-team/keras`, Apache-2.0.

**Para que serve.** Definir, treinar e salvar modelos com a mesma API sobre qualquer backend.

**Estado em 2026.** v3.15.1 (29/07/2026; Python >= 3.11). Ativa.

**Formatos.** `.keras` (zip com `config.json`, `metadata.json` e `model.weights.h5`). No Hub, `model.save("hf://usuario/modelo")`
grava a pasta descompactada (config, metadata, `model.weights.h5`, cartão do modelo). Carregar:
`keras.saving.load_model("hf://...")` (precisa do TensorFlow instalado para ler caminhos `hf://`, segundo o
trecho gerado pelo Hub). Pesos em HDF5.

**Caminho até o katana.** Indireto: exportar os pesos como arrays NumPy, mapear nomes manualmente para
safetensors compatível com a arquitetura Transformers, e converter para GGUF. Sem conversor direto
(verificar). Para LLMs, prefira o checkpoint safetensors do próprio modelo.

**Conceitos que valem.** Pouco: a API não define kernels. Útil conhecer o esquema de quantização
(`keras.quantizers`, int8/float8) (verificar).

**Exemplo mínimo.**
```python
import keras
m = keras.saving.load_model("hf://Wauplin/mnist_example")
```

**Fontes.** https://huggingface.co/docs/hub/keras , https://github.com/keras-team/keras , https://keras.io/ ,
https://pypi.org/project/keras/

---

## TF-Keras

**O que é e quem mantém.** O Keras 2.x, hoje repositório separado `keras-team/tf-keras`, só TensorFlow. A
documentação do Hub o marca como legado ("legacy") e recomenda criar modelos com Keras 3.

**Estado em 2026.** v2.21.0 (18/03/2026), repo com commit em 14/11/2026: mantido para acompanhar o
TensorFlow. Ainda há mais de 4,5 mil modelos no Hub com a etiqueta `tf-keras` (número da doc do Hub; a
doc é a fonte).

**Formatos.** SavedModel (`saved_model.pb`; é o que o Hub usa para contar downloads), HDF5. API do Hub:
`from_pretrained_keras` / `push_to_hub_keras` (exigem `tf-keras` ou `keras<3`).

**Caminho até o katana.** Igual ao Keras: nenhum caminho direto; só vale para modelos antigos (visão, BERT).

**Conceitos.** Nenhum específico.

**Exemplo mínimo.**
```python
from huggingface_hub import from_pretrained_keras
model = from_pretrained_keras("keras-io/mobile-vit-xxs")
```

**Fontes.** https://huggingface.co/docs/hub/tf-keras , https://github.com/keras-team/tf-keras

---

## KerasHub

**O que é e quem mantém.** Biblioteca de modelos pré-treinados com implementações Keras 3 (antes KerasNLP;
no PyPI `keras-nlp` ainda aparece com a mesma versão do `keras-hub`). Time do Keras/Google. Apache-2.0.
Checkpoints distribuídos principalmente no Kaggle Models; integração com o Hub (`hf://` presets).

**Para que serve.** Texto, imagem e áudio (Gemma, Llama, Mistral, BERT, ResNet, etc.) com ajuste fino em
GPU/TPU e PEFT (LoRA); um único código roda em JAX, TensorFlow e PyTorch.

**Estado em 2026.** v0.33.0 (03/11/2026; Python >= 3.11). Ativa (commit em 16/11/2026).

**Formatos.** "Presets": pasta com `config.json`, pesos (`.weights.h5` e, em versões recentes, safetensors;
verificar), tokenizador. Consegue carregar checkpoints do Hub em safetensors de várias arquiteturas
(conversores internos de Transformers para Keras) (verificar lista).

**Caminho até o katana.** Preferir o checkpoint original em safetensors (mesmo modelo, fonte do Hub). Se o
modelo só existe como preset do KerasHub, a saída é exportar para safetensors (verificar o suporte atual
de exportação) e converter para GGUF.

**Conceitos que valem.** Convenções de arquitetura (atenção grouped-query, RoPE, norma) documentadas por
modelo; sem kernels próprios.

**Exemplo mínimo.**
```python
import os; os.environ["KERAS_BACKEND"] = "jax"
import keras_hub
lm = keras_hub.models.CausalLM.from_preset("hf://google/gemma-3-1b-it")  # (verificar o preset)
print(lm.generate("Ola", max_length=30))
```

**Fontes.** https://github.com/keras-team/keras-hub , https://keras.io/keras_hub/ ,
https://pypi.org/project/keras-hub/

---

## PaddlePaddle

**O que é e quem mantém.** Plataforma de deep learning da Baidu (código aberto desde 2016), Apache-2.0.
Inclui PaddleOCR, PaddleNLP e PaddleFormers (bibliotecas de topo).

**Para que serve.** Treino e implantação industrial, forte na China; OCR (PaddleOCR / PP-OCR /
PP-DocLayout), LLMs da família ERNIE (verificar).

**Estado em 2026.** PaddlePaddle v3.3.1 (26/03/2026 no PyPI; README fala em "3.3"); repo ativo. O PaddleNLP
no PyPI parou em 2.8.1 (jun/2024), e a lista do Hub ainda aponta para ele; o PaddleFormers aparece como a
biblioteca de LLMs/VLMs com suporte a safetensors (fonte: documentação do PaddleFormers, via busca; não
confirmei que "substitui" o PaddleNLP).

**Formatos.** Treino: `.pdparams`, `.pdopt`; inferência: `.pdmodel` + `.pdiparams` e, no 3.x, o IR "PIR"
(arquivo JSON de programa; verificar). safetensors é suportado (há API `safetensors.paddle`) e o Hub
conta downloads de PaddleOCR por `.safetensors`, `inference.pdiparams` ou `inference.onnx`. Também exporta ONNX (paddle2onnx).

**Caminho até o katana.** Para LLMs em safetensors com arquitetura padrão (ex.: ERNIE 4.5 publicado em
formato compatível com Transformers; verificar), converter para GGUF como qualquer outro. Para OCR/visão,
passar por ONNX (fora do escopo do katana hoje).

**Conceitos que valem.** PIR (representação intermediária única para dinâmico para estático, otimização
de grafo e inferência; as notas da 3.0 citam >10% de ganho de inferência); paralelismo automático.

**Exemplo mínimo.**
```python
import paddle
x = paddle.randn([2, 3]); w = paddle.randn([3, 4])
print(paddle.matmul(x, w).shape)
```

**Fontes.** https://github.com/PaddlePaddle/Paddle , https://pypi.org/project/paddlepaddle/ ,
https://github.com/PaddlePaddle/PaddleNLP , https://github.com/PaddlePaddle/PaddleOCR ,
https://huggingface.co/docs/safetensors/api/paddle , https://paddlepaddle.org.cn/documentation/docs/en/release_note_en.html

---

## MLX

**O que é e quem mantém.** Framework de arrays para Apple silicon, da Apple Machine Learning Research.
MIT. APIs em Python (estilo NumPy), C++, C e Swift; `mlx.nn` e `mlx.optimizers` espelham o PyTorch.
`mlx-lm` é o pacote para LLMs.

**Para que serve.** Treino, ajuste fino (LoRA) e inferência em Mac/iPhone com a GPU Metal; há backend CUDA
(`pip install mlx[cuda]`, citado no snippet do Hub) e CPU.

**Estado em 2026.** mlx v0.32.4 (14/10/2026), mlx-lm v0.32.2 (16/10/2026). Muito ativa.

**Formatos.** `.safetensors` (padrão do mlx-lm, com `config.json` e quantização gravada na config),
`.npz` e GGUF (mlx lê e grava; o Hub documenta um exemplo de LLM em GGUF). A comunidade `mlx-community`
publica pesos convertidos.

**Caminho até o katana.** Pesos MLX quantizados (grupos de 32/64 com escala e viés por grupo, 2 a 8 bits)
NÃO são o mesmo layout dos blocos GGUF; o mais seguro é pegar o checkpoint original em safetensors e
converter para GGUF. Quando só existe a versão MLX em bf16/fp16, os tensores são compatíveis em nome (esquema
Transformers/Llama), mas verificar caso a caso.

**Conceitos que valem.**
- Memória unificada: arrays ficam em memória compartilhada e qualquer dispositivo opera sem cópia (análogo
  ao caso de iGPUs com Vulkan/OpenCL do katana).
- Computação preguiçosa (lazy) com grafo dinâmico; `mx.compile` funde kernels.
- Quantização por grupos afim (escala e viés) e kernels Metal de matmul quantizado.
- Transformações função-a-função (grad, vmap); autograd não interessa ao katana.

**Exemplo mínimo.**
```python
from mlx_lm import load, generate
model, tok = load("mlx-community/Qwen3-0.6B-4bit")   # (verificar o nome do repositório)
print(generate(model, tok, prompt="Ola", max_tokens=32))
# converter e quantizar: python -m mlx_lm.convert --hf-path <repo> -q
```

**Fontes.** https://github.com/ml-explore/mlx , https://github.com/ml-explore/mlx-lm ,
https://huggingface.co/docs/hub/mlx , https://huggingface.co/mlx-community , https://pypi.org/project/mlx/

---

## fastai

**O que é e quem mantém.** Biblioteca de alto nível sobre PyTorch (Jeremy Howard, Sylvain Gugger e a
comunidade fast.ai) para visão, texto, tabular e filtragem colaborativa; Apache-2.0.

**Para que serve.** Ensino e prototipagem rápida de classificadores; o Hub o suporta com widget de
classificação de imagens.

**Estado em 2026.** v2.8.12 (09/09/2026; Python >= 3.10). Ainda recebe versões, em ritmo baixo (último push
em 21/09/2026). Classificação minha: manutenção.

**Formatos.** `Learner.export()` gera `.pkl` (pickle do Learner inteiro, inseguro e preso a versões);
`from_pretrained_fastai` valida a versão do fastai usada ao salvar. O Hub guarda o `.pkl` e
`push_to_hub_fastai` envia o Learner.

**Caminho até o katana.** Irrelevante para LLMs. Qualquer modelo fastai é PyTorch por baixo: seria
preciso extrair o `state_dict`, o que está fora do escopo do katana (só LLM).

**Conceitos.** Nenhum para inferência de LLM.

**Exemplo mínimo.**
```python
from huggingface_hub import from_pretrained_fastai
learner = from_pretrained_fastai("espejelomar/identify-my-cat")
```

**Fontes.** https://huggingface.co/docs/hub/fastai , https://github.com/fastai/fastai ,
https://pypi.org/project/fastai/

---

## Habana (Gaudi / optimum-habana)

**O que é e quem mantém.** `optimum-habana` ("Optimum for Intel Gaudi"): interface entre Transformers/
Diffusers e aceleradores Intel Gaudi (HPU; Gaudi 1, 2 e 3, este com 128 GB por dispositivo, segundo a doc).
Hugging Face (repositório `huggingface/optimum-habana`, Apache-2.0).

**Para que serve.** Treino e inferência de modelos do Transformers/Diffusers em Gaudi, com DeepSpeed, HPU
graphs, FP8 (verificar detalhes de FP8) e multi-HPU; a tabela de modelos validados inclui Llama 3, Qwen2/3,
Gemma 2/3, DeepSeek-V2/V3, GPT-OSS.

**Estado em 2026.** O README diz: "A versão final é v1.21.2 (Intel Gaudi Software 1.24, Transformers
4.55.x)", publicada em 01/10/2026. Ou seja, o projeto foi encerrado como biblioteca versionada
(o repo teve push recente, 16/11/2026; verificar se segue só como arquivo ou migrou para outro destino).

**Formatos.** Os mesmos do Transformers (safetensors); nenhum formato próprio de modelo.

**Caminho até o katana.** Nenhum especial: o checkpoint é o mesmo safetensors do Transformers.

**Conceitos que valem.** Gaudi usa matmul em matrizes (MME) e núcleos vetoriais programáveis (TPC) com
compilação de grafos (HPU graphs, shapes estáticos) para evitar recompilação; a lição para o katana é o
custo de shapes dinâmicos em aceleradores com grafo. Hardware sem relevância direta (o katana usa
Vulkan/OpenCL).

**Exemplo mínimo.**
```python
# em container Gaudi
from optimum.habana.transformers.modeling_utils import adapt_transformers_to_gaudi
adapt_transformers_to_gaudi()   # depois usar AutoModelForCausalLM normalmente, em device "hpu"
```

**Fontes.** https://huggingface.co/docs/optimum-habana/index , https://github.com/huggingface/optimum-habana ,
https://pypi.org/project/optimum-habana/

---

## Graphcore (IPU / optimum-graphcore)

**O que é e quem mantém.** `optimum-graphcore`: ponte entre Transformers e as IPUs da Graphcore (arquitetura
MIMD, memória SRAM grande junto aos núcleos, programada pelo Poplar SDK). Hugging Face; Apache-2.0.

**Para que serve.** Paralelismo de modelo (pipeline) e treino/inferência de modelos do Transformers em IPUs.

**Estado em 2026.** Em declínio: última versão no PyPI é v0.7.1 (28/07/2023) e os commits de 2026 vistos
(set/out) são só de CI e Dependabot. Requer o Poplar SDK da Graphcore. Não é recomendado para projetos novos
(avaliação minha, a partir dessas datas).

**Formatos.** Transformers (safetensors/pytorch_model.bin) mais um `ipu_config.json` por modelo.

**Caminho até o katana.** Nenhum especial (checkpoint do Transformers).

**Conceitos que valem.** Pouco: ilustra execução em pipeline com pesos residentes em SRAM distribuída.

**Exemplo mínimo.**
```python
from optimum.graphcore import pipeline   # exige Poplar SDK
p = pipeline("text-classification", model="Graphcore/distilbert-base-ipu")  # (verificar nome)
```

**Fontes.** https://github.com/huggingface/optimum-graphcore , https://pypi.org/project/optimum-graphcore/ ,
https://www.graphcore.ai/products/ipu

---

## Rust (bibliotecas de ML em Rust no Hub; principal: candle)

**O que é e quem mantém.** O Hub não tem uma biblioteca chamada "Rust" na tabela de integrações (verificado
na doc); o que existe são bibliotecas em Rust. A principal é o **candle**, "framework minimalista de ML
para Rust com foco em desempenho (incluindo GPU) e facilidade de uso", da Hugging Face (Apache-2.0 e MIT).
Outras do ecossistema (sem fonte consultada nesta pesquisa, marcar como verificar): `burn`, `tch-rs`,
`mistral.rs`, `ort`; a própria `safetensors` e `tokenizers` do Hub têm núcleo em Rust.

**Para que serve.** Inferência leve, sem Python, em CPU/CUDA/Metal e WebAssembly; serviço de embeddings e
LLM (ex.: o `atoma-infer` citado no README usa FlashAttention2 e PagedAttention sobre candle).

**Estado em 2026.** candle-core 0.11.1 (31/10/2026, crates.io; mais de 11,9 milhões de downloads); repositório
muito ativo (21 mil estrelas, push em 16/11/2026). O README cita uma feature opt-in `cutile` para kernels CUDA
JIT escritos em Rust.

**Formatos.** safetensors (principal), GGUF/GGML quantizado (exemplos `quantized` e `quantized-qwen3-moe`),
PyTorch `.pt` e NPZ (verificar nesta ordem na doc do candle).

**Caminho até o katana.** Direto no sentido contrário: modelos GGUF que o katana lê também rodam no candle,
logo o candle serve como segunda implementação de referência (de logits) para tipos K-quant. Para conversão
de safetensors para GGUF usa-se o script do motor de referência.

**Conceitos que valem.**
- Backend CPU otimizado com MKL (x86) ou Accelerate (Mac); kernels CUDA; kernels definidos pelo usuário
  (FlashAttention v2 embutida).
- Tensor com layout em strides, sem autograd obrigatório; quantização por blocos compatível com GGUF.
- Wasm: inferência no navegador, útil como alvo de comparação para binários pequenos e sem dependências.

**Exemplo mínimo (do README do candle).**
```rust
use candle_core::{Device, Tensor};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let device = Device::Cpu;
    let a = Tensor::randn(0f32, 1., (2, 3), &device)?;
    let b = Tensor::randn(0f32, 1., (3, 4), &device)?;
    println!("{}", a.matmul(&b)?);
    Ok(())
}
// GPU: Device::new_cuda(0)?
// LLM quantizado: cargo run --example quantized --release
```

**Fontes.** https://github.com/huggingface/candle , https://crates.io/crates/candle-core ,
https://huggingface.github.io/candle/guide/installation.html

---

## Microduck

**O que é e quem mantém.** Não é um framework de ML. É um robô bípede de código aberto em forma de pato
(cerca de 25 cm e 800 g), da Pollen Robotics (empresa da Hugging Face), com pré-venda a US$ 399 anunciada
em ago/2026 (a notícia está em vários veículos; os dados de hardware vêm do README do repositório).
Repositório `pollen-robotics/microduck` (Apache-2.0): runtime em Rust (um workspace, vários daemons por
JSON-RPC em sockets Unix, como `robotd` para o laço de controle e barramento de motores), placa Rockchip
RK3566, 15 servos, câmera. O treino fica em `microduck_rl` (MuJoCo + PPO). Fonte da entrada do Hub:
`model-libraries.ts`.

**Para que serve.** Executar políticas de aprendizado por reforço que andam, levantam, chutam etc. O laço de
controle roda a 50 Hz.

**Estado em 2026.** Ativo, lançado em 2026 (versão: não informada; verificar).

**Formatos.** ONNX (o Hub conta downloads por `.onnx`). No Hub, o modelo traz a etiqueta
`microduck-slot:<walk|stand|sitstand|ground_pick|kick_left|kick_right|roulade>`.

**Caminho até o katana.** Nenhum: são redes pequenas de controle em ONNX, não LLMs. Só se o katana um dia
lesse ONNX MLP pequeno (fora do escopo).

**Conceitos que valem.** Inferência determinística de baixa latência num laço fixo de 50 Hz em CPU de borda
(RK3566); lembrete de que orçamento por quadro e ausência de alocação dinâmica contam mais que vazão.

**Exemplo mínimo (snippet do Hub).**
```bash
sudo robotctl policy load stand nocrab/microduck-swing   # SLOT conforme a etiqueta do cartão
```
(o repositório `nocrab/microduck-swing` é o exemplo citado numa busca; verificar o slot correto no cartão)

**Fontes.** https://github.com/pollen-robotics/microduck ,
https://github.com/huggingface/huggingface.js/blob/main/packages/tasks/src/model-libraries.ts ,
https://axios.com/2026/08/27/hugging-face-debuts-microduck-a-399-robot ,
https://huggingface.co/nocrab/microduck-swing

---

## Asimov

**O que é e quem mantém.** Entrada "Asimov" da lista do Hub, com repositório
`menloresearch/humanoid-policy-viewer`: visualizador e banco de testes, no navegador, de políticas de
locomoção para o robô humanoide **Asimov 1** da Menlo Research. Roda simulação MuJoCo em WebAssembly e
executa a política com onnxruntime-web, sem GPU. Licença BSD-3 (descrição do robô em CERN-OHL-S-2.0).
Treino e ranking ficam em outro projeto (cyclotron, citado no README; verificar).

**Para que serve.** Carregar políticas do Hub, ajustar comandos de velocidade, empurrões, atrito e ganhos, e
rodar benchmarks (`npm run benchmark`).

**Estado em 2026.** Ativo (versão: verificar). Ainda é um projeto de nicho.

**Formatos.** Pasta de política com `policy.onnx`, `env.yaml` (ganhos, escala de ação, limites de torque; o
Hub conta downloads por `env.yaml`), `agent.yaml` e opcionalmente `reference_policy_config.json`.

**Caminho até o katana.** Nenhum (rede de controle em ONNX, não LLM).

**Conceitos que valem.** Nenhum direto.

**Exemplo mínimo (snippet do Hub).**
```bash
git clone https://github.com/menloresearch/humanoid-policy-viewer
cd humanoid-policy-viewer && npm install
npm run hf <usuario>/<repo-da-politica>
```

**Fontes.** https://github.com/menloresearch/humanoid-policy-viewer ,
https://github.com/huggingface/huggingface.js/blob/main/packages/tasks/src/model-libraries.ts

---

## univa

**O que é e quem mantém.** Entrada "univa" da lista do Hub, cujo repositório é
`PKU-YuanGroup/UniWorld-V1` (grupo YuanGroup, Universidade de Pequim): framework unificado de
compreensão, geração e edição de imagens ("High-Resolution Semantic Encoders for Unified Visual
Understanding and Generation", arXiv 2506.03147, lançado em 03/06/2025; mais de 20 tarefas). O README
indica ainda UniWorld-V2 (19/10/2025) e UniWorld-OSP2.0 (25/11/2025, imagem para vídeo). Licença MIT
indicada, mas os pesos FLUX usados dependem da licença não comercial do FLUX.1 [dev].

**Para que serve.** Modelo multimodal que combina um modelo visão-linguagem com um gerador de imagens.

**Estado em 2026.** Projeto de pesquisa, sucedido por versões novas dentro da mesma organização (nenhuma
informação de atividade em 2026 foi coletada; verificar).

**Formatos.** Pesos em safetensors com `config.json` (o Hub conta downloads por `config.json`). Classe
`UnivaQwen2p5VLForConditionalGeneration`, ou seja, base **Qwen2.5-VL** (vem do snippet do Hub). Usa
`attn_implementation="flash_attention_2"` e bf16.

**Caminho até o katana.** A parte de linguagem é de arquitetura Qwen2.5-VL. Se o katana um dia suportar
visão, é a mesma família; hoje não há conversão de `univa` para GGUF (verificar). O gerador de imagens
(FLUX) está fora do escopo.

**Conceitos que valem.** Codificadores semânticos de alta resolução como entrada visual de um LLM.

**Exemplo mínimo (snippet do Hub).**
```python
# seguir a instalação em https://github.com/PKU-YuanGroup/UniWorld-V1
import torch
from transformers import AutoProcessor
from univa.models.qwen2p5vl.modeling_univa_qwen2p5vl import UnivaQwen2p5VLForConditionalGeneration
model = UnivaQwen2p5VLForConditionalGeneration.from_pretrained(
    "<repo-id>", torch_dtype=torch.bfloat16, attn_implementation="flash_attention_2").to("cuda")
processor = AutoProcessor.from_pretrained("<repo-id>")
```

**Fontes.** https://github.com/PKU-YuanGroup/UniWorld-V1 , https://arxiv.org/abs/2506.03147 ,
https://github.com/huggingface/huggingface.js/blob/main/packages/tasks/src/model-libraries-snippets.ts

---

## Resumo para o katana

- O formato que importa na entrada é safetensors (Transformers/PyTorch); o que importa na saída é GGUF.
  JAX, Keras, Paddle e MLX são fontes secundárias: quando o modelo existe nelas, existe quase sempre em
  safetensors também.
- Técnicas reaproveitáveis: fusão de kernels (torch.compile, mx.compile, XLA), atenção fundida em blocos,
  quantização por blocos/grupos (blocos TFLite 2.23, grupos MLX, K-quants do GGUF), memória unificada (MLX),
  shapes estáticos (Gaudi, XLA).
- Candle é o par mais próximo (inferência sem Python, lê GGUF) e serve de segundo gabarito.
- Microduck, Asimov: robótica com ONNX pequeno; `univa`: base Qwen2.5-VL. Fora do foco atual.
