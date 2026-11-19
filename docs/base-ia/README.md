# Base de conhecimento de desenvolvimento para IA

Estudo das bibliotecas listadas no Hugging Face Hub, feito em novembro de 2026 a partir de fontes
primárias (documentação oficial, repositórios, PyPI). O que não teve fonte está marcado "(verificar)".

| Arquivo | Conteúdo |
|---|---|
| [frameworks.md](frameworks.md) | PyTorch, TensorFlow, JAX, Keras, MLX, PaddlePaddle, fastai, Habana, Graphcore, Rust/candle, Microduck, Asimov, univa |
| [formatos-e-runtimes.md](formatos-e-runtimes.md) | GGUF (tipos e chaves completos), Safetensors (layout exato), ONNX, OpenVINO, Core ML, LiteRT, ExecuTorch, llamafile, Transformers.js, unity-sentis, DDUF, Joblib, TensorBoard |
| [nlp-e-llm.md](nlp-e-llm.md) | Transformers (arquiteturas, chat templates, tokenizadores), PEFT/Adapters (LoRA), sentence-transformers (embeddings), spaCy, NeMo, Flair, Stanza, fastText, BERTopic, AllenNLP, Fairseq, SpanMarker, setfit, paddlenlp |
| [visao-audio-difusao-rl.md](visao-audio-difusao-rl.md) | Diffusers, timm, OpenCLIP, PaddleOCR, ESPnet, speechbrain, Asteroid, pyannote, stable-baselines3, ml-agents, sample-factory, scikit-learn; caminhos para ver, ouvir/falar, gerar imagens e OCR |

Ver também `../providers-study.md` (provedores de inferência e modelos 2025–2026).

## O que vira trabalho no katana (ordem sugerida)

1. Teste dourado de tokenização e template de conversa contra a biblioteca de referência.
2. Ler safetensors direto do Hub (sem converter para GGUF).
3. Tipos GGUF novos (MXFP4, IQ*, NVFP4, Q1_0/Q2_0, F8_E4M3) — em andamento.
4. Memória do agente por embeddings (Qwen3-Embedding 0.6B / EmbeddingGemma em GGUF), busca híbrida.
5. LoRA em tempo de execução.
6. Visão (mmproj do Qwen3.6) e OCR; depois fala (ASR pequeno + TTS).
7. Executável único com o modelo anexado.
