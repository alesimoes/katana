/* ---------- fotos do estado recorrente (DeltaNet) na RAM ----------
 * O cache KV das camadas de atencao so cresce: as linhas antes de uma posicao continuam validas enquanto ninguem escreve
 * por cima. O estado do DeltaNet nao volta atras; uma copia dele numa posicao P (64 MB no Qwen3.6) basta para recomecar de P.
 * Quem escreve no cache avisa pelo kvt (engine/forward.c): tokens a partir de p apagam as fotos depois de p, e os ids da
 * conversa no cache ficam sempre em dia. Uma foto sobrevive a tudo o que nao mexe nas linhas antes dela.
 * Quantidade: KATANA_SNAPSHOTS (padrao 8, 0 desliga). Cheio, sai a que menos falta faz: a distancia entre as vizinhas dividida
 * pela idade (as velhas ficam mais espacadas); fim de etapa do plano vale 4 vezes mais. KATANA_SNAP_DBG=1 mostra as copias. */
#define SNAP_MAX 64
#define SNAP_GAP 96         /* tokens desde a ultima foto que justificam outra */
typedef struct { int pos, step, vram; char *buf; } Snap;
static struct { Snap v[SNAP_MAX], held[SNAP_MAX]; int n, N, init, hold_n, kn, kbad; int *kids; double t_take, t_back; long n_take, n_back; } g_sn;

/* onde mora o estado agora: 1 VRAM (grafo Vulkan), 0 RAM, -1 nao coberto */
static int snap_where(const Model *m) {
    const Config *c = &m->cfg;
    if (!c->ssm_inner || c->q4e || m->ckv) return -1;
    if (gpu.force_cpu) return 0;
    if (vkg.on) return vkq.on ? 1 : -1;
    return gpu.n_gl > 0 ? -1 : 0;
}
static size_t snap_bytes(const Model *m, int vram) {
    const Config *c = &m->cfg;
    return vram ? (size_t)m->n_lin * ((size_t)4 * vkq.CH * 4 + (size_t)vkq.NV * c->ssm_state * c->ssm_state * 4) : (size_t)m->n_lin * dn_state_floats(c) * sizeof(float);
}
/* copia o estado: dir 1 do motor para buf, 0 de volta. Na VRAM em grupos de ate 8 MB, um envio por grupo */
static int snap_io(Model *m, char *buf, int vram, int dir) {
    const Config *c = &m->cfg;
    if (!vram) { const size_t b = snap_bytes(m, 0); if (dir) memcpy(buf, m->ssm, b); else memcpy(m->ssm, buf, b); return 1; }
    const size_t rb = (size_t)4 * vkq.CH * 4, sb = (size_t)vkq.NV * c->ssm_state * c->ssm_state * 4;
    if (!dir) {
        for (int li = 0; li < m->n_lin; li++, buf += rb + sb)
            if (!kvk_buf_write_q(vkq.ring[li], 0, buf, rb) || !kvk_buf_write_q(vkq.st[li], 0, buf + rb, sb)) return 0;
        return kvk_wait();
    }
    KvkBuf b[16]; void *d[16]; size_t nb[16], tot = 0; int k = 0;
    for (int li = 0; li < m->n_lin; li++, buf += rb + sb) {
        b[k] = vkq.ring[li]; d[k] = buf; nb[k++] = rb;
        b[k] = vkq.st[li]; d[k] = buf + rb; nb[k++] = sb;
        tot += rb + sb;
        if (k + 2 > 16 || tot + rb + sb > ((size_t)8 << 20) || li == m->n_lin - 1) { if (!kvk_buf_readv(b, d, nb, k)) return 0; k = 0; tot = 0; }
    }
    return 1;
}
static void snap_drop(int from) {       /* esquece as fotos a partir do indice; os buffers ficam para a proxima */
    if (from < g_sn.n) g_sn.n = from;
    kvt.top = g_sn.n ? g_sn.v[g_sn.n - 1].pos : 0;
}
static void snap_cut(int pos0) {        /* as linhas de pos0 em diante vao ser escritas */
    int k = 0;
    while (k < g_sn.n && g_sn.v[k].pos <= pos0) k++;
    snap_drop(k);
}
static int snap_on(Model *m) {
    if (!g_sn.init) {
        g_sn.init = 1;
        const char *e = getenv("KATANA_SNAPSHOTS");
        g_sn.N = e ? atoi(e) : 8;
        if (g_sn.N > SNAP_MAX) g_sn.N = SNAP_MAX;
        if (g_sn.N < 0 || snap_where(m) < 0) g_sn.N = 0;
        if (g_sn.N) { kvt.cap = m->cfg.ctx; kvt.ids = xalloc(kvt.cap, sizeof(int)); kvt.cut = snap_cut; kvt.bad = 1; kvt.on = 1; }
    }
    return g_sn.N > 0 && kvt.on;
}
/* guarda o estado da posicao atual; step: fim de uma etapa do plano. Devolve a posicao, ou 0 se nao guardou */
static int snap_take(Model *m, int step) {
    if (!snap_on(m) || kvt.bad || kvt.n < 1 || kvt.n > kvt.cap) return 0;
    const int w = snap_where(m), last = g_sn.n ? g_sn.v[g_sn.n - 1].pos : 0;
    if (w < 0) return 0;
    if (last == kvt.n && g_sn.v[g_sn.n - 1].vram == w) { g_sn.v[g_sn.n - 1].step |= step; return last; }
    if (last >= kvt.n) return 0;                                /* fotos adiante ainda validas (logo depois de uma volta) */
    if (last && kvt.n - last < SNAP_GAP && !step) return 0;
    const double t0 = now_sec();
    if (g_sn.n >= g_sn.N) {
        int k = 0; double worst = 1e30;
        for (int i = g_sn.n > 2; i < g_sn.n; i++) {      /* a primeira fica: e o recomeco de quando tudo muda */
            const int a = i ? g_sn.v[i - 1].pos : 0, b = i + 1 < g_sn.n ? g_sn.v[i + 1].pos : kvt.n;
            const double s = (double)(b - a) / (kvt.n - g_sn.v[i].pos + 1) * (g_sn.v[i].step ? 4 : 1);
            if (s < worst) { worst = s; k = i; }
        }
        const Snap x = g_sn.v[k];
        memmove(&g_sn.v[k], &g_sn.v[k + 1], sizeof(Snap) * (size_t)(g_sn.n - 1 - k));
        g_sn.v[--g_sn.n] = x;
    }
    Snap *s = &g_sn.v[g_sn.n];
    if (!s->buf) {      /* um tamanho que serve para os dois lugares */
        size_t nb = snap_bytes(m, 0);
        if (vkq.on && snap_bytes(m, 1) > nb) nb = snap_bytes(m, 1);
        s->buf = malloc(nb + 64);
    }
    if (!s->buf || !snap_io(m, s->buf, w, 1)) { kvt.top = g_sn.n ? g_sn.v[g_sn.n - 1].pos : 0; return 0; }
    s->pos = kvt.n; s->step = step; s->vram = w; g_sn.n++;
    kvt.top = s->pos;
    g_sn.t_take = now_sec() - t0; g_sn.n_take++;
    if (getenv("KATANA_SNAP_DBG")) fprintf(stderr, "[foto do estado: posicao %d, %.0f MB em %.1f ms (%d de %d)]\n", s->pos, snap_bytes(m, w) / 1048576.0, 1e3 * g_sn.t_take, g_sn.n, g_sn.N);
    return s->pos;
}
/* volta o motor a posicao da foto i (as linhas do cache antes dela estao no lugar) */
static int snap_restore(Model *m, int i) {
    if (i < 0 || i >= g_sn.n || g_sn.v[i].vram != snap_where(m)) return 0;
    Snap *s = &g_sn.v[i];
    const double t0 = now_sec();
    if (!snap_io(m, s->buf, s->vram, 0)) { kvt.bad = 1; return 0; }
    kvt.n = s->pos; kvt.bad = 0; kvt.cpu = gpu.force_cpu != 0;
    g_sn.t_back = now_sec() - t0; g_sn.n_back++;
    if (getenv("KATANA_SNAP_DBG")) fprintf(stderr, "[volta ao estado da posicao %d em %.1f ms]\n", s->pos, 1e3 * g_sn.t_back);
    return s->pos;
}
/* a foto mais funda que repete o comeco de ids[0..n) (sobra pelo menos um token); -1 se nenhuma */
static int snap_find(const Model *m, const int *ids, int n) {
    int k = 0;
    const int w = snap_where(m);
    while (k < n - 1 && k < kvt.top && kvt.ids[k] == ids[k]) k++;
    for (int i = g_sn.n - 1; i >= 0; i--) if (g_sn.v[i].pos <= k && g_sn.v[i].vram == w) return i;
    return -1;
}
/* fotos que servem para um texto: as que cobrem um comeco dele. O texto de uma resposta gerada pode nao voltar aos mesmos
   tokens quando e tokenizado de novo, entao a comparacao e em bytes; a foto so serve se o que vem depois e o fecho de turno
   (um token so: a costura nao muda a tokenizacao). idx e off (byte onde o resto comeca), da mais velha a mais nova; devolve
   quantas. *end: o estado atual tambem serve (byte onde ele termina), senao -1 */
static int snap_usable(Model *m, const char *text, int *idx, size_t *off, int cap, long *end) {
    const Tokenizer *tk = &m->tk;
    *end = -1;
    if (!snap_on(m)) return 0;
    const size_t len = strlen(text), ml = strcspn(tf_end(), "\n");
    const int w = snap_where(m), live = kvt.bad || kvt.cpu != (gpu.force_cpu != 0) ? 0 : kvt.n, stop = kvt.top > live ? kvt.top : live;
    size_t at = 0;
    int n = 0, k = 0;
    char tmp[512];
    if (!ml) return 0;
    for (int t = 0; t < stop && t < kvt.cap; t++) {
        if (!(t == 0 && m->cfg.add_bos && kvt.ids[0] == m->cfg.bos)) {
            const size_t tl = tok_append(tk, kvt.ids[t], tmp, 0, sizeof tmp - 1);
            if (!tl || at + tl > len || memcmp(text + at, tmp, tl) != 0) break;
            at += tl;
        }
        while (k < g_sn.n && g_sn.v[k].pos < t + 1) k++;
        if (at >= len || strncmp(text + at, tf_end(), ml) != 0) continue;
        if (k < g_sn.n && g_sn.v[k].pos == t + 1 && g_sn.v[k].vram == w && n < cap) { idx[n] = k; off[n++] = at; }
        if (t + 1 == live) *end = (long)at;
    }
    return n;
}
/* prepara o cache para o texto inteiro: poe o motor na foto mais funda que serve e devolve a posicao (o resto dos ids tem de
   ser processado), com todos os ids em out; 0 se nenhuma serve (out vazio) */
static int snap_plan(Model *m, const char *text, Ids *out) {
    int idx[SNAP_MAX]; size_t off[SNAP_MAX]; long end;
    const int n = snap_usable(m, text, idx, off, SNAP_MAX, &end);
    int pos = 0; size_t at = 0;
    if (end >= 0 && (!n || kvt.n >= g_sn.v[idx[n - 1]].pos)) { pos = kvt.n; at = (size_t)end; }
    else if (n) { pos = snap_restore(m, idx[n - 1]); at = off[n - 1]; }
    if (!pos) return 0;
    for (int i = 0; i < pos; i++) ids_push(out, kvt.ids[i]);
    encode(&m->tk, text + at, out);
    return pos;         /* depois da foto vem pelo menos o fecho de turno: sobra token */
}
/* o laco do agente vai processar texto novo na posicao atual (resultado de ferramenta, pedido): fotografa antes, porque e a
   partir daqui que uma compactacao pode mudar o texto */
static void snap_round(Model *m, int pos, int step) { if (snap_on(m) && pos > 0 && pos == kvt.n) snap_take(m, step); }

/* outra conversa usa o cache por um tempo (subagente, nota de leitura) e depois os mesmos bytes voltam do disco: as fotos
   ficam guardadas e valem de novo se o cache voltou */
static void snap_hold(void) {
    if (!kvt.on) return;
    if (!g_sn.kids) g_sn.kids = xalloc(kvt.cap, sizeof(int));
    memcpy(g_sn.held, g_sn.v, sizeof g_sn.v);
    memset(g_sn.v, 0, sizeof g_sn.v);
    g_sn.hold_n = g_sn.n; g_sn.n = 0;
    g_sn.kn = kvt.n; g_sn.kbad = kvt.bad;
    memcpy(g_sn.kids, kvt.ids, sizeof(int) * (size_t)kvt.cap);
    kvt.top = 0;
}
static void snap_release(int back, int pos) {       /* pos: linhas que voltaram */
    if (!kvt.on) return;
    for (int i = 0; i < SNAP_MAX; i++) free(g_sn.v[i].buf);
    memcpy(g_sn.v, g_sn.held, sizeof g_sn.v);
    g_sn.n = g_sn.hold_n;
    memcpy(kvt.ids, g_sn.kids, sizeof(int) * (size_t)kvt.cap);
    if (back) { kvt.n = pos; kvt.bad = g_sn.kbad || g_sn.kn != pos; kvt.cpu = gpu.force_cpu != 0; snap_cut(pos); }
    else { kvt.n = 0; kvt.bad = 1; snap_drop(0); }
}

/* servidor: o cache tem n tokens e o pedido novo (nnew) repete os primeiros `common`. Como cache_rewind, mas com estado
   recorrente volta a foto mais funda dentro do trecho comum. Devolve quantas posicoes valem */
static int snap_rewind(Model *m, const int *ids, int n, int common, int nnew) {
    const int c = common < nnew ? common : nnew - 1;
    if (c == n || !snap_on(m)) return cache_rewind(m, n, common, nnew);
    const int i = snap_find(m, ids, c + 1);
    if (i < 0) return 0;
    const int p = snap_restore(m, i);
    if (p) fprintf(stderr, "[cache: volta a posicao %d por uma copia do estado (%.0f ms)]\n", p, 1e3 * g_sn.t_back);
    return p;
}
/* processa o pedido deixando uma foto antes da ultima abertura de turno: e ali que o proximo pedido do mesmo cliente se
   separa deste (a resposta volta reescrita, sem o raciocinio) */
static float *snap_feed(Model *m, const int *ids, int n, int from) {
    if (snap_on(m)) {
        const int im = map_get(&m->tk.vocab, "<|im_start|>", 12);
        int p = n - 1;
        while (im >= 0 && p > from && ids[p] != im) p--;
        if (im >= 0 && p > from) {
            forward_seq(m, ids + from, p - from, from, 0);
            snap_take(m, 0);
            from = p;
        }
    }
    return forward_seq(m, ids + from, n - from, from, 1);
}
