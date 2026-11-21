# Le a saida de count.sh e imprime a tabela de tokens por chamada e formato (markdown).
import sys
d = {}; cur = None; models = []
for l in open(sys.argv[1]):
    l = l.split()
    if not l: continue
    if l[0] == '#': cur = l[1].split('-')[0]; models.append(cur); continue
    if len(l) > 1: d[(cur, l[0])] = int(l[1])
names = sorted({k[1].rsplit('_', 1)[0] for k in d}, key=lambda s: (s[0] != 'c', int(s[1:].split('_')[0]) if s[0] == 'c' else 0, s))
fm = ('xml', 'json', 'g4', 'raw')
print('| call | ' + ' | '.join(f'{m} {f}' for m in models for f in fm) + ' |')
print('|---' * (1 + len(models) * len(fm)) + '|')
tot = {}
for n in names:
    row = []
    for m in models:
        for f in fm:
            v = d.get((m, n + '_' + f))
            row.append('' if v is None else str(v))
            if v is not None and n.startswith('c'): tot[(m, f)] = tot.get((m, f), 0) + v
    print(f'| {n} | ' + ' | '.join(row) + ' |')
print('| total (10 calls) | ' + ' | '.join(str(tot.get((m, f), '')) for m in models for f in fm) + ' |')
