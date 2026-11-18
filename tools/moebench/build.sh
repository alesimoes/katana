#!/bin/sh
# compila a microbancada do MoE na CPU (mesmas opcoes do build.sh); saida: tools/moebench/moebench
cd "$(dirname "$0")/../.."
exec gcc -O2 -mavx2 -mfma -mf16c -ffast-math -Wall -Wno-unused -I . -I vk/include tools/moebench/moebench.c -o tools/moebench/moebench${OUT:-} -lm -lpthread -ldl "$@"
