#!/usr/bin/env python3
"""Testes da AST incremental (docs/ucge.md, "AST incremental"): modifica arquivos Python reais pequenos com ucgecli e confere que
(1) so mudaram as linhas esperadas (diff minimo, byte a byte fora delas), (2) o resultado e Python valido (ast), (3) o conteudo certo.
Uso: python3 tools/ucge/test_inc.py /caminho/do/ucgecli"""
import ast, difflib, os, shutil, subprocess, sys, tempfile, time

U = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.join(HERE, "inc")
T = tempfile.mkdtemp(prefix="ucgeinc")
N = BAD = 0


def ok(name): global N; N += 1; print("ok   " + name)
def fail(name, why=""):
    global N, BAD; N += 1; BAD += 1; print("FAIL " + name); print("     " + why.replace("\n", "|")[:700]) if why else None


def run(fixture, spec, extra=(), raw=False):
    src = os.path.join(T, fixture) if os.path.exists(os.path.join(T, fixture)) else os.path.join(INC, fixture)
    sp = os.path.join(T, "spec.txt")
    open(sp, "w").write(spec)
    p = subprocess.run([U, "inc", "apply", src, sp] + list(extra), capture_output=True)
    return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace"), src


def lines_of(b): return b.decode("utf-8").splitlines(keepends=True)


def sym_span(src, name):
    """linhas (1-based, inclusive) de uma def/class do arquivo original, com decoradores; name pode ser Classe.metodo"""
    tree = ast.parse(src)
    node = tree
    for part in name.split("."):
        node = next(n for n in ast.iter_child_nodes(node) if getattr(n, "name", None) == part)
    first = min([node.lineno] + [d.lineno for d in node.decorator_list])
    return first, node.end_lineno


def check(name, fixture, spec, allowed, contains=(), absent=(), maxchanged=None, run_py=None, extra=()):
    """allowed: lista de (lo, hi) em linhas do ORIGINAL onde mudancas sao permitidas (insercao entre lo-1 e hi)."""
    rc, out, err, src = run(fixture, spec, extra)
    if rc != 0: return fail(name, "rc=%d %s" % (rc, err))
    old = open(src, "rb").read()
    new = out
    if not new: return fail(name, "no output: " + err)
    try: ast.parse(new.decode("utf-8"))
    except SyntaxError as e: return fail(name, "result is not valid Python: %s" % e)
    ol, nl = lines_of(old), lines_of(new)
    sm = difflib.SequenceMatcher(None, ol, nl, autojunk=False)
    changed = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal": continue
        changed += max(i2 - i1, j2 - j1)
        if not any(lo - 1 <= i1 and i2 <= hi for lo, hi in allowed) and not any(i1 == i2 and lo - 1 <= i1 <= hi for lo, hi in allowed):
            return fail(name, "change outside the allowed lines: old lines %d-%d (%s)\n%s" % (i1 + 1, i2, tag, "".join(nl[j1:j2])))
    if maxchanged is not None and changed > maxchanged: return fail(name, "too many changed lines: %d > %d" % (changed, maxchanged))
    t = new.decode("utf-8")
    for c in contains:
        if c not in t: return fail(name, "missing: %s" % c)
    for c in absent:
        if c in t: return fail(name, "unexpected: %s" % c)
    if run_py:
        g = {}
        try:
            exec(compile(t.replace("if __name__", "if False and __name__"), "x", "exec"), g)
            r = run_py(g)
        except Exception as e: return fail(name, "execution: %r" % (e,))
        if r is not True: return fail(name, "semantic check failed: %r" % (r,))
    ok(name)
    return new


def check_err(name, fixture, spec, msg):
    rc, out, err, src = run(fixture, spec)
    if rc == 0: return fail(name, "should have failed")
    if msg not in err: return fail(name, "wrong message: " + err)
    if out: return fail(name, "wrote output despite the error")
    ok(name)


def check_nochange(name, fixture, spec):
    rc, out, err, src = run(fixture, spec)
    if rc != 0: return fail(name, err)
    if out or "no change" not in err: return fail(name, "expected no change: " + err)
    ok(name)


def sp(fixture, name): return sym_span(open(os.path.join(INC, fixture)).read(), name)
def mk(name, text):
    p = os.path.join(T, name); open(p, "w", newline="").write(text); return name


INV = "inventory.py"; TXT = "textutil.py"; SHP = "shapes.py"; TOOL = "tool.py"
src_inv = open(os.path.join(INC, INV)).read().splitlines()
guard = next(i for i, l in enumerate(src_inv, 1) if l.startswith("if __name__"))

# ---------------- adicionar ----------------
check("add-function end: fica antes do `if __name__`", INV, """add-function end
  fn double(x: int) -> int
    return x * 2
""", [(guard - 3, guard)], contains=["def double(x: int) -> int:"], maxchanged=6, run_py=lambda g: g["double"](4) == 8)
a, b = sp(INV, "total_cost")
check("add-function after: so linhas novas depois da funcao", INV, """add-function after total_cost
  fn averageCost(items: list[float]) -> float
    if len(items) == 0
      return 0.0
    return sum(items) / len(items)
""", [(b, b + 2)], maxchanged=8, run_py=lambda g: g["averageCost"]([1.0, 3.0]) == 2.0)
a, b = sp(INV, "Stock")
check("add-function before uma classe", INV, """add-function before Stock
  fn noop() -> int
    return 0
""", [(a - 3, a)], maxchanged=5)
check("add-function com varias funcoes e import novo (re)", TXT, """add-function end
  fn isCode(s: str) -> bool
    return matches(s, "^[A-Z]+$")
  fn lenOf(s: str) -> int
    return len(s)
""", [(3, 5), (len(open(os.path.join(INC, TXT)).read().splitlines()), 999)], contains=["import re", "re.search"], run_py=lambda g: g["isCode"]("ABC") and not g["isCode"]("abc"))
a, b = sp(INV, "Stock")
check("add-method no fim da classe", INV, """add-method Stock
  fn clear() -> int
    self.qty = 0
    return 0
""", [(b, b + 1)], contains=["    def clear(self) -> int:"], maxchanged=6)
a, b = sp(INV, "Stock.add")
check("add-method after outro metodo", INV, """add-method Stock after add
  fn double() -> int
    return 2
""", [(b, b + 1)], contains=["    def double(self) -> int:"], maxchanged=5)
# ---------------- trocar ----------------
a, b = sp(INV, "price_with_tax")
check("replace-function troca so a funcao (comentario acima fica)", INV, """replace-function price_with_tax
  fn price_with_tax(price: float, rate: float = 0.2) -> float
    return price * (1 + rate)
""", [(a, b)], contains=["# Compute the price with tax."], absent=["total > LIMIT"], run_py=lambda g: abs(g["price_with_tax"](10) - 12) < 1e-9)
a, b = sp(INV, "total_cost")
check("replace-body: so o corpo", INV, """replace-body total_cost
  let total = 0
  for item in items
    total += item["price"]
  return total
""", [(a + 1, b)], contains=["def total_cost(items):"], absent=["price_with_tax(item"], run_py=lambda g: g["total_cost"]([{"price": 2}, {"price": 3}]) == 5)
a, b = sp(INV, "price_with_tax")
check("replace-body guarda o docstring", INV, """replace-body price_with_tax
  return price * 2
""", [(a + 2, b)], contains=['"""Return the gross price."""', "return price * 2"])
a, b = sp(INV, "Stock.add")
check("replace-body de metodo", INV, """replace-body Stock.add
  return self.qty + n
""", [(a + 1, b)], contains=["        return self.qty + n"], absent=["# add n units"] if False else [])
a, b = sp(INV, "Stock.remove")
check("replace-function de metodo (recuo e outros metodos intactos)", INV, """replace-function Stock.remove
  fn remove(n: int) -> int
    return n
""", [(a, b)], contains=["    def remove(self, n: int) -> int:", "        return n"])
# idempotencia: o mesmo replace aplicado de novo nao escreve
spec_same = """replace-function Stock.remove
  fn remove(n: int) -> int
    return n
"""
rc, out, err, src = run(INV, spec_same)
open(os.path.join(T, "inv2.py"), "wb").write(out)
check_nochange("replace-function com unidade identica: nada e escrito", "inv2.py", spec_same)
# ---------------- renomear ----------------
check("rename de funcao: definicao e uso, mais nada", INV, "rename price_with_tax gross_price\n", [(11, 11), (22, 22)], maxchanged=2, absent=["price_with_tax("], contains=["gross_price(item"])
check("rename de classe: usos no main", INV, "rename Stock Bin\n", [(30, 60), (guard, guard + 5)], contains=["class Bin:", 's = Bin("a", 3)'], absent=["Stock"], maxchanged=3)
check("rename de metodo: def e chamadas com ponto", INV, "rename Stock.report render\n", [(1, 80)], contains=["def render(self)", "s.render()"], maxchanged=2)
mk("shadow.py", "def total(x):\n    return x\n\n\ndef use(total):\n    return total + 1\n\n\ndef use2():\n    return total(1)\n\n\nresult = total(2)\n")
check("rename: parametro com o mesmo nome fica como esta", "shadow.py", "rename total grand\n", [(1, 1), (10, 10), (13, 13)], contains=["def use(total):", "return total + 1", "grand(1)", "result = grand(2)"], maxchanged=3)
mk("kw.py", "def size():\n    return 3\n\n\ndef build(size=0):\n    return size\n\n\nx = build(size=size())\n")
check("rename: argumento nomeado fica", "kw.py", "rename size count\n", [(1, 1), (9, 9)], contains=["build(size=count())", "def build(size=0)"], maxchanged=2)
mk("imp.py", "from lib import helper\n\n\ndef helper2():\n    return helper()\n")
check_err("rename: nome ja existe", INV, "rename total_cost summary\n", "`summary` is already defined")
check_err("rename: simbolo que nao existe lista os que existem", INV, "rename nada outro\n", "Definitions there:")
# ---------------- parametros ----------------
a, b = sp(INV, "total_cost")
check("add-param com default: so o cabecalho", INV, "add-param total_cost tax=0.2\n", [(a, a)], contains=["def total_cost(items, tax=0.2):"], maxchanged=1)
a, b = sp(TXT, "wrap_words"); a2, b2 = sp(TXT, "uses_wrap"); a3, b3 = sp(TXT, "long_call_site")
new = check("add-param + call= em chamadas de uma linha e de varias", TXT, 'add-param wrap_words prefix: str = "" call=">"\n', [(a, a + 3), (a2, b2), (a3, b3)], contains=['    indent: str = "",\n               prefix: str = ""', 'wrap_words(["x", "y"], 5, prefix=">")', 'prefix=">")\n    return a, b', "        width=10,\n        prefix=\">\",\n"], maxchanged=9)
a, b = sp(TXT, "slugify")
check_err("add-param sem default depois de defaults: erro", TXT, "add-param slugify flag\n", "needs a default too")
check_err("add-param repetido: erro", TXT, "add-param slugify sep='.'\n", "already has a parameter")
mk("star.py", "def two(a, *rest, key=None, **kw):\n    return (a, rest, key, kw)\n\n\nprint(two(1))\n")
check("add-param antes de *args", "star.py", "add-param two z=1 call=2\n", [(1, 1), (5, 5)], contains=["def two(a, z=1, *rest, key=None, **kw):", "two(1, z=2)"], maxchanged=2)
check("add-param a metodo (chamadas por .nome)", SHP, "add-param Circle.scale pivot=0 call=1\n", [sp(SHP, "Circle.scale")[0:1] * 2][0:1] and [(sp(SHP, "Circle.scale")[0], sp(SHP, "Circle.scale")[0])], contains=["def scale(self, k, pivot=0):"], maxchanged=1)
# ---------------- inserir ----------------
a, b = sp(TXT, "first_word")
check("insert before 1: acima do comentario que pertence a instrucao", TXT, """insert first_word before 1
  let n = len(s)
""", [(a, a + 1)], contains=["    n = len(s)\n    # the first word, or empty\n    parts = s.split()"], maxchanged=1)
check("insert after 2: entre as instrucoes", TXT, """insert first_word after 2
  let m = 1
""", [(a + 3, b)], contains=["return \"\"\n    m: int = 1\n    return parts[0]"] if False else ["m = 1"], maxchanged=1)
a, b = sp(TXT, "slugify")
check("insert start: depois do docstring", TXT, """insert slugify start
  let out0 = 1
""", [(a + 1, a + 2)], contains=['"""Lower-case slug."""\n    out0 = 1\n    out = []'], maxchanged=1)
a, b = sp(TXT, "outer")
check("insert end com vars", TXT, """insert outer end vars total
  total = total + 1
""", [(b, b + 1)], contains=["return total\n    total = total + 1"], maxchanged=1)
check("insert-py: Python como esta, com o recuo do corpo", TXT, """insert-py outer before 3
  for _ in range(2):
      pass
""", [(a, b)], contains=["    for _ in range(2):\n        pass\n    for i in range(n):"], maxchanged=2)
check_err("insert: instrucao fora do corpo", TXT, "insert outer before 9\n  let q = 1\n", "has only")
check_err("insert: erro no CSP da carga com a linha", TXT, "insert outer end\n  let q = nada + 1\n", "spec line 1: variable `nada` is not declared")
# ---------------- apagar ----------------
a, b = sp(TXT, "count_vowels")
check("delete-function (def de uma linha)", TXT, "delete-function count_vowels\n", [(a - 2, a + 3)], absent=["count_vowels"], maxchanged=3)
a, b = sp(INV, "Stock.report")
check("delete-function de metodo", INV, "delete-function Stock.report\n", [(a - 1, b + 1)], absent=["def report"], maxchanged=3)
a, b = sp(INV, "summary")
check("delete-function da ultima antes do main", INV, "delete-function summary\n", [(a, b + 3)], absent=["def summary"], maxchanged=8)
check_err("delete-function que nao existe", INV, "delete-function nao_existe\n", "no definition")
# ---------------- CRLF, estruturas fora do subconjunto, erros da especificacao ----------------
rc, out, err, src = run("crlf.py", "add-function end\n  fn third() -> int\n    return 3\n")
if rc == 0 and b"\r\n    return 3\r\n" in out or (rc == 0 and out.endswith(b"return 3")): 
    old = open(src, "rb").read()
    if out.startswith(old) and out[len(old):].count(b"\n") == out[len(old):].count(b"\r\n") and b"\n" not in out.replace(b"\r\n", b""): ok("CRLF: texto novo tambem em CRLF, o resto intacto")
    else: fail("CRLF", repr(out[len(old):]))
else: fail("CRLF add", err + repr(out))
rc, out, err, src = run("crlf.py", "rename greet hello\n")
if rc == 0 and b"def hello(name):\r\n" in out and b"return hello(name).upper()\r\n" in out and out.replace(b"hello", b"greet") == open(src, "rb").read(): ok("CRLF: rename mexe so nos tokens")
else: fail("CRLF rename", err)
check_err("indentacao inconsistente: recusa e explica", "badindent.py", "rename f g\n", "inconsistent indentation")
mk("unbal.py", "def f(x):\n    return (x + 1\n")
check_err("parenteses sem fechar: recusa", "unbal.py", "rename f g\n", "unbalanced brackets")
check_err("comando desconhecido", INV, "frobnicate x\n", "unknown command `frobnicate`")
check_err("carga mal recuada", INV, "add-function end\nfn x() -> int\n  return 1\n", "unknown command `fn`")
check_err("operacao falha: nada muda (as anteriores tambem nao)", INV, "rename total_cost sumario\nrename nada nada2\n", "nothing was changed")
# ---------------- casos com formato incomum ----------------
check("unusual.py: def de uma linha, *args, async, string com `def` dentro: rename de uma funcao", "unusual.py", "rename three tres\n", [(13, 13)], contains=["def tres(a):"], maxchanged=1)
check("unusual.py: replace-body com a funcao seguinte a uma string multilinha", "unusual.py", "replace-body three\n  return 7\n", [(13, 16)], contains=["return 7"], maxchanged=2)
check_err("unusual.py: replace-body de def de uma linha", "unusual.py", "replace-body one\n  return 1\n", "no multi-line body")
check_err("unusual.py: replace-body de assinatura com *args pede outro comando", "unusual.py", "replace-body two\n  return a\n", "*args, **kwargs")
mk("deco.py", "import functools\n\n\n@functools.lru_cache(maxsize=None)\ndef fib(n):\n    return n if n < 2 else fib(n - 1) + fib(n - 2)\n")
check("rename mantem o decorador", "deco.py", "rename fib fibo\n", [(5, 6)], contains=["@functools.lru_cache(maxsize=None)\ndef fibo(n):", "fibo(n - 1) + fibo(n - 2)"], maxchanged=2)
check("add-function em arquivo sem outras funcoes (depois do import)", "deco.py", "add-function end\n  fn one() -> int\n    return 1\n", [(7, 8)], contains=["def one() -> int:"])
check("tool.py: add-function end fica antes do ponto de entrada (e o comentario dele)", TOOL, "add-function end\n  fn helper(x: int) -> int\n    return x\n", [(28, 32)], contains=["def helper(x: int) -> int:\n    return x\n\n\n# entry point\nif __name__"])
# ---------------- classes ----------------
a, b = sp(SHP, "Circle.area")
check("replace-function Circle.area", SHP, "replace-function Circle.area\n  fn area() -> float\n    return 3.14 * self.r\n", [(a, b)], contains=["    def area(self) -> float:\n        return 3.14 * self.r"])
check("rename da classe base: todos os usos", SHP, "rename Shape Base\n", [(1, 60)], contains=["class Base:", "class Circle(Base):", "Base.__init__(self, \"circle\")"], absent=["Shape"], maxchanged=6)
# ---------------- acoes por simbolo e modos de gravacao ----------------
rc, out, err, src = run(INV, "replace-body total_cost\n  return 0\nadd-function end\n  fn zed() -> int\n    return 1\n", extra=["--acts"])
if rc == 0 and "act 0 total_cost" in err and "act 1 summary" in err: ok("acoes por simbolo: replace total_cost, after summary (para o Edit por simbolo)")
else: fail("acoes", err)
rc, out, err, src = run(INV, "add-method Stock\n  fn clear() -> int\n    return 0\n", extra=["--acts"])
if "act 1 Stock.remove" in err or "act 1 Stock.report" in err or "act 0 Stock" in err: ok("acoes: metodo novo vira acao no metodo ou na classe")
else: fail("acoes de metodo", err)
w = os.path.join(T, "w.py"); shutil.copy(os.path.join(INC, TXT), w); os.chmod(w, 0o751)
sp_ = os.path.join(T, "w.spec"); open(sp_, "w").write("rename slugify slug\n")
subprocess.run([U, "inc", "apply", w, sp_], capture_output=True)
if open(w).read() == open(os.path.join(INC, TXT)).read(): ok("sem --write o arquivo nao e tocado")
else: fail("dry-run escreveu")
p = subprocess.run([U, "inc", "apply", w, sp_, "--write"], capture_output=True)
if p.returncode == 0 and "def slug(" in open(w).read() and (os.stat(w).st_mode & 0o777) == 0o751 and not [f for f in os.listdir(T) if f.endswith(".tmp")]: ok("--write grava de forma atomica, mantem as permissoes, sem temporario")
else: fail("--write", p.stderr.decode())
# ---------------- leitura para a UIR-H com mapa no -> bytes ----------------
p = subprocess.run([U, "inc", "read", os.path.join(INC, INV)], capture_output=True, text=True)
src_t = open(os.path.join(INC, INV), "rb").read()
good = True; seen = 0
for line in p.stdout.splitlines():
    parts = line.split()
    if parts[0] in ("fn", "class") and "bytes" in parts:
        b0, b1 = map(int, parts[parts.index("bytes") + 1].split("-")); seen += 1
        if not src_t[b0:b1].decode().lstrip().startswith(("def " + parts[1], "class " + parts[1], "@")): good = False; fail("mapa no->bytes", line)
if good and seen >= 4 and "fn     price_with_tax lines 11-16" in p.stdout: ok("UIR-H: cada fn/class mapeia ao intervalo exato de bytes e linhas (%d nos)" % seen)
else: fail("UIR-H leitura", p.stdout[:400])
u = subprocess.run([U, "inc", "uir", os.path.join(INC, INV)], capture_output=True, text=True).stdout
if "fn price_with_tax(price, rate = 0.2)" in u and "raw python defines decorated" in u and "class Stock" in u: ok("UIR-H: o que se entende vira fn/class/const, o resto raw")
else: fail("UIR-H csp", u[:300])
# ---------------- arquivo grande: so a funcao tocada muda e o tempo e baixo ----------------
big = "".join("def f%d(a, b):\n    # work %d\n    t = a * %d + b\n    return t\n\n\n" % (i, i, i) for i in range(3000))
mk("big.py", big)
t0 = time.time()
rc, out, err, src = run("big.py", "replace-body f1500\n  return a + b\n")
dt = time.time() - t0
ol, nl = lines_of(big.encode()), lines_of(out)
diff = [x for x in difflib.unified_diff(ol, nl, n=0) if x[0] in "+-" and x[:3] not in ("+++", "---")]
if rc == 0 and len(diff) == 4 and dt < 3: ok("arquivo de 18000 linhas: 4 linhas mudam (comentario, atribuicao e return saem, 1 entra), %.0f ms" % (dt * 1000))
else: fail("arquivo grande", "%d changed lines, %.1fs %s" % (len(diff), dt, err))
shutil.rmtree(T)
print("\n" + ("OK %d checks" % N if not BAD else "FAILED %d of %d" % (BAD, N)))
sys.exit(1 if BAD else 0)
