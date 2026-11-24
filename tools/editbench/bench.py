#!/usr/bin/env python3
"""Harness do ciclo de edicao validada: Edit por simbolo (symbol=) contra o Edit tradicional (old_string/new_string).

Uso: python3 tools/editbench/bench.py /caminho/do/katana [--runs N] [--json saida.json]

Cada tarefa tem um arquivo sintetico (C ou Python), a chamada por simbolo e a chamada tradicional MINIMA que faz a mesma mudanca
(o menor old_string unico, ou blocos `edits` so com as linhas que mudam, o que for menor). Mede, com o binario real:
  - caracteres de SAIDA do modelo (os argumentos JSON da chamada); tokens ~ caracteres / 4 (aproximacao, sem tokenizador);
  - entrada que o modelo precisa ter visto: o arquivo inteiro para o tradicional (Read obrigatorio), nada para o simbolo;
  - para as tarefas invalidas de proposito: se a edicao foi pega ANTES de gravar (arquivo intacto), depois de gravar (o aviso da
    ferramenta), ou nao foi pega (silenciosa); a verdade vem de um verificador independente (gcc -fsyntax-only, python ast);
  - para as validas: se a edicao foi recusada sem motivo (falso positivo) e se o resultado confere com o esperado;
  - tempo: o que a validacao e a gravacao levam por dentro (KATANA_EDIT_TIME) e o tempo de parede do processo.
Nada aqui usa modelo nem GPU. Os numeros sao do que foi executado; nao ha estimativa.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, statistics

# ---------------------------------------------------------------- fixtures
C_SRC = r'''#include <stdio.h>
#include <string.h>

#define LIMIT 100

struct Acc {
    int total;
    int count;
};

int scale(int v, int k);

/* soma dois inteiros */
int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

int scale(int v, int k) {
    return v * k;
}

static int clamp(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    if (v == 42) {
        return 41;
    }
    return v;
}

static void report(const struct Acc *a) {
    printf("total={%d} count={%d}\n", a->total, a->count);
}

int main(void) {
    struct Acc a = {0, 0};
    a.total = add(1, 2);
    a.total = clamp(a.total, 0, LIMIT);
    a.count = scale(a.total, 2);
    report(&a);
    return 0;
}
'''
C_USE = '''int again(int v) { return add(v, v) + add(v, 1); }
int more(int v) { return add(v, 2); }
'''
PY_SRC = '''import os
import re


class Account:
    """conta simples"""

    def __init__(self, owner, balance=0):
        self.owner = owner
        self.balance = balance
        self.log = []

    def deposit(self, amount):
        if amount <= 0:
            raise ValueError("amount must be positive")
        self.balance += amount
        self.log.append(("deposit", amount))
        return self.balance

    def withdraw(self, amount):
        if amount > self.balance:
            raise ValueError("insufficient funds")
        self.balance -= amount
        self.log.append(("withdraw", amount))
        return self.balance

    def run(self):
        return len(self.log)


class Audit:
    def run(self):
        return 0


def parse(line):
    parts = line.split(",")
    name = parts[0].strip()
    value = int(parts[1])
    return name, value


def fmt(name, value,
        width=10):
    return "%s: %*d" % (name, width, value)


@staticmethod
def cached(x):
    return x * 2


def main():
    acct = Account("ana")
    acct.deposit(10)
    print(fmt(*parse("a, 3")))
'''
PY_USE = '''from m import parse

def load(lines):
    return [parse(l) for l in lines]

def first(lines):
    return parse(lines[0])
'''

def sym(file, symbol, where=None, new=None):
    d = {"file_path": file, "symbol": symbol}
    if where: d["where"] = where
    if new is not None: d["new_string"] = new
    return d

def trad(file, old, new):
    return {"file_path": file, "old_string": old, "new_string": new}

def trad_multi(file, pairs):
    return {"file_path": file, "edits": "".join("<<<<<<< \n%s\n=======\n%s\n>>>>>>> \n" % (o, n) for o, n in pairs)}

T = []
def task(id, lang, file, valid, s, t, note="", callers=None, extra=None, expect_ok=True):
    T.append(dict(id=id, lang=lang, file=file, valid=valid, sym=s, trad=t, note=note, callers=callers or [], extra=extra or {}, expect_ok=expect_ok))

# ----- C
task("c01-rename-param", "c", "calc.c", True,
     sym("calc.c", "sub", "replace", "int sub(int x, int y) {\n    return x - y;\n}"),
     trad_multi("calc.c", [("int sub(int a, int b) {\n    return a - b;", "int sub(int x, int y) {\n    return x - y;")]), "renomear parametros de sub")
task("c02-replace-body", "c", "calc.c", True,
     sym("calc.c", "clamp", "body", "return v < lo ? lo : v > hi ? hi : v;"),
     trad("calc.c", "    if (v < lo) {\n        return lo;\n    }\n    if (v > hi) {\n        return hi;\n    }\n    if (v == 42) {\n        return 41;\n    }\n    return v;\n",
          "    return v < lo ? lo : v > hi ? hi : v;\n"), "trocar o corpo de clamp (9 linhas) por uma expressao")
task("c03-insert-fn", "c", "calc.c", True,
     sym("calc.c", "add", "after", "int mul(int a, int b) {\n    return a * b;\n}"),
     trad("calc.c", "    return a + b;\n}\n", "    return a + b;\n}\n\nint mul(int a, int b) {\n    return a * b;\n}\n"), "inserir mul depois de add")
task("c04-delete-fn", "c", "calc.c", True,
     sym("calc.c", "sub", "delete"),
     trad("calc.c", "int sub(int a, int b) {\n    return a - b;\n}\n\n", ""), "remover sub")
task("c05-add-param", "c", "calc.c", True,
     sym("calc.c", "add", "replace", "/* soma dois inteiros e um desvio */\nint add(int a, int b, int off) {\n    return a + b + off;\n}"),
     trad_multi("calc.c", [("/* soma dois inteiros */\nint add(int a, int b) {\n    return a + b;", "/* soma dois inteiros e um desvio */\nint add(int a, int b, int off) {\n    return a + b + off;")]),
     "acrescentar parametro a add; chamadores em main (mesmo arquivo) e em use.c", callers=["calc.c:main", "use.c:again", "use.c:more"], extra={"use.c": C_USE})
task("c06-tricky-valid", "c", "calc.c", True,
     sym("calc.c", "report", "body", "/* nao fecha ( nem { aqui */\nchar br = '}';\nprintf(\"%d){ %c\\n\", a->total, br); // ) {"),
     trad("calc.c", "    printf(\"total={%d} count={%d}\\n\", a->total, a->count);\n", "    /* nao fecha ( nem { aqui */\n    char br = '}';\n    printf(\"%d){ %c\\n\", a->total, br); // ) {\n"),
     "corpo valido com chaves e parenteses dentro de string, char e comentario (nao pode dar falso positivo)")
task("c07-proto-vs-def", "c", "calc.c", True,
     sym("calc.c", "scale", "body", "return v * k + 0;"),
     trad("calc.c", "int scale(int v, int k) {\n    return v * k;", "int scale(int v, int k) {\n    return v * k + 0;"), "scale tem prototipo e definicao: o simbolo resolve a definicao")
task("c08-bad-paren", "c", "calc.c", False,
     sym("calc.c", "clamp", "body", "return v < lo ? lo : (v > hi ? hi : v;"),
     trad("calc.c", "    return v;\n}\n\nstatic void report", "    return (v < lo ? lo : (v > hi ? hi : v;\n}\n\nstatic void report"), "parentese sem fechar")
task("c09-extra-brace", "c", "calc.c", False,
     sym("calc.c", "scale", "body", "return v * k;\n}\n}"),
     trad("calc.c", "    return v * k;\n}", "    return v * k;\n}\n}"), "chave de fechamento a mais")
task("c10-unclosed-brace", "c", "calc.c", False,
     sym("calc.c", "add", "body", "if (a > 0) {\n    return a + b;"),
     trad("calc.c", "    return a + b;\n}", "    if (a > 0) {\n    return a + b;\n}"), "chave aberta e nunca fechada")
task("c11-semantic-undefined", "c", "calc.c", False,
     sym("calc.c", "sub", "body", "return a - undefined_name;"),
     trad("calc.c", "    return a - b;", "    return a - undefined_name;"), "so semantica (nome nao declarado): estrutura e valida, so um compilador ou LSP ve", expect_ok=True)
# ----- Python
task("p01-rename-param", "py", "m.py", True,
     sym("m.py", "Account.withdraw", "replace", "def withdraw(self, value):\n    if value > self.balance:\n        raise ValueError(\"insufficient funds\")\n    self.balance -= value\n    self.log.append((\"withdraw\", value))\n    return self.balance"),
     trad_multi("m.py", [("    def withdraw(self, amount):\n        if amount > self.balance:", "    def withdraw(self, value):\n        if value > self.balance:"),
                         ("        self.balance -= amount\n        self.log.append((\"withdraw\", amount))", "        self.balance -= value\n        self.log.append((\"withdraw\", value))")]), "renomear amount -> value em withdraw")
task("p02-replace-body", "py", "m.py", True,
     sym("m.py", "Account.deposit", "body", "if amount <= 0:\n    return self.balance\nself.balance += amount\nself.log.append((\"deposit\", amount))\nreturn self.balance"),
     trad("m.py", "        if amount <= 0:\n            raise ValueError(\"amount must be positive\")\n        self.balance += amount\n        self.log.append((\"deposit\", amount))\n        return self.balance\n",
          "        if amount <= 0:\n            return self.balance\n        self.balance += amount\n        self.log.append((\"deposit\", amount))\n        return self.balance\n"), "trocar o corpo de deposit")
task("p03-insert-method", "py", "m.py", True,
     sym("m.py", "Account.withdraw", "after", "def history(self):\n    return list(self.log)"),
     trad("m.py", "        return self.balance\n\n    def run(self):", "        return self.balance\n\n    def history(self):\n        return list(self.log)\n\n    def run(self):"), "inserir metodo history depois de withdraw")
task("p04-delete-fn", "py", "m.py", True,
     sym("m.py", "cached", "delete"),
     trad("m.py", "@staticmethod\ndef cached(x):\n    return x * 2\n\n\n", ""), "remover cached (com decorador)")
task("p05-sig-callers", "py", "m.py", True,
     sym("m.py", "parse", "replace", "def parse(line, sep=\",\"):\n    parts = line.split(sep)\n    name = parts[0].strip()\n    value = int(parts[1])\n    return name, value"),
     trad_multi("m.py", [("def parse(line):\n    parts = line.split(\",\")", "def parse(line, sep=\",\"):\n    parts = line.split(sep)")]),
     "parse ganha parametro; chamadores em m.py (main) e use.py (load, first)", callers=["m.py:main", "use.py:load", "use.py:first"], extra={"use.py": PY_USE})
task("p06-ambiguous", "py", "m.py", True,
     sym("m.py", "Audit.run", "body", "return -1"),
     trad("m.py", "class Audit:\n    def run(self):\n        return 0", "class Audit:\n    def run(self):\n        return -1"),
     "run existe em Account e Audit: o modelo que escreve so `run` recebe a lista de candidatos e repete (contado a parte)", extra={"first_try_symbol": "run"})
task("p07-tricky-valid", "py", "m.py", True,
     sym("m.py", "fmt", "body", "label = f\"{name}: {value:{width}}\"\n\"\"\"nao fecha ( [ aqui:\"\"\"\nkey = lambda x=(1, 2): x  # ) ] }\nreturn label"),
     trad("m.py", "    return \"%s: %*d\" % (name, width, value)\n", "    label = f\"{name}: {value:{width}}\"\n    \"\"\"nao fecha ( [ aqui:\"\"\"\n    key = lambda x=(1, 2): x  # ) ] }\n    return label\n"),
     "f-string com chaves, texto com ( [ e lambda: valido, nao pode dar falso positivo")
task("p08-bad-paren", "py", "m.py", False,
     sym("m.py", "parse", "body", "parts = line.split(\",\"\nname = parts[0].strip()\nreturn name, int(parts[1])"),
     trad("m.py", "    parts = line.split(\",\")\n    name = parts[0].strip()\n    value = int(parts[1])\n    return name, value\n", "    parts = line.split(\",\"\n    name = parts[0].strip()\n    return name, int(parts[1])\n"), "parentese sem fechar")
task("p09-bad-syntax", "py", "m.py", False,
     sym("m.py", "Account.run", "body", "if self.log\n    return 1\nreturn 0"),
     trad("m.py", "        return len(self.log)\n", "        if self.log\n            return 1\n        return 0\n"), "if sem dois pontos")
task("p10-bad-indent", "py", "m.py", False,
     sym("m.py", "parse", "body", "parts = line.split(\",\")\n  name = parts[0].strip()\nreturn name, int(parts[1])"),
     trad("m.py", "    name = parts[0].strip()\n", "      name = parts[0].strip()\n"), "indentacao inesperada")
task("p11-semantic-undefined", "py", "m.py", False,
     sym("m.py", "Account.run", "body", "return len(self.missing_attr) + nope"),
     trad("m.py", "        return len(self.log)\n", "        return len(self.missing_attr) + nope\n"), "so semantica (nome indefinido): estrutura valida; so LSP/execucao ve")

# ---------------------------------------------------------------- execucao
def validity(lang, path):   # as tarefas com chamadores a atualizar nao compilam/rodam ate o modelo atualizar os chamadores: e o que o relatorio lista
    if lang == "c":
        r = subprocess.run(["gcc", "-fsyntax-only", "-x", "c", "-w", path], capture_output=True, text=True)
        return r.returncode == 0
    r = subprocess.run([sys.executable, "-c", "import ast,sys; ast.parse(open(sys.argv[1]).read())", path], capture_output=True, text=True)
    return r.returncode == 0

def norm(s):
    return re.sub(r"\s+", " ", s).strip()

def setup(t, d):
    os.makedirs(d, exist_ok=True)
    src = C_SRC if t["lang"] == "c" else PY_SRC
    with open(os.path.join(d, t["file"]), "w") as f: f.write(src)
    for n, c in t["extra"].items():
        if n != "first_try_symbol":
            with open(os.path.join(d, n), "w") as f: f.write(c)

def katana(K, d, lines, env=None, once=False):
    e = dict(os.environ); e["HOME"] = os.path.join(d, ".home"); e["KATANA_LOG"] = os.path.join(d, ".log"); e["KATANA_EDIT_LSP"] = "0"
    os.makedirs(e["HOME"], exist_ok=True)
    if env: e.update(env)
    t0 = time.time()
    r = subprocess.run([K, "toolrun"], input="\n".join(lines) + "\n", capture_output=True, text=True, cwd=d, env=e)
    return r.stdout, (time.time() - t0) * 1000

def split_results(out, n):
    # toolrun imprime o resultado de cada linha; separamos pelo marcador de linha que ele usa
    return out

def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(2)
    K = os.path.abspath(sys.argv[1]); runs = 3; jout = None
    a = sys.argv[2:]
    while a:
        x = a.pop(0)
        if x == "--runs": runs = int(a.pop(0))
        elif x == "--json": jout = a.pop(0)
    base = tempfile.mkdtemp(prefix="editbench-")
    rows = []
    for t in T:
        d = os.path.join(base, t["id"]); setup(t, d)
        path = os.path.join(d, t["file"]); orig = open(path).read()
        row = dict(id=t["id"], lang=t["lang"], valid=t["valid"], note=t["note"])
        # --- tradicional: Read + Edit no mesmo processo
        sargs = json.dumps(t["sym"], separators=(",", ":")); targs = json.dumps(t["trad"], separators=(",", ":"))
        row["sym_chars"] = len(sargs); row["trad_chars"] = len(targs); row["read_chars"] = len(orig)
        shutil.rmtree(d); setup(t, d)
        o, _ = katana(K, d, ["Read " + json.dumps({"file_path": t["file"]}), "Edit " + targs])
        after_t = open(path).read()
        row["trad_written"] = after_t != orig
        row["trad_flagged"] = "syntax check FAILED" in o
        row["trad_valid_after"] = validity(t["lang"], path)
        trad_final = after_t
        # --- simbolo
        shutil.rmtree(d); setup(t, d)
        first = t["extra"].get("first_try_symbol")
        retry = 0
        if first:
            c0 = dict(t["sym"]); c0["symbol"] = first
            o0, _ = katana(K, d, ["Edit " + json.dumps(c0)])
            row["sym_first_try_refused_with_candidates"] = "matches" in o0 and "definitions" in o0
            row["sym_chars"] += len(json.dumps(c0, separators=(",", ":"))); retry = 1
        o, ms = katana(K, d, ["Edit " + sargs], env={"KATANA_EDIT_TIME": "1"})
        after_s = open(path).read()
        row["sym_written"] = after_s != orig
        row["sym_refused"] = "edit refused" in o
        row["sym_valid_after"] = validity(t["lang"], path)
        m = re.search(r"in (\d+) ms", o); row["sym_internal_ms"] = int(m.group(1)) if m else None
        row["sym_match_trad"] = norm(after_s) == norm(trad_final)
        row["sym_report"] = o.strip()
        got = set()
        for c in t["callers"]:
            f, fn = c.split(":")
            if re.search(re.escape(f) + r":\d+ in " + re.escape(fn), o): got.add(c)
        row["callers_expected"] = len(t["callers"]); row["callers_found"] = len(got)
        # tempo de parede: Edit simbolo vs Read sozinho (mesmo processo), mediana de `runs`
        ws, rs = [], []
        for _ in range(runs):
            shutil.rmtree(d); setup(t, d)
            _, w = katana(K, d, ["Edit " + sargs]); ws.append(w)
            shutil.rmtree(d); setup(t, d)
            _, r = katana(K, d, ["Read " + json.dumps({"file_path": t["file"]})]); rs.append(r)
        row["sym_wall_ms"] = statistics.median(ws); row["read_wall_ms"] = statistics.median(rs)
        rows.append(row)
    # ---------------- resumo
    valid = [r for r in rows if r["valid"]]; inval = [r for r in rows if not r["valid"]]
    structural = [r for r in inval if "semantic" not in r["id"]]
    tok = lambda c: c / 4.0
    print("%-24s %5s %5s %6s %7s %-9s %s" % ("task", "sym", "trad", "read", "ms_int", "result", "note"))
    for r in rows:
        if r["valid"]: res = "ok" if (r["sym_written"] and (r["sym_valid_after"] or r["callers_expected"]) and r["sym_match_trad"]) else ("REFUSED" if r["sym_refused"] else "DIFF")
        else: res = "caught" if (r["sym_refused"] and not r["sym_written"]) else ("written-bad" if not r["sym_valid_after"] else "written-ok")
        r["sym_result"] = res
        print("%-24s %5d %5d %6d %7s %-9s %s" % (r["id"], r["sym_chars"], r["trad_chars"], r["read_chars"], r["sym_internal_ms"], res, r["note"][:60]))
    sc = sum(r["sym_chars"] for r in valid); tc = sum(r["trad_chars"] for r in valid); rc = sum(r["read_chars"] for r in valid)
    print()
    print("VALID tasks (%d): output chars sym=%d trad=%d  (~tokens %.0f vs %.0f, %.0f%% of the traditional)" % (len(valid), sc, tc, tok(sc), tok(tc), 100.0 * sc / tc))
    print("  input the traditional edit needs first (whole-file Read, chars): %d; the symbol edit needs none" % rc)
    fp = [r["id"] for r in valid if r["sym_refused"]]
    diff = [r["id"] for r in valid if not r["sym_refused"] and not r["sym_match_trad"]]
    print("  false positives (valid edit refused): %d %s; result differs from the traditional edit: %d %s" % (len(fp), fp, len(diff), diff))
    print("INVALID on purpose (%d, of which %d structural and %d semantic-only):" % (len(inval), len(structural), len(inval) - len(structural)))
    for name, rs_ in (("structural", structural), ("semantic-only", [r for r in inval if r not in structural])):
        c_before = sum(1 for r in rs_ if r["sym_refused"] and not r["sym_written"])
        c_after = sum(1 for r in rs_ if r["trad_written"] and r["trad_flagged"])
        silent = sum(1 for r in rs_ if r["trad_written"] and not r["trad_valid_after"] and not r["trad_flagged"])
        badw = sum(1 for r in rs_ if r["sym_written"] and not r["sym_valid_after"])
        print("  %-13s symbol edit: caught before writing %d/%d, bad file written %d;  traditional: bad file written %d/%d, flagged after writing %d, silent %d" % (
            name, c_before, len(rs_), badw, sum(1 for r in rs_ if r["trad_written"] and not r["trad_valid_after"]), len(rs_), c_after, silent))
    ims = [r["sym_internal_ms"] for r in rows if r["sym_internal_ms"] is not None]
    print("TIME: validation+write inside the tool (ms): median %.0f, max %d (n=%d);  process wall of Edit(symbol) minus a bare Read, median over tasks: %.0f ms" % (
        statistics.median(ims), max(ims), len(ims), statistics.median([r["sym_wall_ms"] - r["read_wall_ms"] for r in rows])))
    ce = sum(r["callers_expected"] for r in rows); cf = sum(r["callers_found"] for r in rows)
    print("CALLERS listed (derived, by name): %d of %d expected found" % (cf, ce))
    if jout: json.dump(rows, open(jout, "w"), indent=1)
    shutil.rmtree(base, ignore_errors=True)

if __name__ == "__main__":
    main()
