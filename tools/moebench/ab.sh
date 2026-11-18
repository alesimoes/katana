#!/bin/sh
# A/B alternado do caminho de CPU dos especialistas: KATANA_MOE_OLD=1 (anterior) e 0 (novo).
# Uso: tools/moebench/ab.sh <modelo.gguf> [rodadas] [tokens]; mostra load, ms/token e fases.
cd "$(dirname "$0")/../.."
M=$1; N=${2:-3}; T=${3:-16}
uptime
for i in $(seq "$N"); do
  for o in 1 0; do
    printf "OLD=%s " "$o"
    KATANA_MOE_OLD=$o MOEB_NOBW=1 MOEB_PROF=1 ./tools/moebench/moebench "$M" "$T" 3 2>&1 | grep -A1 "rep 2" | tr '\n' ' '
    echo
  done
done
uptime
