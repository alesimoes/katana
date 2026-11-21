#!/bin/sh
# Motor de mentira para o selftest do desktop (--selftest-standin): fala o mesmo protocolo de marcadores do
# `katana agent`, sem modelo e sem GPU. Os pedidos escolhem o comportamento pelo texto.
case "$1" in
hw) printf 'ram_gb\t64\ngpu\t-\nvram_gb\t0\ndisk_gb\t100\n'; exit 0;;
catalog|pull) exit 0;;
esac
[ "$1" = agent ] || exit 0

# especializacoes: a pasta de KATANA_SPEC e a fonte da verdade; "outro-*" simula um .kspec de outro modelo base
if [ -n "$KATANA_SPEC" ] && [ -d "$KATANA_SPEC" ]; then
    n=0; m=0
    for f in "$KATANA_SPEC"/*.kspec; do
        [ -e "$f" ] || continue
        m=$((m+1))
        case "$(basename "$f")" in
        outro-*) echo "especializacao pulada (treinada sobre outro modelo base): $f" >&2;;
        *) echo "especializacao: $f (12 tensores)" >&2; n=$((n+1));;
        esac
    done
    echo "especializacoes de $KATANA_SPEC: $n aplicadas de $m" >&2
fi
echo "arquitetura=qwen35moe ctx=4096" >&2
printf '\001READY\n'
while IFS= read -r line; do
    case "$line" in
    /exit) exit 0;;
    /session*|/cd*) ;;                                   # comandos silenciosos: sem READY
    /*) printf '\001READY\n';;
    *arquivos*)
        printf '\001TOOL notify\t{"message":"pronto"}\n'
        printf '\001NOTIFY Tarefa concluida\tO relatorio ficou pronto\n'
        printf '\001FILE auto\t/tmp/katana-standin/relatorio final.txt\tRelatorio da analise\n'
        printf '\001TOOL report_findings\t{}\n'
        printf '\001FINDING src/engine/a.c\t42\tbug\tIndice fora do limite no laco\n'
        printf '\001FINDING /tmp/katana-standin/b.c\t\tstyle\tNome pouco claro\n'
        printf 'Entreguei o arquivo e os achados.\n'
        printf '\001READY\n';;
    *mudo*)
        printf '\001TOOL list_dir\t{"path":"."}\n'
        printf '\001READY\n';;
    *"parou sem"*)
        printf 'Resumo: listei a pasta; falta conferir os resultados.\n'
        printf '\001READY\n';;
    *)
        printf 'Resposta de teste.\n'
        printf '\001READY\n';;
    esac
done
