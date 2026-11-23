#!/usr/bin/env python3
"""Motor de investigacao (prototipo): sinais alem do BM25, propagacao no grafo de arquivos, ranqueador aprendido e politica
de investigacao simulada. Pesquisa em Python; o que vencer vai para C (ver docs/investigation-engine.md).

  python3 tools/findbench/investigator.py train            # dados de treino (commits) dos tres repositorios + modelo
  python3 tools/findbench/investigator.py commits          # tabela (ii): bench de commits, BM25 x hibrido x aprendido
  python3 tools/findbench/investigator.py questions        # tabelas (i), (iii), politica e ablacao nas 76 perguntas
  python3 tools/findbench/investigator.py case grey-gkatana  # estudo de caso: ordem dos arquivos e a nota de sementes
  python3 tools/findbench/investigator.py timing REPO      # tempo de indexar (grafo) e de consultar

Regras de vazamento: o co-mudanca de uma copia usa so o historico ate o commit base (HEAD da copia). O ranqueador e
treinado so com commits de OUTROS repositorios (leave-one-repo-out); nos repositorios de origem, as perguntas de treino
vem dos commits recentes e o grafo de co-mudanca so dos commits anteriores a eles (divisao temporal).
"""
import json, math, os, pickle, re, subprocess, sys, time
from collections import defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench
from bench import terms

bench.STEM = True
bench.UNIQ = True
HERE = os.path.dirname(os.path.abspath(__file__))
QFILE = os.environ.get('INVEST_QUESTIONS', '/mnt/dados/source/katana/tools/investigate/questions.json')
REPOS = os.environ.get('INVEST_REPOS', '/mnt/dados/investigate-repos')
CACHE = os.environ.get('FINDBENCH_CACHE', os.path.join(os.environ.get('TMPDIR', '/tmp'), 'findbench-cache'))
SOURCES = {'greymod': '/mnt/dados/source/greymod', 'katana': '/mnt/dados/source/katana', 'sonora': '/mnt/dados/sonora-test'}
ETYPES = ('sym', 'path', 'rare', 'co')
STEP_FILES, MAX_STEPS, MAX_TOK = 2, 10, 8000


def git(repo, *a):
    return subprocess.run(['git', '-C', repo] + list(a), capture_output=True, text=True, errors='replace').stdout


def repo_key(path):
    for k, v in SOURCES.items():
        if path.rstrip('/') == v:
            return k
    return os.path.basename(path)


# ------------------------------------------------------------------ grafo de arquivos
_ident = re.compile(r'[A-Za-z_][A-Za-z0-9_]{3,}')
_pathref = re.compile(r'[\w\-./]*[\w\-]\.(?:mjs|cjs|js|ts|tsx|py|sh|c|h|cc|cpp|hpp|json|kt|java|qml|md|toml|ya?ml|go|rs|bat|ps1|cmake|glsl|comp|swift|m|txt|xml|gradle)\b')
_relimp = re.compile(r'''['"](\.{1,2}/[\w\-./]+)['"]''')
_pyimp = re.compile(r'^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w.]+))', re.M)
KW = set('self this that true false null none return static const void char else elif while break continue struct class '
         'import export from function async await default public private protected final override string bool size_t uint '
         'int32 int64 uint8 uint32 uint64 float double main init data name type value text path file line list item args '
         'result error test self'.split())


DOC_EXT = ('.md', '.txt', '.rst', '.adoc', '.html', '.json', '.xml', '.yml', '.yaml', '.toml', '.cfg')
# palavras de pedido (nao dizem ONDE olhar); lista curta e independente de projeto
REQUEST = set(bench.terms('descubra descobrir diga dizer explique explicar altere alterar mude mudar nada precisa precisar '
                          'feito fazer faz funciona funcionar funcionando aparece aparecer acontece porque motivo causa '
                          'onde quando qual quais projeto codigo arquivo problema erro corrija corrigir veja investigue '
                          'find explain tell why what where which change fix project code file problem issue happen happens '
                          'work works working does should need needs'))


def is_code(r):
    return not r.lower().endswith(DOC_EXT) and not os.path.basename(r).upper().startswith(('README', 'CHANGELOG', 'LICENSE'))


def is_identlike(t):
    return '_' in t.strip('_') or bool(re.search(r'[a-z][A-Z]', t)) or (t.isupper() and len(t) >= 6)


class Graph:
    """arestas nao dirigidas por tipo; sym: uso de simbolo definido em outro arquivo; path: um arquivo cita o caminho do
    outro; rare: identificador raro (df 2..4) compartilhado; co: mudaram juntos no git"""

    def __init__(self, repo, files, cutoff=0, history=None):
        t0 = time.time()
        self.rels = list(files)
        self.id = {r: i for i, r in enumerate(self.rels)}
        n = self.n = len(self.rels)
        idents = [set(_ident.findall(files[r])) for r in self.rels]
        self.idents = idents
        udf = defaultdict(int)
        for s in idents:
            for t in s:
                udf[t] += 1
        self.udf = udf
        defs = defaultdict(set)
        self.code = np.array([is_code(r) for r in self.rels])
        for i, r in enumerate(self.rels):
            if not self.code[i]:
                continue
            for ln in files[r].splitlines():
                m = bench._symre.match(ln)
                if m and m.group(1) and len(m.group(1)) >= 4 and m.group(1).lower() not in KW:
                    defs[m.group(1)].add(i)
        self.defs = {k: v for k, v in defs.items() if len(v) <= 3}
        E = {t: defaultdict(float) for t in ETYPES}
        lim = max(30, int(0.02 * n))
        for i, s in enumerate(idents):        # sym (so entre arquivos de codigo; prosa casa nomes por acaso)
            if not self.code[i]:
                continue
            for t in s:
                d = self.defs.get(t)
                if d is None or udf[t] > lim:
                    continue
                w = math.log(1 + n / udf[t])
                for j in d:
                    if j != i:
                        E['sym'][(min(i, j), max(i, j))] += w
        rare = defaultdict(list)                # rare
        for i, s in enumerate(idents):
            for t in s:
                if 2 <= udf[t] <= 4 and len(t) >= 6 and is_identlike(t) and t not in self.defs:
                    rare[t].append(i)
        for t, fl in rare.items():
            w = math.log(1 + n / len(fl)) / (len(fl) - 1)
            for a in range(len(fl)):
                for b in range(a + 1, len(fl)):
                    E['rare'][(fl[a], fl[b])] += w
        base = defaultdict(list)                 # path
        stemmap = defaultdict(list)
        for i, r in enumerate(self.rels):
            b = os.path.basename(r)
            base[b].append(i)
            stemmap[os.path.splitext(r)[0]].append(i)
        for i, r in enumerate(self.rels):
            txt = files[r]
            tg = set()
            for m in _pathref.findall(txt):
                c = base.get(os.path.basename(m))
                if c and len(c) <= 3:
                    tg.update(c)
            d0 = os.path.dirname(r)
            for m in _relimp.findall(txt):
                p = os.path.normpath(os.path.join(d0, m))
                c = stemmap.get(os.path.splitext(p)[0]) or stemmap.get(p) or stemmap.get(p + '/index')
                if c:
                    tg.update(c)
            if r.endswith('.py'):
                for a, b2 in _pyimp.findall(txt):
                    mod = (a or b2).replace('.', '/')
                    for cand in (os.path.join(d0, mod), mod):
                        c = stemmap.get(os.path.normpath(cand))
                        if c:
                            tg.update(c)
            for j in tg:
                if j != i:
                    E['path'][(min(i, j), max(i, j))] += 1.0
        self.last = np.zeros(n)                  # co-mudanca e recencia (so historico antes do corte)
        hist = history if history is not None else commit_log(repo)
        hist = hist[cutoff:]
        for k, (h, ts, s, fl) in enumerate(hist):     # hist: mais recente primeiro
            ids = [self.id[f] for f in fl if f in self.id]
            for i in ids:
                if self.last[i] == 0:
                    self.last[i] = len(hist) - k
            if 2 <= len(ids) <= 20:
                w = 1.0 / (len(ids) - 1)
                for a in range(len(ids)):
                    for b in range(a + 1, len(ids)):
                        E['co'][(min(ids[a], ids[b]), max(ids[a], ids[b]))] += w
        if self.last.max() > 0:
            self.last = self.last / self.last.max()
        self.arr = {}
        for t, d in E.items():
            if d:
                k = np.array(list(d.keys()), dtype=np.int64)
                w = np.array(list(d.values()))
                if t == 'co':
                    w = np.minimum(w, 3.0)
                self.arr[t] = (np.concatenate([k[:, 0], k[:, 1]]), np.concatenate([k[:, 1], k[:, 0]]), np.concatenate([w, w]))
            else:
                self.arr[t] = (np.zeros(0, np.int64), np.zeros(0, np.int64), np.zeros(0))
        self.edges = {t: len(self.arr[t][2]) // 2 for t in ETYPES}
        self._comb = {}
        self.build_s = time.time() - t0

    def comb(self, types):
        key = tuple(sorted(types))
        if key in self._comb:
            return self._comb[key]
        S, D, W = [], [], []
        for t in key:
            s, d, w = self.arr[t]
            if len(w) == 0:
                continue
            rs = np.bincount(s, weights=w, minlength=self.n)
            S.append(s); D.append(d); W.append(w / rs[s])
        if S:
            s, d, w = np.concatenate(S), np.concatenate(D), np.concatenate(W)
            rs = np.bincount(s, weights=w, minlength=self.n)
            w = w / rs[s]
            hasout = rs > 0
        else:
            s, d, w, hasout = np.zeros(0, np.int64), np.zeros(0, np.int64), np.zeros(0), np.zeros(self.n, bool)
        self._comb[key] = (s, d, w, hasout)
        return self._comb[key]

    def ppr(self, seed, types=ETYPES, alpha=0.5, iters=10):
        s, d, w, hasout = self.comb(types)
        r = seed.copy()
        for _ in range(iters):
            push = np.bincount(d, weights=w * r[s], minlength=self.n) if len(w) else np.zeros(self.n)
            r = (1 - alpha) * seed + alpha * (push + r[~hasout].sum() * seed)
        return r

    def linked(self, i, j):
        return [t for t in ETYPES if np.any((self.arr[t][0] == i) & (self.arr[t][1] == j))]

    def why(self, i, j):
        """o que liga dois arquivos (para a nota de sementes): simbolo, caminho, identificador raro"""
        a, b = self.rels[i], self.rels[j]
        both = self.idents[i] & self.idents[j]
        syms = [t for t in both if t in self.defs and (i in self.defs[t] or j in self.defs[t])]
        if syms:
            return 'symbol ' + min(syms, key=lambda t: self.udf[t])
        if os.path.basename(b) in FILES_TEXT.get(a, '') or os.path.basename(a) in FILES_TEXT.get(b, ''):
            return 'path reference'
        rare = [t for t in both if self.udf[t] <= 4 and is_identlike(t)]
        if rare:
            return 'shared ' + min(rare, key=lambda t: self.udf[t])
        return 'changed together'


FILES_TEXT = {}


def commit_log(repo, n=4000):
    out, cur = [], None
    for line in git(repo, 'log', '--no-merges', '--name-only', '--pretty=format:@@%H%x09%ct%x09%s', '-n', str(n)).splitlines():
        if line.startswith('@@'):
            if cur:
                out.append(cur)
            h, ts, s = line[2:].split('\t', 2)
            cur = (h, int(ts), s, [])
        elif line.strip() and cur:
            cur[3].append(line.strip())
    if cur:
        out.append(cur)
    return out


# ------------------------------------------------------------------ um repositorio indexado
class Repo:
    def __init__(self, path, cutoff=0, history=None):
        t0 = time.time()
        self.path = path
        self.files = bench.load_corpus(path)
        FILES_TEXT.clear(); FILES_TEXT.update(self.files)
        self.ix = bench.Index(self.files)
        self.g = Graph(path, self.files, cutoff, history)
        self.fid = self.g.id
        n = self.g.n
        self.size = np.array([math.log(1 + len(self.files[r])) for r in self.g.rels])
        self.isdoc = np.array([r.endswith(('.md', '.txt')) or '/docs/' in '/' + r for r in self.g.rels], float)
        self.istest = np.array([bool(re.search(r'(^|/)(tests?|spec)(/|_)|_test\.|\.test\.|test_', r)) for r in self.g.rels], float)
        self.iscfg = np.array([r.endswith(('.json', '.yml', '.yaml', '.toml', '.xml', '.cfg', '.gradle', '.cmake')) for r in self.g.rels], float)
        # termos por arquivo (para o recorte 'salto' e ancoras): de cada termo, os arquivos que o contem
        self.cfile = np.array([self.fid[c[0]] for c in self.ix.chunks], np.int64)
        self.build_s = time.time() - t0

    def files_with(self, t):
        s = set()
        for f in ('b', 'p'):
            for cid, _ in self.ix.post[f].get(t, ()):
                s.add(self.cfile[cid])
        return s

    def bm25_files(self, q):
        sc = bench.algo_param(cover=0.05, idw=True)(self.ix, q)
        v = np.zeros(self.g.n)
        best = np.zeros(self.g.n, np.int64) - 1
        for cid, s in sc.items():
            i = self.cfile[cid]
            if s > v[i]:
                v[i] = s; best[i] = cid
        return v, best


def seed_vec(v, k=30):
    s = np.zeros_like(v)
    if v.max() <= 0:
        return s
    top = np.argsort(-v)[:k]
    s[top] = (v[top] / v.max()) ** 2
    return s / s.sum()


NFEAT = ['bm25', 'bm25_rr', 'has_qterm', 'path_hit', 'ppr_sym', 'ppr_path', 'ppr_rare', 'ppr_co', 'ppr_all', 'ppr_all_rr',
         'is_doc', 'is_test', 'is_cfg', 'recency', 'log_size']
NOPPR = {'ppr_sym', 'ppr_path', 'ppr_rare', 'ppr_co', 'ppr_all', 'ppr_all_rr'}


def features(R, q, ncand=60, types=ETYPES):
    """candidatos (melhores do BM25 e do PPR) e a matriz de features; tambem as variantes do PPR sem cada tipo (ablacao)"""
    v, best = R.bm25_files(q)
    seed = seed_vec(v)
    n = R.g.n
    pp = {t: R.g.ppr(seed, (t,)) if seed.sum() else np.zeros(n) for t in ETYPES}
    pall = R.g.ppr(seed, types) if seed.sum() else np.zeros(n)
    minus = {t: R.g.ppr(seed, tuple(x for x in ETYPES if x != t)) if seed.sum() else np.zeros(n) for t in ETYPES}
    qt = set(terms(q))
    has = np.zeros(n)
    for t in qt:
        for i in R.files_with(t):
            has[i] = 1
    phit = np.zeros(n)
    for t in qt:
        for cid, _ in R.ix.post['p'].get(t, ()):
            phit[R.cfile[cid]] = 1
    cand = list(dict.fromkeys(list(np.argsort(-v)[:ncand][v[np.argsort(-v)[:ncand]] > 0]) + list(np.argsort(-pall)[:ncand])))
    cand = np.array(cand, np.int64)
    rb = np.empty(n); rb[np.argsort(-v)] = np.arange(1, n + 1)
    rp = np.empty(n); rp[np.argsort(-pall)] = np.arange(1, n + 1)
    nz = lambda x: x / x.max() if x.max() > 0 else x
    cols = {'bm25': nz(v), 'bm25_rr': np.where(v > 0, 1 / rb, 0), 'has_qterm': has, 'path_hit': phit,
            'ppr_sym': nz(pp['sym']), 'ppr_path': nz(pp['path']), 'ppr_rare': nz(pp['rare']), 'ppr_co': nz(pp['co']),
            'ppr_all': nz(pall), 'ppr_all_rr': 1 / rp, 'is_doc': R.isdoc, 'is_test': R.istest, 'is_cfg': R.iscfg,
            'recency': R.g.last, 'log_size': R.size / 12}
    X = np.stack([cols[f][cand] for f in NFEAT], 1)
    extra = {t: nz(minus[t])[cand] for t in ETYPES}
    return cand, X, extra, v, best, seed


# ------------------------------------------------------------------ ranqueador (regressao logistica, numpy)
class LR:
    def __init__(self, feats=None):
        self.feats = feats or NFEAT

    def _sel(self, X):
        return X[:, [NFEAT.index(f) for f in self.feats]]

    def fit(self, X, y, l2=1e-2, it=600, lr=0.3):
        X = self._sel(X)
        self.mu, self.sd = X.mean(0), X.std(0) + 1e-6
        Z = (X - self.mu) / self.sd
        pw = (len(y) - y.sum()) / max(1, y.sum())
        sw = np.where(y > 0, pw, 1.0)
        sw = sw / sw.mean()
        w = np.zeros(Z.shape[1]); b = 0.0
        for _ in range(it):
            p = 1 / (1 + np.exp(-(Z @ w + b)))
            g = (p - y) * sw
            w -= lr * (Z.T @ g / len(y) + l2 * w)
            b -= lr * g.mean()
        self.w, self.b = w, b
        return self

    def score(self, X):
        return (self._sel(X) - self.mu) / self.sd @ self.w + self.b


def hybrid_score(X, lam=0.5):
    return X[:, NFEAT.index('bm25')] + lam * X[:, NFEAT.index('ppr_all')]


# ------------------------------------------------------------------ dados de treino (commits) por repositorio
def commit_queries(hist, files, k):
    """perguntas tiradas dos k commits mais recentes (gabarito = arquivos do commit que existem no HEAD)"""
    qs, seen = [], set()
    for h, ts, s, fl in hist[:k]:
        if s in seen:
            continue
        seen.add(s)
        truth = [f for f in fl if f in files and not f.startswith(('docs/handoff', 'docs/comparisons'))]
        s2 = re.sub(r'^(feat|fix|merge|docs|chore|refactor|test)[:(\w)]*:\s*', '', s, flags=re.I).strip()
        if len(s2) < 25 or not (1 <= len(truth) <= 4) or len(truth) < len(fl) * 0.5:
            continue
        qs.append((h, s2, truth))
    return qs


def build_train(name, nmax=150):
    path = SOURCES[name]
    out = os.path.join(CACHE, f'train-{name}.pkl')
    if os.path.exists(out):
        return pickle.load(open(out, 'rb'))
    hist = commit_log(path)
    k = min(400, int(len(hist) * 0.35))
    R = Repo(path, cutoff=k, history=hist)
    qs = commit_queries(hist, R.files, k)[:nmax]
    rows = []
    for h, q, truth in qs:
        t0 = time.time()
        cand, X, extra, v, best, seed = features(R, q)
        y = np.array([R.g.rels[i] in truth for i in cand], float)
        missing = len([t for t in truth if t not in {R.g.rels[i] for i in cand}])
        rows.append(dict(q=q, truth=truth, cand=[R.g.rels[i] for i in cand], X=X, y=y, extra=extra, missing=missing,
                         ms=(time.time() - t0) * 1000))
    d = dict(name=name, rows=rows, nfiles=R.g.n, build_s=R.build_s, graph_s=R.g.build_s, edges=R.g.edges, cutoff=k)
    os.makedirs(CACHE, exist_ok=True)
    pickle.dump(d, open(out, 'wb'))
    print(f'{name}: {R.g.n} arquivos, {len(rows)} perguntas de commit, indice {R.build_s:.1f}s (grafo {R.g.build_s:.1f}s), '
          f'arestas {R.g.edges}', flush=True)
    return d


def train_model(exclude, feats=None, swap=None):
    X, y = [], []
    for name in SOURCES:
        if name == exclude:
            continue
        for r in build_train(name)['rows']:
            Xi = r['X'].copy()
            if swap:
                Xi[:, NFEAT.index('ppr_all')] = r['extra'][swap]
            X.append(Xi); y.append(r['y'])
    return LR(feats).fit(np.concatenate(X), np.concatenate(y))


def rank_eval(rows, scorer):
    h1 = h5 = h10 = mrr = 0.0
    for r in rows:
        sc = scorer(r)
        order = [r['cand'][i] for i in np.argsort(-sc)]
        ranks = [order.index(t) + 1 for t in r['truth'] if t in order]
        b = min(ranks) if ranks else 0
        h1 += b == 1; h5 += 0 < b <= 5; h10 += 0 < b <= 10; mrr += 1 / b if b else 0
    n = max(1, len(rows))
    return h1 / n, h5 / n, h10 / n, mrr / n


def cmd_commits():
    print('Bench de commits (perguntas = commits recentes; co-mudanca so de commits anteriores; aprendido = leave-one-repo-out)')
    print(f'{"repo":8s} {"metodo":22s} hit@1  hit@5  hit@10  MRR')
    for name in SOURCES:
        d = build_train(name)
        rows = d['rows']
        methods = {'BM25 (Find atual)': lambda r: r['X'][:, 0] + 1e-9 * r['X'][:, 1],
                   'hibrido bm25+0.5ppr': lambda r: hybrid_score(r['X'])}
        m = train_model(name)
        methods['aprendido (LORO)'] = lambda r, m=m: m.score(r['X'])
        m2 = train_model(name, [f for f in NFEAT if f not in NOPPR])
        methods['aprendido sem grafo'] = lambda r, m=m2: m.score(r['X'])
        for t in ETYPES:
            mt = train_model(name, [f for f in NFEAT if f != 'ppr_' + t], swap=t)
            methods[f'aprendido sem {t}'] = (lambda r, m=mt, t=t: m.score(np.concatenate(
                [r['X'][:, :8], r['extra'][t][:, None], r['X'][:, 9:]], 1)))
        for k, f in methods.items():
            h1, h5, h10, mrr = rank_eval(rows, f)
            print(f'{name:8s} {k:22s} {h1:5.1%} {h5:6.1%} {h10:6.1%}  {mrr:.3f}')
        ms = np.mean([r['ms'] for r in rows])
        print(f'{name:8s} ({len(rows)} perguntas, {d["nfiles"]} arquivos, features+PPR {ms:.0f} ms/consulta em Python, '
              f'gabarito fora dos candidatos {np.mean([r["missing"] > 0 for r in rows]):.0%})')
    m = train_model(None)
    print('pesos (todos os repositorios, features padronizadas):')
    for f, w in sorted(zip(m.feats, m.w), key=lambda x: -abs(x[1])):
        print(f'  {f:12s} {w:+.3f}')


# ------------------------------------------------------------------ perguntas de investigacao
def load_questions():
    qs = json.load(open(QFILE, encoding='utf-8'))
    if os.environ.get('INVEST_FAST'):    # amostra rapida: greymod e katana inteiros + 6 do sonora (3 treino, 3 validacao)
        so = [q for q in qs if 'sonora' in q['repo']]
        pick = {q['id'] for q in [x for x in so if x['split'] == 'train'][:3] + [x for x in so if x['split'] == 'val'][:3]}
        qs = [q for q in qs if 'sonora' not in q['repo'] or q['id'] in pick]
    return qs


def qdir(q):
    h = q.get('worktree_hint')
    return h if h and os.path.isdir(h) else os.path.join(REPOS, q['id'])


def resolve_answers(R, q):
    out = []
    for a in q['answer_files']:
        if a in R.fid:
            out.append(a)
        else:   # gabarito com caminho incompleto: casa pelo final do caminho
            c = [r for r in R.fid if r.endswith('/' + a)]
            if len(c) == 1:
                out.append(c[0])
    return out


def anchors(R, q):
    """O QUE: termos da pergunta presentes no indice, por tipo; COMO: a forma da consulta de cada um"""
    n = R.g.n
    res = []
    raw = {}
    for w in re.findall(r'[A-Za-z_][\w./\-]*', bench.strip_acc(q)):
        w = w.strip('.-/')
        for t in terms(w):
            raw.setdefault(t, w)
    for t, w in raw.items():
        fs = R.files_with(t)
        df = len(fs)
        incode = sum(R.g.code[i] for i in fs)
        if t in REQUEST or df == 0:
            kind = 'absent'
        elif incode == 0:
            kind = 'generic'      # so aparece em prosa: nao e ancora
        elif df == 0:
            kind = 'absent'
        elif df > 0.15 * n:
            kind = 'generic'
        elif any(t in terms(os.path.basename(R.g.rels[i])) for i in list(fs)[:200]) and \
                any(t in terms(R.g.rels[i]) for cid, _ in R.ix.post['p'].get(t, ()) for i in [R.cfile[cid]]):
            kind = 'filename'
        elif w in R.g.defs or w in R.g.udf and is_identlike(w):
            kind = 'identifier'
        elif df <= max(3, 0.01 * n):
            kind = 'rare'
        else:
            kind = 'term'
        res.append((t, w, kind, df))
    keep = [a for a in res if a[2] not in ('absent', 'generic')]
    keep.sort(key=lambda a: a[3])
    return keep, [a for a in res if a[2] in ('absent', 'generic')]


def anchor_hits(R, a):
    """COMO: filename -> Glob (arquivos cujo caminho tem o termo); identifier -> refs (definicao e usos); rare/term ->
    Grep literal / busca: os arquivos que contem o termo"""
    t, w, kind, df = a
    if kind == 'filename':
        return {R.cfile[cid] for cid, _ in R.ix.post['p'].get(t, ())}
    if kind == 'identifier' and w in R.g.defs:
        s = set(R.g.defs[w])
        return s | {i for i in R.files_with(t)}
    return R.files_with(t)


def chunk_tokens(R, i, best_cid, terms_bonus=None):
    """custo de ler o arquivo: o melhor trecho (40 linhas) ou, sem trecho casado, as primeiras 40 linhas"""
    rel = R.g.rels[i]
    if best_cid >= 0:
        _, a, b = R.ix.chunks[best_cid]
        txt = '\n'.join(R.files[rel].splitlines()[a - 1:b])
    else:
        txt = '\n'.join(R.files[rel].splitlines()[:40])
    return len(txt) / 3.5 + 30


def simulate(R, q, answers, need, base_scores, flags, trace=None):
    """politica de investigacao. base_scores: prioridade inicial por arquivo (vetor n). flags: expand, anchors, stuck,
    types. Cada passo = uma chamada de ferramenta que le ate STEP_FILES arquivos novos (o melhor trecho de cada)."""
    n = R.g.n
    v, best = R.bm25_files(q)
    base = base_scores.copy()
    if base.max() > base.min():
        base = (base - base.min()) / (base.max() - base.min())
    seed = seed_vec(v)
    anc = anchors(R, q)[0] if flags.get('anchors') else []
    bonus = np.zeros(n)
    used_anchor = 0
    if anc:          # ONDE (inicio): o termo mais raro da pergunta, consultado da forma certa para o tipo dele
        hits = anchor_hits(R, anc[0])
        if 0 < len(hits) <= max(5, 0.02 * n):
            for i in hits:
                bonus[i] += 0.6
        used_anchor = 1
    visited, tok, found = [], 0.0, set()
    stuck = 0
    types = flags.get('types', ETYPES)
    for step in range(1, MAX_STEPS + 1):
        pr = base + bonus
        if flags.get('expand') and visited:   # QUANDO: depois de cada leitura, a fronteira do grafo a partir do lido
            sv = np.zeros(n)
            sv[visited] = 1.0 / len(visited)
            fr = R.g.ppr(0.8 * sv + 0.2 * seed, types, alpha=0.6)
            fr[visited] = 0
            if fr.max() > 0:
                pr = pr + flags.get('wexp', 0.8) * fr / fr.max()
        pr[visited] = -1e9
        pick = [int(i) for i in np.argsort(-pr)[:STEP_FILES]]
        new = 0
        for i in pick:
            visited.append(i)
            tok += chunk_tokens(R, i, best[i])
            hit = R.g.rels[i] in answers
            if hit:
                found.add(R.g.rels[i]); new = 1
            if trace is not None:
                why = ''
                if len(visited) > 1:
                    link = [j for j in visited[:-1] if R.g.linked(j, i)]
                    why = (R.g.why(link[-1], i) + ' <- ' + R.g.rels[link[-1]]) if link else ''
                trace.append((step, R.g.rels[i], hit, 'anchor' if bonus[i] > 0 else ('search' if v[i] > 0 else 'graph'), why))
        if len(found) >= need:
            return step, tok, found
        if tok > MAX_TOK:
            return None, tok, found
        # sinal de travado: dois passos sem arquivo com ligacao forte -> proxima ancora
        stuck = 0 if new else stuck + 1
        if flags.get('stuck') and stuck >= 2 and used_anchor < len(anc):
            hits = anchor_hits(R, anc[used_anchor])
            if 0 < len(hits) <= max(8, 0.03 * n):
                for i in hits:
                    bonus[i] += 0.5
            used_anchor += 1
            stuck = 0
    return None, tok, found


def eval_question(args):
    q, models = args
    t0 = time.time()
    R = Repo(qdir(q))
    ans = resolve_answers(R, q)
    need = min(q['min_files'], len(ans))
    cand, X, extra, v, best, seed = features(R, q['question'])
    n = R.g.n
    full = {}
    full['bm25'] = v.copy()
    hy = np.zeros(n); hy[cand] = hybrid_score(X); full['hybrid'] = np.where(np.isin(np.arange(n), cand), hy, -1)
    for k, m in models.items():
        s = np.full(n, -1e9); s[cand] = m.score(X) if 'swap' not in k else m.score(
            np.concatenate([X[:, :8], extra[k.split(':')[1]][:, None], X[:, 9:]], 1))
        full[k] = s
    qt = set(terms(q['question']))
    with_q = set()
    for t in qt:
        with_q |= R.files_with(t)
    hop = [a for a in ans if R.fid[a] not in with_q]
    out = dict(id=q['id'], split=q['split'], repo=repo_key(q['repo']), n=n, ans=ans, need=need, hop=hop, rank={}, sim={})
    for k, s in full.items():
        order = [R.g.rels[i] for i in np.argsort(-s)[:10]]
        out['rank'][k] = order
    out['conf'] = conf_feats(X, {k: full[k][cand] for k in ('bm25', 'lr') if k in full}, len(qt), R, cand)
    lr = full.get('lr', full['hybrid'])
    lrb = np.where(lr < -1e8, lr.min(), lr) if False else np.where(lr < -1e8, np.nan, lr)
    lrb = np.nan_to_num(lrb, nan=np.nanmin(lrb) if np.isfinite(lrb).any() else 0)
    vb = v.copy()
    pol = {
        'P0 BM25 paginado': (vb, {}),
        'P1 aprendido paginado': (lrb, {}),
        'P2 investigador (tudo)': (lrb, dict(expand=1, anchors=1, stuck=1)),
        'P2 sem expansao': (lrb, dict(anchors=1, stuck=1)),
        'P2 sem ancoras': (lrb, dict(expand=1, stuck=1)),
        'P2 sem travado': (lrb, dict(expand=1, anchors=1)),
        'P2 base BM25': (vb, dict(expand=1, anchors=1, stuck=1)),
    }
    for t in ETYPES:
        pol[f'P2 expansao sem {t}'] = (lrb, dict(expand=1, anchors=1, stuck=1, types=tuple(x for x in ETYPES if x != t)))
    for k, (b, fl) in pol.items():
        st, tok, found = simulate(R, q['question'], set(ans), need, b, fl)
        out['sim'][k] = (st, tok, sorted(found))
    out['secs'] = time.time() - t0
    out['index_s'] = R.build_s
    return out


def cmd_questions(procs=4):
    qs = load_questions()
    models = {}
    for name in SOURCES:
        models[name] = {'lr': train_model(name), 'lr_nograph': train_model(name, [f for f in NFEAT if f not in NOPPR])}
    path = os.path.join(CACHE, 'questions.pkl')
    res = pickle.load(open(path, 'rb')) if os.path.exists(path) and '--fresh' not in sys.argv else None
    if res is None:
        from multiprocessing import Pool
        jobs = [(q, models[repo_key(q['repo'])]) for q in qs]
        with Pool(procs) as p:
            res = p.map(eval_question, jobs, chunksize=1)
        pickle.dump(res, open(path, 'wb'))
    report_questions(res)


def hop_map():
    """recorte 'salto': arquivos-gabarito sem NENHUM termo informativo da pergunta (termos informativos: fora da lista de
    palavras de pedido e presentes em no maximo 15% dos arquivos). Calculado uma vez e guardado."""
    path = os.path.join(CACHE, 'hop.json')
    if os.path.exists(path):
        return json.load(open(path))
    out = {}
    for q in load_questions():
        d = qdir(q)
        files = bench.load_corpus(d)
        low = [bench.strip_acc(t).lower() for t in files.values()]
        qt = {t for t in terms(q['question']) if t not in REQUEST}
        inf = {t for t in qt if sum(t in x for x in low) <= 0.15 * len(low)}
        ans = []
        for a in q['answer_files']:
            c = [r for r in files if r == a or r.endswith('/' + a)]
            if c:
                ans.append(c[0])
        out[q['id']] = dict(hop=[a for a in ans if not (set(terms(files[a])) | set(terms(a))) & inf], informative=sorted(inf))
    json.dump(out, open(path, 'w'), indent=1)
    return out


def recall_at(r, k, key):
    top = r['rank'][key][:k]
    return sum(a in top for a in r['ans']) / max(1, len(r['ans']))


def report_questions(res):
    keys = ['bm25', 'hybrid', 'lr_nograph', 'lr']
    names = {'bm25': 'BM25 (Find atual)', 'hybrid': 'hibrido bm25+0.5ppr', 'lr_nograph': 'aprendido sem grafo', 'lr': 'aprendido (LORO)'}
    for label, sub in (('todas', res), ('treino', [r for r in res if r['split'] == 'train']),
                       ('validacao', [r for r in res if r['split'] == 'val']),
                       ('todas sem grey-gkatana', [r for r in res if r['id'] != 'grey-gkatana'])):
        print(f'\nRecall dos answer_files ({label}, {len(sub)} perguntas)')
        print(f'  {"metodo":22s}  R@3    R@5    R@10   min_files@10')
        for k in keys:
            r3, r5, r10 = (np.mean([recall_at(r, kk, k) for r in sub]) for kk in (3, 5, 10))
            mf = np.mean([sum(a in r['rank'][k][:10] for a in r['ans']) >= r['need'] for r in sub])
            print(f'  {names[k]:22s} {r3:6.1%} {r5:6.1%} {r10:6.1%} {mf:6.1%}')
    hm = hop_map()
    for r in res:
        r['hop'] = hm[r['id']]['hop']
    hops = [(r, a) for r in res for a in r['hop']]
    tot = sum(len(r['ans']) for r in res)
    print(f'\nRecorte salto: {len(hops)} de {tot} arquivos-gabarito ({len(hops) / tot:.0%}) nao contem nenhuma palavra da pergunta '
          f'({len({r["id"] for r, a in hops})} perguntas)')
    for k in keys:
        print(f'  {names[k]:22s} R@5 {np.mean([a in r["rank"][k][:5] for r, a in hops]):6.1%}  '
              f'R@10 {np.mean([a in r["rank"][k][:10] for r, a in hops]):6.1%}')
    print('  na politica (arquivo-salto visitado dentro de 10 passos / 8 mil tokens):')
    for k in res[0]['sim']:
        print(f'    {k:28s} {np.mean([a in r["sim"][k][2] for r, a in hops]):6.1%}')
    for label, sub in (('todas', res), ('treino', [r for r in res if r['split'] == 'train']),
                       ('validacao', [r for r in res if r['split'] == 'val']),
                       ('todas sem grey-gkatana', [r for r in res if r['id'] != 'grey-gkatana'])):
        print(f'\nPolitica simulada ({label}, {len(sub)}): min_files alcancado em <=10 passos e <=8000 tokens')
        print(f'  {"politica":28s} sucesso  passos(med. sucesso)  tokens(med.)  passos(todos, falha=11)')
        for k in res[0]['sim']:
            ok = [r['sim'][k] for r in sub if r['sim'][k][0]]
            allst = [r['sim'][k][0] or 11 for r in sub]
            print(f'  {k:28s} {len(ok) / len(sub):6.1%}  {np.mean([s for s, _, _ in ok]) if ok else 0:8.2f}'
                  f'            {np.mean([t for _, t, _ in ok]) if ok else 0:8.0f}      {np.mean(allst):5.2f}')
    print('\nPor repositorio (sucesso da politica, todas): ', end='')
    for rp in sorted({r['repo'] for r in res}):
        sub = [r for r in res if r['repo'] == rp]
        print(f'{rp} n={len(sub)} P0 {np.mean([bool(r["sim"]["P0 BM25 paginado"][0]) for r in sub]):.0%} '
              f'P2 {np.mean([bool(r["sim"]["P2 investigador (tudo)"][0]) for r in sub]):.0%}; ', end='')
    print(f'\nTempo medio por pergunta (indice Python + todas as simulacoes): {np.mean([r["secs"] for r in res]):.1f} s')


# ------------------------------------------------------------------ confianca e abstencao
CONF = ['margin', 'top1_norm', 'qterm_cov', 'agree_ppr', 'agree_lr', 'nq']


def conf_feats(X, scores, nq, R=None, cand=None):
    """sinais de confianca de uma consulta (sobre o ranking BM25): margem relativa 1o-2o, pontuacao por termo da pergunta,
    cobertura (fracao dos termos raros da pergunta presentes no 1o), concordancia com o grafo e com o aprendido"""
    b = scores['bm25']
    o = np.argsort(-b)
    s1, s2 = b[o[0]], b[o[1]] if len(o) > 1 else 0
    pa = X[:, NFEAT.index('ppr_all')]
    d = dict(margin=(s1 - s2) / s1 if s1 > 0 else 0, top1_norm=s1 / max(1, nq), nq=nq,
             agree_ppr=float(np.argmax(pa) == o[0]),
             agree_lr=float(np.argmax(scores['lr']) == o[0]) if 'lr' in scores else 0.0,
             qterm_cov=0.0, top=list(o[:5]))
    return d


def conf_score(d, w=None):
    w = w or dict(margin=2.0, top1_norm=0.15, agree_ppr=0.5, agree_lr=0.5)
    return sum(w[k] * d[k] for k in w)


def risk_coverage(items, label):
    """items: (confianca, acerto top1, acerto top5). Imprime a precisao por faixa de cobertura (mais confiantes primeiro)"""
    items = sorted(items, key=lambda x: -x[0])
    n = len(items)
    out = []
    for cov in (0.1, 0.25, 0.5, 0.75, 1.0):
        k = max(1, int(round(cov * n)))
        sub = items[:k]
        out.append(f'{cov:4.0%}: top1 {np.mean([x[1] for x in sub]):4.0%} top5 {np.mean([x[2] for x in sub]):4.0%}')
    print(f'  {label:34s} ' + ' | '.join(out))


def threshold_for(items, prec=0.9, key=1):
    """o menor limiar de confianca cujo conjunto acima dele tem precisao >= prec (top1 se key=1, top5 se key=2)"""
    items = sorted(items, key=lambda x: -x[0])
    best = None
    for k in range(5, len(items) + 1):
        if np.mean([x[key] for x in items[:k]]) >= prec:
            best = items[k - 1][0]
    return best


def cmd_confidence():
    print('Risco-cobertura (BM25; confianca = 2*margem + 0.15*pontuacao/termo + 0.5*concorda_grafo + 0.5*concorda_aprendido)')
    allc = {}
    for name in SOURCES:
        rows = build_train(name)['rows']
        m = train_model(name)
        it = []
        for r in rows:
            b = r['X'][:, 0] + 1e-9 * r['X'][:, 1]
            d = conf_feats(r['X'], {'bm25': b, 'lr': m.score(r['X'])}, len(set(terms(r['q']))))
            top = [r['cand'][i] for i in d['top']]
            it.append((conf_score(d), top[0] in r['truth'], any(t in r['truth'] for t in top[:5]), d['margin']))
        allc[name] = it
        risk_coverage(it, f'commits {name} ({len(it)})')
        risk_coverage([(x[3], x[1], x[2]) for x in it], f'  so a margem')
    # limiar calibrado leave-one-repo-out: limiar dos outros dois, aplicado no terceiro
    print('Limiares LORO (precisao alvo no top1: 90% para semente nao pedida, 75% para resposta a uma chamada):')
    for name in SOURCES:
        others = [x for k, v in allc.items() if k != name for x in v]
        for tgt, lab in ((0.9, 'semente'), (0.75, 'resposta')):
            th = threshold_for(others, tgt)
            mine = [x for x in allc[name] if th is not None and x[0] >= th]
            print(f'  {name:8s} {lab:9s} limiar {th if th is None else round(th, 2)}: cobre {len(mine) / len(allc[name]):4.0%}, '
                  f'top1 real {np.mean([x[1] for x in mine]) if mine else float("nan"):4.0%}, top5 {np.mean([x[2] for x in mine]) if mine else float("nan"):4.0%}')
    path = os.path.join(CACHE, 'questions.pkl')
    if os.path.exists(path):
        res = [r for r in pickle.load(open(path, 'rb')) if r['id'] != 'grey-gkatana']
        it = []
        for r in res:
            d = r['conf']
            top = r['rank']['bm25']
            it.append((conf_score(d), top[0] in r['ans'], any(t in r['ans'] for t in top[:5])))
        risk_coverage(it, f'76 perguntas sem gkatana ({len(it)})')
        allth = [x for v in allc.values() for x in v]
        for tgt in (0.9, 0.75):
            th = threshold_for(allth, tgt)
            mine = [x for x in it if th is not None and x[0] >= th]
            print(f'  perguntas, limiar dos commits p/ {tgt:.0%}: {th if th is None else round(th, 2)} cobre {len(mine)}/{len(it)}, '
                  f'top1 {np.mean([x[1] for x in mine]) if mine else float("nan"):.0%}, top5 {np.mean([x[2] for x in mine]) if mine else float("nan"):.0%}')


# ------------------------------------------------------------------ roteador: escolhas de ferramenta x acerto
GREP_NL = re.compile(r'^[A-Za-z\u00c0-\u00ff]+( [A-Za-z\u00c0-\u00ff]+){2,}$')   # frase de 3+ palavras, sem simbolo de regex


def classify_call(name, args):
    """rotulos de chamada ruim (as regras de interceptacao do roteador)"""
    tags = []
    if name == 'Grep':
        p = str(args.get('pattern', ''))
        if GREP_NL.match(p.strip()):
            tags.append('grep_phrase')
        if not args.get('path') or os.path.isdir(str(args.get('path'))) and not args.get('glob'):
            tags.append('grep_wide')
    if name == 'Glob' and re.match(r'^\*\*/\*[^/]*\*', str(args.get('pattern', ''))):
        tags.append('glob_word')
    if name == 'Read' and 'limit' not in args and 'offset' not in args:
        tags.append('read_whole')
    return tags


def cmd_toolstats(dirs):
    """le pastas do eval.sh (summary.tsv + <id>.log) e conta, por veredito, as ferramentas e as chamadas ruins"""
    rows = []
    for d in dirs:
        sm = os.path.join(d, 'summary.tsv')
        if not os.path.exists(sm):
            continue
        for ln in open(sm, encoding='utf-8', errors='replace').read().splitlines()[1:]:
            f = ln.split('\t')
            if len(f) < 6:
                continue
            cnt = defaultdict(int)
            for cl in open(os.path.join(d, f[0] + '.log'), encoding='utf-8', errors='replace') if os.path.exists(os.path.join(d, f[0] + '.log')) else []:
                c = cl.split('\t')
                if len(c) < 3:
                    continue
                cnt[c[1]] += 1
                m = re.search(r'"arguments": (\{.*?\})(?:\t|$)', '\t'.join(c[2:3]))
                try:
                    args = json.loads(m.group(1)) if m else {}
                except ValueError:
                    args = {}
                for t in classify_call(c[1], args):
                    cnt[t] += 1
            rows.append((f[0], f[5], cnt))
    if not rows:
        print('nenhuma execucao terminada encontrada (summary.tsv com linhas)')
        return
    keys = sorted({k for _, _, c in rows for k in c})
    print(f'{"veredito":12s} n   ' + ' '.join(f'{k[:11]:>11s}' for k in keys))
    for v in sorted({v for _, v, _ in rows}):
        sub = [c for _, vv, c in rows if vv == v]
        print(f'{v:12s} {len(sub):3d} ' + ' '.join(f'{np.mean([c.get(k, 0) for c in sub]):11.1f}' for k in keys))


def cmd_case(qid):
    q = [x for x in load_questions() if x['id'] == qid][0]
    m = train_model(repo_key(q['repo']))
    R = Repo(qdir(q))
    ans = resolve_answers(R, q)
    cand, X, extra, v, best, seed = features(R, q['question'])
    s = np.full(R.g.n, np.nan); s[cand] = m.score(X)
    s = np.nan_to_num(s, nan=np.nanmin(s))
    anc, drop = anchors(R, q['question'])
    print('pergunta:', q['question'])
    print('gabarito:', ans)
    print('ancoras (O QUE / COMO):', [(w, k, df) for t, w, k, df in anc])
    print('descartados (ausentes/genericos):', [w for t, w, k, df in drop])
    print('BM25 top 10:', [R.g.rels[i] for i in np.argsort(-v)[:10]])
    print('aprendido top 10:', [R.g.rels[i] for i in np.argsort(-s)[:10]])
    for label, base, fl in (('P0 BM25 paginado', v, {}), ('P2 investigador', s, dict(expand=1, anchors=1, stuck=1))):
        tr = []
        st, tok, found = simulate(R, q['question'], set(ans), min(q['min_files'], len(ans)), base, fl, trace=tr)
        print(f'\n{label}: passos {st}, tokens {tok:.0f}, achados {found}')
        for step, rel, hit, src, why in tr:
            print(f'  passo {step}: {"*" if hit else " "} {rel:40s} [{src}] {why}')
    print('\nligacoes entre arquivos do caminho:')
    chain = list(dict.fromkeys([rel for _, rel, _, _, _ in tr] + ans))
    for a in chain:
        for b in chain:
            if a < b:
                i, j = R.fid[a], R.fid[b]
                ty = R.g.linked(i, j)
                if ty:
                    print(f'  {a} -- {b}: {ty} ({R.g.why(i, j)})')


def cmd_timing(path):
    t0 = time.time()
    R = Repo(path)
    print(f'{path}: {R.g.n} arquivos, {sum(len(x) for x in R.files.values()) / 1e6:.1f} MB; indice BM25 {R.ix.build_s:.1f} s, '
          f'grafo {R.g.build_s:.1f} s (Python); arestas {R.g.edges}')
    qs = ['why does the session list not show the new terminal', 'erro ao salvar arquivo grande', 'fix crash when plugin lane is empty']
    for q in qs:
        t1 = time.time(); v, _ = R.bm25_files(q); t2 = time.time(); seed = seed_vec(v); R.g.ppr(seed); t3 = time.time()
        R.g.ppr(seed); t4 = time.time()
        print(f'  "{q[:40]}": BM25 {1000 * (t2 - t1):.0f} ms, PPR (1a, monta matriz) {1000 * (t3 - t2):.0f} ms, PPR {1000 * (t4 - t3):.1f} ms')
    mem = sum(R.g.arr[t][0].size for t in ETYPES) * 12
    print(f'  grafo em C (int32 origem+destino, float32 peso): {mem / 1e6:.1f} MB')


if __name__ == '__main__':
    os.makedirs(CACHE, exist_ok=True)
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    if c == 'train':
        for nm in SOURCES:
            build_train(nm)
    elif c == 'commits':
        cmd_commits()
    elif c == 'questions':
        cmd_questions()
    elif c == 'hop':
        hm = hop_map()
        n = sum(len(v['hop']) for v in hm.values())
        print(f'{n} arquivos-salto em {sum(1 for v in hm.values() if v["hop"])} perguntas')
        for k, v in hm.items():
            if v['hop']:
                print(f'  {k}: {v["hop"]}  (termos informativos: {" ".join(v["informative"][:12])})')
    elif c == 'confidence':
        cmd_confidence()
    elif c == 'toolstats':
        cmd_toolstats(sys.argv[2:] or [os.path.join('/mnt/dados/investigate-eval', x) for x in os.listdir('/mnt/dados/investigate-eval')])
    elif c == 'case':
        cmd_case(sys.argv[2])
    elif c == 'timing':
        cmd_timing(sys.argv[2])
    else:
        print(__doc__)
