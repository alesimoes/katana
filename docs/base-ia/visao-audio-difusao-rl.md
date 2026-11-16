# Visao, audio, difusao, OCR, RL e ML classico: bibliotecas e o que o katana pode aproveitar

Pesquisa feita em novembro de 2026 em paginas oficiais (GitHub, Hugging Face, docs). O que nao tinha fonte
esta marcado "(verificar)". Numeros de velocidade/tamanho para a RTX 3050 4 GB + i7 sao **estimativas**,
nao medidas, salvo quando a fonte diz o contrario. Nenhuma dessas bibliotecas foi executada para este documento.

## Tabela-resumo

| Nome | Area | Ativa em 2026? | Relevancia para o katana |
|---|---|---|---|
| Diffusers | Difusao (imagem/video/audio), Python | Sim (7 mil+ commits, Apache-2.0) | Media: referencia de arquiteturas e pesos; nao roda no katana. Util para converter/validar |
| timm | Codificadores de imagem (ViT, ConvNeXt...) | Sim (v1.0.31, 03/11/2026) | Alta como fonte de pesos/arquitetura de ViT; o ViT do katana teria que ser reescrito em C |
| OpenCLIP | CLIP/SigLIP (imagem-texto) | Sim (serie 4.x) | Alta: SigLIP2 e o mesmo tipo de torre visual usada em VLMs |
| PaddleOCR | OCR e parsing de documentos | Sim (3.7.0, 11/06/2026) | Alta para (d): PaddleOCR-VL 0,9B ja tem GGUF + mmproj |
| ESPnet | Fala (ASR, TTS, SE, diarizacao) | Sim (release 202611) | Media: OWSM (ASR) e F5-TTS; pesquisa, pouco GGUF |
| SpeechBrain | Fala (ASR, locutor, separacao, TTS) | Sim (comunitaria; ultima novidade grande: 1.0, jan/2024) | Baixa/media: modelos de locutor, VAD; pouco para portar |
| Asteroid | Separacao de fontes de audio | Duvidosa (verificar atividade) | Baixa |
| pyannote.audio | Diarizacao (quem falou quando) | Sim (pipeline community-1) | Media: so se o assistente tiver reuniao/multiplos locutores |
| stable-baselines3 | RL (PPO, SAC, DQN...) | Sim (PyTorch >= 2.8, Py 3.10+) | Baixa: treino de politicas, fora do foco |
| ml-agents (Unity) | RL em ambientes Unity | Pouco ativa (Release 23, ago/2025) | Muito baixa |
| sample-factory | RL de alto throughput | Ativa moderada (v2) | Muito baixa |
| Scikit-learn | ML classico | Sim (1.9.1) | Baixa/media: utilitarios (clustering, TF-IDF, avaliacao) em scripts de apoio |

Resposta curta para as quatro capacidades: (a) visao e (d) OCR passam pelo **mesmo** trabalho (torre ViT + projetor
via `mmproj`); (b) fala: whisper.cpp/Kokoro/Piper sao os candidatos de porte; (c) difusao e a mais cara e a menos
alinhada ao motor atual.

---

## 1. Diffusers

- **O que e / quem mantem:** biblioteca PyTorch de modelos de difusao, da Hugging Face. Licenca Apache-2.0. Autores citados: von Platen, Patil, Lozhkov, Cuenca, Paul e outros.
- **Para que serve:** texto-para-imagem, img2img, inpainting, ControlNet, super-resolucao, e tambem video e audio.
- **Estado em 2026:** muito ativa (7.069 commits no `main`). Guia de memoria oficial cobre CPU offload, group offload, VAE slicing/tiling, layerwise casting fp8 e backends de atencao. Versao exata (verificar no PyPI).
- **Modelos e formatos:** safetensors, em pastas por componente (`unet`/`transformer`, `vae`, `text_encoder`). Suporte a quantizacao/GGUF no Diffusers: (verificar).
- **O que o katana aproveita:** nao roda em C. Serve como referencia numerica (comparar saidas) e para entender a estrutura de cada pipeline antes de portar. Para execucao em C/C++ o caminho real e o stable-diffusion.cpp (secao abaixo).

Exemplo minimo (README oficial):
```python
from diffusers import DiffusionPipeline
import torch
p = DiffusionPipeline.from_pretrained("stable-diffusion-v1-5/stable-diffusion-v1-5", dtype=torch.float16)
p.to("cuda")
p("An image of a squirrel in Picasso style").images[0]
```
Em 4 GB, trocar `.to("cuda")` por `p.enable_model_cpu_offload()` (guia de memoria).

Fontes: https://github.com/huggingface/diffusers · https://huggingface.co/docs/diffusers/main/en/optimization/memory

## 2. timm (PyTorch Image Models)

- **O que e / quem mantem:** colecao de modelos de imagem (ResNet, EfficientNet, ViT, Swin, ConvNeXt, MaxViT, MobileNet) com pesos, treino e inferencia. Autor: Ross Wightman; repositorio no org `huggingface`. A pagina diz que a 1.0.26 foi a primeira manutencao apos a saida dele da Hugging Face (ele segue como autor citado).
- **Estado em 2026:** ativa; v1.0.31 em 03/11/2026. Em set/2026 entraram variantes de ViT do Qwen3-VL/Qwen3.5, entre outras. Licenca Apache-2.0 no codigo; alguns pesos tem termos proprios (ex.: Facebook WSL nao comercial).
- **Modelos e formatos:** `.safetensors` no Hub; API comum (`forward_features`, `forward_intermediates`).
- **O que o katana aproveita:** dicionario de arquiteturas de torre visual. Para (a), o que importa e reproduzir em C o ViT usado pelo modelo escolhido (patch embed, atencao, MLP, norm, posicao 2D); timm ajuda a gerar tensores de referencia por camada para validar o `selftest`.

Exemplo minimo (de memoria da API documentada em huggingface.co/docs/timm, nao presente no README: verificar):
```python
import timm, torch
m = timm.create_model("vit_base_patch16_224", pretrained=True).eval()
print(m(torch.randn(1,3,224,224)).shape)
```
Fonte: https://github.com/huggingface/pytorch-image-models · https://huggingface.co/docs/timm

## 3. OpenCLIP

- **O que e / quem mantem:** implementacao aberta de CLIP e familia (SigLIP, SigLIP2, CoCa, MaMMUT, PE-Core). Liderada por Ross Wightman, Romain Beaumont, Cade Gordon, Vaishaal Shankar. Licenca: (verificar; link para LICENSE na pagina).
- **Para que serve:** embeddings imagem-texto, classificacao zero-shot, busca de imagens; as mesmas torres visuais servem de codificador em VLMs.
- **Estado em 2026:** serie 4.x (4.2.1 e a primeira no PyPI; data: verificar). Novidades: CLAP 2 (audio-texto), NaFlex (resolucao variavel), requer PyTorch 2.6+. Tabela da pagina: ViT-SO400M-14-SigLIP-384 83,1% zero-shot ImageNet; ViT-SO400M-16-SigLIP2-384 84,1%.
- **Modelos e formatos:** pesos no Hub (safetensors/pt).
- **O que o katana aproveita:** busca semantica de imagens/capturas de tela local (embedding + similaridade, trivial depois que a torre existir em C) e, principalmente, validacao: SigLIP-so400m e da mesma classe de torre que a do Qwen3.6 (ver secao 13).

Exemplo minimo (README oficial):
```python
import torch, open_clip
from PIL import Image
model, _, preprocess = open_clip.create_model_and_transforms('ViT-B-32', pretrained='laion2b_s34b_b79k')
tok = open_clip.get_tokenizer('ViT-B-32')
img = preprocess(Image.open("a.png")).unsqueeze(0); txt = tok(["a diagram","a dog"])
with torch.no_grad():
    i = model.encode_image(img); t = model.encode_text(txt)
```
Fonte: https://github.com/mlfoundations/open_clip

## 4. PaddleOCR

- **O que e / quem mantem:** toolkit de OCR e parsing de documentos da PaddlePaddle (Baidu). Apache-2.0.
- **Estado em 2026:** muito ativa. 3.7.0 (11/06/2026) traz PP-OCRv6 em tres niveis (tiny/small/medium); 3.6.0 (28/05/2026) PaddleOCR-VL-1.6; 3.4.0 (29/01/2026) PaddleOCR-VL-1.5; 22/07/2026 HPD-Parsing (VLM leve para parsing em alta vazao).
- **Modelos e formatos:** PP-OCRv5 multilingue (2M parametros, 109 idiomas, segundo as notas da 3.3.0); PaddleOCR-VL 0,9B (VLM de documentos). Exportacao ONNX documentada ("Obtaining ONNX Models"). **GGUF + mmproj oficiais** para PaddleOCR-VL-1.5 e 1.6 no Hub, rodando no llama.cpp (suporte entrou em build b8110, PR #18825 segundo cartao de modelo comunitario).
- **O que o katana aproveita:** o caminho (d). Prompts: `OCR:`, `Table Recognition:`, `Formula Recognition:`, `Chart Recognition:`, `Seal Recognition:`. Alerta de cartoes: possivel loop de repeticao; mmproj em F32 recomendado.

Exemplo minimo via llama.cpp (cartao oficial):
```
llama-cli -m PaddleOCR-VL-1.5.gguf --mmproj PaddleOCR-VL-1.5-mmproj.gguf -p 'OCR:' --image test_image.jpg
```
Exemplo Python (API `PaddleOCR(...).predict(...)`): (verificar na doc paddleocr.ai).

Fontes: https://github.com/PaddlePaddle/PaddleOCR · https://huggingface.co/PaddlePaddle/PaddleOCR-VL-1.5-GGUF · https://huggingface.co/PaddlePaddle/PaddleOCR-VL-1.6-GGUF

## 5. ESPnet

- **O que e:** toolkit de fala de ponta a ponta (CMU/comunidade; mantenedores nao nomeados na pagina). Apache-2.0.
- **Para que serve:** ASR, TTS, canto, melhoria/separacao, traducao fala-fala, SLU, diarizacao, SSL, codecs neurais, SpeechLM.
- **Estado em 2026:** ativa; release 202611 (F5-TTS, avaliacao de qualidade de audio, CLI de uma linha `espnet transcribe`/`synthesize`). OWSM v4 (ex.: `espnet/owsm_ctc_v4_1B`) como ASR multilingue aberto.
- **Formatos:** checkpoints PyTorch; ONNX: so ha citacao do artigo ESPnet-ONNX (2022), suporte atual (verificar). GGUF: nao ha.
- **O que o katana aproveita:** pouco direto; e fonte de modelos de pesquisa. Para producao local, whisper.cpp e mais pratico.

Exemplo minimo (README):
```python
from espnet2.bin.s2t_inference import Speech2Text
s2t = Speech2Text.from_pretrained("espnet/owsm_ctc_v4_1B")
for start, end, text in s2t.decode_long("audio.wav"): print(text)
```
Fonte: https://github.com/espnet/espnet

## 6. SpeechBrain

- **O que e:** toolkit PyTorch de fala/conversacao; Ravanelli, Parcollet, Moumen e comunidade. Apache-2.0. Artigo da 1.0 no JMLR (2024).
- **Para que serve:** ASR, reconhecimento de locutor, separacao, melhoria, TTS/vocoder, VAD, emocao, ID de idioma, diarizacao; mais de 200 receitas e 100 modelos no Hub.
- **Estado em 2026:** ativo (10,6 mil commits; 11,9 mil estrelas), mas a ultima novidade destacada no README e a 1.0 de jan/2024 (releases atuais: verificar).
- **Formatos:** PyTorch/HF Hub; ONNX/GGUF: (verificar).
- **O que o katana aproveita:** embeddings de locutor (ECAPA) e VAD seriam pequenos e portaveis, se um dia precisar identificar quem fala; fora isso, baixa prioridade.

Exemplo minimo (README):
```python
from speechbrain.inference import EncoderDecoderASR
m = EncoderDecoderASR.from_hparams(source="speechbrain/asr-conformer-transformerlm-librispeech", savedir="pretrained_models/asr")
m.transcribe_file("example.wav")
```
Fonte: https://github.com/speechbrain/speechbrain

## 7. Asteroid

- **O que e:** toolkit PyTorch para separacao de fontes de audio (ConvTasNet, DPRNN, DPTNet, DCUNet; datasets WSJ0-2mix, WHAM, LibriMix, MUSDB18). MIT. 2,6 mil estrelas, 923 commits.
- **Estado em 2026:** a pagina nao mostra data do ultimo commit nem release; **atividade nao confirmada (verificar)**. Docs referenciam v0.7.0.
- **O que o katana aproveita:** praticamente nada; separacao de voz so importaria para audio muito ruidoso. Modelos pequenos (ConvTasNet) seriam portaveis, mas fora do roteiro.

Exemplo minimo (README): `pip install asteroid`, depois `cd egs/wham/ConvTasNet && . ./run.sh` (receita de treino).
Fonte: https://github.com/asteroid-team/asteroid

## 8. pyannote.audio

- **O que e:** toolkit de diarizacao ("quem falou quando"). Hervé Bredin, Alexis Plaquet e org `pyannote`. MIT (codigo); os pipelines pedem aceitar condicoes no Hugging Face.
- **Estado em 2026:** ativo. Pipelines: `speaker-diarization-community-1` (aberto, local), `precision-2` (pago, servidores da pyannoteAI), e o legado 3.1.
- **Formatos:** PyTorch; tamanho e ONNX nao constam na pagina (verificar).
- **O que o katana aproveita:** combinado com ASR, daria transcricao de reunioes com locutores. Etapa posterior a (b).

Exemplo minimo (README):
```python
from pyannote.audio import Pipeline
p = Pipeline.from_pretrained("pyannote/speaker-diarization-community-1", token="HF_TOKEN")
for turn, spk in p("audio.wav").speaker_diarization: print(turn.start, turn.end, spk)
```
Fonte: https://github.com/pyannote/pyannote-audio

## 9. stable-baselines3

- **O que e:** algoritmos de RL em PyTorch, interface estilo scikit-learn. Mantenedores: Hill, Raffin, Ernestus, Gleave, Kanervisto, Gallouédec (DLR-RM). MIT.
- **Estado em 2026:** ativo; exige PyTorch >= 2.8 e Python 3.10+. Nucleo: A2C, DDPG, DQN, HER, PPO, SAC, TD3. Contrib: ARS, CrossQ, MaskablePPO, QR-DQN, RecurrentPPO, TQC, TRPO. SBX (versao JAX).
- **O que o katana aproveita:** nada direto agora. Se o dono quiser ajustar politicas (ex.: escolha de ferramentas) por RL, seria aqui; politicas MLP sao minusculas e exportaveis para qualquer runtime.

Exemplo minimo (README): `PPO("MlpPolicy", "CartPole-v1").learn(10_000)`
Fonte: https://github.com/DLR-RM/stable-baselines3

## 10. ml-agents (Unity)

- **O que e:** Unity Technologies; RL e imitacao com ambientes dentro do Unity. Apache-2.0.
- **Estado:** Release 23 (tag em 02/09/2025; changelog 28/08/2025), pacote `com.unity.ml-agents` 4.0.0, migrou para Inference Engine 2.2.1 (antes Sentis). Sem release mais recente na pagina de releases: **parece pouco ativa (verificar)**. Algoritmos nao listados na pagina principal.
- **Formatos:** politicas treinadas exportadas em ONNX para o Unity.
- **O que o katana aproveita:** quase nada; so se o assistente controlar simulacoes Unity.

Fontes: https://github.com/Unity-Technologies/ml-agents · https://github.com/Unity-Technologies/ml-agents/releases

## 11. sample-factory

- **O que e:** RL de alto throughput (PPO sincrono/assincrono), de Aleksei Petrenko. MIT. Versao 2; Linux e macOS (sem Windows).
- **Estado:** publico, 1,4 mil commits, 30 issues abertas; release/data: (verificar).
- **O que o katana aproveita:** nada.

Exemplo minimo (README): `python -m sf_examples.mujoco.train_mujoco --env=mujoco_ant --experiment=Ant --train_dir=./train_dir`
Fonte: https://github.com/alex-petrenko/sample-factory

## 12. Scikit-learn

- **O que e:** biblioteca padrao de ML classico (classificacao, regressao, clustering, pre-processamento, validacao). Projeto comunitario, BSD (verificar na pagina).
- **Estado:** versao estavel 1.9.1 (docs). Data e requisito de Python: (verificar).
- **O que o katana aproveita:** em scripts de apoio (avaliacao, TF-IDF/BM25 prototipo, KMeans de embeddings, metricas). Nao entra no motor C.

Exemplo minimo:
```python
from sklearn.feature_extraction.text import TfidfVectorizer
X = TfidfVectorizer().fit_transform(["gato preto","cachorro branco"])
```
Fonte: https://scikit-learn.org/stable/

---

## 13. As quatro capacidades, caminho mais curto

Todas as estimativas de velocidade abaixo sao **hipoteses a medir** na RTX 3050 4 GB + i7; nenhuma foi medida.

### (a) Ver imagens

**Como funciona hoje no ecossistema:** dois GGUFs. O modelo de linguagem normal e um `mmproj` com o codificador de imagem (ViT) e o projetor. Fluxo documentado no llama.cpp (`tools/mtmd`): pre-processamento da imagem, ViT gera features, projetor converte em embeddings, os embeddings entram no LLM como tokens. Os `mmproj` sao especificos por arquitetura e gerados por `convert_hf_to_gguf.py --mmproj`.

**Qwen3.6:** a ficha do Qwen3.6-35B-A3B diz "Causal Language Model with Vision Encoder" (imagem e video, Apache-2.0). Do `config.json`: `vision_config` com depth 27, hidden 1152, patch 16, `spatial_merge_size` 2, `out_hidden_size` 2048 (= dimensao do LLM). Isso aponta para ~0,4 B de parametros na torre (**calculo meu, estimativa**, ordem de grandeza de um SigLIP-so400m) e cada bloco de 2x2 patches vira 1 token. Repos GGUF comunitarios listam `mmproj` de ~0,9 a 1,8 GB (F16/BF16/F32) para o 27B (fonte secundaria); alguns repos nao trazem mmproj (verificar o arquivo antes). Ha relatos de conflito entre visao e MTP em builds antigas (verificar).

**Caminho mais curto no katana:**
1. Ler o `mmproj` GGUF (o leitor de GGUF ja existe) e implementar o ViT em C: patch embed (conv 16x16 como matmul), atencao completa com posicao 2D/RoPE (verificar detalhe exato lendo a arquitetura), MLP, merge 2x2, projetor. Reuso dos kernels de matmul/atencao ja existentes.
2. Pre-processar imagem (redimensionar mantendo proporcao, normalizar) em C; decodificar JPEG/PNG com um decodificador de dominio publico de 1 arquivo (ex.: stb_image, verificar licenca/adequacao a regra "nada de servicos externos": e so codigo local).
3. Injetar os embeddings no lugar de tokens-marcador no prefill (o Qwen usa posicoes M-RoPE para imagens: **verificar**).
4. Validar com tensores de referencia (timm/transformers) por camada, como o `selftest` ja faz com texto.

**Tamanho/velocidade esperados (estimativa):** o ViT corre uma vez por imagem; ~0,4 B de parametros e algumas centenas de tokens visuais por imagem de ~512x512 (um token por 32x32 pixels, aritmetica a partir de patch 16 e merge 2) significa uns poucos segundos na GPU 3050 se o codificador couber; a carga real e o prefill extra no LLM. Em 4 GB, o `mmproj` em F16 (~0,9 GB) disputa memoria com o modelo; rodar o ViT na GPU e depois descarregar, ou rodar no CPU, sao opcoes. **Medir.** Alternativa menor para testar primeiro: um VLM pequeno com projetor simples (SmolVLM, listado como suportado pelo llama.cpp).

### (b) Ouvir e falar

**Ouvir (ASR).**
- whisper.cpp: formato ggml proprio (nao GGUF; verificar se ja ha variante GGUF), CPU/CUDA/Vulkan. Tamanhos da pagina oficial: tiny 75 MiB, base 142 MiB, small 466 MiB, medium 1,5 GiB, large 2,9 GiB; quantizados em `ggerganov/whisper.cpp`: `large-v3-turbo-q5_0` 547 MiB, `q8_0` 834 MiB, `small-q8_0` 252 MiB, `base-q8_0` 78 MiB.
- Moonshine (moonshine-ai): MIT, STT em 10 idiomas, modelos de ate "1 MB" (diretorio `micro/`); afirma precisao maior que Whisper Large V3 segundo placar externo (verificar); ONNX: (verificar).
- ESPnet OWSM v4: pesquisa, pesado.
- **Caminho curto:** portar o Whisper (encoder conformer/transformer sobre mel-espectrograma + decoder autoregressivo) para C reaproveitando atencao e matmul do katana; o decoder e um LLM pequeno com cross-attention. Whisper-small quantizado (~250 MB) ou large-v3-turbo q5_0 (~550 MB) cabem folgado em 4 GB. **Estimativa:** small/turbo transcrevem em tempo bem superior ao real numa 3050; medir.

**Falar (TTS).**
- Kokoro-82M: Apache-2.0, StyleTTS 2 + ISTFTNet, 82 M parametros, 54 voces, 8 idiomas, 24 kHz. Depende de fonemizacao (espeak-ng no exemplo oficial). ONNX nao consta na ficha oficial (ha conversoes comunitarias, verificar).
- llama.cpp ja tem `llama-tts` (originalmente para OuteTTS; hoje "model-agnostic" via `libmtmd`) com exemplo `Qwen3-TTS-12Hz-1.7B-Base-GGUF`. Fontes secundarias citam MioTTS (0,1B ~80 MB em Q4_K_M) e OuteTTS como TTS em GGUF (sem verificacao em repositorio oficial); Kokoro/Piper/Kitten TTS (15M, <25 MB) usam motor proprio.
- **Caminho curto:** TTS baseado em LLM + codec (formato OuteTTS/MioTTS/Qwen3-TTS) e o que mais se aproveita do motor atual, pois o gerador de tokens de audio e um transformer comum; falta apenas o decodificador do codec (convolucional/vocoder) em C. Kokoro e menor e de boa qualidade, mas exige portar StyleTTS2/ISTFTNet inteiros. **Estimativa:** Kokoro roda em tempo real em CPU; medir.

### (c) Gerar imagens (difusao)

- **Referencia pronta:** stable-diffusion.cpp (leejet): C/C++ sobre ggml, GGUF, backends CPU(AVX/AVX2/AVX512)/CUDA/Vulkan/Metal/OpenCL/SYCL. Suporta SD1.x/2.x, SDXL, SD3/3.5, FLUX.1, FLUX.2, Qwen Image, Z-Image, Chroma, edicao (Kontext, Qwen Image Edit) e video (Wan, LTX, HunyuanVideo). Ultima noticia na pagina: 11/11/2026.
- **Receita oficial para 4 GB (wiki, Z-Image):** `z_image_turbo-Q3_K.gguf` (Q4_0 ou Q3_K recomendados para 4 GB; Q2_K pior qualidade) + texto Qwen3-4B-Instruct-2507 Q4_K_M + VAE `ae.safetensors`, com `--offload-to-cpu --diffusion-fa`, resolucao do exemplo 512x1024, `--cfg-scale 1.0`; flags extras `--vae-tiling`, `--clip-on-cpu`. A wiki **nao informa passos nem tempo**; Z-Image Turbo usa poucos passos (8, segundo comparacao de terceiros em 4090: ~2-3 s a 1024 px; nao se traduz para 3050).
- **Tamanhos tipicos:** SD1.5 ~860 M parametros no U-Net (conhecimento geral, verificar); SDXL ~2,6 B; FLUX.1 12 B (a doc do Diffusers cita 12,5 B); Z-Image Turbo e menor (parametros: verificar).
- **Caminho curto para o katana:** nao portar. Chamar `sd-cli` como ferramenta externa do agente (ja ha cliente MCP/ferramentas), com confirmacao do usuario. Portar difusao para `katana.c` so se virar objetivo: exigiria U-Net/DiT, VAE, agendadores, e o text encoder (que no Z-Image e um LLM Qwen3-4B, reaproveitavel pelo motor atual).
- **Expectativa na 3050 4 GB (estimativa):** SD1.5 q8/q4 512x512 em segundos por dezenas de segundos; Z-Image Q3_K com offload, dezenas de segundos a minutos (offload para CPU custa; **medir**).

### (d) Ler documentos escaneados (OCR)

**Caminho mais curto de todos:** PaddleOCR-VL-1.5/1.6 (0,9 B) ja existe como GGUF + mmproj no Hub e roda no llama.cpp. Ou seja, assim que o katana tiver a pilha de visao do item (a) (ViT + projetor, aqui de arquitetura propria do PaddleOCR-VL, outro mmproj: **cada arquitetura exige seu codificador**), OCR e so um prompt (`OCR:`, `Table Recognition:`...). Imediato, sem portar nada: usar `llama-mtmd-cli`/`llama-server` como ferramenta externa.

**Alternativa leve sem VLM:** PP-OCRv5/v6 (deteccao + reconhecimento, 2 M parametros no reconhecedor multilingue) em ONNX; exige ONNX Runtime ou portar CNN pequena (viavel em C, mas com pos-processamento de deteccao e CTC).

**Tamanho/velocidade (estimativa):** 0,9 B em Q4/Q8 cabe em 4 GB com folga; uma pagina A4 gera algumas centenas a poucos milhares de tokens; ordem de dezenas de tokens/s numa 3050 mais o ViT por pagina: segundos a dezenas de segundos por pagina. Medir; observar o risco de repeticao citado nos cartoes.

---

## 14. Ordem sugerida

1. Agora, sem codigo: ferramentas externas (`llama-mtmd-cli` com PaddleOCR-VL para (d); `whisper.cpp`; `sd-cli`) integradas pelo agente/MCP existente.
2. Em C: leitor de `mmproj` + ViT + injecao de embeddings (serve (a) e (d) e e a base de qualquer VLM).
3. Em C: Whisper (ASR) pequeno, aproveitando atencao/matmul.
4. TTS: decodificador de codec para TTS baseado em LLM.
5. Difusao: so por demanda.
6. RL/ML classico: ignorar para o motor; usar em scripts.

## Fontes consultadas
- https://github.com/huggingface/diffusers · https://huggingface.co/docs/diffusers/main/en/optimization/memory
- https://github.com/huggingface/pytorch-image-models
- https://github.com/mlfoundations/open_clip
- https://github.com/PaddlePaddle/PaddleOCR · https://huggingface.co/PaddlePaddle/PaddleOCR-VL-1.5-GGUF
- https://github.com/espnet/espnet · https://github.com/speechbrain/speechbrain · https://github.com/asteroid-team/asteroid
- https://github.com/pyannote/pyannote-audio
- https://github.com/DLR-RM/stable-baselines3 · https://github.com/Unity-Technologies/ml-agents (+/releases) · https://github.com/alex-petrenko/sample-factory
- https://scikit-learn.org/stable/whats_new.html
- https://github.com/ggml-org/llama.cpp/blob/master/tools/mtmd/README.md · https://github.com/ggml-org/llama.cpp/tree/master/tools/tts
- https://github.com/ggml-org/whisper.cpp · https://huggingface.co/ggerganov/whisper.cpp
- https://github.com/leejet/stable-diffusion.cpp · https://github.com/leejet/stable-diffusion.cpp/wiki/How-to-Use-Z%E2%80%90Image-on-a-GPU-with-Only-4GB-VRAM
- https://huggingface.co/Qwen/Qwen3.6-35B-A3B (+ config.json)
- https://huggingface.co/hexgrad/Kokoro-82M · https://github.com/moonshine-ai/moonshine
- Fontes secundarias (blogs, cartoes comunitarios), usadas so como indicio: MioTTS (lilting.ch), OuteTTS, repos GGUF comunitarios do Qwen3.6 e do PaddleOCR-VL-1.6.
