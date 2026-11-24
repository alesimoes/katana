#!/bin/sh
# Testa o Edit por simbolo (symbol=..., where=...) sem modelo: sh tools/tooltest/symedit.sh /caminho/do/katana
# Cobre C e Python a fundo, e JS, Go, shell, Java e Rust no nivel estrutural; o LSP e testado com um servidor falso (fake_lsp.py).
# Pasta temporaria que cria e apaga. Uma linha por checagem; FAILED n ou OK n checks no fim.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}
[ -x "$K" ] || { echo "uso: $0 <binario katana>"; exit 2; }
T=$(mktemp -d) || exit 2
HOME_SAVE=$HOME
HOME=$T/home; export HOME; mkdir -p "$HOME"
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 2
N=0; BAD=0
out=
run() { out=$("$K" tool Edit "$1" 2>/dev/null); }
ok()   { N=$((N+1)); echo "ok   $1"; }
fail() { N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; echo "     got: $(printf '%s' "$out" | head -c 700 | tr '\n' '|')"; }
has()  { case "$out" in *"$2"*) ok "$1";; *) fail "$1 (missing: $2)";; esac; }
hasnt(){ case "$out" in *"$2"*) fail "$1 (unexpected: $2)";; *) ok "$1";; esac; }
fileis() { if [ "$(cat "$2")" = "$3" ]; then ok "$1"; else N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1"; echo "     file: $(head -c 500 "$2" | tr '\n' '|')"; fi; }
unchanged() { if cmp -s "$2" "$2.orig"; then ok "$1"; else N=$((N+1)); BAD=$((BAD+1)); echo "FAIL $1 (file changed)"; fi; }
snap() { cp "$1" "$1.orig"; }

cat > a.c <<'XX'
#include <stdio.h>

/* soma */
int add(int a, int b) {
    return a + b;
}

static int twice(int x) {
    return add(x, x);
}

struct P {
    int x;
};

int main(void) {
    printf("%d\n", twice(3));
    return 0;
}
XX
snap a.c
# ---------------- C: replace, body, ambiguidade, estrutura ----------------
run '{"file_path":"a.c","symbol":"add","new_string":"int add(int a, int b, int c) {\n    return a + b + c;\n}"}'
has "C replace: relatorio" "Edited func \`add\`"
has "C replace: grau structural" "[structural] brackets balanced"
has "C replace: assinatura mudou" "signature changed"
has "C replace: parametros 2 -> 3" "(2 -> 3 parameters)"
has "C replace: chamador do mesmo arquivo (derived)" "[derived] callers of \`add\`"
has "C replace: lista twice" "a.c:9 in twice"
hasnt "C replace: nunca diz exato" "exact"
fileis "C replace: arquivo" a.c "$(sed 's/int add(int a, int b) {/int add(int a, int b, int c) {/; s/return a + b;/return a + b + c;/' a.c.orig)"
snap a.c
run '{"file_path":"a.c","symbol":"twice","where":"body","new_string":"int y = x + 1;\nreturn add(y, y, 0);"}'
has "C body: ok" "(body)"
hasnt "C body: assinatura igual, sem aviso" "signature changed"
fileis "C body: so o corpo mudou" a.c "$(sed '/^static int twice/,/^}/c\
static int twice(int x) {\
    int y = x + 1;\
    return add(y, y, 0);\
}' a.c.orig)"
snap a.c
run '{"file_path":"a.c","symbol":"twice","where":"body","new_string":"return add((x, x, 0;"}'
has "C parenteses quebrado: recusado" "edit refused, the file was NOT changed"
has "C parenteses: linha e o que esperava" "line 9: the \`(\` opened here is never closed (expected \`)\`"
has "C parenteses: mostra a linha" "return add((x, x, 0;"
unchanged "C parenteses: arquivo intacto" a.c
run '{"file_path":"a.c","symbol":"twice","where":"body","new_string":"return 1; }\n}"}'
has "C chave sobrando: recusado" "unexpected \`}\`"
unchanged "C chave sobrando: arquivo intacto" a.c
run '{"file_path":"a.c","symbol":"twice","new_string":"return 1;"}'
has "C replace sem definicao: recusado" "finds no definition in new_string"
unchanged "C replace sem definicao: intacto" a.c
run '{"file_path":"a.c","symbol":"twice","new_string":"struct twice {\n    int q;\n};"}'
has "C troca de tipo: recusado" "the symbol would change kind"
unchanged "C troca de tipo: intacto" a.c
run '{"file_path":"a.c","symbol":"nope","new_string":"x"}'
has "C simbolo inexistente: lista os que ha" "Symbols here: add"
run '{"file_path":"a.c","symbol":"P","new_string":"struct P {\n    int x;\n    int y;\n};"}'
has "C struct com ; final" "still a struct"
has "C struct: novo campo" "new in the file: P.y"
fileis "C struct: sem ; duplicado" a.c "$(sed 's/^    int x;$/    int x;\n    int y;/' a.c.orig)"
snap a.c
# comentario do simbolo dentro do new_string substitui o antigo (nao duplica)
snap a.c
run '{"file_path":"a.c","symbol":"add","new_string":"/* soma tres */\nint add(int a, int b, int c) {\n    return a + b + c;\n}"}'
has "C replace com comentario: ok" "Edited func"
case "$(cat a.c)" in *"/* soma */"*) fail "C replace com comentario: o antigo sai";; *) ok "C replace com comentario: o antigo sai";; esac
cp a.c.orig a.c
# strings, caracteres e comentarios com delimitadores nao confundem o balanceamento
run '{"file_path":"a.c","symbol":"twice","where":"body","new_string":"/* nao fecha ( nem { */\nchar br = '"'"'}'"'"';\nprintf(\"%d){ %c\\n\", x, br); // ) {\nreturn add(x, x);"}'
has "C delimitadores em string, char e comentario: aceito" "still a func"
cp a.c.orig a.c
# remover e inserir
run '{"file_path":"a.c","symbol":"twice","where":"delete"}'
has "C delete: removido" "removed: twice"
has "C delete: chamador em main (derived)" "[derived] callers of \`twice\`"
has "C delete: main" "a.c:14 in main"
hasnt "C delete: sumiu" "static int twice" 
snap a.c
run '{"file_path":"a.c","symbol":"add","where":"before","new_string":"/* produto */\nint mul(int a, int b) {\n    return a * b;\n}"}'
has "C before: novo simbolo" "new in the file: mul"
run '{"file_path":"a.c","symbol":"add","where":"after","new_string":"int sub(int a, int b) {\n    return a - b;\n}"}'
has "C after: novo simbolo" "new in the file: sub"
case "$(cat a.c)" in *"/* soma */
int add"*) ok "C before: comentario do simbolo continua colado a ele";; *) fail "C before: comentario do simbolo continua colado a ele";; esac
# ambiguidade em C++-ish: dois metodos de mesmo nome
cat > amb.py <<'XX'
class A:
    def run(self):
        return 1

class B:
    def run(self):
        return 2

def run():
    return 3
XX
snap amb.py
run '{"file_path":"amb.py","symbol":"run","new_string":"def run():\n    return 9"}'
has "ambiguo: erro lista candidatos" "matches 3 definitions"
has "ambiguo: A.run" "A.run method (lines 2-3)"
has "ambiguo: B.run" "B.run method (lines 6-7)"
has "ambiguo: funcao" "run func (lines 9-10)"
unchanged "ambiguo: arquivo intacto" amb.py
run '{"file_path":"amb.py","symbol":"B.run","where":"body","new_string":"return 22"}'
has "Class.nome resolve" "Edited method \`run\`"
run '{"file_path":"amb.py","symbol":"run:9","where":"delete"}'
has "nome:linha resolve" "Deleted func \`run\`"
fileis "ambiguo: so a funcao saiu" amb.py "class A:
    def run(self):
        return 1

class B:
    def run(self):
        return 22"
# ---------------- Python ----------------
cat > m.py <<'XX'
import os

class Box:
    def __init__(self, n):
        self.n = n

    def grow(self, k=1):
        """grow it"""
        self.n += k
        return self.n

def helper(a,
           b):
    return a * b

@staticmethod
def deco(x):
    return x

def other():
    return helper(2, 3)
XX
snap m.py
run '{"file_path":"m.py","symbol":"Box.grow","where":"body","new_string":"self.n += k\nreturn self.n * 2"}'
has "Py body de metodo: indenta sozinho" "still a method"
fileis "Py body: resultado" m.py "$(sed '/^        """grow it"""$/d; s/^        return self.n$/        self.n += k\n        return self.n * 2/; 0,/^        self.n += k$/{//d}' m.py.orig)"
snap m.py
run '{"file_path":"m.py","symbol":"helper","where":"body","new_string":"return a +"}'
has "Py sintaxe quebrada: recusado" "edit refused, the file was NOT changed"
has "Py sintaxe: erro do python" "invalid syntax"
unchanged "Py sintaxe: intacto" m.py
run '{"file_path":"m.py","symbol":"helper","where":"body","new_string":"label = f\"{a}: {b:{a}}\"\n\"\"\"nao fecha ( [ aqui:\"\"\"\nk = lambda x=(1, 2): x  # ) ] }\nreturn label"}'
has "Py f-string, texto e lambda com delimitadores: aceito" "still a func"
cp m.py.orig m.py
run '{"file_path":"m.py","symbol":"helper","where":"body","new_string":"x = (a +\nreturn x"}'
has "Py parenteses: recusado" "never closed"
unchanged "Py parenteses: intacto" m.py
run '{"file_path":"m.py","symbol":"helper","new_string":"def helper(a, b, c):\n    return a * b * c"}'
has "Py replace: assinatura" "signature changed"
has "Py replace: chamador" "m.py:19 in other"
run '{"file_path":"m.py","symbol":"deco","new_string":"def deco(x):\n    return x * 2"}'
has "Py decorador perdido: aviso" "decorators/annotations and yours does not"
run '{"file_path":"m.py","symbol":"other","where":"after","new_string":"def last():\n    return 0"}'
has "Py after: novo" "new in the file: last"
case "$(cat m.py)" in *"

def other():
    return helper(2, 3)


def last():"*) ok "Py after de funcao de topo: duas linhas vazias";; *) fail "Py after de funcao de topo: duas linhas vazias";; esac
run '{"file_path":"m.py","symbol":"Box","where":"delete"}'
has "Py delete classe: filhos listados" "Box.grow"
hasnt "Py delete classe: arquivo valido" "refused"
# classe que ficaria vazia: sintaxe do python recusa
cat > e.py <<'XX'
class C:
    def only(self):
        return 1

x = 1
XX
snap e.py
run '{"file_path":"e.py","symbol":"only","where":"delete"}'
has "Py bloco vazio: recusado pela sintaxe" "edit refused"
unchanged "Py bloco vazio: intacto" e.py
# ---------------- outras familias (nivel estrutural) ----------------
cat > t.js <<'XX'
function sum(a, b) {
  return a + b;
}

class Acc {
  add(x) {
    return x;
  }
}
XX
run '{"file_path":"t.js","symbol":"Acc.add","where":"body","new_string":"this.t = x * 2;\nreturn this;"}'
has "JS body" "still a method"
run '{"file_path":"t.js","symbol":"sum","where":"body","new_string":"return a + ;;; }"}'
has "JS chave sobrando: recusado" "unexpected \`}\`"
cat > g.go <<'XX'
package main

type P struct {
	X int
}

func (p *P) Inc() {
	p.X++
}
XX
run '{"file_path":"g.go","symbol":"Inc","where":"body","new_string":"p.X += 2"}'
has "Go body" "still a method"
cat > s.sh <<'XX'
#!/bin/bash
greet() {
  echo "hi $1"
}
greet world
XX
run '{"file_path":"s.sh","symbol":"greet","where":"body","new_string":"echo \"hello $1\""}'
has "Shell body" "syntax ok"
run '{"file_path":"s.sh","symbol":"greet","where":"body","new_string":"if [ -n \"$1\" ]; then\n echo hi"}'
has "Shell if sem fi: recusado pelo bash -n" "edit refused"
cat > R.java <<'XX'
public class R {
    private int v;

    public int get() {
        return v;
    }
}
XX
run '{"file_path":"R.java","symbol":"get","where":"after","new_string":"public int twice() {\n    return v * 2;\n}"}'
has "Java after" "new in the file: R.twice"
cat > s.rs <<'XX'
struct A { x: i32 }

impl A {
    fn get(&self) -> i32 {
        self.x
    }
}
XX
run '{"file_path":"s.rs","symbol":"get","where":"body","new_string":"self.x + (1"}'
has "Rust parenteses: recusado" "never closed"
cat > d.md <<'XX'
# Titulo

## Um

texto um

## Dois

texto dois
XX
run '{"file_path":"d.md","symbol":"Um","where":"body","new_string":"outro texto"}'
has "Markdown: so ate o que o scanner enxerga" "has no body to replace"
# ---------------- chamadores em outros arquivos (indice) ----------------
mkdir proj && cd proj
printf 'int mul(int a, int b) {\n    return a * b;\n}\n' > lib.c
printf 'int mul(int, int);\nint run(int v) { return mul(v, 2) + mul(v, 3); }\n' > main.c
printf 'int again(int v) { return mul(v, v); }\n' > other.c
run '{"file_path":"lib.c","symbol":"mul","new_string":"int mul(int a, int b, int c) {\n    return a * b * c;\n}"}'
has "indice: chamador em outro arquivo" "main.c:2 in run"
has "indice: outro chamador" "other.c:1 in again"
has "indice: derived por nome" "(by name"
cd ..
# ---------------- erros de uso ----------------
run '{"file_path":"a.c","symbol":"add","where":"sideways","new_string":"x"}'
has "where invalido" "where must be replace, body, before, after or delete"
run '{"file_path":"a.c","symbol":"add"}'
has "replace sem new_string" "needs new_string"
run '{"file_path":"nao.c","symbol":"add","new_string":"x"}'
has "arquivo inexistente" "does not exist"
printf 'so texto\n' > plain.txt
run '{"file_path":"plain.txt","symbol":"x","new_string":"x"}'
has "tipo sem scanner" "finds no symbols"
# Edit tradicional continua igual
printf 'alpha\nbeta\n' > t.txt
out=$(printf 'Read {"file_path":"t.txt"}\nEdit {"file_path":"t.txt","old_string":"beta","new_string":"gamma"}\n' | "$K" toolrun 2>/dev/null)
has "Edit tradicional continua" "has been updated"
# ---------------- LSP (servidor falso) ----------------
if command -v python3 >/dev/null 2>&1; then
  cat > l.py <<'XX'
def f(x):
    return x

def g():
    return f(1)
XX
  export KATANA_LSP_PY="python3 $HERE/fake_lsp.py"
  run '{"file_path":"l.py","symbol":"f","where":"body","new_string":"y = BAD_TOKEN\nreturn x  # TODO_WARN"}'
  has "LSP: erro introduzido (so ele)" "1 new error introduced by this edit"
  has "LSP: linha do erro" "line 2: undefined name BAD_TOKEN"
  has "LSP: aviso novo contado" "1 new warning"
  has "LSP: grau lsp" "[lsp "
  run '{"file_path":"l.py","symbol":"g","where":"body","new_string":"return f(2)"}'
  has "LSP: o erro que ja existia nao e culpa da edicao" "no new errors"
  KATANA_EDIT_LSP=0 run '{"file_path":"l.py","symbol":"g","where":"body","new_string":"return f(3)"}'
  hasnt "LSP: desligado por KATANA_EDIT_LSP=0" "[lsp"
  export KATANA_LSP_PY="/nao/existe/servidor"
  run '{"file_path":"l.py","symbol":"g","where":"body","new_string":"return f(4)"}'
  hasnt "LSP: servidor inexistente nao bloqueia nem aparece" "[lsp"
  has "LSP: servidor inexistente: a edicao vale" "Edited func"
  unset KATANA_LSP_PY
fi
HOME=$HOME_SAVE
if [ "$BAD" -eq 0 ]; then echo "OK $N checks"; exit 0; else echo "FAILED $BAD (de $N checks)"; exit 1; fi
