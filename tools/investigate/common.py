# Funcoes comuns da bancada de investigacao (perguntas, copias dos repositorios).
import json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
KATANA = os.environ.get("KATANA_BIN", "/mnt/dados/source/katana/katana")
REPOS = os.environ.get("INVEST_REPOS", "/tmp/katana-investigate")


def questions():
    with open(os.path.join(HERE, "questions.json"), encoding="utf-8") as f:
        return json.load(f)


def question(qid):
    for q in questions():
        if q["id"] == qid:
            return q
    sys.exit("pergunta desconhecida: " + qid)


def worktree(q):
    """Pasta da copia (worktree destacado no commit base). Cria se nao existir."""
    hint = q.get("worktree_hint")
    if hint and os.path.isdir(hint):
        return hint
    d = os.path.join(REPOS, q["id"])
    if not os.path.isdir(d):
        os.makedirs(REPOS, exist_ok=True)
        r = subprocess.run(["git", "-C", q["repo"], "worktree", "add", "--detach", d, q["base"]],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("worktree falhou: " + r.stderr)
    return d


MAX_TEXT = 140
MAX_LIMIT = 80
# esquema curto das ferramentas (src/tools/table.c, g_ct_short): nome -> (propriedades, obrigatorias)
SCHEMA = {
    "Read": ({"file_path": str, "offset": int, "limit": int}, {"file_path"}),
    "Grep": ({"pattern": str, "path": str, "glob": str, "output_mode": str, "-i": bool, "-C": int, "literal": bool}, {"pattern"}),
    "Glob": ({"pattern": str, "path": str}, {"pattern"}),
}
OUTPUT_MODES = {"content", "files_with_matches", "count"}


def check_args(name, args):
    if name not in SCHEMA:
        return "ferramenta sem esquema: " + name
    props, req = SCHEMA[name]
    for k, v in args.items():
        if k not in props:
            return "argumento fora do esquema de %s: %s" % (name, k)
        t = props[k]
        if t is int and (isinstance(v, bool) or not isinstance(v, int)):
            return "%s.%s deve ser inteiro" % (name, k)
        if t is bool and not isinstance(v, bool):
            return "%s.%s deve ser booleano" % (name, k)
        if t is str and not isinstance(v, str):
            return "%s.%s deve ser texto" % (name, k)
    for k in req:
        if k not in args:
            return "falta %s.%s" % (name, k)
    if name == "Grep":
        if args.get("output_mode", "content") not in OUTPUT_MODES:
            return "Grep.output_mode invalido"
        try:
            re.compile(args["pattern"]) if not args.get("literal") else None
        except re.error:
            pass     # o motor usa regex propria (POSIX); nao reprovamos por diferenca de dialeto
    if name == "Read":
        lim = args.get("limit")
        if lim is None or lim < 1 or lim > MAX_LIMIT:
            return "Read sem limit ou com limit > %d" % MAX_LIMIT
        if args.get("offset", 1) < 1:
            return "Read.offset < 1"
    return None


