#!/usr/bin/env python3
"""Banca de teste da busca por intencao (futuro Find e docsel).

Perguntas com resposta conhecida vem do historico do git: a mensagem de um commit e o pedido; os arquivos que o commit
alterou (e que ainda existem no HEAD) sao o gabarito. O indice e feito sobre o HEAD. Mede, por algoritmo: acerto de arquivo em
1/5/10, MRR, caracteres devolvidos, tempo de indexar e de consultar. Uso humano e de pesquisa: o algoritmo que vencer vai para C.

  python3 tools/findbench/bench.py REPO [--n 120] [--algos a,b] [--seed 1] [--show 5]
"""
import argparse, math, os, random, re, subprocess, sys, time, unicodedata
from collections import defaultdict

TEXT_EXT = {'.c', '.h', '.cpp', '.cc', '.hpp', '.py', '.js', '.mjs', '.ts', '.kt', '.java', '.go', '.rs', '.sh', '.md', '.qml',
            '.glsl', '.comp', '.ps1', '.xml', '.toml', '.yml', '.yaml', '.cfg', '.txt', '.cmake', '.gradle', '.swift', '.m'}
STOP = set('a o as os um uma de do da dos das em no na nos nas para por com sem e ou que se ao aos como mais ja so the of to in on for and or with is are be from by it its this that an as at not into per via'.split())


def strip_acc(s):
    return ''.join(c for c in unicodedata.normalize('NFD', s) if unicodedata.category(c) != 'Mn')


_split1 = re.compile(r'([a-z0-9])([A-Z])')
_split2 = re.compile(r'([A-Z]+)([A-Z][a-z])')
_word = re.compile(r'[A-Za-z_][A-Za-z0-9_]*|\d+')


STEM = False
UNIQ = False


def stem(t):
    if len(t) < 5 or t.isdigit():
        return t
    if t.endswith('ies'):
        return t[:-3] + 'y'
    if t.endswith('coes'):
        return t[:-4] + 'cao'
    if t.endswith('ing') and len(t) > 6:
        return t[:-3]
    if t.endswith('ed') and len(t) > 5:
        return t[:-2]
    if t.endswith('s') and not t.endswith('ss') and not t.endswith('us'):
        return t[:-1]
    return t


def terms(text, ident=True):
    """tokens minusculos sem acento; cada identificador vira ele inteiro e suas partes (camelCase, snake_case)"""
    out = []
    for w in _word.findall(strip_acc(text)):
        parts = [p for p in re.split(r'_+', _split2.sub(r'\1 \2', _split1.sub(r'\1 \2', w)).replace(' ', '_')) if p]
        low = [p.lower() for p in parts]
        if ident and len(low) > 1:
            out.append('_'.join(low))
        out.extend(low)
    out = [t for t in out if len(t) >= 2 and t not in STOP]
    return [stem(t) for t in out] if STEM else out


def qweights(q):
    """peso por termo da pergunta: identificador (snake_case, camelCase, com ponto ou barra) vale mais, e o que aparece junto"""
    w = {}
    for raw in re.findall(r'[A-Za-z_][A-Za-z0-9_./\-]*', strip_acc(q)):
        idlike = '_' in raw or '.' in raw.strip('.') or '/' in raw or bool(re.search(r'[a-z][A-Z]', raw))
        for t in terms(raw):
            w[t] = max(w.get(t, 1.0), 1.8 if idlike else 1.0)
    return w


def git(repo, *a):
    return subprocess.run(['git', '-C', repo] + list(a), capture_output=True, text=True, errors='replace').stdout


def load_corpus(repo, maxbytes=300_000):
    files = {}
    for rel in git(repo, 'ls-files').splitlines():
        if os.path.splitext(rel)[1].lower() not in TEXT_EXT:
            continue
        p = os.path.join(repo, rel)
        try:
            if os.path.getsize(p) > maxbytes or os.path.isdir(p):
                continue
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                files[rel] = f.read()
        except OSError:
            pass
    return files


def build_queries(repo, files, n, seed):
    rng = random.Random(seed)
    out = []
    log = git(repo, 'log', '--no-merges', '--name-only', '--pretty=format:@@%H%x09%s', '-n', '3000')
    cur = None
    for line in log.splitlines():
        if line.startswith('@@'):
            if cur:
                out.append(cur)
            h, _, s = line[2:].partition('\t')
            cur = (h, s, [])
        elif line.strip() and cur:
            cur[2].append(line.strip())
    if cur:
        out.append(cur)
    qs, seen = [], set()
    for h, s, fl in out:
        if UNIQ and s in seen:      # mensagens repetidas (ex.: "Regenerate PROGRESS.md") pesam demais no hit@1
            continue
        seen.add(s)
        truth = [f for f in fl if f in files and not f.startswith(('docs/handoff', 'docs/comparisons'))]
        s2 = re.sub(r'^(feat|fix|merge|docs|chore|refactor|test)[:(\w)]*:\s*', '', s, flags=re.I).strip()
        if len(s2) < 25 or not (1 <= len(truth) <= 4) or len(truth) < len(fl) * 0.5:
            continue
        qs.append((s2, truth))
    rng.shuffle(qs)
    return qs[:n]


def chunks_of(rel, text, size=40, stride=25):
    lines = text.splitlines()
    if not lines:
        return []
    if len(lines) <= size:
        return [(1, len(lines))]
    res, i = [], 0
    while i < len(lines):
        res.append((i + 1, min(i + size, len(lines))))
        if i + size >= len(lines):
            break
        i += stride
    return res


_symre = re.compile(r'^\s*(?:static\s+|export\s+|async\s+|pub\s+|public\s+|private\s+|fun\s+|func\s+|def\s+|class\s+|struct\s+|enum\s+|typedef\s+|#define\s+|interface\s+|object\s+|void\s+|int\s+|const\s+|[A-Za-z_][\w:<>\*&\s]*\s+)([A-Za-z_][\w:]*)\s*[\(\{=:]|^#+\s+(.*)')


class Index:
    """indice invertido por campo (caminho, simbolos, corpo) sobre trechos de arquivo"""

    def __init__(self, files, size=40, stride=25):
        t0 = time.time()
        self.files = files
        self.chunks = []      # (rel, a, b)
        self.len = {'p': [], 's': [], 'b': []}
        self.post = {'p': defaultdict(list), 's': defaultdict(list), 'b': defaultdict(list)}
        for rel, text in files.items():
            lines = text.splitlines()
            ptoks = terms(rel)
            for a, b in chunks_of(rel, text, size, stride):
                cid = len(self.chunks)
                self.chunks.append((rel, a, b))
                body = '\n'.join(lines[a - 1:b])
                syms = []
                for ln in lines[a - 1:b]:
                    m = _symre.match(ln)
                    if m:
                        syms.append(m.group(1) or m.group(2) or '')
                for f, toks in (('p', ptoks), ('s', terms(' '.join(syms))), ('b', terms(body))):
                    self.len[f].append(len(toks))
                    tf = defaultdict(int)
                    for t in toks:
                        tf[t] += 1
                    for t, c in tf.items():
                        self.post[f][t].append((cid, c))
        self.N = len(self.chunks)
        self.avg = {f: (sum(v) / max(1, len(v))) for f, v in self.len.items()}
        self.build_s = time.time() - t0

    def bm25(self, f, qterms, k1=1.2, b=0.75, qw=None):
        sc = defaultdict(float)
        for t in set(qterms):
            wt = qw.get(t, 1.0) if qw else 1.0
            pl = self.post[f].get(t)
            if not pl:
                continue
            idf = math.log(1 + (self.N - len(pl) + 0.5) / (len(pl) + 0.5))
            for cid, tf in pl:
                dl = self.len[f][cid]
                sc[cid] += wt * idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * dl / (self.avg[f] or 1)))
        return sc


AGG = 0.0


def rank_files(chunk_scores, index, k=10):
    best, second = {}, {}
    for cid, s in chunk_scores.items():
        rel = index.chunks[cid][0]
        if rel not in best or s > best[rel][0]:
            if rel in best:
                second[rel] = max(second.get(rel, 0), best[rel][0])
            best[rel] = (s, cid)
        else:
            second[rel] = max(second.get(rel, 0), s)
    fin = {rel: (s + AGG * second.get(rel, 0), cid) for rel, (s, cid) in best.items()}
    top = sorted(fin.items(), key=lambda kv: -kv[1][0])[:k]
    return [(rel, cid, s) for rel, (s, cid) in top]


# ------------------------------------------------------------------ algoritmos
def algo_body(ix, q):
    return ix.bm25('b', terms(q))


def algo_fields(ix, q, wp=3.0, ws=2.0, wb=1.0):
    qt = terms(q)
    sc = defaultdict(float)
    for f, w in (('p', wp), ('s', ws), ('b', wb)):
        for cid, s in ix.bm25(f, qt).items():
            sc[cid] += w * s
    return sc


def algo_fields_prox(ix, q):
    """campos + bonus por termos distintos da pergunta presentes no mesmo trecho (cobertura)"""
    qt = list(dict.fromkeys(terms(q)))
    sc = algo_fields(ix, q)
    cover = defaultdict(int)
    for f in ('s', 'b'):
        seen = defaultdict(set)
        for t in qt:
            for cid, _ in ix.post[f].get(t, ()):
                seen[cid].add(t)
        for cid, st in seen.items():
            cover[cid] = max(cover[cid], len(st))
    out = {}
    for cid, s in sc.items():
        out[cid] = s * (1 + 0.15 * cover.get(cid, 0))
    return out


def algo_prf(ix, q, ndocs=5, nterms=8):
    """realimentacao de pseudo-relevancia: os melhores trechos da primeira rodada trazem termos novos para a segunda"""
    sc = algo_fields_prox(ix, q)
    top = sorted(sc.items(), key=lambda kv: -kv[1])[:ndocs]
    qt = set(terms(q))
    cand = defaultdict(float)
    for cid, s in top:
        rel, a, b = ix.chunks[cid]
        tf = defaultdict(int)
        for t in terms('\n'.join(ix.files[rel].splitlines()[a - 1:b])):
            tf[t] += 1
        for t, c in tf.items():
            if t in qt:
                continue
            df = len(ix.post['b'].get(t, ()))
            if df < 2 or df > ix.N * 0.2:
                continue
            cand[t] += s * math.log(1 + ix.N / df) * (1 + math.log(c))
    extra = [t for t, _ in sorted(cand.items(), key=lambda kv: -kv[1])[:nterms]]
    sc2 = algo_fields_prox(ix, q + ' ' + ' '.join(extra))
    out = defaultdict(float)
    for cid, s in sc.items():
        out[cid] += s
    mx = max(sc2.values()) if sc2 else 1
    mx1 = max(sc.values()) if sc else 1
    for cid, s in sc2.items():
        out[cid] += 0.5 * s * mx1 / mx
    return out


def algo_file_bm25(ix, q):
    """linha de base: arquivo inteiro como um documento (sem trechos), so o corpo"""
    return ix.bm25('b', terms(q))   # os trechos ja somam; o ranking por arquivo usa o melhor trecho


def algo_param(wp=3.0, ws=2.0, wb=1.0, cover=0.0, idw=False):
    def run(ix, q):
        qt = list(dict.fromkeys(terms(q)))
        qw = qweights(q) if idw else None
        sc = defaultdict(float)
        for f, w in (('p', wp), ('s', ws), ('b', wb)):
            for cid, s in ix.bm25(f, qt, qw=qw).items():
                sc[cid] += w * s
        if cover:
            seen = defaultdict(set)
            for f in ('s', 'b'):
                for t in qt:
                    for cid, _ in ix.post[f].get(t, ()):
                        seen[cid].add(t)
            for cid in list(sc):
                sc[cid] *= 1 + cover * len(seen.get(cid, ()))
        return sc
    return run


ALGOS = {'body': algo_body, 'fields': algo_fields, 'cover.15': algo_fields_prox,
         'f+cover.05': algo_param(cover=0.05), 'f+id': algo_param(idw=True), 'f+id+cov.05': algo_param(cover=0.05, idw=True),
         'w(4,2,1)+id': algo_param(4, 2, 1, 0, True), 'w(2,2,1)+id': algo_param(2, 2, 1, 0, True), 'w(3,3,1)+id': algo_param(3, 3, 1, 0, True)}


def evaluate(repo, nq, algos, seed, show):
    files = load_corpus(repo)
    qs = build_queries(repo, files, nq, seed)
    print(f'{repo}: {len(files)} arquivos, {len(qs)} perguntas')
    ix = Index(files)
    print(f'  indice: {ix.N} trechos, {ix.build_s:.1f} s')
    for name in algos:
        fn = ALGOS[name]
        h1 = h5 = h10 = 0
        mrr = 0.0
        chars = 0
        tq = 0.0
        shown = 0
        for q, truth in qs:
            t0 = time.time()
            res = rank_files(fn(ix, q), ix, 10)
            tq += time.time() - t0
            rels = [r for r, _, _ in res]
            ranks = [rels.index(t) + 1 for t in truth if t in rels]
            best = min(ranks) if ranks else 0
            h1 += best == 1
            h5 += 0 < best <= 5
            h10 += 0 < best <= 10
            mrr += 1 / best if best else 0
            for r, cid, _ in res[:5]:
                rel, a, b = ix.chunks[cid]
                chars += sum(len(l) + 1 for l in ix.files[rel].splitlines()[a - 1:b])
            if shown < show and not (0 < best <= 5):
                shown += 1
                print(f'    [{name}] erro: "{q[:90]}" -> certo {truth[:2]} ; achou {rels[:3]}')
        n = max(1, len(qs))
        print(f'  {name:14s} hit@1 {h1 / n:5.1%}  hit@5 {h5 / n:5.1%}  hit@10 {h10 / n:5.1%}  MRR {mrr / n:.3f}  '
              f'chars(top5) {chars / n:7.0f}  consulta {tq / n * 1000:6.1f} ms')


def evaluate_katana(repo, nq, seed, show, binary, label='katana'):
    """o Find do katana (C): uma chamada `katana tool Find` por pergunta, na pasta do repositorio"""
    import json
    files = load_corpus(repo)
    qs = build_queries(repo, files, nq, seed)
    import tempfile
    env = dict(os.environ, KATANA_FIND_CHARS='100000', KATANA_FIND_TIME='1', KATANA_LOG=os.devnull,
               KATANA_CACHE_DIR=os.environ.get('FINDBENCH_CACHE', os.path.join(tempfile.gettempdir(), 'katana-findbench')))
    t0 = time.time()
    r = subprocess.run([binary, 'tool', 'Find', json.dumps({'query': 'index warm up', 'limit': 1})], cwd=repo, env=env,
                       capture_output=True, text=True, errors='replace')
    print(f'{repo}: {len(qs)} perguntas; primeira chamada (indice) {time.time() - t0:.2f} s; {r.stderr.strip().splitlines()[:1]}')
    h1 = h5 = h10 = 0
    mrr = 0.0
    tq = tw = 0.0
    shown = 0
    rx = re.compile(r'^\d+\. (.+?):(\d+)-(\d+)', re.M)
    for q, truth in qs:
        t0 = time.time()
        r = subprocess.run([binary, 'tool', 'Find', json.dumps({'query': q, 'limit': 10})], cwd=repo, env=env,
                           capture_output=True, text=True, errors='replace')
        tw += time.time() - t0
        m = re.search(r'query ([\d.]+) ms', r.stderr)
        tq += float(m.group(1)) if m else 0
        rels = [x.group(1) for x in rx.finditer(r.stdout)]
        ranks = [rels.index(t) + 1 for t in truth if t in rels]
        best = min(ranks) if ranks else 0
        h1 += best == 1
        h5 += 0 < best <= 5
        h10 += 0 < best <= 10
        mrr += 1 / best if best else 0
        if shown < show and not (0 < best <= 5):
            shown += 1
            print(f'    [{label}] erro: "{q[:90]}" -> certo {truth[:2]} ; achou {rels[:3]}')
    n = max(1, len(qs))
    print(f'  {label:14s} hit@1 {h1 / n:5.1%}  hit@5 {h5 / n:5.1%}  hit@10 {h10 / n:5.1%}  MRR {mrr / n:.3f}  '
          f'consulta {tq / n:6.1f} ms  chamada {tw / n * 1000:6.0f} ms')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('repo')
    ap.add_argument('--n', type=int, default=120)
    ap.add_argument('--algos', default=','.join(ALGOS))
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=0)
    ap.add_argument('--stem', action='store_true')
    ap.add_argument('--uniq', action='store_true', help='uma pergunta por mensagem de commit (sem repetidas)')
    ap.add_argument('--agg', type=float, default=0.0)
    ap.add_argument('--katana', help='binario do katana: mede o Find em C com as mesmas perguntas')
    ap.add_argument('--query', help='uma pergunta: mostra os arquivos e trechos que a busca devolveria')
    a = ap.parse_args()
    STEM = a.stem
    UNIQ = a.uniq
    AGG = a.agg
    if a.katana:
        evaluate_katana(a.repo, a.n, a.seed, a.show, os.path.abspath(a.katana))
    elif a.query:
        files = load_corpus(a.repo)
        ix = Index(files)
        for r, cid, sc in rank_files(algo_param(cover=0.05)(ix, a.query), ix, 8):
            rel, x, y = ix.chunks[cid]
            print(f'{sc:7.1f}  {rel}:{x}-{y}')
    else:
        evaluate(a.repo, a.n, a.algos.split(','), a.seed, a.show)
