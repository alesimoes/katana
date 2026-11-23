#!/usr/bin/env python3
"""FolderModel: um micro-modelo da pasta de trabalho, treinado ao indexar e so com o conteudo dela.

Vetores de termos por PPMI + SVD randomizada sobre a co-ocorrencia em janela; vetor de arquivo = media ponderada por idf;
variante 'joint' poe termos e ARQUIVOS num so espaco (co-ocorrencia termo-termo + termo-arquivo + arestas do grafo de
arquivos: simbolos, caminhos, identificadores raros, co-mudanca do git); variantes com a tabela de embeddings do Qwen
(token_embd, Q8_0, lida direto do GGUF). Perfil da pasta (cartao) e atualizacao por fold-in.

  python3 tools/findbench/folder.py questions     # recall nas 76 perguntas, por variante e mistura com o BM25
  python3 tools/findbench/folder.py commits       # bench de commits (mesmas perguntas do investigator.py)
  python3 tools/findbench/folder.py card REPO     # o cartao de perfil de uma pasta
  python3 tools/findbench/folder.py cardeval      # o cartao aponta a area certa das 76 perguntas?
  python3 tools/findbench/folder.py foldin        # erro e custo do fold-in contra o SVD completo
  python3 tools/findbench/folder.py neighbors REPO TERM   # vizinhos de um termo/arquivo no espaco da pasta
"""
import math, os, pickle, re, sys, time
from collections import Counter, defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench
import investigator as inv
from bench import terms

GGUF = os.environ.get('QWEN_GGUF', '/mnt/dados/source/katana/models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf')
GGUF_PY = '/mnt/dados/source/katana/ref/llama-src/gguf-py'
SEED = 7


# ------------------------------------------------------------------ algebra esparsa minima (CSR em numpy)
def coo_sum(r, c, v, n, m):
    key = r.astype(np.int64) * m + c
    u, inv_ = np.unique(key, return_inverse=True)
    vv = np.bincount(inv_, weights=v)
    return (u // m).astype(np.int64), (u % m).astype(np.int64), vv


def spmm(r, c, v, B, n):
    """(n x m esparsa, linhas ordenadas) @ B denso"""
    out = np.zeros((n, B.shape[1]), np.float32)
    if len(v) == 0:
        return out
    step = 1 << 15
    for s in range(0, len(v), step):
        rr, cc, vv = r[s:s + step], c[s:s + step], v[s:s + step]
        tmp = vv[:, None].astype(np.float32) * B[cc]
        starts = np.flatnonzero(np.r_[True, rr[1:] != rr[:-1]])
        out[rr[starts]] += np.add.reduceat(tmp, starts, axis=0)
    return out


def rsvd_sym(r, c, v, n, k, power=2):
    """autovetores dominantes de uma matriz simetrica esparsa (Halko et al.): Q = orth(M W), B = Q'MQ, eigh(B)"""
    rng = np.random.default_rng(SEED)
    Y = spmm(r, c, v, rng.standard_normal((n, k + 16)).astype(np.float32), n)
    for _ in range(power):
        Q, _ = np.linalg.qr(Y)
        Y = spmm(r, c, v, Q.astype(np.float32), n)
    Q, _ = np.linalg.qr(Y)
    B = Q.T @ spmm(r, c, v, Q.astype(np.float32), n)
    ev, W = np.linalg.eigh((B + B.T) / 2)
    o = np.argsort(-ev)[:k]
    ev = np.maximum(ev[o], 1e-9)
    return (Q @ W[:, o]).astype(np.float32), ev


def ppmi(r, c, v, cds=0.75, sym=True):
    tot = v.sum()
    rs = np.bincount(r, weights=v)
    cs = np.bincount(c, weights=v)
    cs_a = cs ** cds
    pc = cs_a / cs_a.sum()
    pmi = np.log((v / tot) / ((rs[r] / tot) * pc[c]))
    keep = pmi > 0
    return r[keep], c[keep], pmi[keep]


# ------------------------------------------------------------------ o modelo da pasta
class Folder:
    def __init__(self, files, dim=96, win=5, cds=0.75, vmax=30000, graph=None, joint=False, gw=1.0, keep_counts=False):
        t0 = time.time()
        self.rels = list(files)
        self.fid = {r: i for i, r in enumerate(self.rels)}
        F = len(self.rels)
        seqs = [terms(files[r]) for r in self.rels]
        self.tok_s = time.time() - t0
        df = Counter()
        for s in seqs:
            df.update(set(s))
        cand = [t for t, d in df.items() if d >= 2 and d <= 0.5 * F]
        cand.sort(key=lambda t: -df[t])
        vocab = cand[:vmax]
        self.vocab = vocab
        self.tid = {t: i for i, t in enumerate(vocab)}
        V = len(vocab)
        self.idf = np.array([math.log(1 + F / df[t]) for t in vocab], np.float32)
        ids = [np.array([self.tid[t] for t in s if t in self.tid], np.int64) for s in seqs]
        self.ids = ids
        R, C, W = [], [], []
        for a in ids:      # janela simetrica; peso 1/d (mais perto conta mais)
            for d in range(1, win + 1):
                if len(a) <= d:
                    break
                R += [a[:-d], a[d:]]; C += [a[d:], a[:-d]]; W += [np.full(len(a) - d, 1.0 / d)] * 2
        if R:
            r, c, w = coo_sum(np.concatenate(R), np.concatenate(C), np.concatenate(W), V, V)
        else:
            r = c = np.zeros(0, np.int64); w = np.zeros(0)
        if keep_counts:
            self.counts = (r, c, w)
        # termo-arquivo (tf log) para os arquivos e, no modo joint, como bloco da matriz
        fr, fc, fw = [], [], []
        for i, a in enumerate(ids):
            if len(a):
                u, k = np.unique(a, return_counts=True)
                fr.append(np.full(len(u), i)); fc.append(u); fw.append(1 + np.log(k))
        self.tf = (np.concatenate(fr), np.concatenate(fc), np.concatenate(fw)) if fr else None
        self.joint = joint
        n = V
        if joint:
            n = V + F
            tr, tc, tw = self.tf
            s = w.sum() / max(1, tw.sum())     # cada bloco com a mesma massa
            r = np.concatenate([r, tc, tr + V]); c = np.concatenate([c, tr + V, tc]); w = np.concatenate([w, tw * s, tw * s])
            if graph is not None:
                gs, gd, gwt, _ = graph.comb(inv.ETYPES)
                gmap = np.array([self.fid.get(x, -1) for x in graph.rels])
                ok = (gmap[gs] >= 0) & (gmap[gd] >= 0)
                gwt = gwt[ok] * (w.sum() / 3) / max(1e-9, gwt[ok].sum()) * gw
                r = np.concatenate([r, gmap[gs[ok]] + V]); c = np.concatenate([c, gmap[gd[ok]] + V]); w = np.concatenate([w, gwt])
            r, c, w = coo_sum(r, c, w, n, n)
        pr, pc_, pv = ppmi(r, c, w, cds)
        o = np.lexsort((pc_, pr))
        self.M = (pr[o], pc_[o], pv[o].astype(np.float32))
        U, ev = rsvd_sym(*self.M, n, dim)
        self.ev = ev
        E = U * np.sqrt(ev)[None, :]
        self.T = norm_rows(E[:V])
        self.Fv = norm_rows(E[V:]) if joint else self.file_vectors(self.T)
        self.dim = dim
        self.build_s = time.time() - t0
        self.nnz = len(pv)

    def file_vectors(self, T):
        r, c, w = self.tf
        return norm_rows(spmm(r, c, (w * self.idf[c]).astype(np.float32), T, len(self.rels)))

    def qvec(self, q, T=None):
        T = self.T if T is None else T
        ii = [self.tid[t] for t in set(terms(q)) if t in self.tid]
        if not ii:
            return None
        v = (T[ii] * self.idf[ii, None]).sum(0)
        nv = np.linalg.norm(v)
        return v / nv if nv else None

    def cos(self, q, Fv=None, T=None):
        v = self.qvec(q, T)
        Fv = self.Fv if Fv is None else Fv
        return np.zeros(len(self.rels)) if v is None else Fv @ v


def norm_rows(X):
    n = np.linalg.norm(X, axis=1, keepdims=True)
    return X / np.maximum(n, 1e-9)


# ------------------------------------------------------------------ Qwen: token_embd (Q8_0) e tokenizacao gulosa
_QW = None


def qwen():
    global _QW
    if _QW is None:
        sys.path.insert(0, GGUF_PY)
        import gguf
        rd = gguf.GGUFReader(GGUF)
        f = rd.fields['tokenizer.ggml.tokens']
        toks = [bytes(f.parts[i]).decode('utf-8', 'replace') for i in f.data]
        t = [x for x in rd.tensors if x.name == 'token_embd.weight'][0]
        _QW = (dict((s, i) for i, s in enumerate(toks)), t.data, t.tensor_type, gguf)
    return _QW


def qwen_ids(term):
    """termo -> pecas do vocabulario do Qwen (com e sem o prefixo de espaco), maior prefixo primeiro"""
    vocab = qwen()[0]
    if 'Ġ' + term in vocab:
        return [vocab['Ġ' + term]]
    out, s, first = [], term, True
    while s:
        for L in range(len(s), 0, -1):
            p = ('Ġ' if first else '') + s[:L]
            if p in vocab:
                out.append(vocab[p]); s = s[L:]; first = False
                break
        else:
            s = s[1:]; first = False
    return out


def qwen_term_vectors(vocab_terms, dim=96):
    vocab, data, qt, gguf = qwen()
    rows = {}
    per = [qwen_ids(t.replace('_', '')) for t in vocab_terms]
    need = sorted({i for p in per for i in p})
    deq = gguf.quants.dequantize(np.asarray(data[need]), qt).astype(np.float32)
    for k, i in enumerate(need):
        rows[i] = deq[k]
    X = np.stack([np.mean([rows[i] for i in p], 0) if p else np.zeros(deq.shape[1], np.float32) for p in per])
    X = X - X.mean(0)
    U, S, Vt = np.linalg.svd(X, full_matrices=False)      # projecao (PCA) para a dimensao da pasta
    return norm_rows(U[:, :dim] * S[:dim])


# ------------------------------------------------------------------ avaliacao nas perguntas
def mix(bm, cs, mu):
    b = bm / bm.max() if bm.max() > 0 else bm
    c = cs - cs.min()
    c = c / c.max() if c.max() > 0 else c
    return b + mu * c


CONFIGS = {   # nome -> kwargs do Folder
    'tok d64 w5': dict(dim=64, win=5), 'tok d128 w5': dict(dim=128, win=5), 'tok d128 w2': dict(dim=128, win=2),
    'tok d128 w10': dict(dim=128, win=10), 'tok d128 w5 cds1': dict(dim=128, win=5, cds=1.0),
    'joint d128 w5': dict(dim=128, win=5, joint=True),
}
MUS = (0.25, 0.5, 1.0)


def eval_q(q):
    t0 = time.time()
    R = inv.Repo(inv.qdir(q))
    ans = inv.resolve_answers(R, q)
    v, _ = R.bm25_files(q['question'])
    qt = set(terms(q['question']))
    withq = set()
    for t in qt:
        withq |= R.files_with(t)
    hop = [a for a in ans if R.fid[a] not in withq]
    rels = R.g.rels
    out = dict(id=q['id'], split=q['split'], repo=inv.repo_key(q['repo']), n=R.g.n, ans=ans, hop=hop, rank={}, t={})
    out['rank']['bm25'] = [rels[i] for i in np.argsort(-v)[:10]]
    files = {r: R.files[r] for r in rels}
    for name, kw in CONFIGS.items():
        t1 = time.time()
        fm = Folder(files, graph=R.g, **kw)
        out['t'][name] = (time.time() - t1, fm.nnz, len(fm.vocab))
        cs = fm.cos(q['question'])
        out['rank'][name + ' only'] = [rels[i] for i in np.argsort(-cs)[:10]]
        for mu in MUS:
            out['rank'][f'{name} mu{mu}'] = [rels[i] for i in np.argsort(-mix(v, cs, mu))[:10]]
        if name == 'tok d128 w5':
            try:
                QT = qwen_term_vectors(fm.vocab, 128)
                QF = fm.file_vectors(QT)
                cq = fm.cos(q['question'], QF, QT)
                out['rank']['qwen only'] = [rels[i] for i in np.argsort(-cq)[:10]]
                for mu in MUS:
                    out['rank'][f'qwen mu{mu}'] = [rels[i] for i in np.argsort(-mix(v, cq, mu))[:10]]
                CT = norm_rows(np.concatenate([fm.T, 0.7 * QT], 1))      # Qwen projetado + pasta
                CF = fm.file_vectors(CT)
                cc = fm.cos(q['question'], CF, CT)
                out['rank']['qwen+folder only'] = [rels[i] for i in np.argsort(-cc)[:10]]
                for mu in MUS:
                    out['rank'][f'qwen+folder mu{mu}'] = [rels[i] for i in np.argsort(-mix(v, cc, mu))[:10]]
            except Exception as e:     # sem o GGUF: segue sem as variantes do Qwen
                out['qwen_err'] = repr(e)
    out['secs'] = time.time() - t0
    return out


def cmd_questions(procs=3):
    path = os.path.join(inv.CACHE, 'folder-questions.pkl')
    if os.path.exists(path) and '--fresh' not in sys.argv:
        res = pickle.load(open(path, 'rb'))
    else:
        from multiprocessing import Pool
        with Pool(procs) as p:
            res = p.map(eval_q, inv.load_questions(), chunksize=1)
        pickle.dump(res, open(path, 'wb'))
    hm = inv.hop_map()
    for r in res:
        r['hop'] = hm[r['id']]['hop']
    keys = list(res[0]['rank'])
    def rec(sub, k, K):
        return np.mean([sum(a in r['rank'][k][:K] for a in r['ans']) / max(1, len(r['ans'])) for r in sub])
    def hop(sub, k):
        h = [(r, a) for r in sub for a in r['hop']]
        return np.mean([a in r['rank'][k][:10] for r, a in h]) if h else float('nan')
    tr = [r for r in res if r['split'] == 'train']
    va = [r for r in res if r['split'] == 'val']
    small = [r for r in res if r['n'] < 1500]
    big = [r for r in res if r['n'] >= 1500]
    nog = [r for r in res if r['id'] != 'grey-gkatana']
    print(f'R@10 dos answer_files (treino {len(tr)}, validacao {len(va)}; pequenas <1500 arquivos {len(small)}, grandes {len(big)})')
    print(f'{"variante":28s} trR@5  trR@10 vaR@5  vaR@10 salto@10 peq@10 gde@10 todas-sem-gk@10')
    for k in keys:
        print(f'{k:28s} {rec(tr, k, 5):6.1%} {rec(tr, k, 10):6.1%} {rec(va, k, 5):6.1%} {rec(va, k, 10):6.1%} '
              f'{hop(res, k):7.1%} {rec(small, k, 10):6.1%} {rec(big, k, 10):6.1%} {rec(nog, k, 10):6.1%}')
    best = max((k for k in keys if k != 'bm25'), key=lambda k: rec(tr, k, 10))
    print(f'melhor no treino: {best}; validacao R@10 {rec(va, best, 10):.1%} contra BM25 {rec(va, "bm25", 10):.1%}')
    print('tempo de construir (Python), por variante, media sobre as copias pequenas / grandes:')
    for name in CONFIGS:
        print(f'  {name:20s} {np.mean([r["t"][name][0] for r in small]):6.1f} s / {np.mean([r["t"][name][0] for r in big]):6.1f} s'
              f'  vocab {np.mean([r["t"][name][2] for r in big]):.0f}  nnz(PPMI) {np.mean([r["t"][name][1] for r in big]) / 1e6:.1f} M')
    if any('qwen_err' in r for r in res):
        print('erro no Qwen:', [r['qwen_err'] for r in res if 'qwen_err' in r][0])


# ------------------------------------------------------------------ bench de commits
def cmd_commits():
    print(f'{"repo":8s} {"variante":26s} hit@1  hit@5  hit@10  MRR')
    for name, path in inv.SOURCES.items():
        hist = inv.commit_log(path)
        k = min(400, int(len(hist) * 0.35))
        R = inv.Repo(path, cutoff=k, history=hist)
        qs = inv.commit_queries(hist, R.files, k)[:150]
        files = {r: R.files[r] for r in R.g.rels}
        fms = {'tok d128 w5': Folder(files, dim=128, win=5), 'joint d128 w5': Folder(files, dim=128, win=5, joint=True, graph=R.g)}
        rows = defaultdict(list)
        for h, q, truth in qs:
            v, _ = R.bm25_files(q)
            scs = {'bm25': v}
            for fn, fm in fms.items():
                cs = fm.cos(q)
                for mu in MUS:
                    scs[f'{fn} mu{mu}'] = mix(v, cs, mu)
            for kk, s in scs.items():
                order = [R.g.rels[i] for i in np.argsort(-s)[:10]]
                ranks = [order.index(t) + 1 for t in truth if t in order]
                rows[kk].append(min(ranks) if ranks else 0)
        for kk, b in rows.items():
            b = np.array(b)
            print(f'{name:8s} {kk:26s} {np.mean(b == 1):5.1%} {np.mean((b > 0) & (b <= 5)):6.1%} {np.mean(b > 0):6.1%}  '
                  f'{np.mean(np.where(b > 0, 1 / np.maximum(b, 1), 0)):.3f}')


# ------------------------------------------------------------------ perfil da pasta (cartao)
LANG = {'.c': 'C', '.h': 'C', '.cpp': 'C++', '.cc': 'C++', '.hpp': 'C++', '.py': 'Python', '.js': 'JavaScript', '.mjs': 'JavaScript',
        '.ts': 'TypeScript', '.kt': 'Kotlin', '.java': 'Java', '.go': 'Go', '.rs': 'Rust', '.sh': 'Shell', '.qml': 'QML',
        '.swift': 'Swift', '.m': 'Objective-C', '.ps1': 'PowerShell', '.glsl': 'GLSL', '.comp': 'GLSL', '.md': 'Markdown'}
BUILD = re.compile(r'(^|/)(Makefile|CMakeLists\.txt|package\.json|Cargo\.toml|go\.mod|setup\.py|pyproject\.toml|build\.(bat|sh|gradle(\.kts)?)|'
                   r'settings\.gradle(\.kts)?|meson\.build|install\.sh|Dockerfile|[^/]+\.sln|[^/]+\.pro)$')
ENTRY = re.compile(r'(^|/)(main|index|server|app|cli|__main__)\.[a-z]+$')


def kmeans(X, k, it=30):
    rng = np.random.default_rng(SEED)
    C = X[rng.choice(len(X), k, replace=False)]
    for _ in range(it):
        a = np.argmax(X @ C.T, 1)
        for j in range(k):
            m = X[a == j]
            if len(m):
                C[j] = m.mean(0) / max(1e-9, np.linalg.norm(m.mean(0)))
    return np.argmax(X @ C.T, 1), C


def profile(R, fm, k=None):
    rels = R.g.rels
    n = len(rels)
    lang = Counter(LANG.get(os.path.splitext(r)[1].lower(), None) for r in rels)
    lang.pop(None, None)
    sizes = Counter()
    for r in rels:
        l = LANG.get(os.path.splitext(r)[1].lower())
        if l:
            sizes[l] += len(R.files[r])
    tot = sum(sizes.values()) or 1
    k = k or int(min(8, max(4, round(math.sqrt(n / 40)))))
    code = np.array([inv.is_code(r) for r in rels])
    a, C = kmeans(fm.Fv, k)
    deg = np.bincount(R.g.arr['sym'][0], minlength=n) + np.bincount(R.g.arr['path'][0], minlength=n)
    # termos salientes por area: massa tf-idf na area / massa global
    r_, c_, w_ = fm.tf
    wt = w_ * fm.idf[c_]
    glob = np.bincount(c_, weights=wt, minlength=len(fm.vocab)) + 1e-9
    areas = []
    for j in range(k):
        m = np.flatnonzero(a == j)
        if not len(m):
            continue
        sel = np.isin(r_, m)
        loc = np.bincount(c_[sel], weights=wt[sel], minlength=len(fm.vocab))
        sal = loc * np.log(loc / glob * n / len(m) + 1e-9).clip(0)
        lab = [fm.vocab[i] for i in np.argsort(-sal) if '_' not in fm.vocab[i] and len(fm.vocab[i]) > 2][:4]
        dirs = Counter(os.path.dirname(rels[i]) or '.' for i in m)
        top = sorted(m, key=lambda i: -(deg[i] + 5 * code[i]))[:3]
        areas.append(dict(j=j, files=m, label=lab, dirs=[d for d, _ in dirs.most_common(2)], top=[rels[i] for i in top]))
    links = Counter()
    s, d, _ = R.g.arr['sym'] if len(R.g.arr['sym'][0]) else (np.zeros(0, int),) * 3
    for t in inv.ETYPES:
        s, d, _ = R.g.arr[t]
        for x, y in zip(a[s], a[d]):
            if x < y:
                links[(x, y)] += 1
    lines = [f'Folder: {n} text files; languages: ' + ', '.join(f'{l} {sizes[l] / tot:.0%}' for l, _ in sizes.most_common(4)),
             'Build/entry: ' + ', '.join([r for r in rels if BUILD.search(r)][:4] + [r for r in rels if ENTRY.search(r) and code[R.fid[r]]][:3])]
    for ar in sorted(areas, key=lambda x: -len(x['files'])):
        lines.append(f'Area {ar["j"]} ({len(ar["files"])} files, {"/".join(ar["dirs"])}): {" ".join(ar["label"])} -- {", ".join(ar["top"])}')
    lines.append('Links: ' + '; '.join(f'{x}-{y} ({c})' for (x, y), c in links.most_common(3)))
    return lines[:12], a, areas


def cmd_card(path):
    R = inv.Repo(path)
    fm = Folder({r: R.files[r] for r in R.g.rels}, dim=128, win=5, joint=True, graph=R.g)
    lines, _, _ = profile(R, fm)
    print('\n'.join(lines))


def eval_card(q):
    R = inv.Repo(inv.qdir(q))
    fm = Folder({r: R.files[r] for r in R.g.rels}, dim=128, win=5, joint=True, graph=R.g)
    lines, a, areas = profile(R, fm)
    ans = inv.resolve_answers(R, q)
    ansareas = {a[R.fid[x]] for x in ans}
    qt = set(terms(q['question']))
    # area escolhida: (1) termos do rotulo/diretorios/arquivos do cartao em comum com a pergunta; (2) coseno com o centroide
    def ov(ar):
        txt = ' '.join(ar['label'] + ar['dirs'] + ar['top'])
        return len(qt & set(terms(txt)))
    pick_lab = max(areas, key=lambda ar: (ov(ar), len(ar['files'])))['j']
    qv = fm.qvec(q['question'])
    cents = {ar['j']: fm.Fv[ar['files']].mean(0) for ar in areas}
    pick_cos = max(cents, key=lambda j: float(cents[j] @ qv) if qv is not None else 0)
    big = max(areas, key=lambda ar: len(ar['files']))['j']
    share = sum(len(ar['files']) for ar in areas if ar['j'] in ansareas) / R.g.n
    return dict(id=q['id'], split=q['split'], lab=pick_lab in ansareas, cos=pick_cos in ansareas, big=big in ansareas,
                share=share, k=len(areas), lines=lines)


def cmd_cardeval(procs=3):
    from multiprocessing import Pool
    with Pool(procs) as p:
        res = p.map(eval_card, inv.load_questions(), chunksize=1)
    for label, sub in (('todas', res), ('sem grey-gkatana', [r for r in res if r['id'] != 'grey-gkatana'])):
        print(f'{label} ({len(sub)}): area certa pelo rotulo do cartao {np.mean([r["lab"] for r in sub]):.0%}, pelo vetor da pergunta '
              f'{np.mean([r["cos"] for r in sub]):.0%}, linha de base "maior area" {np.mean([r["big"] for r in sub]):.0%}; '
              f'fracao media de arquivos nas areas-gabarito {np.mean([r["share"] for r in sub]):.0%}; areas por cartao {np.mean([r["k"] for r in sub]):.1f}')
    print('\nexemplo de cartao (primeira pergunta de cada repositorio):')
    seen = set()
    for q, r in zip(inv.load_questions(), res):
        rk = inv.repo_key(q['repo'])
        if rk not in seen:
            seen.add(rk)
            print(f'--- {q["id"]}\n' + '\n'.join(r['lines']))


# ------------------------------------------------------------------ atualizacao: fold-in contra SVD completo
def cmd_foldin():
    qs = inv.load_questions()
    for qid in ('grey-anr', 'sonora-lv2-other'):
        q = [x for x in qs if x['id'] == qid][0]
        R = inv.Repo(inv.qdir(q))
        files = {r: R.files[r] for r in R.g.rels}
        full = Folder(files, dim=128, win=5, keep_counts=True)
        rng = np.random.default_rng(SEED)
        for frac in (0.01, 0.1):
            held = set(rng.choice(R.g.rels, max(1, int(frac * len(files))), replace=False))
            part = Folder({r: t for r, t in files.items() if r not in held}, dim=128, win=5)
            # fold-in de termos novos: linha PPMI (no modelo completo) contra os vetores de contexto ja conhecidos
            t0 = time.time()
            T = np.zeros((len(full.vocab), 128), np.float32)
            known = np.array([part.tid.get(t, -1) for t in full.vocab])
            T[known >= 0] = part.T[known[known >= 0]]
            r, c, v = full.M
            newt = np.flatnonzero(known < 0)
            sel = np.isin(r, newt) & (known[c] >= 0)
            if sel.any():
                acc = np.zeros((len(full.vocab), 128), np.float32)
                np.add.at(acc, r[sel], v[sel, None] * part.T[known[c[sel]]])
                T[newt] = norm_rows(acc[newt])
            # arquivos (todos, inclusive os novos) re-dobrados com os termos atuais: so uma soma ponderada
            Ff = norm_rows(spmm(*full.tf[:2], (full.tf[2] * full.idf[full.tf[1]]).astype(np.float32), T, len(files)))
            dt = time.time() - t0
            # erro: alinhar os espacos (Procrustes ortogonal nos termos comuns) e comparar vetores de arquivo
            m = known >= 0
            Uo, _, Vo = np.linalg.svd(T[m].T @ full.T[m])
            Rm = Uo @ Vo
            idx = [full.fid[h] for h in held]
            cosf = np.mean(np.sum(norm_rows(Ff[idx] @ Rm) * full.Fv[idx], 1))
            cosa = np.mean(np.sum(norm_rows(Ff @ Rm) * full.Fv, 1))
            # efeito na busca: vizinhos de arquivo (top 10 por coseno) iguais?
            ov = []
            for i in idx[:50]:
                a1 = set(np.argsort(-(full.Fv @ full.Fv[i]))[1:11]); a2 = set(np.argsort(-(Ff @ Ff[i]))[1:11])
                ov.append(len(a1 & a2) / 10)
            print(f'{qid}: {len(files)} arquivos; mudam {len(held)} ({frac:.0%}): termos novos {len(newt)}; fold-in {dt * 1000:.0f} ms '
                  f'(SVD completo {full.build_s:.1f} s); coseno arquivo dobrado x completo: mudados {cosf:.3f}, todos {cosa:.3f}; '
                  f'vizinhos top10 em comum {np.mean(ov):.0%}')


def cmd_neighbors(path, term):
    R = inv.Repo(path)
    fm = Folder({r: R.files[r] for r in R.g.rels}, dim=128, win=5, joint=True, graph=R.g)
    if term in fm.fid:
        v = fm.Fv[fm.fid[term]]
    else:
        v = fm.qvec(term)
    if v is None:
        print('termo fora do vocabulario da pasta (df < 2)')
        return
    print('termos:', [fm.vocab[i] for i in np.argsort(-(fm.T @ v))[:15]])
    print('arquivos:', [fm.rels[i] for i in np.argsort(-(fm.Fv @ v))[:10]])


if __name__ == '__main__':
    os.makedirs(inv.CACHE, exist_ok=True)
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    {'questions': cmd_questions, 'commits': cmd_commits, 'cardeval': cmd_cardeval, 'foldin': cmd_foldin}.get(c, lambda: None)() \
        if c in ('questions', 'commits', 'cardeval', 'foldin') else \
        (cmd_card(sys.argv[2]) if c == 'card' else cmd_neighbors(sys.argv[2], sys.argv[3]) if c == 'neighbors' else print(__doc__))
