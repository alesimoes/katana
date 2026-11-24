#!/bin/sh
# Motor de mentira para o selftest do desktop (--selftest-standin): fala o mesmo protocolo de marcadores do
# `katana agent`, sem modelo e sem GPU. Os pedidos escolhem o comportamento pelo texto.
case "$1" in
hw) printf 'ram_gb\t64\ngpu\t-\nvram_gb\t0\ndisk_gb\t100\n'; exit 0;;
catalog|pull) exit 0;;
render)
    # `katana render generate --engine E ... --out ARQ`: stable grava a imagem de amostra; flux e krea saem com 3 (WIP), como o motor real;
    # um prompt com a palavra "falhe" faz o stable falhar (codigo 2)
    [ "$2" = generate ] || exit 1
    shift 2; E=stable; OUT=; PF=; NL=
    while [ $# -gt 0 ]; do
        case "$1" in --engine) E=$2; shift;; --out) OUT=$2; shift;; --prompt-file) PF=$2; shift;; --nl) NL=$2; shift;; esac
        shift
    done
    case "$E" in
    flux|krea) echo "render: o motor $E esta em desenvolvimento (WIP), ainda nao disponivel" >&2; exit 3;;
    esac
    if { [ -n "$PF" ] && grep -q falhe "$PF"; } || case "$NL" in *falhe*) true;; *) false;; esac; then echo "render: nao achei os pesos do Stable" >&2; exit 2; fi
    cp "$(dirname "$0")/samples/render-saida.png" "$OUT"; exit 0;;
coder)
    # `katana coder status <pasta>`: saida real (tools/samples/coder-status.txt); a primeira consulta de cada execucao responde com o
    # rastreador em andamento (N2 em 40%), as seguintes com o indice pronto
    if [ "$2" = status ]; then
        D=$(dirname "$0")/samples
        C="${HOME:-/tmp}/.standin-idx-count"      # o pedido "consulte o indice" cria o arquivo; sem ele o indice esta pronto
        n=-1; [ -f "$C" ] && n=$(cat "$C")
        [ "$n" -ge 0 ] && echo $((n+1)) > "$C"
        if [ "$n" -eq 0 ]; then
            sed -e 's/N2 simbolos  \([0-9]*\) de \([0-9]*\) elegiveis (100%)/N2 simbolos  12 de \2 elegiveis (40%)/' \
                -e 's/rastreador: nao esta rodando/rastreador: em execucao (CRAWL.LOCK tomado)/' "$D/coder-status.txt"
        else
            cat "$D/coder-status.txt"
        fi
    fi
    exit 0;;
esac
[ "$1" = agent ] || exit 0

# saida de uma ferramenta copiada de um arquivo real (tools/samples), com \n escapado como o motor faz; $2 = nota opcional no fim
emit_out() {
    printf '\001TOOLOUT %s\t' "$1"
    awk 'NR>1{printf "\\n"} {printf "%s",$0}' "$(dirname "$0")/samples/$2"
    [ -n "$3" ] && printf '\\n%s' "$3"
    printf '\n'
}

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
    *"execute o plano"*)
        # pedido com plano de 3 passos: chamadas com e sem descricao, uma que falha (com notas do motor no resultado),
        # mudanca do plano (passo reescrito, passo novo, passo descartado) e o fim do pedido
        printf '\001TOOL Read\t{"file_path":"README.md"}\n'
        printf '\001TOOLOUT Read\t# katana\\nmotor de inferencia\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [ ] Ler o projeto\\n[ ] Corrigir o erro\\n[ ] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved: 0 of 3 done. Next: Ler o projeto\n'
        sleep 0.3
        printf '\001TOOL Bash\t{"command":"ls -la src","description":"Listar os arquivos da pasta"}\tListar os arquivos da pasta\n'
        printf '\001TOOLOUT Bash\ttotal 8\\nmain.c\\nutil.c\\n\n'
        printf '\001TOOL Read\t{"file_path":"src/main.c"}\n'
        printf '\001TOOLOUT Read\tint main() { return 0; }\\n\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [x] Ler o projeto\\n[ ] Corrigir o erro\\n[ ] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved: 1 of 3 done. Next: Corrigir o erro\\n[plan: step 2 of 3 in progress: Corrigir o erro; mark it completed with TodoWrite when done]\n'
        sleep 0.3
        printf '\001TOOL Bash\t{"command":"cc -o app src/main.c && ./app","description":"Compilar e rodar o programa"}\tCompilar e rodar o programa\n'
        printf '\001TOOLOUT Bash\terror: cc: command not found\\n[katana: 3 calls in a row failed. Stop guessing. What do the errors have in common?]\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [x] Ler o projeto\\n[ ] Corrigir o erro de leitura\\n[ ] Escrever o teste novo\\n[ ] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved\n'
        printf '\001TOOL Edit\t{"file_path":"src/main.c","old_string":"return 0","new_string":"return 1"}\n'
        printf '\001TOOLOUT Edit\tedited src/main.c\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [x] Ler o projeto\\n[x] Corrigir o erro de leitura\\n[ ] Escrever o teste novo\\n[ ] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved\n'
        printf '\001TOOL Bash\t{"command":"touch tests/novo.sh","description":"Criar o arquivo do teste novo"}\tCriar o arquivo do teste novo\n'
        printf '\001TOOLOUT Bash\t(no output) Exit code 0\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [x] Ler o projeto\\n[x] Corrigir o erro de leitura\\n[ ] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved\n'
        sleep 0.3
        printf '\001TOOL Bash\t{"command":"sleep 3; ./run-tests.sh","description":"Rodar a suite de testes"}\tRodar a suite de testes\n'
        sleep 3
        printf '\001TOOLOUT Bash\t12 passed\\nExit code 0\n'
        printf '\001TOOL TodoWrite\t{"todos":[...]}\n'
        printf '\001TODO [x] Ler o projeto\\n[x] Corrigir o erro de leitura\\n[x] Rodar os testes\n'
        printf '\001TOOLOUT TodoWrite\tlist saved: all 3 steps done\n'
        printf 'Pronto: li o projeto, corrigi o erro e os testes passam.\n'
        printf '\001READY\n';;
    *"consulte o indice"*)
        # Find e Symbols com saidas reais do katana (tools/samples); a ultima chamada leva uma nota longa do motor; Symbols flow,
        # Symbols flow e tree so conferem o verbo e a acao; o motor avisa em stderr que anexou o bloco [katana: context ...]
        echo 0 > "${HOME:-/tmp}/.standin-idx-count"
        echo "[context: about 640 tokens from the project index, 2 verified step(s) (UIOE; KATANA_UIOE=off turns it off)]" >&2
        sleep 4
        printf '\001TOOL Find\t{"query":"ct_sb_puts"}\n'
        emit_out Find find.txt
        printf '\001TOOL Find\t{"query":"\\"zzqqnotthere\\"","mode":"refs"}\n'
        emit_out Find find-nf.txt
        printf '\001TOOL Symbols\t{"action":"callers","name":"ct_sb_puts"}\n'
        emit_out Symbols symbols.txt
        printf '\001TOOL Symbols\t{"action":"def","name":"nonexistent_fn","file":"tools/glob.c"}\n'
        emit_out Symbols symbols-nf.txt '[katana: this exact call already gave this exact result (2 times now). Repeating it changes nothing. Do something different: read the message above, change the arguments, check the command'"'"'s help, use another tool, or tell the user what is blocking you.]'
        printf '\001TOOL Symbols\t{"action":"flow","name":"ct_glob"}\n'
        printf '\001TOOLOUT Symbols\tflow: ct_glob -> ct_gmatch\n'
        printf '\001TOOL Symbols\t{"action":"tree","path":"src"}\n'
        printf '\001TOOLOUT Symbols\tsrc/ (12 files)\\n  engine/ (30 files)\n'
        printf 'Consultei o indice.\n'
        printf '\001READY\n';;
    *"morra agora"*)
        echo "erro: nao foi possivel mapear o modelo (teste do standin)" >&2
        exit 3;;
    *mudo*)
        printf '\001TOOL Bash\t{"command":"ls"}\n'
        printf '\001READY\n';;
    *"parou sem"*)
        printf 'Resumo: listei a pasta; falta conferir os resultados.\n'
        printf '\001READY\n';;
    *)
        printf 'Resposta de teste.\n'
        printf '\001READY\n';;
    esac
done
