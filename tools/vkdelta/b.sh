#!/bin/sh
# compila a bancada no Linux com o gcc de ~/.local/tc (shaders: sh gen.sh antes)
cd "$(dirname "$0")"
R=$HOME/.local/tc/root
PATH=$R/usr/bin:$PATH LD_LIBRARY_PATH=$R/usr/lib/x86_64-linux-gnu gcc-15 -O2 -Wall -isystem $R/usr/include \
  -isystem $R/usr/include/x86_64-linux-gnu -B$R/usr/lib/x86_64-linux-gnu -L$R/usr/lib/x86_64-linux-gnu \
  -I ../../vk/include -o vkdelta vkdelta.c -lm -ldl
