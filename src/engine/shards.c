/* ---------- SHA-256 (enderecamento e verificacao dos shards) ---------- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; } Sha;

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25), ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
        const uint32_t t1 = a[7] + S1 + ch + SHA_K[i] + w[i];
        const uint32_t S0 = ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22), mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + S0 + mj;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_hex(const void *data, size_t n, char *hex) {
    Sha s = { { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }, 0, { 0 }, 0 };
    const uint8_t *p = data;
    uint8_t last[128] = { 0 };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
    const size_t r = n - i;
    memcpy(last, p + i, r);
    last[r] = 0x80;
    const size_t tot = r + 9 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) last[tot - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha_block(&s, last);
    if (tot == 128) sha_block(&s, last + 64);
    for (int k = 0; k < 8; k++) snprintf(hex + 8 * k, 9, "%08x", s.h[k]);
}

/* ---------- shards: buffers ja no layout da GPU, um arquivo cada, com hash no manifesto ----------
 * <modelo>.gguf.ktn/manifest.txt + <nome>.bin. `katana pack` gera, a carga usa se existir
 * (pula a conversao do GGUF) e `katana verify` confere os hashes. E a unidade que uma rede
 * p2p distribuiria: cada shard e autocontido e verificavel pelo hash. */
typedef struct { char name[32]; size_t bytes; char hex[65]; } Shard;
static struct { char dir[1024]; int use, n, cap; Shard *e; } shards;

static void shards_open(const char *path, int64_t fsz) {
    char mf[1100], line[256];
    long long origem = -1;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    if (getenv("KATANA_NOSHARDS")) return;
    snprintf(mf, sizeof mf, "%s/manifest.txt", shards.dir);
    FILE *f = fopen(mf, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Shard e; unsigned long long b;
        if (sscanf(line, "origem %lld", &origem) == 1) continue;
        if (sscanf(line, "%64s %llu %31s", e.hex, &b, e.name) == 3 && strlen(e.hex) == 64) {
            if (shards.n == shards.cap) { shards.cap = shards.cap ? shards.cap * 2 : 256; shards.e = realloc(shards.e, shards.cap * sizeof(Shard)); }
            e.bytes = (size_t)b;
            shards.e[shards.n++] = e;
        }
    }
    fclose(f);
    shards.use = origem == (long long)fsz && shards.n > 0;
    if (!shards.use) shards.n = 0;
}

/* le um shard inteiro; NULL se nao existir ou o tamanho nao bater */
static uint32_t *shard_read(const char *name, size_t bytes) {
    char fn[1100];
    int found = 0;
    for (int i = 0; i < shards.n; i++) if (strcmp(shards.e[i].name, name) == 0 && shards.e[i].bytes == bytes) found = 1;
    if (!found) return NULL;
    snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
    FILE *f = fopen(fn, "rb");
    if (!f) return NULL;
    uint32_t *buf = malloc(bytes);
    if (buf && fread(buf, 1, bytes, f) != bytes) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

typedef struct { Mat **ms; int n; size_t rows, cols, wpr; uint32_t *buf; } PackJob;
static void pack_job(void *c, int g0, int g1) {
    const PackJob *j = c;
    uint32_t *tmp = malloc(j->wpr * 4);
    if (!tmp) die("sem memoria");
    for (int g = g0; g < g1; g++) {
        const Mat *W = j->ms[g % j->n];
        const size_t r0 = (size_t)(g / j->n) * 32;
        uint32_t *base = j->buf + (size_t)g * j->wpr * 32;
        for (size_t k = 0; k < 32 && r0 + k < j->rows; k++) {
            gpu_pack_row(W, r0 + k, j->cols, tmp);
            for (size_t w = 0; w < j->wpr; w++) base[w * 32 + k] = tmp[w];
        }
    }
    free(tmp);
}

/* monta o buffer de n matrizes de mesmo formato: layout entrelacado, intercaladas por grupo de 32 linhas */
static uint32_t *gpu_build_buf(Mat **ms, int n, size_t rows, size_t cols, size_t *bytes_out) {
    const size_t wpr = gpu_wpr(ms[0], cols), rowsP = (rows + 127) & ~(size_t)127;
    uint32_t *buf = calloc(rowsP * wpr * n, 4);
    *bytes_out = rowsP * wpr * 4 * n;
    if (!buf) return NULL;
    PackJob job = { ms, n, rows, cols, wpr, buf };
    pool_for(pack_job, &job, (int)(rowsP / 32) * n, 4);
    return buf;
}

/* envia n matrizes para um unico buffer na VRAM (do shard, se houver; senao convertendo o GGUF) */
#ifdef _WIN32
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif
/* estado do cache de especialistas (MoE) na VRAM; explicado junto de expert_ensure */
typedef struct { uint32_t *glu, *down; int slot, pending; } ExpSlot;  /* blobs mornos (RAM), vaga na VRAM (-1 = fora), pedido em curso */
/* fila circular de cargas em segundo plano: a thread carregadora le (ou converte) o especialista
   para o buffer do pedido; a thread principal instala na VRAM quando fica pronto */
#define LQ 64
#define MOE_KMAX 64     /* especialistas ativos por token, no maximo */
typedef struct { int l, e; uint8_t *buf; volatile long done; } LoadReq;
static struct {
    clh bqn, bkn, *qn, *kn;         /* norma de q e k */
    clh *sg, *sd;                   /* por camada: vagas dos pares gate/up e dos down */
    clh bh, bt;                     /* saidas dos especialistas do lote */
    clh bsh, bso; float *hout;      /* modo hibrido: meio e saida do compartilhado; saidas lidas da GPU */
    clh bmeta, bxn;                 /* metadados do lote (faixas, pesos, linhas) e entradas por token */
    uint32_t *meta; float *w; uint32_t *rowi; float *xn;
    ExpSlot *slot;                  /* [camada][especialista] */
    int *owner; uint64_t *stamp;    /* [camada][vaga]: quem ocupa e quando foi usado */
    int on, S;                      /* vagas por camada */
    size_t *bg, *bd;                /* por camada: bytes de um par gate/up e de um down */
    LoadReq rq[LQ];
    volatile long head;             /* proximo pedido a criar (thread principal) */
    long fin, fin_safe;             /* proximo a instalar; ate onde as copias ja terminaram na GPU */
    int loader;                     /* thread carregadora ligada */
    uint32_t *pre; int n_pre, i_pre;        /* especialistas quentes da sessao anterior, a pre-carregar */
    float swap_margin, decay;     /* decay: fator do calor por token (KATANA_HEAT_DECAY, padrao 0,98) */
    Model *m;
    FILE **pf;                      /* por camada: arquivo com os especialistas ja no layout da GPU (pack) */
    uint8_t *stage;                 /* um especialista lido do arquivo, a caminho da VRAM */
    float *heat;                    /* [camada][especialista]: uso recente (decai a cada token) */
    float *cpu_out;                 /* saidas dos especialistas que rodaram na CPU neste token */
    int cpu_cold;                   /* especialista frio roda na CPU ate esquentar */
    uint64_t clock, hits, miss, evict, conv, cpu_runs, loads;
    double t_up, t_cpu, t_read;
} gx;
static void loader_start(Model *m, size_t maxper);
static void heat_load(const Model *m);
static void preload_feed(const Model *m);
static void moe_tables(Model *m, size_t S, size_t per);
static KvkBuf *vk_sg, *vk_sd;
static int gx_host_layer(int l);       /* Vulkan: por camada, buffer com as vagas dos pares gate/up e dos down */
static void exp_write(int l, int v, const void *g, const void *dn);

static void *vk_wbuf(const uint32_t *host, size_t bytes);     /* Vulkan: buffer de pesos na VRAM */
static void gpu_upload_n(Mat **ms, int n, size_t rows, size_t cols, const char *name) {
    size_t bytes = ((rows + 127) & ~(size_t)127) * gpu_wpr(ms[0], cols) * 4 * n;
    gpu.n_total += n;
    if (!gpu_has(ms[0])) return;        /* tipo sem kernel de GPU: fica na CPU */
    if (gpu.used + bytes > gpu_budget()) return;
    uint32_t *buf = shards.use ? shard_read(name, bytes) : NULL;
    if (buf) gpu.n_shard++;
    else buf = gpu_build_buf(ms, n, rows, cols, &bytes);
    if (!buf) return;
    int32_t err = 0;
    clh b;
    if (gpu.vk) { b = vk_wbuf(buf, bytes); free(buf); if (!b) return; }
    else {
        b = cl.CreateBuffer(gpu.ctx, 4 | 32 /* READ_ONLY | COPY_HOST_PTR */, bytes, buf, &err);
        free(buf);
        if (err || !b) return;
    }
    for (int i = 0; i < n; i++) { ms[i]->gbuf = b; ms[i]->gs = n; ms[i]->go = i; }
    gpu.used += bytes; gpu.n_up += n;
}

static void gpu_upload(Mat *W, size_t rows, size_t cols, const char *name) { gpu_upload_n(&W, 1, rows, cols, name); }

/* gate e up tem o mesmo tipo? entao vao intercalados num buffer so (um kernel faz o SwiGLU) */
static int glu_pair(const Mat *gate, const Mat *up) {
    return (gate->q != NULL) == (up->q != NULL) && gate->type == up->type && !getenv("KATANA_NOPAIR");
}

static void gpu_upload_glu(Mat *gate, Mat *up, size_t rows, size_t cols, int l) {
    char nm[32];
    if (glu_pair(gate, up)) {
        Mat *ms[2] = { gate, up };
        snprintf(nm, sizeof nm, "L%03d.glu", l);
        gpu_upload_n(ms, 2, rows, cols, nm);
        gate->pair = gate->gbuf != NULL;
    } else {
        snprintf(nm, sizeof nm, "L%03d.gate", l); gpu_upload(gate, rows, cols, nm);
        snprintf(nm, sizeof nm, "L%03d.up", l);   gpu_upload(up, rows, cols, nm);
    }
}

/* MLA (deepseek2): ainda sem grafo na GPU. As camadas rodam na CPU e os produtos densos (q, kv_a,
   saida da atencao, feed-forward denso, compartilhados e vocabulario) vao para a GPU com ida e volta;
   os especialistas roteados e a atencao ficam na CPU. */
static int host_moe_setup(Model *m);
static int vk_host_moe_setup(Model *m, size_t S, size_t per, size_t work);
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols);
static int vk_mla_build(Model *m);
static int vk_g4_build(Model *m);
static int vk_qd_build(Model *m);
static struct {
    int ok, P;
    KvkBuf *an, *fn, *dg;           /* normas por camada; par gate/up das camadas densas */
    float *Qh, *Kh, *Q, *OT;
    int gatt;                       /* atencao na GPU (mla_att.comp); 0 = na CPU */
    KvkBuf qb, cb, wk, wv; KvkPipe att;
    double t_attn, t_rd; long passes;
} vkml;
static void mla_gpu_upload(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, nh = c->n_heads, W = (size_t)c->mla + c->rope_dim;
    char nm[32];
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        if (c->g4) {        /* gemma4: atencao (largura por camada), feed-forward denso; roteador e normas na CPU.
                               Hd e por camada (L->hidden_l): c->hidden_dense e so o maior entre todas (E2B/E4B tem
                               MLP duplo-largo nas camadas finais), subir com ele uma camada mais estreita le lixo. */
            const size_t hq = nh * L->hd_l, hk = (size_t)L->nkv_l * L->hd_l, Hd = (size_t)L->hidden_l;
            snprintf(nm, sizeof nm, "L%03d.wq", l); gpu_upload(&L->wq, hq, d, nm);
            if (!L->kv_shared) {    /* camada sem K/V proprio (kv_shared_n): wk/wv ficam zerados, nada para subir */
                snprintf(nm, sizeof nm, "L%03d.wk", l); gpu_upload(&L->wk, hk, d, nm);
                if (L->wv.q || L->wv.f) { snprintf(nm, sizeof nm, "L%03d.wv", l); gpu_upload(&L->wv, hk, d, nm); }
            }
            snprintf(nm, sizeof nm, "L%03d.wo", l); gpu_upload(&L->wo, d, hq, nm);
            snprintf(nm, sizeof nm, "L%03d.gate", l); gpu_upload(&L->w_gate, Hd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.up", l);   gpu_upload(&L->w_up, Hd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, Hd, nm);
            continue;
        }
        if (c->ssm_inner && L->lin) {       /* DeltaNet: qkv, portao z e saida (alpha/beta sao float, ficam na CPU) */
            const size_t C = 2 * (size_t)c->ssm_groups * c->ssm_state + c->ssm_inner;
            snprintf(nm, sizeof nm, "L%03d.qkv", l);  gpu_upload(&L->wqkv, C, d, nm);
            snprintf(nm, sizeof nm, "L%03d.z", l);    gpu_upload(&L->wz, c->ssm_inner, d, nm);
            snprintf(nm, sizeof nm, "L%03d.sout", l); gpu_upload(&L->wssm_out, d, c->ssm_inner, nm);
        } else if (c->ssm_inner) {          /* atencao com portao */
            const size_t kvd = (size_t)c->n_kv * c->hd;
            snprintf(nm, sizeof nm, "L%03d.wq", l); gpu_upload(&L->wq, 2 * nh * c->hd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.wk", l); gpu_upload(&L->wk, kvd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.wv", l); gpu_upload(&L->wv, kvd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.wo", l); gpu_upload(&L->wo, d, nh * c->hd, nm);
        } else {
            if (L->wq.q) { snprintf(nm, sizeof nm, "L%03d.wq", l);  gpu_upload(&L->wq, nh * c->hd, d, nm); }
            snprintf(nm, sizeof nm, "L%03d.kva", l); gpu_upload(&L->wkv_a, W, d, nm);
            snprintf(nm, sizeof nm, "L%03d.wo", l);  gpu_upload(&L->wo, d, nh * c->hd_v, nm);
        }
        if (!L->eg) {
            const size_t Hd = c->n_expert ? (size_t)c->hidden_dense : H;
            snprintf(nm, sizeof nm, "L%03d.gate", l); gpu_upload(&L->w_gate, Hd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.up", l);   gpu_upload(&L->w_up, Hd, d, nm);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, Hd, nm);
        } else if (L->sg.q || L->sg.f) {
            const size_t Hs = (size_t)c->n_shared * H;
            if (glu_pair(&L->sg, &L->su)) {
                Mat *ms[2] = { &L->sg, &L->su };
                snprintf(nm, sizeof nm, "L%03d.sglu", l); gpu_upload_n(ms, 2, Hs, d, nm);
                L->sg.pair = L->sg.gbuf != NULL;
            } else {
                snprintf(nm, sizeof nm, "L%03d.sgate", l); gpu_upload(&L->sg, Hs, d, nm);
                snprintf(nm, sizeof nm, "L%03d.sup", l);   gpu_upload(&L->su, Hs, d, nm);
            }
            snprintf(nm, sizeof nm, "L%03d.sdown", l); gpu_upload(&L->sd, d, Hs, nm);
        }
    }
    gpu_upload(&m->lm_head, c->vocab, d, "head");
    gpu.n_gl = 0; gpu.on = 0; gpu.full = 0; gpu.mv_host = 1;
    fprintf(stderr, "GPU: %s%s (%.1f GB) - %s: camadas na CPU, %d produtos densos na GPU (%.0f MB usados)\n",
            gpu.vk ? "Vulkan " : "", gpu.name, gpu.vram / 1073741824.0, c->mla ? "MLA" : c->g4 ? "gemma4" : "DeltaNet", gpu.n_up, gpu.used / 1048576.0);
    if (gpu.vk && c->mla) vk_mla_build(m);     /* prompt em lote: antes das vagas, que ficam com o resto da VRAM */
    if (c->n_expert && !host_moe_setup(m) && !gpu.vk) fprintf(stderr, "GPU: especialistas roteados na CPU\n");
    if (gpu.vk && c->g4) vk_g4_build(m);
    if (gpu.vk && c->ssm_inner && !c->n_expert) vk_qd_build(m);
    if (vkml.ok && !vk_sg) {    /* o lote MLA usa as vagas (vk_moe_n); sem cache de especialistas (VRAM ocupada, KATANA_NOEXPCACHE) fica token a token */
        vkml.ok = 0;
        fprintf(stderr, "Vulkan: sem cache de especialistas: prompt MLA token a token\n");
    }
}

/* Cache de especialistas no modo hibrido (camadas na CPU, ex.: MLA): so as camadas MoE tem vagas; as densas
   iniciais e os compartilhados ja subiram como matrizes normais. O resto da VRAM vira vagas, como no grafo
   completo. 0 = nao coube (os especialistas roteados ficam na CPU). KATANA_NOEXPCACHE=1 desliga. */
static int host_moe_setup(Model *m) {
    const Config *c = &m->cfg;
    const int NL = c->n_layers, E = c->n_expert, K = c->n_used;
    const size_t d = c->dim, H = c->hidden, Hs = (size_t)c->n_shared * H;
    if (getenv("KATANA_NOEXPCACHE") || getenv("KATANA_NOGX")) return 0;
    if (gpu.vk && c->ssm_inner) return 0;      /* qwen35moe: o grafo Vulkan proprio (vkq_setup) cria as vagas */
    gx.bg = xalloc(NL, sizeof(size_t)); gx.bd = xalloc(NL, sizeof(size_t));
    size_t per = 0;
    for (int l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        if (!L->eg) continue;
        if (!glu_pair(&L->eg[0], &L->eu[0]) || !gpu_has(&L->eg[0]) || !gpu_has(&L->ed[0])) { fprintf(stderr, "GPU: gate e up dos especialistas em formatos diferentes\n"); return 0; }
        gx.bg[l] = 2 * gpu_mat_bytes(&L->eg[0], H, d); gx.bd[l] = gpu_mat_bytes(&L->ed[0], d, H);
        per += gx.bg[l] + gx.bd[l];
    }
    if (!per) return 0;
    const size_t work = ((size_t)K * (H + 128) + (size_t)K * (d + 128) + (size_t)K * 8 + Hs + 128 + d + 128) * 4;
    const uint64_t budget = gpu_budget(), keep = work + ((size_t)8 << 20);
    size_t avail = budget > gpu.used + keep ? (size_t)(budget - gpu.used - keep) : 0;
    if (getenv("KATANA_EXPERT_MB")) { const size_t lim = (size_t)atoll(getenv("KATANA_EXPERT_MB")) << 20; if (lim < avail) avail = lim; }
    size_t S = avail / per;
    if (S > (size_t)E) S = E;
    if (S < (size_t)K) {
        /* Falta pouco para fechar as K vagas (ex.: gemma4 cabe 7, precisa de 8): o orcamento
           padrao e conservador. Tenta um teto de VRAM um pouco mais alto, ainda seguro (abaixo
           do limite que estoura a alocacao), so o suficiente para as K vagas. So atua neste caso
           de quase-encaixe; os modelos que ja cabem no orcamento padrao nao sao tocados. */
        const int hpct = getenv("KATANA_VRAM_PCT_HYBRID") ? atoi(getenv("KATANA_VRAM_PCT_HYBRID")) : 91;
        const uint64_t hb = gpu.vram / 100 * (uint64_t)hpct;
        size_t avail2 = hb > gpu.used + keep ? (size_t)(hb - gpu.used - keep) : 0;
        if (getenv("KATANA_EXPERT_MB")) { const size_t lim = (size_t)atoll(getenv("KATANA_EXPERT_MB")) << 20; if (lim < avail2) avail2 = lim; }
        size_t S2 = avail2 / per;
        if (S2 > (size_t)E) S2 = E;
        if (S2 >= (size_t)K) { fprintf(stderr, "GPU: cache de especialistas usa ate %d%% da VRAM para caber %d vagas (falta pouco no padrao)\n", hpct, K); S = S2; }
    }
    if (S < (size_t)K) { fprintf(stderr, "GPU: sobra VRAM para %zu vagas por camada, precisa de %d\n", S, K); return 0; }
    if (gpu.vk) return vk_host_moe_setup(m, S, per, work);
    int32_t e1 = 0, e2 = 0, e3 = 0, e4 = 0, e5 = 0, e6 = 0, e7 = 0;
    gx.sg = xalloc(NL, sizeof(clh)); gx.sd = xalloc(NL, sizeof(clh));
    for (int l = 0; l < NL && !e1 && !e2; l++) {
        if (!m->layers[l].eg) continue;
        gx.sg[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bg[l], NULL, &e1);
        if (!e1) gx.sd[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bd[l], NULL, &e2);
    }
    gx.bh = cl.CreateBuffer(gpu.ctx, 1, (size_t)K * (H + 128) * 4, NULL, &e3);
    gx.bt = cl.CreateBuffer(gpu.ctx, 1, (size_t)K * (d + 128) * 4, NULL, &e4);
    gx.bmeta = cl.CreateBuffer(gpu.ctx, 1, (size_t)K * 8 * 4, NULL, &e5);
    gx.bsh = cl.CreateBuffer(gpu.ctx, 1, (Hs + 128) * 4, NULL, &e6);
    gx.bso = cl.CreateBuffer(gpu.ctx, 1, (d + 128) * 4, NULL, &e7);
    if (!e1 && !e2 && !getenv("KATANA_NOTOUCH")) {     /* reserva a memoria agora, fora do primeiro token */
        const uint32_t zero = 0;
        for (int l = 0; l < NL; l++) if (gx.sg[l]) {
            cl.EnqueueFillBuffer(gpu.queue, gx.sg[l], &zero, 4, 0, S * gx.bg[l], 0, NULL, NULL);
            cl.EnqueueFillBuffer(gpu.queue, gx.sd[l], &zero, 4, 0, S * gx.bd[l], 0, NULL, NULL);
        }
        e1 = cl.Finish(gpu.queue);
    }
    if (e1 || e2 || e3 || e4 || e5 || e6 || e7) {
        clh *all[5] = { &gx.bh, &gx.bt, &gx.bmeta, &gx.bsh, &gx.bso };
        for (int l = 0; l < NL; l++) {
            if (gx.sg[l]) cl.ReleaseMemObject(gx.sg[l]);
            if (gx.sd[l]) cl.ReleaseMemObject(gx.sd[l]);
        }
        for (int i = 0; i < 5; i++) { if (*all[i]) cl.ReleaseMemObject(*all[i]); *all[i] = NULL; }
        free(gx.sg); free(gx.sd); gx.sg = gx.sd = NULL;
        fprintf(stderr, "GPU: sem memoria para o cache de especialistas\n");
        return 0;
    }
    gpu.used += S * per + work;
    gx.S = (int)S;
    gx.meta = xalloc((size_t)K * 8, sizeof(uint32_t)); gx.w = xalloc(K, sizeof(float)); gx.rowi = xalloc(K, sizeof(uint32_t));
    gx.xn = xalloc(d, sizeof(float)); gx.hout = xalloc((size_t)K * d, sizeof(float));
    moe_tables(m, S, per);
    return 1;
}

/* buffer de leitura/escrita na VRAM; NULL se nao couber */
static clh gbuf_rw(size_t bytes) {
    int32_t err = 0;
    if (gpu.used + bytes > gpu_budget()) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 1, bytes, NULL, &err);
    if (err) return NULL;
    gpu.used += bytes;
    return b;
}
/* vetor float32 constante (normas, bias); NULL se src for NULL */
static clh gbuf_f(const float *src, size_t n, int *fail) {
    int32_t err = 0;
    if (!src) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 4 | 32, n * sizeof(float), (void *)src, &err);
    if (err || !b) { *fail = 1; return NULL; }
    gpu.used += n * sizeof(float);
    return b;
}

/* quantos segmentos de colunas (divisor de nb, ate 8 ou 4) usar para `threads` threads */
static int gpu_nseg(size_t threads, int nb, int lote_x_linhas) {
    const int maxseg = lote_x_linhas > 32 ? 4 : 8;      /* igual ao MAXSEG dos kernels */
    int best = 1;
    if (!gpu.seg_target || nb <= 1 || threads >= gpu.seg_target) return 1;
    for (int sg = 1; sg <= maxseg && sg <= nb; sg++) if (nb % sg == 0) {
        best = sg;
        if (threads * sg >= gpu.seg_target) break;
    }
    return best;
}


/* ================= backend Vulkan (so o produto matriz x vetor; o grafo ainda roda na CPU) ================= */

/* um buffer por matriz (ou por par gate/up); o handle fica em Mat.gbuf como ponteiro para KvkBuf */
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols);
static void *vk_wbuf(const uint32_t *host, size_t bytes) {
    KvkBuf b = kvk_buf_create(bytes, KVK_USAGE_RW);
    if (!b.b) { fprintf(stderr, "Vulkan: buffer de %.1f MB falhou: %s\n", bytes / 1048576.0, kvk_error()); return NULL; }
    if (!kvk_buf_write(b, 0, host, bytes)) { fprintf(stderr, "Vulkan: envio falhou: %s\n", kvk_error()); kvk_buf_destroy(&b); return NULL; }
    KvkBuf *p = xalloc(1, sizeof(KvkBuf));
    *p = b;
    gpu.vk_nbuf++;
    return p;
}

static const uint32_t *vk_spv[7]; static size_t vk_spv_n[7];
static KvkPipe vk_pipe[7][9];      /* [tipo][nseg]: criados sob demanda */

static int vk_kind_idx(uint32_t kind) { return kind == T_F32 ? 0 : kind == T_Q8_0 ? 1 : kind == T_Q4_K ? 2 : kind == T_Q5_K ? 3 : kind == T_Q2_K ? 5 : kind == T_Q5_1 ? 6 : 4; }

static KvkPipe vk_pipe2[7][9];     /* mesmos kernels com NR=2 (par gate/up) */
static int vk_nmat, vk_mx, vk_mo, vk_mcol;     /* modo de varias matrizes (vk_gmv_multi) */

/* enfileira out = W * x (um token) sem esperar; mesmo layout, gs/go e nseg dos kernels OpenCL.
   mode: 0 grava, 1 soma, 2 SwiGLU com out, bit 4 = soma bias (buffer `bias`), pair = gate/up intercalados (grava silu(g)*u) */
static int vk_gmv_raw(uint32_t kind, KvkBuf wb, KvkBuf x, KvkBuf out, KvkBuf bias, int rows, int cols, int mode, int pair, int gs, int go, int woff, int ooff) {
    if (!vk_spv[0]) {
        vk_spv[0] = mv_f32_spv; vk_spv_n[0] = sizeof mv_f32_spv; vk_spv[1] = mv_q8_spv; vk_spv_n[1] = sizeof mv_q8_spv;
        vk_spv[2] = mv_q4k_spv; vk_spv_n[2] = sizeof mv_q4k_spv; vk_spv[3] = mv_q5k_spv; vk_spv_n[3] = sizeof mv_q5k_spv;
        vk_spv[4] = mv_q6k_spv; vk_spv_n[4] = sizeof mv_q6k_spv;
        vk_spv[5] = mv_q2k_spv; vk_spv_n[5] = sizeof mv_q2k_spv;
        vk_spv[6] = mv_q51_spv; vk_spv_n[6] = sizeof mv_q51_spv;
    }
    const int ki = vk_kind_idx(kind);
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127;
    const int nb = kind == T_F32 ? 1 : (kind == T_Q8_0 || kind == T_Q5_1) ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(rowsP, nb, pair ? 2 : 1);
    KvkPipe *pp = pair ? &vk_pipe2[ki][nseg] : &vk_pipe[ki][nseg];
    if (!pp->p) {
        const uint32_t ids[2] = { 0, 1 }, vals[2] = { pair ? 2u : 1u, (uint32_t)nseg };
        *pp = kvk_pipe_create(vk_spv[ki], vk_spv_n[ki], 4, 44, ids, vals, 2);
        if (!pp->p) { fprintf(stderr, "Vulkan: pipeline falhou: %s\n", kvk_error()); return 0; }
    }
    const KvkBuf bufs[4] = { wb, x, out, bias };
    /* woff: deslocamento em palavras dentro do buffer de pesos (vagas dos especialistas); ooff: deslocamento da saida */
    /* nmat > 0: nmat matrizes num lancamento; a tabela (6 uint por matriz) vem em `bias` (ver mv_common.glsl) */
    const int nm = vk_nmat > 0 ? vk_nmat : 0;
    const int act = g_gelu ? 16 : 0;     /* bit 16: GELU (tanh) em vez de SiLU no par gate/up e no modo 2 */
    const int pc[11] = { cols, (pair ? (mode | 8) : mode) | act, rows, gs, go, woff, ooff, nm ? (int)(rowsP / 32) : 0, vk_mx, vk_mo, vk_mcol };
    return kvk_dispatch(*pp, bufs, 4, pc, sizeof pc, (uint32_t)(rowsP / 32) * (uint32_t)(nm ? nm : 1), 1, 1);
}

/* varias matrizes (vagas de especialistas) num lancamento: tabela em meta, colunas mcol..mcol+2 */
static int vk_gmv_multi(uint32_t kind, KvkBuf wb, KvkBuf x, KvkBuf out, KvkBuf meta, int rows, int cols, int pair, int nmat, int mxrow, int morow, int mcol) {
    vk_nmat = nmat; vk_mx = mxrow; vk_mo = morow; vk_mcol = mcol;
    const int ok = vk_gmv_raw(kind, wb, x, out, meta, rows, cols, 0, pair, 1, 0, 0, 0);
    vk_nmat = 0;
    return ok;
}

static int vk_gmv_ex(const Mat *W, KvkBuf x, KvkBuf out, KvkBuf bias, int rows, int cols, int mode, int pair) {
    return vk_gmv_raw(gpu_kind(W), *(const KvkBuf *)W->gbuf, x, out, bias, rows, cols, mode, pair, pair ? 1 : W->gs, pair ? 0 : W->go, 0, 0);
}

static int vk_gmv(const Mat *W, KvkBuf x, KvkBuf out, int rows, int cols, int mode) {
    return vk_gmv_ex(W, x, out, x, rows, cols, mode, 0);
}

/* le k buffers com uma espera so; KATANA_VK_READV=0: um de cada vez (conferencia) */
static int vk_readv(const KvkBuf *b, void *const *dst, const size_t *n, int k) {
    static int on = -1;
    if (on < 0) on = getenv("KATANA_VK_READV") ? atoi(getenv("KATANA_VK_READV")) != 0 : 1;
    if (on) return kvk_buf_readv(b, dst, n, k);
    for (int i = 0; i < k; i++) if (!kvk_buf_read(b[i], 0, dst[i], n[i])) return 0;
    return 1;
}

static void vk_matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    if (!kvk_buf_write_q(gpu.vxb, 0, x, (size_t)cols * sizeof(float)) || !vk_gmv(W, gpu.vxb, gpu.vob, rows, cols, 0)
        || !kvk_buf_read(gpu.vob, 0, out, (size_t)rows * sizeof(float))) die("falha na execucao em Vulkan");
}

/* k produtos com a mesma entrada (q, k e v; gate e up): a entrada sobe uma vez, as saidas ficam lado a lado em vob e
   descem numa leitura so. 0 = nao da (alguma matriz fora da placa): quem chama faz um a um. KATANA_VK_MVS=0 desliga */
static int vk_matvecs(float *const *out, const Mat *const *W, const int *rows, int k, const float *x, int cols) {
    static int on = -1;
    if (on < 0) on = getenv("KATANA_VK_MVS") ? atoi(getenv("KATANA_VK_MVS")) != 0 : 1;
    if (!on || !gpu.vk || !gpu.mv_host || gpu.force_cpu || gpu.test_mv) return 0;
    size_t tot = 0;
    for (int i = 0; i < k; i++) {
        if (!W[i]->gbuf) return 0;
        tot += ((size_t)rows[i] + 127) & ~(size_t)127;
    }
    if (tot * 4 > gpu.vob.size) return 0;
    static float *buf; static size_t cap;
    if (cap < tot) { free(buf); buf = xalloc(tot, sizeof(float)); cap = tot; }
    int ok = kvk_buf_write_q(gpu.vxb, 0, x, (size_t)cols * sizeof(float));
    size_t o = 0;
    for (int i = 0; i < k; i++) {
        ok &= vk_gmv_raw(gpu_kind(W[i]), *(const KvkBuf *)W[i]->gbuf, gpu.vxb, gpu.vob, gpu.vxb, rows[i], cols, 0, 0, W[i]->gs, W[i]->go, 0, (int)o);
        o += ((size_t)rows[i] + 127) & ~(size_t)127;
    }
    if (!ok || !kvk_buf_read(gpu.vob, 0, buf, tot * sizeof(float))) die("falha na execucao em Vulkan");
    o = 0;
    for (int i = 0; i < k; i++) { memcpy(out[i], buf + o, (size_t)rows[i] * sizeof(float)); o += ((size_t)rows[i] + 127) & ~(size_t)127; }
    return 1;
}

