#!/bin/sh
# Testa EnterWorktree, ExitWorktree, PowerShell, PushNotification, SendUserFile e EndConversation.
# Uso: sh tools/tooltest/misc.sh /caminho/do/katana
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
T=$(cd "$T" && pwd -P)
N=0; F=0
ok() { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; echo "---- saida:"; echo "$2"; echo "----"; }
has() { case "$2" in *"$3"*) ok "$1";; *) bad "$1 (esperava: $3)" "$2";; esac; }
hasnt() { case "$2" in *"$3"*) bad "$1 (nao esperava: $3)" "$2";; *) ok "$1";; esac; }
eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (esperava '$3', veio '$2')" ""; }
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
unset GIT_DIR GIT_WORK_TREE KATANA_UI KATANA_CONFIRM KATANA_NO_NOTIFY

# ---------- worktree ----------
mkdir "$T/plain" && cd "$T/plain" || exit 2
o=$("$K" tool EnterWorktree '{}' 2>&1); has "Enter fora de repositorio" "$o" "error: not a git repository"
o=$("$K" tool ExitWorktree '{}' 2>&1); has "Exit sem sessao" "$o" "error: no worktree session is active"

mkdir "$T/repo" && cd "$T/repo" || exit 2
git init -q -b main . && echo a > a.txt && git add a.txt && git commit -qm um
R=$(pwd -P)
o=$("$K" tool EnterWorktree '{"name":"bad name"}' 2>&1); has "nome com espaco" "$o" "error: invalid worktree name"
o=$("$K" tool EnterWorktree '{"name":"a/b"}' 2>&1); has "nome com barra" "$o" "error: invalid worktree name"
o=$("$K" tool ExitWorktree '{"action":"explode"}' 2>&1); has "action invalida" "$o" "error: invalid action"

o=$("$K" tool EnterWorktree '{"name":"feat-1"}' EnterWorktree '{"name":"outro"}' ExitWorktree '{}' 2>&1)
has "criar" "$o" "created worktree $R/.katana/worktrees/feat-1 on branch worktree-feat-1"
has "ja dentro de uma sessao" "$o" "error: already inside a worktree session"
has "keep (padrao)" "$o" "were kept on disk"
[ -d "$R/.katana/worktrees/feat-1" ] && ok "pasta do worktree existe" || bad "pasta do worktree existe" ""
git branch --list worktree-feat-1 | grep -q . && ok "ramo existe" || bad "ramo existe" ""
o=$("$K" tool EnterWorktree '{"name":"feat-1"}' 2>&1); has "nome ja existente" "$o" "already exists"

# forma path
o=$("$K" tool EnterWorktree '{"path":"'"$T"'"}' 2>&1); has "path nao e worktree" "$o" "is not a worktree of this repository"
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/feat-1"}' ExitWorktree '{}' 2>&1)
has "path: entra" "$o" "switched into worktree"
has "path: pasta de trabalho muda" "$o" "the working folder is now $R/.katana/worktrees/feat-1 "
has "exit volta" "$o" "working folder is back to $R"

# nome gerado
o=$("$K" tool EnterWorktree '{}' 2>&1); has "nome gerado" "$o" ".katana/worktrees/wt-"
g=$(ls "$R/.katana/worktrees" | grep '^wt-'); [ -n "$g" ] && ok "worktree gerado no disco" || bad "worktree gerado no disco" ""

# remove limpo: apaga pasta e ramo
o=$("$K" tool EnterWorktree '{"name":"limpo"}' ExitWorktree '{"action":"remove"}' 2>&1)
has "remove limpo" "$o" "removed the worktree $R/.katana/worktrees/limpo"
has "remove apaga o ramo" "$o" "Branch worktree-limpo deleted"
[ ! -d "$R/.katana/worktrees/limpo" ] && ok "pasta removida" || bad "pasta removida" ""
if git branch --list worktree-limpo | grep -q .; then bad "ramo removido" ""; else ok "ramo removido"; fi

# remove com mudancas nao commitadas
"$K" tool EnterWorktree '{"name":"sujo"}' >/dev/null 2>&1; echo x > "$R/.katana/worktrees/sujo/novo.txt"
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/sujo"}' ExitWorktree '{"action":"remove"}' 2>&1)
has "remove recusa mudancas" "$o" "error: the worktree has work that remove would lose"
has "lista as mudancas" "$o" "novo.txt"
[ -d "$R/.katana/worktrees/sujo" ] && ok "nada removido apos recusa" || bad "nada removido apos recusa" ""
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/sujo"}' ExitWorktree '{"action":"remove","discard_changes":true}' 2>&1)
has "discard_changes remove" "$o" "removed the worktree"
[ ! -d "$R/.katana/worktrees/sujo" ] && ok "pasta sujo removida" || bad "pasta sujo removida" ""

# o que o interpretador gera sozinho (__pycache__, .pyc sem rastreio) nao segura o remove; um arquivo de verdade ao lado segura
"$K" tool EnterWorktree '{"name":"gerado"}' >/dev/null 2>&1
mkdir -p "$R/.katana/worktrees/gerado/pkg/__pycache__"; echo x > "$R/.katana/worktrees/gerado/pkg/__pycache__/m.cpython-312.pyc"; echo y > "$R/.katana/worktrees/gerado/solto.pyc"
mkdir -p "$R/.katana/worktrees/gerado/__pycache__"; echo z > "$R/.katana/worktrees/gerado/__pycache__/t.pyc"
echo w > "$R/.katana/worktrees/gerado/nota.txt"
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/gerado"}' ExitWorktree '{"action":"remove"}' 2>&1)
has "arquivo de verdade ao lado do __pycache__ segura o remove" "$o" "nota.txt"
hasnt "a recusa nao lista o __pycache__" "$o" "__pycache__"
rm "$R/.katana/worktrees/gerado/nota.txt"
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/gerado"}' ExitWorktree '{"action":"remove"}' 2>&1)
has "__pycache__ e .pyc nao seguram o remove" "$o" "removed the worktree $R/.katana/worktrees/gerado"
[ ! -d "$R/.katana/worktrees/gerado" ] && ok "pasta gerado removida" || bad "pasta gerado removida" ""

# remove com commit fora do ramo original
"$K" tool EnterWorktree '{"name":"commitado"}' >/dev/null 2>&1
(cd "$R/.katana/worktrees/commitado" && echo b > b.txt && git add b.txt && git commit -qm dois)
o=$("$K" tool EnterWorktree '{"path":"'"$R"'/.katana/worktrees/commitado"}' ExitWorktree '{"action":"remove"}' 2>&1)
has "remove recusa commits" "$o" "commits not in the original branch"
has "lista o commit" "$o" "dois"
git branch --list worktree-commitado | grep -q . && ok "ramo mantido apos recusa" || bad "ramo mantido apos recusa" ""

# marcador de interface
o=$(KATANA_UI=1 "$K" tool EnterWorktree '{"name":"ui1"}' ExitWorktree '{}' 2>&1 | cat -vT)
has "marcador CWD ao entrar" "$o" "^ACWD $R/.katana/worktrees/ui1"
has "marcador CWD ao sair" "$o" "^ACWD $R"

# ---------- PowerShell ----------
cd "$T" || exit 2
mkdir "$T/nobin" "$T/fake"
o=$(PATH="$T/nobin" "$K" tool PowerShell '{"command":"Get-Date"}' 2>&1)
has "PowerShell ausente" "$o" "error: PowerShell is not installed"
has "PowerShell ausente: manda usar Bash" "$o" "use the Bash tool"
o=$("$K" tool PowerShell '{}' 2>&1); has "PowerShell sem command" "$o" "error: missing command"
o=$("$K" tool PowerShell '{"command":"x","run_in_background":true}' 2>&1); has "PowerShell background" "$o" "not supported"
cat > "$T/fake/pwsh" <<'EOS'
#!/bin/sh
echo "args: $*"
[ "$4" = "Write-Output 'it's'" ] && exit 3
case "$4" in *sleep*) sleep 5;; esac
exit 0
EOS
chmod +x "$T/fake/pwsh"
o=$(PATH="$T/fake:$PATH" "$K" tool PowerShell '{"command":"Get-Date","description":"d"}' 2>&1)
has "PowerShell roda pwsh" "$o" "args: -NoProfile -NonInteractive -Command Get-Date"
has "PowerShell codigo de saida" "$o" "[exit code 0]"
o=$(PATH="$T/fake:$PATH" "$K" tool PowerShell '{"command":"Write-Output '"'"'it'"'"'s'"'"'"}' 2>&1)
has "PowerShell aspas simples chegam inteiras" "$o" "[exit code 3]"
o=$(PATH="$T/fake:$PATH" "$K" tool PowerShell '{"command":"sleep","timeout":1000}' 2>&1)
has "PowerShell timeout em ms" "$o" "[timeout apos 1 s]"
rm "$T/fake/pwsh"
printf '#!/bin/sh\necho legado\n' > "$T/fake/powershell"; chmod +x "$T/fake/powershell"
o=$(PATH="$T/fake:$PATH" "$K" tool PowerShell '{"command":"x"}' 2>&1); has "PowerShell usa powershell se nao ha pwsh" "$o" "legado"
rm "$T/fake/powershell"

# ---------- PushNotification ----------
o=$("$K" tool PushNotification '{}' 2>&1); has "Push sem message" "$o" "error: missing message"
o=$(PATH="$T/nobin" "$K" tool PushNotification '{"message":"oi"}' 2>&1); has "Push sem canal" "$o" "no notification channel is available"
printf '#!/bin/sh\nfor a in "$@"; do echo "[$a]"; done > "%s/ns.log"\n' "$T" > "$T/fake/notify-send"; chmod +x "$T/fake/notify-send"
o=$(PATH="$T/fake:$PATH" "$K" tool PushNotification '{"message":"pronto","title":"Build"}' 2>&1)
has "Push via notify-send" "$o" "notification shown (notify-send)"
if grep -q '^\[Build\]$' "$T/ns.log" && grep -q '^\[pronto\]$' "$T/ns.log"; then ok "notify-send recebeu titulo e texto"; else bad "notify-send recebeu titulo e texto" "$(cat "$T/ns.log")"; fi
m=$(awk 'BEGIN{for(i=0;i<300;i++)printf "x"}')
o=$(PATH="$T/fake:$PATH" "$K" tool PushNotification '{"message":"'"$m"'"}' 2>&1); has "Push avisa corte" "$o" "cut to 200 characters"
l=$(grep '^\[x' "$T/ns.log" | tr -d '\n' | wc -c)
eq "mensagem cortada em 200 (mais 2 colchetes)" "$l" "202"
o=$(PATH="$T/nobin" KATANA_UI=1 "$K" tool PushNotification '{"message":"a\tb","title":"T"}' 2>&1 | cat -vT)
has "marcador NOTIFY" "$o" "^ANOTIFY T^Ia b"
has "so o marcador ja conta como canal" "$o" "notification shown (desktop application)"
# com a interface ligada, ou com KATANA_NO_NOTIFY=1, o notify-send nao e chamado
rm -f "$T/ns.log"
o=$(PATH="$T/fake:$PATH" KATANA_UI=1 "$K" tool PushNotification '{"message":"so na interface"}' 2>&1 | cat -vT)
has "com interface: marcador" "$o" "^ANOTIFY katana^Iso na interface"
has "com interface: resultado cita so a interface" "$o" "notification shown (desktop application)"
[ ! -e "$T/ns.log" ] && ok "com interface: notify-send nao foi chamado" || bad "com interface: notify-send nao foi chamado" "$(cat "$T/ns.log")"
o=$(PATH="$T/fake:$PATH" KATANA_NO_NOTIFY=1 "$K" tool PushNotification '{"message":"calado"}' 2>&1)
has "KATANA_NO_NOTIFY: resultado diz que nada foi mostrado" "$o" "notifications are turned off here (KATANA_NO_NOTIFY is set); nothing was shown"
[ ! -e "$T/ns.log" ] && ok "KATANA_NO_NOTIFY: notify-send nao foi chamado" || bad "KATANA_NO_NOTIFY: notify-send nao foi chamado" "$(cat "$T/ns.log")"
o=$(PATH="$T/fake:$PATH" KATANA_NO_NOTIFY=0 "$K" tool PushNotification '{"message":"volta"}' 2>&1)
has "KATANA_NO_NOTIFY=0 nao desliga" "$o" "notification shown (notify-send)"

# ---------- SendUserFile ----------
echo hello > "$T/f1.txt"; echo world!! > "$T/f2.txt"
o=$("$K" tool SendUserFile '{}' 2>&1); has "SendUserFile sem files" "$o" "error: files must be an array"
o=$("$K" tool SendUserFile '{"files":[]}' 2>&1); has "SendUserFile vazio" "$o" "error: files is empty"
o=$("$K" tool SendUserFile '{"files":["'"$T"'/f1.txt","'"$T"'/nao.txt"]}' 2>&1); has "SendUserFile inexistente" "$o" "error: cannot open"
o=$("$K" tool SendUserFile '{"files":["'"$T"'"]}' 2>&1); has "SendUserFile pasta" "$o" "is not a regular file"
if [ "$(id -u)" != 0 ]; then
  chmod 000 "$T/f2.txt"
  o=$("$K" tool SendUserFile '{"files":["'"$T"'/f2.txt"]}' 2>&1); has "SendUserFile ilegivel" "$o" "is not readable"
  chmod 644 "$T/f2.txt"
fi
o=$("$K" tool SendUserFile '{"files":["'"$T"'/f1.txt","'"$T"'/f2.txt"],"caption":"dois"}' 2>&1)
has "SendUserFile lista" "$o" "2 files handed to the user:"
has "SendUserFile caminho e tamanho" "$o" "$T/f1.txt (6 bytes)"
has "SendUserFile segundo" "$o" "$T/f2.txt (8 bytes)"
has "SendUserFile legenda" "$o" "caption: dois"
o=$(KATANA_UI=1 "$K" tool SendUserFile '{"files":["f1.txt"],"caption":"c","display":"inline"}' 2>&1 | cat -vT)
has "marcador FILE" "$o" "^AFILE inline^I$T/f1.txt^Ic"
o=$(KATANA_UI=1 "$K" tool SendUserFile '{"files":["f1.txt"]}' 2>&1 | cat -vT)
has "FILE display auto" "$o" "^AFILE auto^I"

# ---------- EndConversation ----------
o=$("$K" tool EndConversation '{}' 2>&1); has "End sem reason" "$o" "error: missing reason"
o=$("$K" tool EndConversation '{"reason":"abuso continuado apos aviso"}' 2>&1)
has "End confirma" "$o" "ok: the conversation will end after this call; reason recorded: abuso continuado apos aviso"

if [ "$F" -eq 0 ]; then echo "OK $N checks"; exit 0; else echo "FAILED $F"; exit 1; fi
