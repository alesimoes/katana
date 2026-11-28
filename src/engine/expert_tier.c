/* Camadas de memoria para especialistas (MoE maiores que a RAM): GPU, RAM e SSD.
 *
 * Desligado por padrao. KATANA_EXPERT_RAM=<GB> liga um orcamento explicito de RAM para os especialistas
 * lidos pelo caminho da CPU (moe_cpu_multi): em vez de paginas do arquivo mapeado (falta de pagina = leitura
 * de 4 KB por vez, sincrona), cada especialista e lido do arquivo por varias threads, com O_DIRECT, para
 * uma vaga de uma arena de RAM. Os bytes na vaga sao os do arquivo (mesmo alinhamento dentro da pagina), entao
 * o resultado dos produtos e identico bit a bit ao do caminho mapeado.
 *
 *   KATANA_EXPERT_RAM=<GB>      orcamento (maximo 25)
 *   KATANA_EXPERT_POLICY=heat|lru   despejo: frequencia com decaimento (padrao) ou menos recente
 *   KATANA_EXPERT_DECAY=<g>     fator de decaimento por token da frequencia (padrao 0.95)
 *   KATANA_EXPERT_PRED=early|cooc|prev|none   quem adivinha os especialistas da proxima camada (padrao early)
 *   KATANA_EXPERT_PFN=<n>       quantos prever por camada (padrao K)
 *   KATANA_EXPERT_IO=<n>        threads de leitura (padrao 16)
 *   KATANA_EXPERT_DIRECT=0      leitura com cache do sistema em vez de O_DIRECT
 *   KATANA_EXPERT_PFD=<d>       distancia da previsao em camadas (padrao 1; 2 e 3 usam o roteador antecipado)
 *
 * Pacote de especialistas (katana bundle <modelo.gguf>): no GGUF as tres matrizes de um especialista (gate, up, down)
 * ficam em tres regioes afastadas do arquivo, entao trazer um especialista sao tres leituras. O comando grava
 * <modelo.gguf>.exb com as tres juntas, um registro por especialista comecando em fronteira de pagina, e a arena
 * passa a fazer UMA leitura por especialista. Os bytes sao os do GGUF e cada matriz fica no mesmo alinhamento
 * de 64 bytes do original, entao o resultado continua identico bit a bit. Se o arquivo existe e confere com o
 * modelo (tamanho do GGUF, posicao e tamanho de cada matriz), a arena o usa sozinha; se nao confere, avisa e le
 * do GGUF. katana bundle check confere os bytes pelo caminho real da arena; katana bundle bench mede as leituras.
 *
 * Um token nunca usa especialista que o roteador nao escolheu: a previsao so adianta leituras; o que o roteador
 * escolhe e pedido na hora (se a arena nao comporta os pedidos de uma chamada, a chamada usa o caminho mapeado).
 *
 * Quantizacao mista por calor (EdgeMoE/arXiv:2308.14352, "LLM in a Flash"/arXiv:2312.11514): o pacote pode gravar,
 * alem da copia na quantizacao original (hi), uma segunda copia menor de cada especialista, requantizada mais
 * agressiva (cold_type, Q2_K por padrao; KATANA_BUNDLE_COLD=0 desliga ao gravar o pacote). As duas copias ficam em
 * posicoes separadas do arquivo, cada uma alinhada a pagina e lida inteira numa leitura so (gate+up+down juntos,
 * como antes); a arena nunca le as duas. Escolhida a mais simples das opcoes discutidas: nada de estatistica
 * persistida entre sessoes nem de reescrever o pacote sob comando - o calor ja rastreado por especialista
 * (et.ghot, mesma ideia do heat por vaga, so que por (camada, especialista) e nao por vaga, entao sobrevive ao
 * especialista ser despejado) decide na hora de cada leitura: abaixo da media global, le a copia fria (menos
 * bytes do disco); do contrario le a hi. Um especialista nunca visto neste processo comeca "quente" (ve a copia
 * cheia primeiro) e so cai pra fria depois de ficar repetidamente abaixo da media. O tipo da matriz (Mat.type e
 * .stride) muda por chamada conforme a vaga que serviu o pedido: o motor ja escolhe o kernel pelo tipo de cada
 * Mat, entao misturar hi e fria no mesmo lote de tokens funciona sem mudar os kernels de produto.
 * Medidas (katana tiersim ...) e o teste de leitura aleatoria do disco (katana iobench) ficam no fim do arquivo. */
#ifdef __linux__
#include <errno.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#ifndef O_DIRECT
#define O_DIRECT 040000
#endif

static const uint8_t *et_base;                          /* inicio do arquivo mapeado */
static char et_path[1024];

/* ---------------- a arena de especialistas ---------------- */
typedef struct {
    int key;                    /* camada * E + especialista; -1 = livre */
    int state;                  /* 0 livre, 1 lendo, 2 pronta */
    int pend;                   /* leituras que faltam */
    int pf;                     /* 1 = veio de previsao e ainda nao foi usada */
    uint64_t pin;               /* serial da chamada que a usa */
    uint64_t stamp;             /* ultimo uso */
    uint64_t ptick;             /* token em que a previsao a pediu */
    double heat;                /* frequencia com decaimento (escala crescente) */
    int cold;                   /* 1 = a vaga tem a copia fria (et.cold_type), nao a original */
    size_t doff[3];             /* onde os dados de cada matriz comecam na vaga */
} EtSlot;
typedef struct { int slot; size_t dst, off, len; } EtJob;      /* le [off, off+len) do arquivo para vaga + dst */
/* pacote de especialistas (<modelo>.exb): cabecalho, indice (um EtBx por camada x especialista) e os registros.
   v02: cada registro pode ter, alem do hi (quantizacao original), uma copia fria (cold_type, 0 = pacote sem
   copia fria) numa posicao separada do arquivo, tambem alinhada a pagina. */
#define ET_BX_MAGIC "KTNEXB02"
typedef struct { char magic[8]; uint32_t L, E; uint64_t gguf_size, data_start; uint32_t cold_type; uint8_t pad[28]; } EtBxHdr;
typedef struct {
    uint64_t pos;               /* inicio do registro hi no pacote (multiplo de pagina) */
    uint64_t src[3];            /* onde cada matriz hi esta no GGUF (confere o pacote com o modelo) */
    uint32_t len[3], rel[3];    /* tamanho de cada matriz hi e onde ela comeca dentro do registro */
    uint32_t rec, pad;          /* tamanho do registro hi, arredondado para paginas */
    uint64_t cpos;              /* inicio do registro frio (0 = este especialista nao tem copia fria) */
    uint32_t clen[3], crel[3];  /* tamanho/posicao de cada matriz na copia fria */
    uint32_t crec, pad2;        /* tamanho do registro frio, arredondado para paginas */
} EtBx;
static struct {
    int L, E, K, nslots, nthr, direct, policy, pred, pfn, pfd, fd, last_l;
    size_t slot_bytes; uint8_t *arena; int *where; EtSlot *sl;
    double decay, grow; uint64_t tick, serial, clock;
    pthread_mutex_t mu; pthread_cond_t cv_job, cv_done;
    EtJob *q; int qh, qt, qcap; EtJob *pq; int pqh, pqt;     /* fila da demanda (q) e da previsao (pq) */
    int stop;
    /* previsao: coocorrencia aprendida e ultima escolha por camada */
    float *co; float *conn; int *prev;
    /* estatisticas */
    uint64_t hit, miss, bypass, pf_issued, pf_useful, pf_late, pf_wasted, calls;
    double bytes, t_stall, t_read; uint64_t reads;
    double t_pred;
    EtBx *bx;                   /* indice do pacote; NULL = le as tres regioes do GGUF */
    /* quantizacao mista por calor (so com pacote): calor global por especialista, independente da vaga que o
       guarda (sobrevive ao despejo), e a soma de todos pra comparar com a media sem recalcula-la a cada leitura */
    double *ghot; double gsum; uint32_t cold_type; uint64_t fetch_hot, fetch_cold;
} et;
static int et_no_bundle;        /* katana bundle check|bench ... scattered: ignora o pacote (comparar os dois caminhos) */
static int et_force_cold, et_force_hot;   /* katana bundle check ... cold: forca hi ou fria nos testes, ignorando o calor */

static int et_pread_full(int fd, void *buf, size_t len, off_t off) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, (char *)buf + got, len - got, off + got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;                      /* fim do arquivo: o resto da pagina nao importa */
        got += (size_t)r;
    }
    return 0;
}
static void *et_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&et.mu);
    for (;;) {
        EtJob j;
        while (!et.stop && et.qh == et.qt && et.pqh == et.pqt) pthread_cond_wait(&et.cv_job, &et.mu);
        if (et.stop) break;
        if (et.qh != et.qt) j = et.q[et.qh++ % et.qcap];
        else j = et.pq[et.pqh++ % et.qcap];
        pthread_mutex_unlock(&et.mu);
        const double t0 = now_sec();
        uint8_t *dst = et.arena + (size_t)j.slot * et.slot_bytes + j.dst;
        if (et_pread_full(et.fd, dst, j.len, (off_t)j.off) != 0) { perror("expert_tier: pread"); exit(1); }
        const double dt = now_sec() - t0;
        pthread_mutex_lock(&et.mu);
        et.t_read += dt; et.bytes += (double)j.len; et.reads++;
        if (--et.sl[j.slot].pend == 0) { et.sl[j.slot].state = 2; pthread_cond_broadcast(&et.cv_done); }
    }
    pthread_mutex_unlock(&et.mu);
    return NULL;
}

/* onde ficam as tres matrizes do especialista (deslocamento no arquivo e tamanho) */
static int et_regions(const Model *m, int l, int e, size_t off[3], size_t len[3]) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[l];
    const Mat *ms[3] = { &L->eg[e], &L->eu[e], &L->ed[e] };
    const size_t rows[3] = { (size_t)c->hidden, (size_t)c->hidden, (size_t)c->dim };
    for (int i = 0; i < 3; i++) {
        if (!ms[i]->q) return 0;
        off[i] = (size_t)(ms[i]->q - et_base); len[i] = rows[i] * ms[i]->stride;
    }
    return 1;
}
#define ET_PG 4096u
/* leitura alinhada: [a, b) cobre [off, off+len) em paginas; o byte `off` cai em dest + (off - a) */
static void et_span(size_t off, size_t len, size_t *a, size_t *b) {
    *a = off & ~(size_t)(ET_PG - 1); *b = (off + len + ET_PG - 1) & ~(size_t)(ET_PG - 1);
}

/* registro do pacote: as tres matrizes em seguida, cada uma no mesmo alinhamento de 64 bytes que tem no GGUF.
   Devolve o tamanho do registro (multiplo de pagina). */
static size_t et_bx_layout(const size_t off[3], const size_t len[3], size_t rel[3]) {
    size_t pos = 0;
    for (int r = 0; r < 3; r++) { rel[r] = pos + ((off[r] - pos) & 63); pos = rel[r] + len[r]; }
    return (pos + ET_PG - 1) & ~(size_t)(ET_PG - 1);
}
/* layout da copia fria: gate, up e down requantizados para cold_type, em sequencia, cada um alinhado a 64 bytes
   (nao precisa casar com o alinhamento do GGUF, que so existe pra o tipo original). Os tres tem o mesmo numero
   de elementos (hidden*dim: gate/up tem hidden linhas de dim colunas, down tem dim linhas de hidden colunas, o
   produto e igual), so o tipo muda o tamanho em bytes. Devolve 0 (sem copia fria) se cold_type for 0 ou n nao for
   multiplo do bloco desse tipo (nao acontece nos modelos do catalogo, e um especialista so sem copia fria). */
static size_t et_bx_cold_layout(const Config *c, uint32_t cold_type, uint32_t clen[3], uint32_t crel[3]) {
    const size_t ne = (size_t)c->hidden * (size_t)c->dim;
    const size_t cb = cold_type ? qbytes(cold_type, ne) : 0;
    if (!cb) { clen[0] = clen[1] = clen[2] = 0; crel[0] = crel[1] = crel[2] = 0; return 0; }
    size_t pos = 0;
    for (int r = 0; r < 3; r++) { clen[r] = (uint32_t)cb; crel[r] = (uint32_t)pos; pos += (cb + 63) & ~(size_t)63; }
    return (pos + ET_PG - 1) & ~(size_t)(ET_PG - 1);
}
static void et_bx_name(char *out, size_t cap, const char *model) { snprintf(out, cap, "%s.exb", model); }
/* abre o pacote do modelo e confere com ele; devolve o indice ou NULL (sem pacote, ou pacote de outro arquivo).
   cold_type_out recebe o tipo da copia fria do pacote (0 = pacote sem copia fria). */
static EtBx *et_bx_open(const Model *m, const char *model, int L, int E, int *fd_out, int direct, uint32_t *cold_type_out) {
    char fn[1100]; et_bx_name(fn, sizeof fn, model);
    struct stat sg, sb;
    int fd = open(fn, O_RDONLY);
    if (fd < 0) return NULL;
    EtBxHdr h; EtBx *bx = NULL; const size_t n = (size_t)L * E;
    const char *why = "cabecalho";
    if (fstat(fd, &sb) != 0 || stat(model, &sg) != 0 || et_pread_full(fd, &h, sizeof h, 0) != 0 || (size_t)sb.st_size < sizeof h) goto bad;
    if (memcmp(h.magic, ET_BX_MAGIC, 8) || h.L != (uint32_t)L || h.E != (uint32_t)E || h.gguf_size != (uint64_t)sg.st_size) { why = "e de outro modelo"; goto bad; }
    if ((uint64_t)sb.st_size < sizeof h + n * sizeof(EtBx)) { why = "indice cortado"; goto bad; }
    bx = xalloc(n, sizeof(EtBx));
    if (et_pread_full(fd, bx, n * sizeof(EtBx), sizeof h) != 0) goto bad;
    why = "indice nao confere com o modelo";
    {
        uint32_t clen[3], crel[3];
        const size_t crec = et_bx_cold_layout(&m->cfg, h.cold_type, clen, crel);
        for (int l = 0; l < L; l++) for (int e = 0; e < E; e++) {
            const EtBx *b = &bx[(size_t)l * E + e];
            size_t off[3], len[3], rel[3];
            if (!et_regions(m, l, e, off, len)) goto bad;
            const size_t rec = et_bx_layout(off, len, rel);
            for (int r = 0; r < 3; r++) if (b->src[r] != off[r] || b->len[r] != len[r] || b->rel[r] != rel[r]) goto bad;
            if (b->rec != rec || (b->pos & (ET_PG - 1)) || b->pos < h.data_start || b->pos + rec > (uint64_t)sb.st_size) goto bad;
            if (crec == 0) { if (b->cpos) goto bad; }
            else {
                for (int r = 0; r < 3; r++) if (b->clen[r] != clen[r] || b->crel[r] != crel[r]) goto bad;
                if (b->cpos && (b->crec != crec || (b->cpos & (ET_PG - 1)) || b->cpos < h.data_start || b->cpos + crec > (uint64_t)sb.st_size)) goto bad;
            }
        }
    }
    close(fd);
    fd = open(fn, O_RDONLY | (direct ? O_DIRECT : 0));
    if (fd < 0) { perror("expert_tier: open do pacote"); free(bx); return NULL; }
    *fd_out = fd;
    if (cold_type_out) *cold_type_out = h.cold_type;
    return bx;
bad:
    fprintf(stderr, "expert_tier: %s ignorado (%s); refaca com katana bundle\n", fn, why);
    close(fd); free(bx);
    return NULL;
}

/* escolhe a vaga a despejar: menor valor entre as prontas, sem uso na chamada e sem previsao pendente de uso
   (previsao so despeja as outras). Chamada com o mutex tomado. Devolve -1 se nao houver. */
static int et_victim(void) {
    int best = -1;
    for (int s = 0; s < et.nslots; s++) {
        const EtSlot *p = &et.sl[s];
        if (p->key < 0) return s;
        if (p->state != 2 || p->pin == et.serial || (p->pf && p->ptick == et.tick)) continue;
        if (best < 0) { best = s; continue; }
        const EtSlot *b = &et.sl[best];
        if (et.policy == 0 ? (p->heat < b->heat || (p->heat == b->heat && p->stamp < b->stamp)) : p->stamp < b->stamp) best = s;
    }
    return best;
}
/* poe as leituras de um especialista numa vaga (mutex tomado) */
static void et_fill(Model *m, int s, int l, int e, int demand) {
    EtSlot *p = &et.sl[s];
    size_t off[3], len[3], pos = 0;
    if (p->key >= 0) {
        if (p->pf) et.pf_wasted++;
        et.where[p->key] = -1;
    }
    p->key = l * et.E + e; et.where[p->key] = s; p->state = 1; p->pf = !demand; p->ptick = et.tick; p->heat = 0; p->cold = 0;
    if (et.bx) {                                /* pacote: o registro inteiro (hi ou frio) numa leitura so */
        const EtBx *b = &et.bx[p->key];
        /* calor deste especialista contra a media global: abaixo da media (e com copia fria) le a fria, que e
           menos bytes; et_force_cold/et_force_hot forcam a escolha nos testes (katana bundle check ... cold) */
        const int cold = b->cpos && (et_force_cold || (!et_force_hot && et.ghot[p->key] * (double)(et.L * et.E) < et.gsum));
        p->cold = cold;
        if (cold) { et.fetch_cold++; for (int r = 0; r < 3; r++) p->doff[r] = b->crel[r]; }
        else { et.fetch_hot++; for (int r = 0; r < 3; r++) p->doff[r] = b->rel[r]; }
        EtJob j = { s, 0, (size_t)(cold ? b->cpos : b->pos), cold ? b->crec : b->rec };
        p->pend = 1;
        if (demand) et.q[et.qt++ % et.qcap] = j; else et.pq[et.pqt++ % et.qcap] = j;
        pthread_cond_broadcast(&et.cv_job);
        return;
    }
    et_regions(m, l, e, off, len);
    p->pend = 3;
    for (int r = 0; r < 3; r++) {
        size_t a, b; et_span(off[r], len[r], &a, &b);
        p->doff[r] = pos + (off[r] - a);        /* a leitura grava em pos; o byte `off` cai em pos + (off - a) */
        EtJob j = { s, pos, a, b - a };
        pos += b - a;
        if (demand) et.q[et.qt++ % et.qcap] = j; else et.pq[et.pqt++ % et.qcap] = j;
    }
    pthread_cond_broadcast(&et.cv_job);
}

/* os dados da matriz r (0 gate, 1 up, 2 down) do especialista que esta na vaga s */
static const uint8_t *et_slot_data(int s, int r) { return et.arena + (size_t)s * et.slot_bytes + et.sl[s].doff[r]; }

/* chamada do moe_cpu_multi: devolve 0 se a arena nao pode servir (usa o caminho mapeado) */
static int et_wrap(Model *m, const Layer *L, const int *ex, const int *xi, int n, int n_x, const float *xs, float *outs) {
    static Mat *eg, *eu, *ed; static int *ex2, *uniq, *map; static int cap;
    const int l = (int)(L - m->layers), E = et.E;
    if (!eg) { cap = 4096; eg = xalloc(cap, sizeof(Mat)); eu = xalloc(cap, sizeof(Mat)); ed = xalloc(cap, sizeof(Mat)); ex2 = xalloc(cap, 4); uniq = xalloc(cap, 4); map = xalloc(E, 4); }
    if (n > cap) return 0;
    et.calls++;
    int nu = 0;
    for (int e = 0; e < E; e++) map[e] = -1;
    for (int j = 0; j < n; j++) { if (map[ex[j]] < 0) { map[ex[j]] = nu; uniq[nu++] = ex[j]; } ex2[j] = map[ex[j]]; }
    if (nu > et.nslots) { et.bypass++; return 0; }
    if (l <= et.last_l) {                                   /* novo token: a escala do calor cresce */
        et.tick++; et.grow /= et.decay;
        if (et.grow > 1e100) {
            for (int s = 0; s < et.nslots; s++) et.sl[s].heat *= 1e-100;
            if (et.ghot) { for (int i = 0; i < et.L * et.E; i++) et.ghot[i] *= 1e-100; et.gsum *= 1e-100; }
            et.grow = 1.0;
        }
    }
    et.last_l = l;
    pthread_mutex_lock(&et.mu);
    et.serial++;
    if (et.ghot) for (int u = 0; u < nu; u++) {             /* calor global por especialista (sobrevive ao despejo da vaga) */
        et.ghot[l * E + uniq[u]] += et.grow; et.gsum += et.grow;
    }
    int *slot = xalloc(nu, 4); int *need = xalloc(nu, 4), nneed = 0;
    for (int u = 0; u < nu; u++) {                          /* 1) o que ja esta (ou esta vindo) */
        const int s = et.where[l * E + uniq[u]];
        slot[u] = s;
        if (s >= 0) {
            EtSlot *p = &et.sl[s];
            p->pin = et.serial;
            if (p->state == 1) { if (p->pf) et.pf_late++; }
            else et.hit++;
            if (p->pf) { et.pf_useful++; p->pf = 0; }
        } else need[nneed++] = u;
    }
    int ok = 1;
    for (int i = 0; i < nneed && ok; i++) {                 /* 2) o que falta: despeja e le */
        const int u = need[i], s = et_victim();
        if (s < 0) { ok = 0; break; }
        et_fill(m, s, l, uniq[u], 1);
        et.sl[s].pin = et.serial; slot[u] = s; et.miss++;
    }
    if (!ok) {                                              /* nao coube: espera o que ja pediu e usa o mapeado */
        et.bypass++;
        pthread_mutex_unlock(&et.mu); free(slot); free(need); return 0;
    }
    const double t0 = now_sec();
    int waited = 0;
    for (int u = 0; u < nu; u++) while (et.sl[slot[u]].state != 2) { waited = 1; pthread_cond_wait(&et.cv_done, &et.mu); }
    if (waited) et.t_stall += now_sec() - t0;
    et.clock++;
    for (int u = 0; u < nu; u++) {
        EtSlot *p = &et.sl[slot[u]];
        p->stamp = et.clock; p->heat += et.grow;
    }
    pthread_mutex_unlock(&et.mu);
    const Config *cfg = &m->cfg;
    for (int u = 0; u < nu; u++) {                          /* Mats que apontam para a arena, com o mesmo alinhamento do arquivo */
        eg[u] = L->eg[uniq[u]]; eu[u] = L->eu[uniq[u]]; ed[u] = L->ed[uniq[u]];
        eg[u].q = et_slot_data(slot[u], 0); eu[u].q = et_slot_data(slot[u], 1); ed[u].q = et_slot_data(slot[u], 2);
        if (et.sl[slot[u]].cold) {                          /* vaga com a copia fria: tipo e passo de linha trocados */
            eg[u].type = eu[u].type = ed[u].type = et.cold_type;
            eg[u].stride = eu[u].stride = (size_t)qbytes(et.cold_type, (size_t)cfg->dim);
            ed[u].stride = (size_t)qbytes(et.cold_type, (size_t)cfg->hidden);
        }
    }
    free(slot); free(need);
    Layer tmp = *L; tmp.eg = eg; tmp.eu = eu; tmp.ed = ed;
    et_on = 0;                                              /* a chamada interna usa os Mats trocados */
    moe_cpu_multi(m, &tmp, ex2, xi, n, n_x, xs, outs);
    et_on = 1;
    return 1;
}

/* ---------------- previsao ---------------- */
/* top-n de um vetor de pontuacoes (n pequeno) */
static void et_topn(const float *v, int E, int n, int *out) {
    for (int k = 0; k < n; k++) {
        int b = -1;
        for (int e = 0; e < E; e++) {
            int used = 0;
            for (int i = 0; i < k; i++) if (out[i] == e) { used = 1; break; }
            if (!used && (b < 0 || v[e] > v[b])) b = e;
        }
        out[k] = b;
    }
}
/* roteador da camada lt aplicado ao estado x (normalizado com a norma da propria camada lt) */
static void et_early(Model *m, int lt, const float *x, float *lg) {
    static float *xn; const Config *c = &m->cfg;
    if (!xn) xn = xalloc(c->dim, 4);
    rmsnorm(xn, x, m->layers[lt].ffn_norm, c->dim, c->eps);
    matvec(lg, &m->layers[lt].router, xn, c->n_expert, c->dim);
}
static void et_prefetch(Model *m, int lt, const int *pred, int np) {
    if (lt >= et.L) return;
    pthread_mutex_lock(&et.mu);
    for (int i = 0; i < np; i++) {
        const int e = pred[i];
        if (e < 0 || et.where[lt * et.E + e] >= 0) continue;
        int s = et_victim();
        if (s < 0) break;
        et_fill(m, s, lt, e, 0); et.pf_issued++;
        et.sl[s].stamp = et.clock;
    }
    pthread_mutex_unlock(&et.mu);
}
/* no fim de moe_route da camada l: prever a l + d */
static void et_route(Model *m, const Layer *L, int l) {
    const Config *c = &m->cfg;
    const int E = et.E, K = et.K, lt = l + et.pfd;
    if (et.pred == 3) return;
    const double t0 = now_sec();
    /* aprende a coocorrencia entre a camada anterior e esta, e guarda a escolha */
    if (et.pred == 1 && l > 0) {
        for (int a = 0; a < K; a++) {
            const int pa = et.prev[(size_t)(l - 1) * K + a];
            et.conn[(size_t)(l - 1) * E + pa] += 1.0f;
            for (int b = 0; b < K; b++) et.co[((size_t)(l - 1) * E + pa) * E + moe.sel[b]] += 1.0f;
        }
    }
    for (int k = 0; k < K; k++) et.prev[(size_t)l * K + k] = moe.sel[k];
    pthread_mutex_lock(&et.mu);                             /* o que a camada l vai usar nao pode ser despejado pela previsao */
    et.serial++;
    for (int k = 0; k < K; k++) { const int s = et.where[l * E + moe.sel[k]]; if (s >= 0) et.sl[s].pin = et.serial; }
    pthread_mutex_unlock(&et.mu);
    if (lt >= et.L) return;
    int pred[64]; const int np = et.pfn < 64 ? et.pfn : 64;
    static float *lg, *sc;
    if (!lg) { lg = xalloc(E, 4); sc = xalloc(E, 4); }
    if (et.pred == 0) { et_early(m, lt, m->x, lg); et_topn(lg, E, np, pred); }
    else if (et.pred == 1) {
        memset(sc, 0, E * 4);
        for (int a = 0; a < K; a++) {
            const int pa = moe.sel[a]; const float n = et.conn[(size_t)l * E + pa];
            if (n <= 0 || lt != l + 1) continue;
            const float *row = et.co + ((size_t)l * E + pa) * E;
            for (int b = 0; b < E; b++) sc[b] += row[b] / n;
        }
        et_topn(sc, E, np, pred);
    } else {                                                /* escolha do token anterior na camada alvo */
        for (int i = 0; i < np; i++) pred[i] = i < K ? et.prev[(size_t)lt * K + i] : -1;
    }
    et.t_pred += now_sec() - t0;
    et_prefetch(m, lt, pred, np);
    (void)L; (void)c;
}

static void et_report(void) {
    if (!et_on) return;
    const uint64_t tot = et.hit + et.miss + et.pf_late;
    fprintf(stderr, "[expert_tier] %d vagas de %.2f MB (%.2f GB), %llu chamadas, %llu pedidos: %llu na RAM (%.1f%%), %llu ainda em voo (previsao), %llu lidos na hora, "
            "%llu chamadas pelo mapeado\n", et.nslots, et.slot_bytes / 1048576.0, (double)et.nslots * et.slot_bytes / 1073741824.0,
            (unsigned long long)et.calls, (unsigned long long)tot, (unsigned long long)et.hit, tot ? 100.0 * et.hit / tot : 0.0,
            (unsigned long long)et.pf_late, (unsigned long long)et.miss, (unsigned long long)et.bypass);
    fprintf(stderr, "[expert_tier] previsao: %llu pedidas, %llu uteis (%llu ainda lendo quando o roteador chegou), %llu desperdicadas; "
            "lidos %.2f GB em %llu leituras (%.2f s de thread), espera do token %.2f s, previsor %.2f s\n",
            (unsigned long long)et.pf_issued, (unsigned long long)et.pf_useful, (unsigned long long)et.pf_late,
            (unsigned long long)et.pf_wasted, et.bytes / 1073741824.0, (unsigned long long)et.reads, et.t_read, et.t_stall, et.t_pred);
    if (et.cold_type) {
        const uint64_t totf = et.fetch_hot + et.fetch_cold;
        fprintf(stderr, "[expert_tier] quantizacao mista por calor: copia fria em %s; %llu leituras cheias, %llu frias (%.1f%%)\n",
                qtype_name(et.cold_type), (unsigned long long)et.fetch_hot, (unsigned long long)et.fetch_cold,
                totf ? 100.0 * et.fetch_cold / totf : 0.0);
    }
}

/* liga a arena depois do modelo carregado (chamado no fim de load_model); sem KATANA_EXPERT_RAM nao faz nada */
static void et_init(Model *m, const char *path, const uint8_t *base) {
    et_base = base;
    snprintf(et_path, sizeof et_path, "%s", path);
    const char *ev = getenv("KATANA_EXPERT_RAM");
    if (!ev || atof(ev) <= 0) return;
    const Config *c = &m->cfg;
    if (!c->n_expert) { fprintf(stderr, "expert_tier: modelo sem especialistas, ignorado\n"); return; }
    double gb = atof(ev);
    if (gb > 25.0) { fprintf(stderr, "expert_tier: orcamento limitado a 25 GB\n"); gb = 25.0; }
    memset(&et, 0, sizeof et);
    et.L = c->n_layers; et.E = c->n_expert; et.K = c->n_used;
    for (int l = 0; l < et.L; l++) if (!m->layers[l].eg) { fprintf(stderr, "expert_tier: camada %d sem especialistas empilhados, ignorado\n", l); return; }
    size_t mx = 0;
    for (int l = 0; l < et.L; l++) for (int e = 0; e < et.E; e += (e + 1 < et.E ? et.E - 1 : 1)) {
        size_t off[3], len[3], t = 0;
        if (!et_regions(m, l, e, off, len)) { fprintf(stderr, "expert_tier: especialistas nao quantizados, ignorado\n"); return; }
        for (int r = 0; r < 3; r++) { size_t a, b; et_span(off[r], len[r], &a, &b); t += b - a; }
        if (t > mx) mx = t;
    }
    const char *s;
    et.direct = (s = getenv("KATANA_EXPERT_DIRECT")) ? atoi(s) : 1;
    et.fd = -1;
    if (!et_no_bundle) et.bx = et_bx_open(m, path, et.L, et.E, &et.fd, et.direct, &et.cold_type);
    if (et.bx) { mx = 0; for (int i = 0; i < et.L * et.E; i++) if (et.bx[i].rec > mx) mx = et.bx[i].rec; }
    if (et.bx && et.cold_type) et.ghot = xalloc((size_t)et.L * et.E, sizeof(double));   /* calor global por especialista: hi x fria */
    et.slot_bytes = (mx + ET_PG - 1) & ~(size_t)(ET_PG - 1);
    size_t ns = (size_t)(gb * 1073741824.0 / (double)et.slot_bytes);
    if (ns > (size_t)et.L * et.E) ns = (size_t)et.L * et.E;
    if (ns < 8) { fprintf(stderr, "expert_tier: orcamento pequeno demais\n"); return; }
    et.nslots = (int)ns;
    et.policy = (s = getenv("KATANA_EXPERT_POLICY")) && !strcmp(s, "lru") ? 1 : 0;
    et.decay = (s = getenv("KATANA_EXPERT_DECAY")) ? atof(s) : 0.95; et.grow = 1.0;
    et.pred = (s = getenv("KATANA_EXPERT_PRED")) ? (!strcmp(s, "cooc") ? 1 : !strcmp(s, "prev") ? 2 : !strcmp(s, "none") ? 3 : 0) : 0;
    et.pfn = (s = getenv("KATANA_EXPERT_PFN")) ? atoi(s) : et.K;
    et.pfd = (s = getenv("KATANA_EXPERT_PFD")) ? atoi(s) : 1;
    if (et.pfd < 1) et.pfd = 1;
    if (et.pred == 1) et.pfd = 1;
    et.nthr = (s = getenv("KATANA_EXPERT_IO")) ? atoi(s) : 16;
    if (!et.bx) et.fd = open(path, O_RDONLY | (et.direct ? O_DIRECT : 0));
    if (et.fd < 0) { perror("expert_tier: open"); return; }
    et.arena = mmap(NULL, (size_t)et.nslots * et.slot_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (et.arena == MAP_FAILED) { perror("expert_tier: mmap"); return; }
    et.where = xalloc((size_t)et.L * et.E, 4);
    for (int i = 0; i < et.L * et.E; i++) et.where[i] = -1;
    et.sl = xalloc(et.nslots, sizeof(EtSlot));
    for (int i = 0; i < et.nslots; i++) et.sl[i].key = -1;
    et.qcap = 1 << 16;
    et.q = xalloc(et.qcap, sizeof(EtJob)); et.pq = xalloc(et.qcap, sizeof(EtJob));
    et.prev = xalloc((size_t)et.L * et.K, 4);
    if (et.pred == 1) { et.co = xalloc((size_t)et.L * et.E * et.E, 4); et.conn = xalloc((size_t)et.L * et.E, 4); }
    et.last_l = 1 << 30;
    pthread_mutex_init(&et.mu, NULL); pthread_cond_init(&et.cv_job, NULL); pthread_cond_init(&et.cv_done, NULL);
    for (int i = 0; i < et.nthr; i++) { pthread_t t; pthread_create(&t, NULL, et_worker, NULL); pthread_detach(t); }
    prefetch_on = 0;
    et_hook_route = et_route;
    et_on = 1;
    atexit(et_report);
    fprintf(stderr, "[expert_tier] ligado: %d vagas de %.2f MB = %.2f GB; despejo %s, previsao %s (%d por camada, %d camada(s) a frente), %d threads, %s, %s\n",
            et.nslots, et.slot_bytes / 1048576.0, (double)et.nslots * et.slot_bytes / 1073741824.0, et.policy ? "lru" : "frequencia",
            et.pred == 0 ? "roteador antecipado" : et.pred == 1 ? "coocorrencia" : et.pred == 2 ? "token anterior" : "nenhuma",
            et.pfn, et.pfd, et.nthr, et.direct ? "O_DIRECT" : "com cache",
            et.bx ? "pacote .exb (1 leitura por especialista)" : "GGUF (3 leituras por especialista)");
    if (et.cold_type) fprintf(stderr, "[expert_tier] quantizacao mista: especialistas abaixo da media de uso leem a copia fria (%s) do pacote\n", qtype_name(et.cold_type));
}

/* ====================== medidas: katana tiersim run|sim, katana iobench ======================
 * tiersim run <modelo> <texto> <trace.bin>   roda o texto na CPU (segmentos separados por \f) e grava, por token e
 *                                            camada, os escolhidos e as listas do roteador antecipado
 * tiersim sim <trace.bin>                    caches LRU e frequencia com decaimento, e recall dos previsores */
#define ETS_PD 3                                /* distancias do roteador antecipado: 1, 2, 3 camadas a frente */
static struct { FILE *f; int NP; int16_t *pa, *pb; } ets;
static void ets_clear(void) {
    const int L = et.L, NP = ets.NP;
    for (int i = 0; i < ETS_PD * L * NP; i++) ets.pa[i] = -1;
    for (int i = 0; i < L * NP; i++) ets.pb[i] = -1;
}
static void ets_route(Model *m, const Layer *L, int l) {
    const int E = et.E, K = et.K, NP = ets.NP;
    static float *lg; if (!lg) lg = xalloc(E, 4);
    if (l == 0) ets_clear();
    uint16_t rec[8 + 2 * 32 + 3 * 2 * 32 + 64];
    int n = 0;
    for (int k = 0; k < K; k++) rec[n++] = (uint16_t)moe.sel[k];
    for (int d = 0; d < ETS_PD; d++) for (int i = 0; i < NP; i++) rec[n++] = (uint16_t)ets.pa[((size_t)d * et.L + l) * NP + i];
    for (int i = 0; i < NP; i++) rec[n++] = (uint16_t)ets.pb[(size_t)l * NP + i];
    fwrite(rec, 2, n, ets.f);
    /* preditores para as camadas a frente, a partir do estado desta camada (residual depois da atencao) */
    for (int d = 1; d <= ETS_PD; d++) {
        const int lt = l + d;
        if (lt >= et.L) break;
        int top[32];
        et_early(m, lt, m->x, lg); et_topn(lg, E, NP, top);
        for (int i = 0; i < NP; i++) ets.pa[((size_t)(d - 1) * et.L + lt) * NP + i] = (int16_t)top[i];
    }
    (void)L;
}
static void ets_post(Model *m, int l) {
    const int NP = ets.NP, lt = l + 1;
    static float *lg; if (!lg) lg = xalloc(et.E, 4);
    if (lt >= et.L) return;
    int top[32];
    et_early(m, lt, m->x, lg); et_topn(lg, et.E, NP, top);       /* residual depois do MoE desta camada */
    for (int i = 0; i < NP; i++) ets.pb[(size_t)lt * NP + i] = (int16_t)top[i];
}
static int ets_run(const char *model, const char *text, const char *out) {
    Model m; memset(&m, 0, sizeof m);
    setenv("KATANA_CPU", "1", 1);
    load_model(&m, model);
    const Config *c = &m.cfg;
    if (!c->n_expert) die("modelo sem especialistas");
    et.L = c->n_layers; et.E = c->n_expert; et.K = c->n_used; ets.NP = 2 * et.K;
    if (ets.NP > 32) die("K grande demais para o rastro");
    ets.pa = xalloc((size_t)ETS_PD * et.L * ets.NP, 2); ets.pb = xalloc((size_t)et.L * ets.NP, 2);
    char *txt = read_text(text);
    Ids ids = {0}; int starts[8], nseg = 0;
    for (char *p = txt; p && nseg < 8;) {
        char *q = strchr(p, '\f'); if (q) *q = 0;
        starts[nseg++] = ids.n;
        if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
        encode(&m.tk, p, &ids);
        p = q ? q + 1 : NULL;
    }
    if (ids.n >= c->ctx) { fprintf(stderr, "%d tokens\n", ids.n); die("texto maior que o contexto (KATANA_CTX)"); }
    ets.f = fopen(out, "wb");
    if (!ets.f) die("nao abri o rastro");
    int hdr[16] = { et.L, et.E, et.K, ets.NP, ids.n, nseg, starts[0], starts[1], starts[2], starts[3], starts[4], starts[5], starts[6], starts[7], 0, 0 };
    fwrite(hdr, 4, 16, ets.f);
    et_hook_route = ets_route; et_hook_post = ets_post;
    fprintf(stderr, "%d tokens em %d segmentos, %d camadas x %d especialistas (K=%d)\n", ids.n, nseg, et.L, et.E, et.K);
    const double t0 = now_sec();
    for (int i = 0; i < ids.n; i++) {
        forward(&m, ids.ids[i], i, 0);
        if (i % 100 == 99) {        /* cabecalho sempre valido: um rastro parcial ainda serve */
            fprintf(stderr, "  %d tokens, %.2f tok/s\n", i + 1, (i + 1) / (now_sec() - t0));
            fflush(ets.f); const long pos = ftell(ets.f);
            int nt = i + 1, ns = 0; for (int s = 0; s < nseg; s++) ns += starts[s] < nt;
            fseek(ets.f, 16, SEEK_SET); fwrite(&nt, 4, 1, ets.f); fwrite(&ns, 4, 1, ets.f); fseek(ets.f, pos, SEEK_SET);
        }
    }
    { int nt = ids.n; fseek(ets.f, 16, SEEK_SET); fwrite(&nt, 4, 1, ets.f); fseek(ets.f, 0, SEEK_END); }
    fclose(ets.f);
    return 0;
}

/* ---- analise do rastro ---- */
typedef struct { int L, E, K, NP, T, nseg, st[9]; uint16_t *sel, *pa, *pb; } EtTr;
static EtTr *ets_load(const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) die("nao abri o rastro");
    int h[16];
    if (fread(h, 4, 16, f) != 16) die("rastro curto");
    EtTr *t = xalloc(1, sizeof *t);
    t->L = h[0]; t->E = h[1]; t->K = h[2]; t->NP = h[3]; t->T = h[4]; t->nseg = h[5];
    for (int i = 0; i < t->nseg; i++) t->st[i] = h[6 + i];
    t->st[t->nseg] = t->T;
    const size_t R = (size_t)t->T * t->L;
    t->sel = xalloc(R * t->K, 2); t->pa = xalloc(R * ETS_PD * t->NP, 2); t->pb = xalloc(R * t->NP, 2);
    uint16_t *rec = xalloc(t->K + (ETS_PD + 1) * t->NP, 2);
    for (size_t r = 0; r < R; r++) {
        if (fread(rec, 2, t->K + (ETS_PD + 1) * t->NP, f) != (size_t)(t->K + (ETS_PD + 1) * t->NP)) die("rastro curto");
        memcpy(t->sel + r * t->K, rec, t->K * 2);
        memcpy(t->pa + r * ETS_PD * t->NP, rec + t->K, ETS_PD * t->NP * 2);
        memcpy(t->pb + r * t->NP, rec + t->K + ETS_PD * t->NP, t->NP * 2);
    }
    free(rec); fclose(f);
    return t;
}
static int ets_seg(const EtTr *t, int tok) { int s = 0; while (s + 1 < t->nseg && tok >= t->st[s + 1]) s++; return s; }

/* cache simulado: pol 0 = LRU, 1 = frequencia com decaimento g; shared 1 = um conjunto para todas as camadas */
static void ets_cache(const EtTr *t, int pol, int shared, int pct, double g, double *hit_seg, double *tot_seg, double *hit_first, double *tot_first) {
    const int L = t->L, E = t->E, K = t->K, N = L * E;
    const int cap_all = N * pct / 100, cap_l = E * pct / 100;
    uint8_t *res = calloc(N, 1); uint64_t *stamp = calloc(N, 8), *gid = calloc(N, 8); double *ls = calloc(N, sizeof(double));
    int *cnt = calloc(L, 4), nall = 0;
    uint64_t clock = 0; double grow = 1.0;
    for (int s = 0; s < 8; s++) hit_seg[s] = tot_seg[s] = hit_first[s] = tot_first[s] = 0;
    for (int tk = 0; tk < t->T; tk++) {
        grow /= g; if (grow > 1e100) { for (int i = 0; i < N; i++) ls[i] *= 1e-100; grow = 1.0; }
        const int sg = ets_seg(t, tk), first = tk - t->st[sg] < 100;
        for (int l = 0; l < L; l++) {
            const uint64_t G = (uint64_t)tk * L + l + 1;
            for (int k = 0; k < K; k++) {
                const int id = l * E + t->sel[((size_t)tk * L + l) * K + k];
                clock++;
                tot_seg[sg]++; if (first) tot_first[sg]++;
                if (res[id]) { hit_seg[sg]++; if (first) hit_first[sg]++; }
                else {
                    const int full = shared ? nall >= cap_all : cnt[l] >= cap_l;
                    if (cap_all == 0 || cap_l == 0) goto touch;
                    if (full) {
                        int lo = shared ? 0 : l * E, hi = shared ? N : (l + 1) * E, v = -1;
                        for (int j = lo; j < hi; j++) {
                            if (!res[j] || gid[j] == G) continue;
                            if (v < 0 || (pol == 0 ? stamp[j] < stamp[v] : (ls[j] < ls[v] || (ls[j] == ls[v] && stamp[j] < stamp[v])))) v = j;
                        }
                        if (v < 0) goto touch;
                        res[v] = 0; cnt[v / E]--; nall--;
                    }
                    res[id] = 1; cnt[l]++; nall++; ls[id] = 0;
                }
            touch:
                stamp[id] = clock; gid[id] = G; ls[id] += grow;
            }
        }
    }
    free(res); free(stamp); free(gid); free(ls); free(cnt);
}

static int ets_in(const uint16_t *set, int n, int v) { for (int i = 0; i < n; i++) if (set[i] == v) return 1; return 0; }
static int ets_sim(const char *fn) {
    EtTr *t = ets_load(fn);
    const int L = t->L, E = t->E, K = t->K, NP = t->NP, N = L * E;
    printf("rastro: %d tokens, %d segmentos (inicios", t->T, t->nseg);
    for (int s = 0; s < t->nseg; s++) printf(" %d", t->st[s]);
    printf("), %d camadas x %d especialistas, K=%d\n\n", L, E, K);
    static const int pcts[] = { 10, 15, 25, 50, 75 };
    printf("(a) taxa de acerto do cache (pedidos de especialista atendidos na RAM), partida fria\n");
    printf("%-28s %7s %7s %7s %7s %7s\n", "politica / escopo", "10%", "15%", "25%", "50%", "75%");
    static const struct { const char *nm; int pol, sh; double g; } cfgs[] = {
        { "LRU, conjunto unico", 0, 1, 0 }, { "LRU, por camada", 0, 0, 0 },
        { "freq g=0.95, unico", 1, 1, 0.95 }, { "freq g=0.98, unico", 1, 1, 0.98 }, { "freq g=0.995, unico", 1, 1, 0.995 },
        { "freq g=0.98, por camada", 1, 0, 0.98 }, { "freq g=0.995, por camada", 1, 0, 0.995 } };
    double hs[8], ts[8], hf[8], tf[8];
    double seg25[8][8], first25[8][8]; int nc = (int)(sizeof cfgs / sizeof cfgs[0]);
    for (int ci = 0; ci < nc; ci++) {
        printf("%-28s", cfgs[ci].nm);
        for (int pi = 0; pi < 5; pi++) {
            ets_cache(t, cfgs[ci].pol, cfgs[ci].sh, pcts[pi], cfgs[ci].g, hs, ts, hf, tf);
            double h = 0, tt = 0; for (int s = 0; s < t->nseg; s++) { h += hs[s]; tt += ts[s]; }
            printf(" %6.1f%%", 100 * h / tt);
            if (pcts[pi] == 25) for (int s = 0; s < t->nseg; s++) { seg25[ci][s] = 100 * hs[s] / ts[s]; first25[ci][s] = tf[s] ? 100 * hf[s] / tf[s] : 0; }
        }
        printf("\n"); fflush(stdout);
    }
    printf("\nacerto com 25%% dos especialistas, por segmento (todo o segmento / primeiros 100 tokens dele)\n%-28s", "");
    for (int s = 0; s < t->nseg; s++) printf("   seg %d      ", s + 1);
    printf("\n");
    for (int ci = 0; ci < nc; ci++) {
        printf("%-28s", cfgs[ci].nm);
        for (int s = 0; s < t->nseg; s++) printf(" %5.1f/%5.1f  ", seg25[ci][s], first25[ci][s]);
        printf("\n");
    }
    /* (b) previsores */
    printf("\n(b) recall dos especialistas da camada l prevendo ANTES do roteador dela (media sobre tokens >= 1 e camadas >= 1; N = K, 1.5K, 2K previstos)\n");
    const int ns[3] = { K, K * 3 / 2, 2 * K };
    enum { P_PREV, P_FREQ, P_COOC, P_A1, P_A2, P_A3, P_B1, P_NP };
    static const char *pn[P_NP] = { "token anterior (mesma camada)", "frequencia da camada (calor, g=0.98)", "coocorrencia camada l-1 -> l (online)",
        "roteador antecipado, estado de l-1 (1 a frente)", "roteador antecipado, estado de l-2 (2 a frente)", "roteador antecipado, estado de l-3 (3 a frente)",
        "roteador antecipado, saida de l-1 (depois do MoE)" };
    double rc[P_NP][3] = {{0}}; double rc2[P_NP][3] = {{0}}; double cntr = 0, cnt2 = 0;
    float *co = calloc((size_t)L * E * E, 4), *conn = calloc((size_t)L * E, 4), *fr = calloc((size_t)L * E, 4), *sc = malloc(E * 4);
    const int half = t->T / 2;
    double grow = 1.0;
    for (int tk = 0; tk < t->T; tk++) {
        grow /= 0.98; if (grow > 1e100) { for (int i = 0; i < L * E; i++) fr[i] *= 1e-100; grow = 1.0; }
        for (int l = 0; l < L; l++) {
            const uint16_t *tr = t->sel + ((size_t)tk * L + l) * K;
            if (tk >= 1 && l >= 1) {
                uint16_t pl[P_NP][32];
                /* token anterior: t-1, depois t-2, t-3... ate N */
                int n = 0;
                for (int b = 1; b <= tk && n < 2 * K; b++)
                    for (int k = 0; k < K && n < 2 * K; k++) { const uint16_t v = t->sel[((size_t)(tk - b) * L + l) * K + k]; if (!ets_in(pl[P_PREV], n, v)) pl[P_PREV][n++] = v; }
                for (int e = 0; e < E; e++) sc[e] = fr[(size_t)l * E + e];
                { int top[32]; et_topn(sc, E, 2 * K, top); for (int i = 0; i < 2 * K; i++) pl[P_FREQ][i] = (uint16_t)top[i]; }
                memset(sc, 0, E * 4);
                for (int a = 0; a < K; a++) {
                    const int pa = t->sel[((size_t)tk * L + l - 1) * K + a]; const float nn = conn[(size_t)(l - 1) * E + pa];
                    if (nn <= 0) continue;
                    const float *row = co + ((size_t)(l - 1) * E + pa) * E;
                    for (int b = 0; b < E; b++) sc[b] += row[b] / nn;
                }
                { int top[32]; et_topn(sc, E, 2 * K, top); for (int i = 0; i < 2 * K; i++) pl[P_COOC][i] = (uint16_t)top[i]; }
                for (int d = 0; d < 3; d++) memcpy(pl[P_A1 + d], t->pa + (((size_t)tk * L + l) * ETS_PD + d) * NP, 2 * NP);
                memcpy(pl[P_B1], t->pb + ((size_t)tk * L + l) * NP, 2 * NP);
                for (int p = 0; p < P_NP; p++) {
                    if ((p == P_A2 && l < 2) || (p == P_A3 && l < 3)) continue;
                    for (int ni = 0; ni < 3; ni++) {
                        int hit = 0;
                        for (int k = 0; k < K; k++) hit += ets_in(pl[p], ns[ni], tr[k]);
                        rc[p][ni] += (double)hit / K; if (tk >= half) rc2[p][ni] += (double)hit / K;
                    }
                }
                cntr++; if (tk >= half) cnt2++;
            }
            /* aprende (depois de prever) */
            if (l >= 1) for (int a = 0; a < K; a++) {
                const int pa = t->sel[((size_t)tk * L + l - 1) * K + a];
                conn[(size_t)(l - 1) * E + pa] += 1;
                for (int b = 0; b < K; b++) co[((size_t)(l - 1) * E + pa) * E + tr[b]] += 1;
            }
            for (int k = 0; k < K; k++) fr[(size_t)l * E + tr[k]] += (float)grow;
        }
    }
    printf("%-52s %8s %8s %8s   | %8s %8s %8s\n", "previsor", "N=K", "N=1.5K", "N=2K", "2a metade", "", "");
    for (int p = 0; p < P_NP; p++) {
        printf("%-52s", pn[p]);
        for (int ni = 0; ni < 3; ni++) printf(" %7.1f%%", 100 * rc[p][ni] / cntr);
        printf("   |");
        for (int ni = 0; ni < 3; ni++) printf(" %7.1f%%", 100 * rc2[p][ni] / cnt2);
        printf("\n");
    }
    /* referencia: sorteio */
    printf("%-52s %7.1f%% %7.1f%% %7.1f%%\n", "sorteio", 100.0 * K / E, 150.0 * K / E, 200.0 * K / E);
    (void)N;
    return 0;
}

/* ---- katana iobench: leitura aleatoria de blocos do tamanho de um especialista ---- */
typedef struct { int fd; size_t blk, span; int n; uint64_t seed; double lat; } IobT;
static uint64_t iob_rng(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void *iob_thread(void *a) {
    IobT *t = a;
    void *buf = aligned_alloc(4096, t->blk);
    uint64_t s = t->seed;
    for (int i = 0; i < t->n; i++) {
        const size_t off = (iob_rng(&s) % ((t->span - t->blk) / 4096)) * 4096;
        const double t0 = now_sec();
        if (pread(t->fd, buf, t->blk, (off_t)off) < 0) { perror("iobench pread"); break; }
        t->lat += now_sec() - t0;
    }
    free(buf);
    return NULL;
}
static int iob_sys_setup(unsigned e, struct io_uring_params *p) { return (int)syscall(__NR_io_uring_setup, e, p); }
static int iob_sys_enter(int fd, unsigned sub, unsigned min) { return (int)syscall(__NR_io_uring_enter, fd, sub, min, IORING_ENTER_GETEVENTS, NULL, 0); }
/* io_uring: mantem qd leituras em voo; devolve segundos, ou -1 se o kernel nao deixa */
static double iob_uring(int fd, size_t blk, size_t span, int qd, int total, uint64_t seed) {
    struct io_uring_params p; memset(&p, 0, sizeof p);
    const int rfd = iob_sys_setup(qd < 4 ? 4 : qd, &p);
    if (rfd < 0) return -1;
    const size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned), cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    const size_t msz = sqsz > cqsz ? sqsz : cqsz;
    uint8_t *sq = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_SQ_RING);
    struct io_uring_sqe *sqe = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || sqe == MAP_FAILED) { close(rfd); return -1; }
    uint8_t *cq = (p.features & IORING_FEAT_SINGLE_MMAP) ? sq : mmap(NULL, cqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, rfd, IORING_OFF_CQ_RING);
    unsigned *sqh = (unsigned *)(sq + p.sq_off.head), *sqt = (unsigned *)(sq + p.sq_off.tail), *sqm = (unsigned *)(sq + p.sq_off.ring_mask), *sqa = (unsigned *)(sq + p.sq_off.array);
    unsigned *cqh = (unsigned *)(cq + p.cq_off.head), *cqt = (unsigned *)(cq + p.cq_off.tail), *cqm = (unsigned *)(cq + p.cq_off.ring_mask);
    struct io_uring_cqe *cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    uint8_t *bufs = aligned_alloc(4096, (size_t)qd * blk);
    uint64_t s = seed; int issued = 0, done = 0, infl = 0, err = 0;
    const double t0 = now_sec();
    while (done < total) {
        unsigned n = 0;
        while (infl < qd && issued < total) {
            const unsigned tail = *sqt, idx = tail & *sqm;
            const size_t off = (iob_rng(&s) % ((span - blk) / 4096)) * 4096;
            memset(&sqe[idx], 0, sizeof sqe[idx]);
            sqe[idx].opcode = IORING_OP_READ; sqe[idx].fd = fd; sqe[idx].off = off;
            sqe[idx].addr = (uint64_t)(uintptr_t)(bufs + (size_t)(issued % qd) * blk); sqe[idx].len = (uint32_t)blk;
            sqe[idx].user_data = (uint64_t)issued;
            sqa[idx] = idx;
            __atomic_store_n(sqt, tail + 1, __ATOMIC_RELEASE);
            issued++; infl++; n++;
        }
        if (iob_sys_enter(rfd, n, 1) < 0 && errno != EINTR) { err = 1; break; }
        unsigned head = *cqh;
        while (head != __atomic_load_n(cqt, __ATOMIC_ACQUIRE)) {
            if (cqes[head & *cqm].res < 0) err = 1;
            head++; done++; infl--;
        }
        __atomic_store_n(cqh, head, __ATOMIC_RELEASE);
        if (err) break;
    }
    const double dt = now_sec() - t0;
    (void)sqh; free(bufs); close(rfd);
    return err ? -1 : dt;
}
static int iob_main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "katana iobench <arquivo> [bloco_KB=590] [leituras=1500] [bufferizado=0|1]\n"); return 1; }
    const size_t blk = (size_t)(argc > 3 ? atoi(argv[3]) : 590) * 1024;
    const int total = argc > 4 ? atoi(argv[4]) : 1500, buffered = argc > 5 ? atoi(argv[5]) : 0;
    struct stat st;
    int fd = open(argv[2], O_RDONLY | (buffered ? 0 : O_DIRECT));
    if (fd < 0 || fstat(fd, &st) != 0) { perror("open"); return 1; }
    printf("%s, %s, blocos de %zu KB, %d leituras por ponto\n%-6s %-14s %-12s\n", argv[2], buffered ? "com cache do sistema" : "O_DIRECT", blk / 1024, total, "voo", "threads GB/s", "io_uring GB/s");
    static const int qds[] = { 1, 2, 4, 8, 16, 32, 64 };
    for (int qi = 0; qi < 7; qi++) {
        const int qd = qds[qi];
        pthread_t th[64]; IobT t[64];
        const double t0 = now_sec();
        for (int i = 0; i < qd; i++) {
            t[i] = (IobT){ fd, blk, (size_t)st.st_size, total / qd, 88172645463325252ull + 7919ull * i * 7919ull + qd, 0 };
            pthread_create(&th[i], NULL, iob_thread, &t[i]);
        }
        double lat = 0; for (int i = 0; i < qd; i++) { pthread_join(th[i], NULL); lat += t[i].lat / t[i].n; }
        const double dt = now_sec() - t0, gbs = (double)(total / qd * qd) * blk / dt / 1e9;
        const double du = iob_uring(fd, blk, (size_t)st.st_size, qd, total, 99991u + qd);
        printf("%-6d %6.2f GB/s (%.1f ms/leitura)   ", qd, gbs, 1e3 * lat / qd);
        if (du < 0) printf("indisponivel\n"); else printf("%6.2f GB/s\n", (double)total * blk / du / 1e9);
        fflush(stdout);
    }
    return 0;
}

/* enc_block: app/main.c, incluido depois deste arquivo em katana.c (quantiza em blocos; katana quantize usa pra
   requantizar um tensor inteiro, aqui usamos a mesma funcao pra gravar a copia fria de um especialista) */
static void enc_block(uint32_t t, const float *x, uint8_t *dst, size_t n);

/* ====================== katana bundle: pacote de especialistas ======================
 * bundle <modelo.gguf>                        grava <modelo.gguf>.exb (gate+up+down de cada especialista juntos,
 *                                             e a copia fria de quem tiver: ver KATANA_BUNDLE_COLD acima)
 * bundle check <modelo.gguf> [passo] [scattered] [cold]   le cada especialista (ou um a cada `passo`) pelo caminho
 *                                             real da arena (fila, threads, O_DIRECT); sem `cold` compara a copia
 *                                             hi byte a byte com o GGUF mapeado; com `cold` forca a copia fria e
 *                                             compara com uma requantizacao recalculada agora (nao o que foi
 *                                             gravado), a verdade independente do caminho de escrita
 * bundle bench <modelo.gguf> [rodadas] [scattered]   tempo para trazer K especialistas que nao estao na arena (o custo
 *                                             de uma camada sem acerto); `scattered` ignora o pacote e le do GGUF */
/* bits por peso dos tipos que enc_block sabe gravar; os outros (so leitura) devolvem -1 e ficam de fora da copia
   fria, que so vale a pena quando cold_type tem menos bits que o tipo original */
static double etb_bpw(uint32_t t) {
    switch (t) {
        case T_Q8_0: return 8.5; case T_Q6_K: return 6.56; case T_Q5_K: return 5.5;
        case T_Q4_K: return 4.5; case T_Q2_K: return 2.6; default: return -1;
    }
}
static void etb_load(Model *m, const char *model, int arena) {
    memset(m, 0, sizeof *m);
    setenv("KATANA_CPU", "1", 1);
    if (!arena) unsetenv("KATANA_EXPERT_RAM");
    else if (!getenv("KATANA_EXPERT_RAM")) setenv("KATANA_EXPERT_RAM", "0.25", 1);
    load_model(m, model);
    if (!m->cfg.n_expert) die("modelo sem especialistas");
    if (!et_base) die("modelo em partes: o pacote ainda nao cobre");
    if (arena && !et_on) die("a arena de especialistas nao ligou");
}
static int etb_write(const char *model) {
    Model m; etb_load(&m, model, 0);
    const Config *c = &m.cfg;
    const int L = c->n_layers, E = c->n_expert; const size_t n = (size_t)L * E;
    struct stat sg;
    if (stat(model, &sg) != 0) die("nao li o tamanho do modelo");
    const char *cv = getenv("KATANA_BUNDLE_COLD");
    const uint32_t try_cold = (cv && !strcmp(cv, "0")) ? 0 : T_Q2_K;   /* Q2_K e o unico tipo que enc_block sabe gravar abaixo de Q4_K */
    EtBxHdr h; memset(&h, 0, sizeof h);
    memcpy(h.magic, ET_BX_MAGIC, 8); h.L = (uint32_t)L; h.E = (uint32_t)E; h.gguf_size = (uint64_t)sg.st_size;
    EtBx *bx = xalloc(n, sizeof(EtBx));
    size_t mx = 0, mxc = 0; int n_cold = 0;
    /* passo 1: geometria de cada especialista. hi sempre; fria so quando o tipo original tiver mais bits que
       try_cold (senao requantizar nao reduz nada, so perde qualidade de graca) */
    for (int l = 0; l < L; l++) for (int e = 0; e < E; e++) {
        EtBx *b = &bx[(size_t)l * E + e]; memset(b, 0, sizeof *b);
        size_t off[3], len[3], rel[3];
        if (!m.layers[l].eg || !et_regions(&m, l, e, off, len)) die("especialistas nao quantizados ou nao empilhados");
        const size_t rec = et_bx_layout(off, len, rel);
        if (rec > 0xffffffffu) die("especialista grande demais para o pacote");
        b->rec = (uint32_t)rec;
        for (int r = 0; r < 3; r++) { b->src[r] = off[r]; b->len[r] = (uint32_t)len[r]; b->rel[r] = (uint32_t)rel[r]; }
        if (rec > mx) mx = rec;
        const double hb = etb_bpw(m.layers[l].eg[e].type);
        uint32_t clen[3], crel[3];
        const size_t crec = (try_cold && hb > 0 && etb_bpw(try_cold) < hb - 1e-6) ? et_bx_cold_layout(c, try_cold, clen, crel) : 0;
        if (crec) {
            b->crec = (uint32_t)crec; for (int r = 0; r < 3; r++) { b->clen[r] = clen[r]; b->crel[r] = crel[r]; }
            if (crec > mxc) mxc = crec;
            n_cold++;
        }
    }
    h.cold_type = n_cold ? try_cold : 0;
    /* passo 2: posicoes (hi e fria de cada especialista em sequencia, uma depois da outra) */
    h.data_start = (sizeof h + n * sizeof(EtBx) + ET_PG - 1) & ~(uint64_t)(ET_PG - 1);
    uint64_t pos = h.data_start, hi_total = 0, cold_total = 0;
    for (size_t i = 0; i < n; i++) {
        EtBx *b = &bx[i];
        b->pos = pos; pos += b->rec; hi_total += b->rec;
        if (b->crec) { b->cpos = pos; pos += b->crec; cold_total += b->crec; }
    }
    char fn[1100], tmp[1110]; et_bx_name(fn, sizeof fn, model); snprintf(tmp, sizeof tmp, "%s.tmp", fn);
    FILE *f = fopen(tmp, "wb");
    if (!f) die("nao criei o pacote");
    uint8_t *rec = xalloc(mx > mxc ? mx : mxc, 1), *zero = xalloc(ET_PG, 1);
    float *fx = xalloc((size_t)c->hidden * c->dim, sizeof(float));
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(bx, sizeof(EtBx), n, f) == n;
    const size_t padn = (size_t)(h.data_start - (sizeof h + n * sizeof(EtBx)));
    if (padn) ok = ok && fwrite(zero, 1, padn, f) == padn;
    const double t0 = now_sec();
    const size_t ne = (size_t)c->hidden * c->dim;
    for (int l = 0; l < L && ok; l++) {
        for (int e = 0; e < E && ok; e++) {
            const EtBx *b = &bx[(size_t)l * E + e];
            const Mat *ms[3] = { &m.layers[l].eg[e], &m.layers[l].eu[e], &m.layers[l].ed[e] };
            memset(rec, 0, b->rec);
            for (int r = 0; r < 3; r++) memcpy(rec + b->rel[r], et_base + b->src[r], b->len[r]);
            ok = fwrite(rec, 1, b->rec, f) == b->rec;
            if (ok && b->crec) {        /* copia fria: desquantiza a original e requantiza mais agressivo (katana quantize faz o mesmo com dequant+enc_block) */
                memset(rec, 0, b->crec);
                for (int r = 0; r < 3 && ok; r++) {
                    dequant(ms[r]->type, et_base + b->src[r], fx, ne);
                    enc_block(h.cold_type, fx, rec + b->crel[r], ne);
                }
                ok = fwrite(rec, 1, b->crec, f) == b->crec;
            }
        }
        fprintf(stderr, "\rcamada %d de %d", l + 1, L);
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, fn) != 0) { remove(tmp); die("falha ao gravar o pacote (disco cheio?)"); }
    fprintf(stderr, "\n%s: %d camadas x %d especialistas, %.2f GB na quantizacao original; %d especialistas com copia fria em %s (+%.2f GB); registro hi de ate %.1f KB; %.0f s\n",
            fn, L, E, hi_total / 1073741824.0, n_cold, n_cold ? qtype_name(h.cold_type) : "-", cold_total / 1073741824.0, mx / 1024.0, now_sec() - t0);
    return 0;
}
/* pede o especialista (l, e) a arena como uma chamada de verdade; devolve a vaga (mutex tomado na volta nao) */
static int etb_request(Model *m, int l, int e) {
    int s = et.where[l * et.E + e];
    if (s < 0) { s = et_victim(); if (s < 0) die("arena sem vaga livre"); et_fill(m, s, l, e, 1); }
    et.sl[s].pin = et.serial; et.sl[s].pf = 0;
    return s;
}
static int etb_check(const char *model, int step, int scattered, int cold) {
    Model m; et_no_bundle = scattered; etb_load(&m, model, 1);
    if (cold && !et.cold_type) { fprintf(stderr, "bundle check cold: o pacote nao tem copia fria (refaca com katana bundle)\n"); return 1; }
    et_force_cold = cold; et_force_hot = !cold;
    const int L = et.L, E = et.E;
    if (step < 1) step = 1;
    const size_t ne = (size_t)m.cfg.hidden * m.cfg.dim;
    float *fx = cold ? xalloc(ne, sizeof(float)) : NULL;
    uint8_t *ref = cold ? xalloc(qbytes(et.cold_type, ne), 1) : NULL;
    long nchecked = 0, bad = 0; double bytes = 0;
    const double t0 = now_sec();
    for (int l = 0; l < L; l++) {
        for (int e = l % step; e < E; e += step) {
            if (cold && !et.bx[l * E + e].cpos) continue;   /* este especialista nao tem copia fria: nada a conferir */
            pthread_mutex_lock(&et.mu);
            et.serial++;
            const int s = etb_request(&m, l, e);
            while (et.sl[s].state != 2) pthread_cond_wait(&et.cv_done, &et.mu);
            pthread_mutex_unlock(&et.mu);
            size_t off[3], len[3];
            et_regions(&m, l, e, off, len);
            const Mat *ms[3] = { &m.layers[l].eg[e], &m.layers[l].eu[e], &m.layers[l].ed[e] };
            for (int r = 0; r < 3; r++) {
                const uint8_t *q = et_slot_data(s, r);
                if (!cold) {
                    bytes += (double)len[r];
                    if ((((uintptr_t)q ^ (uintptr_t)ms[r]->q) & 63) || memcmp(q, ms[r]->q, len[r])) {
                        if (bad++ < 10) fprintf(stderr, "\nDIFERENTE: camada %d especialista %d matriz %d\n", l, e, r);
                    }
                } else {    /* requantiza agora, a partir do GGUF mapeado: verdade independente do que foi gravado no pacote */
                    dequant(ms[r]->type, ms[r]->q, fx, ne);
                    enc_block(et.cold_type, fx, ref, ne);
                    const size_t clen = qbytes(et.cold_type, ne);
                    bytes += (double)clen;
                    if (memcmp(q, ref, clen)) { if (bad++ < 10) fprintf(stderr, "\nDIFERENTE (fria): camada %d especialista %d matriz %d\n", l, e, r); }
                }
            }
            nchecked++;
        }
        fprintf(stderr, "\rcamada %d de %d", l + 1, L);
    }
    free(fx); free(ref);
    printf("\n%s%s: %ld especialistas (%.2f GB) lidos pela arena e comparados %s em %.1f s: %s\n",
           et.bx ? "pacote .exb" : "GGUF, 3 regioes", cold ? ", copia fria" : "", nchecked, bytes / 1073741824.0,
           cold ? "com a requantizacao recalculada agora" : "com o GGUF mapeado", now_sec() - t0, bad ? "FALHOU" : "OK, identicos byte a byte");
    if (bad) printf("%ld matrizes diferentes\n", bad);
    et_force_cold = et_force_hot = 0;
    return bad ? 1 : 0;
}
static int etb_bench(const char *model, int rounds, int scattered) {
    Model m; et_no_bundle = scattered; etb_load(&m, model, 1);
    const int L = et.L, E = et.E, K = et.K;
    if (rounds < 1) rounds = 300;
    if (K > 64 || 2 * K > et.nslots) die("arena pequena para o teste");
    uint64_t rs = 88172645463325252ull; double worst = 0, bytes0 = et.bytes; uint64_t reads0 = et.reads;
    const double t0 = now_sec();
    for (int i = 0; i < rounds; i++) {
        const int l = (int)(iob_rng(&rs) % (uint64_t)L);
        int sl[64], ex[64], n = 0;
        while (n < K) {                                 /* K especialistas distintos da camada que nao estao na arena */
            const int e = (int)(iob_rng(&rs) % (uint64_t)E);
            int dup = et.where[l * E + e] >= 0;
            for (int j = 0; j < n; j++) dup |= ex[j] == e;
            if (!dup) ex[n++] = e;
        }
        const double t1 = now_sec();
        pthread_mutex_lock(&et.mu);
        et.serial++;
        for (int j = 0; j < K; j++) sl[j] = etb_request(&m, l, ex[j]);
        for (int j = 0; j < K; j++) while (et.sl[sl[j]].state != 2) pthread_cond_wait(&et.cv_done, &et.mu);
        pthread_mutex_unlock(&et.mu);
        const double dt = now_sec() - t1;
        if (dt > worst) worst = dt;
    }
    const double dt = now_sec() - t0, gb = (et.bytes - bytes0) / 1073741824.0;
    printf("%s, %s, %d threads: %d rodadas de %d especialistas fora da arena: %.2f ms por rodada (pior %.2f ms), %.0f especialistas/s, %.2f GB/s, %llu leituras\n",
           et.bx ? "pacote .exb (1 leitura)" : "GGUF (3 leituras)", et.direct ? "O_DIRECT" : "com cache", et.nthr, rounds, K, 1e3 * dt / rounds, 1e3 * worst,
           (double)rounds * K / dt, gb / dt, (unsigned long long)(et.reads - reads0));
    return 0;
}
static int etb_main(int argc, char **argv) {
    int sc = 0, cold = 0, na = argc;
    while (na > 3 && (!strcmp(argv[na - 1], "scattered") || !strcmp(argv[na - 1], "cold"))) {
        if (!strcmp(argv[na - 1], "scattered")) sc = 1; else cold = 1;
        na--;
    }
    if (na >= 4 && !strcmp(argv[2], "check")) return etb_check(argv[3], na > 4 ? atoi(argv[4]) : 1, sc, cold);
    if (na >= 4 && !strcmp(argv[2], "bench")) return etb_bench(argv[3], na > 4 ? atoi(argv[4]) : 300, sc);
    if (argc == 3) return etb_write(argv[2]);
    fprintf(stderr, "katana bundle <modelo.gguf> | check <modelo.gguf> [passo] [scattered] [cold] | bench <modelo.gguf> [rodadas] [scattered]\n"
                    "  KATANA_BUNDLE_COLD=0 desliga a copia fria ao gravar o pacote (padrao: Q2_K quando a original tiver mais bits)\n");
    return 1;
}

/* despacho dos subcomandos de medida; devolve -1 se o comando nao e nosso */
static int et_cmd(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "tiersim") && !strcmp(argv[2], "run")) return ets_run(argv[3], argv[4], argc > 5 ? argv[5] : "trace.bin");
    if (argc >= 4 && !strcmp(argv[1], "tiersim") && !strcmp(argv[2], "sim")) return ets_sim(argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "iobench")) return iob_main(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bundle")) return etb_main(argc, argv);
    return -1;
}

#else   /* sem Linux: sem arena de RAM nem medidas */
static int et_wrap(Model *m, const Layer *L, const int *ex, const int *xi, int n, int n_x, const float *xs, float *outs) { return 0; }
static void et_init(Model *m, const char *path, const uint8_t *base) { (void)m; (void)path; (void)base; }
static int et_cmd(int argc, char **argv) { (void)argc; (void)argv; return -1; }
#endif
