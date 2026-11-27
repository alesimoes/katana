#!/bin/sh
# Compila o katana no Linux e gera dist/katana-linux-x86_64.tar.gz (nada e enviado a lugar nenhum).
# Conteudo: katana, README.md, LICENSE (se existir) e data/coder/kb (o binario procura em data/coder/kb ao lado do executavel).
# Os shaders Vulkan, a pagina web e os kernels CUDA (PTX) ja vao dentro do binario; as bibliotecas de GPU
# (Vulkan, CUDA, OpenCL) sao carregadas em tempo de execucao com dlopen, so se existirem na maquina.
# O aplicativo desktop (desktop-qt) fica de fora.
set -e
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
cd "$RAIZ"
NOME=katana-linux-x86_64

sh build.sh
[ -x ./katana ] || { echo "erro: build.sh nao gerou ./katana" >&2; exit 1; }

echo "Bibliotecas compartilhadas necessarias (ldd):"
ldd ./katana

rm -rf "dist/$NOME"
mkdir -p "dist/$NOME/data/coder"
cp katana "dist/$NOME/katana"
cp README.md "dist/$NOME/README.md"
[ -f LICENSE ] && cp LICENSE "dist/$NOME/LICENSE"
[ -f LICENSE.md ] && cp LICENSE.md "dist/$NOME/LICENSE.md"
cp -R data/coder/kb "dist/$NOME/data/coder/kb"

tar -C dist -czf "dist/$NOME.tar.gz" "$NOME"
rm -rf "dist/$NOME"
echo ""
echo "Gerado: dist/$NOME.tar.gz"
ls -l "dist/$NOME.tar.gz"
sha256sum "dist/$NOME.tar.gz"
