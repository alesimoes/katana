#!/bin/sh
# Compila o katana-desktop (Qt6). Uso: ./build.sh  -> desktop-qt/build/katana-desktop-qt
# Para instalar no lugar do app antigo: cp build/katana-desktop-qt ../katana-desktop
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
echo "pronto: $(pwd)/build/katana-desktop-qt"
