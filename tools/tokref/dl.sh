#!/bin/sh
# Baixa as dependencias da referencia (wheels publicos do PyPI, sem pip) e os arquivos publicos dos tokenizadores.
# Nada e instalado no sistema: tudo fica em tools/tokref/{vendor,dl} (fora do git).
cd "$(dirname "$0")" || exit 1
mkdir -p vendor dl
python3 -I - <<'PY'
import json, urllib.request, zipfile, io, sys
for pkg, pick in (("tokenizers", lambda f: "abi3" in f and "manylinux" in f and "x86_64" in f),
                  ("jinja2", lambda f: "none-any" in f),
                  ("markupsafe", lambda f: "cp314-cp314-manylinux" in f and "x86_64" in f)):   # ajuste cp3NN ao python3 local
    d = json.load(urllib.request.urlopen("https://pypi.org/pypi/%s/json" % pkg))
    u = [f["url"] for f in d["urls"] if f["filename"].endswith(".whl") and pick(f["filename"])][0]
    zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(u).read())).extractall("vendor")
    print("ok", pkg)
PY
get() { mkdir -p "dl/$2"; for f in $3; do curl -sL -o "dl/$2/$f" -w "$1 $f %{http_code}\n" "https://huggingface.co/$1/resolve/main/$f"; done; }
get Qwen/Qwen2.5-1.5B-Instruct Qwen2.5-1.5B-Instruct "tokenizer.json tokenizer_config.json"
get Qwen/Qwen3-30B-A3B-Instruct-2507 Qwen3-30B-A3B-Instruct-2507 "tokenizer.json tokenizer_config.json"
get Qwen/Qwen3.6-35B-A3B Qwen3.6-35B-A3B "tokenizer.json tokenizer_config.json chat_template.jinja"
get deepseek-ai/DeepSeek-V2-Lite-Chat DeepSeek-V2-Lite-Chat "tokenizer.json tokenizer_config.json"
get moonshotai/Moonlight-16B-A3B-Instruct Moonlight-16B-A3B-Instruct "tokenizer_config.json tokenization_moonshot.py tiktoken.model"   # nao tem tokenizer.json
get allenai/OLMoE-1B-7B-0924-Instruct OLMoE-1B-7B-0924-Instruct "tokenizer.json tokenizer_config.json"   # o GGUF testado e o 0924
get allenai/OLMoE-1B-7B-0125-Instruct OLMoE-1B-7B-0125-Instruct "tokenizer.json tokenizer_config.json"
