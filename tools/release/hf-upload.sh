#!/bin/sh
# Publica os modelos convertidos no Hugging Face, um repositorio por grupo, um commit por repositorio.
#
# Uso: sh tools/release/hf-upload.sh <organizacao-ou-usuario> [--yes] [--public] [--only grupo[,grupo]] [--models pasta]
#
#   <organizacao-ou-usuario>  obrigatorio, sem padrao (os repos ficam em <dono>/katana-...)
#   --yes                     sem isto so mostra o que enviaria (arquivos e tamanhos) e sai
#   --public                  cria os repos publicos; sem isto sao privados
#   --only                    grupos: flux2-klein, sd15, embeddinggemma (padrao: todos que tem cartao em cards/)
#   --models                  pasta models/ (padrao: models/ na raiz do repositorio)
#
# Pre-requisitos: CLI "hf" instalada e "hf auth login" feito. Nada e enviado sem --yes.
# Ao fim imprime o commit sha (revisao) de cada envio, para colar em src/app/catalog.c.
# Grupos com licenca BLOCKED (ver LICENSES.md) nao tem cartao em cards/ e por isso sao pulados.

set -u
AQUI=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$AQUI/../.." && pwd)

DONO=""; YES=0; PUBLICO=0; SO=""; MODELOS="$RAIZ/models"
while [ $# -gt 0 ]; do
  case "$1" in
    --yes) YES=1 ;;
    --public) PUBLICO=1 ;;
    --only) shift; SO=${1:-} ;;
    --models) shift; MODELOS=${1:-} ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    -*) echo "opcao desconhecida: $1" >&2; exit 2 ;;
    *) if [ -z "$DONO" ]; then DONO=$1; else echo "argumento a mais: $1" >&2; exit 2; fi ;;
  esac
  shift
done
[ -n "$DONO" ] || { echo "erro: informe a organizacao ou o usuario do Hugging Face como primeiro argumento" >&2; echo "uso: sh tools/release/hf-upload.sh <dono> [--yes] [--public]" >&2; exit 2; }

command -v hf >/dev/null 2>&1 || { echo "erro: a CLI 'hf' nao foi encontrada. Instale com: pip install -U huggingface_hub" >&2; exit 1; }
QUEM=$(hf auth whoami 2>/dev/null) || QUEM=""
[ -n "$QUEM" ] || { echo "erro: voce nao esta logado no Hugging Face. Rode: hf auth login" >&2; exit 1; }
[ -d "$MODELOS" ] || { echo "erro: pasta de modelos nao encontrada: $MODELOS" >&2; exit 1; }

# arquivos de cada grupo (separados por espaco; caminhos sem espaco)
arquivos_do_grupo() {
  case "$1" in
    flux2-klein) ls "$MODELOS"/flux2klein/gguf/*.gguf 2>/dev/null ;;
    sd15) ls "$MODELOS"/render/sd15-*.gguf 2>/dev/null ;;
    embeddinggemma) ls "$MODELOS"/embeddinggemma-2/embeddinggemma-2-q8_0.gguf 2>/dev/null ;;
  esac
}
repo_do_grupo() {
  case "$1" in
    flux2-klein) echo "katana-flux2-klein" ;;
    sd15) echo "katana-sd15" ;;
    embeddinggemma) echo "katana-embeddinggemma" ;;
  esac
}
cartao_do_grupo() { echo "$AQUI/cards/$(repo_do_grupo "$1").md"; }

TODOS="flux2-klein sd15 embeddinggemma"
if [ -n "$SO" ]; then GRUPOS=$(echo "$SO" | tr ',' ' '); else GRUPOS=$TODOS; fi

# plano
if [ "$PUBLICO" = 1 ]; then VIS=publico; else VIS=privado; fi
echo "Dono: $DONO (logado como: $QUEM)   visibilidade: $VIS"
LISTA=""
for g in $GRUPOS; do
  case " $TODOS " in *" $g "*) ;; *) echo "erro: grupo desconhecido: $g" >&2; exit 2 ;; esac
  if [ ! -f "$(cartao_do_grupo "$g")" ]; then
    echo "PULADO  $g: sem cartao em tools/release/cards (licenca BLOCKED? ver tools/release/LICENSES.md)"
    continue
  fi
  fs=$(arquivos_do_grupo "$g")
  [ -n "$fs" ] || { echo "erro: nenhum arquivo para o grupo $g em $MODELOS" >&2; exit 1; }
  echo ""
  echo "Repositorio $DONO/$(repo_do_grupo "$g")  ($VIS)"
  echo "  README.md  <- tools/release/cards/$(repo_do_grupo "$g").md"
  for f in $fs; do
    printf '  %-45s %14s bytes\n' "$(basename "$f")" "$(wc -c < "$f" | tr -d ' ')"
  done
  LISTA="$LISTA $g"
done
[ -n "$LISTA" ] || { echo "nada para enviar" >&2; exit 1; }

if [ "$YES" != 1 ]; then
  echo ""
  echo "Nada foi enviado. Rode de novo com --yes para enviar."
  exit 0
fi

SAIDA=""
for g in $LISTA; do
  repo="$DONO/$(repo_do_grupo "$g")"
  stage=$(mktemp -d) || exit 1
  ln -s "$(cartao_do_grupo "$g")" "$stage/README.md"
  for f in $(arquivos_do_grupo "$g"); do ln -s "$f" "$stage/$(basename "$f")"; done
  echo ""
  echo ">> criando $repo ($VIS)"
  if [ "$PUBLICO" = 1 ]; then
    hf repo create "$repo" --repo-type model --exist-ok || { rm -rf "$stage"; exit 1; }
  else
    hf repo create "$repo" --repo-type model --private --exist-ok || { rm -rf "$stage"; exit 1; }
  fi
  echo ">> enviando $repo (um commit)"
  log=$(mktemp)
  if ! hf upload "$repo" "$stage" . --repo-type model --commit-message "katana: $g (conversao de formato, sem retreino)" | tee "$log"; then
    rm -rf "$stage" "$log"; echo "erro: o envio de $repo falhou" >&2; exit 1
  fi
  rev=$(grep -o '/\(commit\|tree\)/[0-9a-f]\{40\}' "$log" | tail -n 1 | sed 's|.*/||')
  rm -rf "$stage" "$log"
  [ -n "$rev" ] || rev="(nao identificado na saida; veja o historico de commits do repo no site)"
  SAIDA="$SAIDA$repo  $rev
"
done

echo ""
echo "Revisoes (cole em src/app/catalog.c, campos repo e rev):"
printf '%s' "$SAIDA"
