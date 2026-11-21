#!/bin/sh
# organiza os arquivos de uma pasta em subpastas por extensao
if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "uso: $0 PASTA (a pasta precisa existir)" >&2
    exit 1
fi
dir=${1%/}
[ -n "$dir" ] || dir=/
n=0
for f in "$dir"/*; do
    [ -f "$f" ] || continue
    name=${f##*/}
    case $name in
        *.?*) ext=${name##*.}; base=${name%.*}; dot=. ;;
        *) ext=; base=$name; dot= ;;
    esac
    if [ -n "$ext" ]; then
        sub=$(printf '%s' "$ext" | tr 'A-Z' 'a-z')
    else
        sub=sem-extensao
    fi
    mkdir -p "$dir/$sub"
    target="$dir/$sub/$name"
    i=0
    while [ -e "$target" ]; do
        i=$((i + 1))
        target="$dir/$sub/$base-$i$dot$ext"
    done
    mv -- "$f" "$target"
    n=$((n + 1))
done
case $n in
    0) echo "nenhum arquivo" ;;
    1) echo "1 arquivo movido" ;;
    *) echo "$n arquivos movidos" ;;
esac
