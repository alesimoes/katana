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
