#!/usr/bin/env python3
"""Ativacao esparsa sobre o micro-modelo da pasta: 'quais neuronios disparam' para um pedido.

Neuronios: tokens (vocabulario da pasta), trechos (40 linhas) e arquivos. Entrada: vetor esparso dos tokens do pedido (idf).
  passo 1 token->token  pela matriz de associacao da pasta (PPMI da co-ocorrencia em janela)
  passo 2 token->trecho pelo indice invertido (BM25 por termo), trecho->arquivo pelo maximo
  passo 3 arquivo->arquivo pelas arestas do grafo (simbolos, caminhos, identificadores raros, co-mudanca), 2 rodadas
Em cada passo: decaimento, limiar relativo e inibicao lateral (k vencedores). Saida: so os nos acima do limiar, com o
caminho (qual no disparou e por qual aresta) e a confianca (margem entre vencedores, concentracao da energia). Sem nada
acima do limiar: abstencao.

  python3 tools/findbench/activation.py questions     # recall, salto, risco-cobertura nas 76 perguntas (x BM25 e PPR)
  python3 tools/findbench/activation.py commits       # bench de commits nos tres repositorios
  python3 tools/findbench/activation.py show ID       # a ativacao de uma pergunta, com os caminhos
"""
import math, os, pickle, sys, time
from collections import defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench
import investigator as inv
import folder
from bench import terms

P = dict(k_tok=24, w_assoc=0.35, k_file=40, decay=0.5, k_graph=20, rounds=2, theta=0.15)


def kwta(d, k):
    if len(d) <= k:
        return d
    return dict(sorted(d.items(), key=lambda kv: -kv[1])[:k])


class Net:
    def __init__(self, R, fm):
        self.R, self.fm = R, fm
        r, c, v = fm.M          # PPMI token x token, linhas ordenadas
        self.rowptr = np.searchsorted(r, np.arange(len(fm.vocab) + 1))
        self.c, self.v = c, v
        s, d, w, _ = R.g.comb(inv.ETYPES)
        o = np.argsort(s, kind='stable')
        self.gs, self.gd, self.gw = s[o], d[o], w[o]
        self.gptr = np.searchsorted(self.gs, np.arange(R.g.n + 1))
        self.gtype = {}

    def run(self, q, p=P, use_assoc=True, use_graph=True):
        fm, R = self.fm, self.R
        why = {}
        # entrada
        a = {}
        qterms = [t for t in set(terms(q)) if t not in inv.REQUEST]
        for t in qterms:
            i = fm.tid.get(t)
            if i is not None:
                a[i] = fm.idf[i]
        if not a:          # termos fora do vocabulario da pasta (df<2) ainda entram pelo indice
            pass
        # passo 1: token -> token (associacao da pasta)
        tok = {fm.vocab[i]: x for i, x in a.items()}
        for t in qterms:
            tok.setdefault(t, 1.0)
        if use_assoc and a:
            spread = defaultdict(float)
            for i, x in a.items():
                lo, hi = self.rowptr[i], self.rowptr[i + 1]
                cc, vv = self.c[lo:hi], self.v[lo:hi]
                if len(vv) == 0:
                    continue
                top = np.argsort(-vv)[:p['k_tok']]
                norm = vv[top].sum()
                for j, w in zip(cc[top], vv[top]):
                    spread[int(j)] += p['w_assoc'] * x * w / norm
            for j, x in kwta(spread, p['k_tok']).items():
                t = fm.vocab[j]
                if t not in tok:
                    tok[t] = x
        # passo 2: token -> trecho -> arquivo (BM25 por termo, com o peso de ativacao do token)
        sc = R.ix.bm25('b', list(tok), qw=tok)
        sp = R.ix.bm25('p', list(tok), qw=tok)
        f = defaultdict(float)
        bestc = {}
        for cid, s in sc.items():
            i = int(R.cfile[cid])
            s2 = s + 3.0 * sp.get(cid, 0)
            if s2 > f[i]:
                f[i] = s2; bestc[i] = cid
        f = kwta(f, p['k_file'])
        if f:
            mx = max(f.values())
            f = {i: x / mx for i, x in f.items() if x >= p['theta'] * mx}
        for i in f:
            why[i] = ('match', bestc.get(i))
        # passo 3: arquivo -> arquivo pelo grafo
        if use_graph:
            cur = dict(f)
            for _ in range(p['rounds']):
                nxt = defaultdict(float)
                src = {}
                for i, x in kwta(cur, p['k_graph']).items():
                    lo, hi = self.gptr[i], self.gptr[i + 1]
                    for j, w in zip(self.gd[lo:hi], self.gw[lo:hi]):
                        y = p['decay'] * x * w
                        if y > nxt[int(j)]:
                            src[int(j)] = i
                        nxt[int(j)] += y
                cur = {}
                for j, y in kwta(nxt, p['k_graph']).items():
                    if j not in f:
                        cur[j] = y
                        if y > f.get(j, 0):
                            why[j] = ('edge', src[j])
                    f[j] = f.get(j, 0) + y
        if not f:
            return [], 0.0, {}, tok
        mx = max(f.values())
        out = sorted(((i, x / mx) for i, x in f.items() if x >= p['theta'] * mx), key=lambda kv: -kv[1])
        xs = [x for _, x in out]
        margin = (xs[0] - xs[1]) / xs[0] if len(xs) > 1 else 1.0
        conc = xs[0] / sum(xs[:10])
        conf = margin + conc
        return out, conf, why, tok

    def explain(self, i, why, tok):
        R = self.R
        k, x = why.get(i, ('?', None))
        if k == 'match':
            _, a, b = R.ix.chunks[x]
            hit = [t for t in tok if R.ix.post['b'].get(t) and any(c == x for c, _ in R.ix.post['b'][t])][:4]
            return f'{R.g.rels[i]}:{a}-{b} matched {",".join(hit)}'
        if k == 'edge':
            return f'{R.g.rels[i]} <- {R.g.why(x, i)} <- {R.g.rels[x]}'
        return R.g.rels[i]


def eval_q(q):
    R = inv.Repo(inv.qdir(q))
    ans = inv.resolve_answers(R, q)
    fm = folder.Folder({r: R.files[r] for r in R.g.rels}, dim=16, win=5)   # so a matriz PPMI importa aqui
    net = Net(R, fm)
    qt = set(terms(q['question']))
    withq = set()
    for t in qt:
        withq |= R.files_with(t)
    hop = [a for a in ans if R.fid[a] not in withq]
    out = dict(id=q['id'], split=q['split'], repo=inv.repo_key(q['repo']), ans=ans, hop=hop, rank={}, conf={}, ms={})
    v, _ = R.bm25_files(q['question'])
    o = np.argsort(-v)
    out['rank']['bm25'] = [R.g.rels[i] for i in o[:10] if v[i] > 0]
    out['conf']['bm25'] = (v[o[0]] - v[o[1]]) / v[o[0]] if v[o[0]] > 0 else 0
    seed = inv.seed_vec(v)
    pr = R.g.ppr(seed) if seed.sum() else np.zeros(R.g.n)
    o2 = np.argsort(-pr)
    out['rank']['ppr'] = [R.g.rels[i] for i in o2[:10]]
    out['conf']['ppr'] = (pr[o2[0]] - pr[o2[1]]) / pr[o2[0]] if pr[o2[0]] > 0 else 0
    for name, kw in (('act', {}), ('act sem assoc', dict(use_assoc=False)), ('act sem grafo', dict(use_graph=False))):
        t0 = time.time()
        res, conf, why, tok = net.run(q['question'], **kw)
        out['ms'][name] = (time.time() - t0) * 1000
        out['rank'][name] = [R.g.rels[i] for i, _ in res[:10]]
        out['conf'][name] = conf
        if name == 'act':
            out['nact'] = len(res)
            out['paths'] = [net.explain(i, why, tok) for i, _ in res[:6]]
    return out


def report(res, label):
    print(f'\n{label}: {len(res)} perguntas')
    hm = inv.hop_map()
    for r in res:
        r['hop'] = hm[r['id']]['hop']
    keys = list(res[0]['rank'])
    hops = [(r, a) for r in res for a in r['hop']]
    print(f'  {"metodo":16s}  R@3    R@5    R@10  salto@10  top1   top5')
    for k in keys:
        rr = lambda K: np.mean([sum(a in r['rank'][k][:K] for a in r['ans']) / max(1, len(r['ans'])) for r in res])
        print(f'  {k:16s} {rr(3):6.1%} {rr(5):6.1%} {rr(10):6.1%} {np.mean([a in r["rank"][k][:10] for r, a in hops]):7.1%} '
              f'{np.mean([bool(r["rank"][k]) and r["rank"][k][0] in r["ans"] for r in res]):6.1%} '
              f'{np.mean([any(x in r["ans"] for x in r["rank"][k][:5]) for r in res]):6.1%}')
    print('  risco-cobertura (top1 / top5 entre as mais confiantes):')
    for k in keys:
        it = sorted(((r['conf'][k], bool(r['rank'][k]) and r['rank'][k][0] in r['ans'],
                      any(x in r['ans'] for x in r['rank'][k][:5])) for r in res), key=lambda x: -x[0])
        cells = []
        for cov in (0.25, 0.5, 0.75, 1.0):
            sub = it[:max(1, int(round(cov * len(it))))]
            cells.append(f'{cov:4.0%} {np.mean([x[1] for x in sub]):4.0%}/{np.mean([x[2] for x in sub]):4.0%}')
        print(f'    {k:16s} ' + ' | '.join(cells))


def cmd_questions(procs=3):
    path = os.path.join(inv.CACHE, 'activation-questions.pkl')
    if os.path.exists(path) and '--fresh' not in sys.argv:
        res = pickle.load(open(path, 'rb'))
    else:
        from multiprocessing import Pool
        with Pool(procs) as pl:
            res = pl.map(eval_q, inv.load_questions(), chunksize=1)
        pickle.dump(res, open(path, 'wb'))
    report(res, 'todas')
    report([r for r in res if r['id'] != 'grey-gkatana'], 'todas sem grey-gkatana')
    report([r for r in res if r['split'] == 'train'], 'treino')
    report([r for r in res if r['split'] == 'val'], 'validacao')
    print(f'\nnos ativos acima do limiar (media) {np.mean([r["nact"] for r in res]):.1f}; abstencoes {sum(r["nact"] == 0 for r in res)}; '
          f'tempo da ativacao (Python) {np.mean([r["ms"]["act"] for r in res]):.0f} ms')


def cmd_commits():
    print(f'{"repo":8s} {"metodo":16s} hit@1  hit@5  hit@10  | top1 nos 25%/50% mais confiantes')
    for name, path in inv.SOURCES.items():
        hist = inv.commit_log(path)
        k = min(400, int(len(hist) * 0.35))
        R = inv.Repo(path, cutoff=k, history=hist)
        qs = inv.commit_queries(hist, R.files, k)[:150]
        net = Net(R, folder.Folder({r: R.files[r] for r in R.g.rels}, dim=16, win=5))
        rows = defaultdict(list)
        for h, q, truth in qs:
            v, _ = R.bm25_files(q)
            o = np.argsort(-v)
            rk = {'bm25': ([R.g.rels[i] for i in o[:10] if v[i] > 0], (v[o[0]] - v[o[1]]) / max(v[o[0]], 1e-9))}
            for nm, kw in (('act', {}), ('act sem grafo', dict(use_graph=False)), ('act sem assoc', dict(use_assoc=False))):
                res, conf, _, _ = net.run(q, **kw)
                rk[nm] = ([R.g.rels[i] for i, _ in res[:10]], conf)
            for nm, (order, conf) in rk.items():
                ranks = [order.index(t) + 1 for t in truth if t in order]
                rows[nm].append((min(ranks) if ranks else 0, conf))
        for nm, b in rows.items():
            r = np.array([x[0] for x in b]); c = np.array([x[1] for x in b])
            o = np.argsort(-c)
            q1 = np.mean(r[o[:len(o) // 4]] == 1); q2 = np.mean(r[o[:len(o) // 2]] == 1)
            print(f'{name:8s} {nm:16s} {np.mean(r == 1):5.1%} {np.mean((r > 0) & (r <= 5)):6.1%} {np.mean(r > 0):6.1%}  | {q1:4.0%} / {q2:4.0%}')


def cmd_show(qid):
    q = [x for x in inv.load_questions() if x['id'] == qid][0]
    R = inv.Repo(inv.qdir(q))
    net = Net(R, folder.Folder({r: R.files[r] for r in R.g.rels}, dim=16, win=5))
    res, conf, why, tok = net.run(q['question'])
    print('gabarito:', inv.resolve_answers(R, q))
    print('tokens ativos:', ', '.join(f'{t}:{x:.2f}' for t, x in sorted(tok.items(), key=lambda kv: -kv[1])[:20]))
    print(f'confianca {conf:.2f}; {len(res)} arquivos acima do limiar')
    for i, x in res[:12]:
        print(f'  {x:5.2f} {net.explain(i, why, tok)}')


if __name__ == '__main__':
    c = sys.argv[1] if len(sys.argv) > 1 else ''
    if c == 'questions':
        cmd_questions()
    elif c == 'commits':
        cmd_commits()
    elif c == 'show':
        cmd_show(sys.argv[2])
    else:
        print(__doc__)
