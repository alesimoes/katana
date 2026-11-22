#!/usr/bin/env python3
"""Painel local do katana: python3 tools/dashboard/server.py [porta]. So biblioteca padrao, so 127.0.0.1, so leitura.
O que e automatico: maquina, placa, processos, fila de medicao, agentes, commits, treino (--log) e provas.
O que e escrito a mao: ~/.katana/dashboard/state.json (etapas, medidas, pendencias)."""
import glob, http.server, json, os, re, subprocess, sys, threading, time
HOME = os.path.expanduser('~'); ROOT = '/mnt/dados/source/katana'
HERE = os.path.dirname(os.path.abspath(__file__)); STATE = HOME + '/.katana/dashboard/state.json'
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
hist, fast, slow = [], {}, {}

def rd(p, d=''):
    try:
        with open(p, errors='replace') as f: return f.read()
    except OSError: return d
def run(*a, cwd=None):
    try: return subprocess.run(a, cwd=cwd, capture_output=True, text=True, timeout=8).stdout.strip()
    except Exception: return ''
def state():
    try: return json.loads(rd(STATE, '{}'))
    except ValueError: return {'erro': 'state.json invalido'}

def cpu_raw():
    v = [int(x) for x in rd('/proc/stat').split('\n', 1)[0].split()[1:9]]
    return sum(v), v[3] + v[4]
def disk_raw():
    r = w = 0
    for l in rd('/proc/diskstats').splitlines():
        p = l.split()
        if re.fullmatch(r'nvme\dn\d', p[2]): r += int(p[5]) * 512; w += int(p[9]) * 512
    return r, w

def agent_of(path):
    m = re.search(r'worktrees/agent-(\w+)', path); return m.group(1) if m else ('junção' if 'worktrees/integ' in path else '')
def label(args):
    """rotulo curto: programa e subcomando, sem argumentos (podem ter texto privado)"""
    s = ' '.join(args)
    for k, n in (('selftest', 'selftest'), ('gradcheck', 'gradiente'), (' train ', 'treino'), ('specialize', 'specialize'), ('bench', 'medição'), ('q4', 'Flash-Next'),
                 ('flash', 'Flash-Next'), ('compare', 'comparação'), ('tmp-iso', 'isolar diferença da placa'), ('measure', 'medição'), ('agent', 'agente katana'), (' run ', 'geração'), (' chat ', 'geração')):
        if k in s: return n
    return os.path.basename(args[0]) if args else '?'
def procs():
    kat, locks, uptime = [], [], float(rd('/proc/uptime', '0').split()[0] or 0)
    hz = os.sysconf('SC_CLK_TCK')
    for d in glob.glob('/proc/[0-9]*'):
        try:
            raw = open(d + '/cmdline', 'rb').read().split(b'\0')[:-1]
            if not raw: continue
            a = [x.decode(errors='replace') for x in raw]; b = os.path.basename(a[0])
            if b != 'flock':
                x = os.readlink(d + '/exe')            # binarios do katana, com qualquer nome, aqui ou nas copias dos agentes
                if not (os.path.basename(x).startswith(('katana', 'tmp-katana')) or x.startswith(ROOT + '/.claude/worktrees/') or '/scratchpad/' in x and 'kat' in x or x.startswith('/mnt/dados/tmp')): continue
            st = rd(d + '/stat').rsplit(')', 1)[1].split(); el = uptime - int(st[19]) / hz
            pid = int(d[6:]); who = agent_of(os.readlink(d + '/cwd'))
            if b == 'flock':
                if not any('katana-train-measure' in x for x in a): continue
                locks.append({'running': bool(rd(f'{d}/task/{pid}/children').strip()), 'what': label(a[2:]), 'who': who, 'sec': int(el)})
            elif int(st[21]) * 4096 < 3e7: continue
            else:
                if len(a) > 1 and a[1] == '__reap': continue
                o = {'what': label(a), 'who': who, 'sec': int(el), 'ram': int(st[21]) * 4096 / 1e9}
                if '--log' in a[:-1]: o['log'] = a[a.index('--log') + 1]
                kat.append(o)
        except (OSError, IndexError, ValueError): continue
    locks.sort(key=lambda x: (not x['running'], -x['sec'])); return kat, locks

def losses(p):
    tr, va = [], []
    for l in rd(p).splitlines()[-4000:]:
        try: j = json.loads(l)
        except ValueError: continue
        if 'loss' in j: tr.append([j['step'], j['loss']])
        if 'val_loss' in j: va.append([j['step'], j['val_loss']])
    k = max(1, len(tr) // 300); return {'train': tr[::k], 'val': va}

teach_cache = {'key': None, 'fam': {}, 'out': None}
def teach(st):
    """o que os dados de treino ensinam: conversas, falas e texto por familia, chamadas por ferramenta (le o arquivo so quando muda)"""
    p = os.path.expanduser(st.get('teach_file', ''))
    try: key = (p, os.stat(p).st_mtime)
    except OSError: return None
    if key == teach_cache['key']: return teach_cache['out']
    fam, tools, conv = {}, {}, {}
    with open(p) as fh:
        for l in fh:
            try: d = json.loads(l)
            except ValueError: continue
            n = d.get('family', '?'); f = fam.setdefault(n, {'conv': 0, 'turns': 0, 'chars': 0}); f['conv'] += 1; conv.setdefault(n, []).append(d['messages'])
            for m in d['messages']:
                if m['role'] != 'assistant' or m.get('train') is False: continue
                f['turns'] += 1; f['chars'] += len(m.get('content') or '')
                for c in m.get('tool_calls') or []:
                    q = c['function']; t = q['name'] if '__' not in q['name'] else 'servidores MCP'; tools[t] = tools.get(t, 0) + 1
                    f['chars'] += len(q['name']) + len(q['arguments'] if isinstance(q['arguments'], str) else json.dumps(q['arguments'], ensure_ascii=False))
    teach_cache.update(key=key, fam=conv, out={'fam': fam, 'tools': sorted(tools.items(), key=lambda x: -x[1]), 'mt': int(key[1])})
    return teach_cache['out']
def sample(q):
    import urllib.parse
    a = urllib.parse.parse_qs(q); c = teach_cache['fam'].get(a.get('f', [''])[0], [])
    if not c: return {'msgs': []}
    i = int(a.get('i', ['0'])[0]) % len(c); out = []
    for m in c[i]:
        out.append({'role': m['role'], 'text': (m.get('content') or '')[:2500], 'train': m['role'] == 'assistant' and m.get('train') is not False,
                    'calls': [{'n': x['function']['name'], 'a': (x['function']['arguments'] if isinstance(x['function']['arguments'], str) else json.dumps(x['function']['arguments'], ensure_ascii=False, indent=1))[:2500]} for x in m.get('tool_calls') or []]})
    return {'msgs': out, 'i': i, 'n': len(c)}

def sampler():
    c0, d0, t0 = cpu_raw(), disk_raw(), time.time()
    while True:
        time.sleep(2)
        try:
            c1, d1, t1 = cpu_raw(), disk_raw(), time.time(); dt = t1 - t0
            mi = {l.split(':')[0]: int(l.split()[1]) / 1048576 for l in rd('/proc/meminfo').splitlines()[:5]}
            g = run('nvidia-smi', '--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw', '--format=csv,noheader,nounits').split(', ')
            def num(i):
                try: return float(g[i])
                except (IndexError, ValueError): return None
            s = {'t': int(t1), 'cpu': 100 * (1 - (c1[1] - c0[1]) / max(1, c1[0] - c0[0])), 'ram': mi['MemTotal'] - mi['MemAvailable'], 'gpu': num(0),
                 'vram': (num(1) or 0) / 1024, 'temp': num(3), 'watt': num(4), 'rd': (d1[0] - d0[0]) / dt / 1e6, 'wr': (d1[1] - d0[1]) / dt / 1e6}
            hist.append(s); del hist[:-300]
            kat, locks = procs(); tl = next((k['log'] for k in kat if 'log' in k), None)
            fast.update(now=int(t1), ram_total=mi['MemTotal'], load=rd('/proc/loadavg').split()[0], cores=os.cpu_count(), procs=kat, queue=locks, loss=losses(tl) if tl else None)
            c0, d0, t0 = c1, d1, t1
        except Exception as e: fast['erro'] = repr(e)[:200]

def slow_loop():
    while True:
        try:
            st = state(); ag = []
            for a in st.get('agents', []):
                w = f"{ROOT}/.claude/worktrees/agent-{a['id']}"; o = {'name': a['name'], 'goal': a.get('goal', '')}
                f = next(iter(glob.glob(f"/tmp/claude-{os.getuid()}/*/*/tasks/{a['id']}.output")), '')
                try: o['idle'] = int(time.time() - os.stat(f).st_mtime)
                except OSError: o['idle'] = None
                o['done'] = bool(a.get('done')); o['changed'] = len(run('git', 'status', '--short', cwd=w).splitlines())
                o['commits'] = int(run('git', 'rev-list', '--count', '--no-merges', 'integ..HEAD', cwd=w) or 0)
                o['last'] = run('git', 'log', '-1', '--format=%s', cwd=w)[:140]; ag.append(o)
            ev = []
            for l in run('git', 'log', '--all', '--no-merges', '-14', '--format=%ct%x09%s', cwd=ROOT).splitlines():
                t, m = l.split('\t', 1); ev.append({'t': int(t), 'm': m[:150]})
            exams = []
            for p in sorted(glob.glob(HOME + '/.katana/bench/*/results.jsonl')):
                r = []
                for l in rd(p).splitlines():
                    try: j = json.loads(l); r.append([j['task'], bool(j['passed']), j['seconds'], bool(j['timed_out'])])
                    except (ValueError, KeyError): pass
                exams.append({'name': os.path.basename(os.path.dirname(p)), 'r': r, 'mt': int(os.stat(p).st_mtime)})
            slow.update(agents=ag, events=ev, exams=exams, teach=teach(st))
        except Exception as e: slow['erro'] = repr(e)[:200]
        time.sleep(10)

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get('Host', '').split(':')[0] not in ('127.0.0.1', 'localhost'): return self.send_error(403)
        if self.path == '/api': b, t = json.dumps({**fast, **slow, 'hist': hist[:], 'state': state()}).encode(), 'application/json'
        elif self.path.startswith('/sample?'):
            try: b, t = json.dumps(sample(self.path[8:])).encode(), 'application/json'
            except ValueError: return self.send_error(400)
        elif self.path == '/': b, t = open(HERE + '/index.html', 'rb').read(), 'text/html; charset=utf-8'
        else: return self.send_error(404)
        self.send_response(200); self.send_header('Content-Type', t); self.send_header('Cache-Control', 'no-store'); self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
for f in (sampler, slow_loop): threading.Thread(target=f, daemon=True).start()
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
