/* ---------- cache de prefixo persistente ----------
 * O prompt de sistema do agente (com as ferramentas) tem 1.000-1.500 tokens e era processado a cada abertura.
 * Depois de processar esse prefixo, o estado do cache KV (e, em modelo com estado recorrente, m->ssm)
 * vai para ~/.katana/cache/pfx-<chave>.kv; a chave e um hash do modelo, dos tokens do prefixo, do contexto
 * (KATANA_CTX) e do tipo do cache (CPU, Vulkan ou OpenCL; 16 ou 32 bits). Ao abrir, se a chave bate, o
 * estado e carregado em vez de recalculado. Na GPU os buffers do cache KV da placa sao lidos/gravados
 * como bytes crus (camadas na GPU) e o cache da CPU cobre as camadas restantes. Nao cobre: MLA
 * (deepseek2) e estado recorrente na GPU; nesses casos o prefixo e recalculado. KATANA_PFX=0 desliga. */
typedef struct { uint64_t magic, key; int32_t n, L, kvd, ctx, ngl, esz, backend, pad; uint64_t ssm_bytes; } PfxHdr;
#define PFX_MAGIC 0x3143505858464b4bULL
#define PFX_KEEP 8

static uint64_t pfx_hash(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* camadas no cache da GPU, tamanho do elemento e backend (0 CPU, 1 Vulkan, 2 OpenCL); 0 se nao cobre */
static int pfx_geom(const Model *m, int *ngl, int *esz, int *backend) {
    const Config *c = &m->cfg;
    if (m->ckv || !(m->kcache || m->kh)) return 0;
    *ngl = 0; *esz = m->kh ? 2 : 4; *backend = 0;      /* cache da CPU em fp16 (m->kh/m->vh, contexto longo): 2 bytes */
    if (vkg.on && !gpu.force_cpu) { *ngl = vkg.nl; *esz = vkg.kv16 ? 2 : 4; *backend = 1; }
    else if (gpu.n_gl > 0 && !gpu.force_cpu) { *ngl = gpu.n_gl; *esz = gpu.kv16 ? 2 : 4; *backend = 2; }
    if (c->ssm_inner && *backend != 0) return 0;
    if (c->g4 && *backend != 0) return 0;       /* gemma4: so o cache da CPU (anel por camada) */
    return 1;
}
/* bytes de K (ou de V) de uma camada para n posicoes no arquivo do prefixo. gemma4: geometria por camada e anel,
   guarda os primeiros min(n, kv_cap) enderecos do anel (se n passou da capacidade, o anel inteiro) */
static size_t pfx_lbytes(const Model *m, int l, int n, int ngl, int esz) {
    const Config *c = &m->cfg;
    const size_t kvd = (size_t)c->n_kv * c->hd;
    if (l < ngl) return (size_t)n * kvd * (size_t)esz;
    const size_t cs = m->kh ? 2 : 4;
    if (c->g4) {
        const Layer *Ly = &m->layers[l];
        const size_t np = (size_t)n < (size_t)Ly->kv_cap ? (size_t)n : (size_t)Ly->kv_cap;
        return np * (size_t)Ly->nkv_l * (size_t)Ly->hd_l * cs;
    }
    return (size_t)n * kvd * cs;
}

static uint64_t pfx_key(const Model *m, const int *ids, int n, int ngl, int esz, int backend) {
    const Config *c = &m->cfg;
    uint64_t h = 1469598103934665603ULL;
    const int32_t g[] = { c->dim, c->n_layers, c->n_heads, c->n_kv, c->hd, c->hidden, c->ctx, c->vocab, c->n_expert, ngl, esz, backend, n };
    h = pfx_hash(h, g, sizeof g);
    h = pfx_hash(h, c->arch, strlen(c->arch));
    h = pfx_hash(h, &m->fsize, sizeof m->fsize);
    h = pfx_hash(h, &m->n_params, sizeof m->n_params);
    h = pfx_hash(h, m->out_norm, sizeof(float) * (size_t)(c->dim < 256 ? c->dim : 256));
    if (m->kh) { const int k16 = 1; h = pfx_hash(h, &k16, sizeof k16); }
    if (c->g4) for (int l = 0; l < c->n_layers; l++) {
        const int32_t gl[] = { m->layers[l].kv_cap, m->layers[l].nkv_l, m->layers[l].hd_l, m->layers[l].swa };
        h = pfx_hash(h, gl, sizeof gl);
    }
    h = pfx_hash(h, ids, sizeof(int) * (size_t)n);
    return h;
}

static int pfx_path(char *out, size_t cap, uint64_t key, int mk) {
    char home[700], d1[800], d2[900];
    agent_home(home, sizeof home);
    const char *over = getenv("KATANA_CACHE_DIR");
    if (over && *over) snprintf(d2, sizeof d2, "%s", over);
    else {
        snprintf(d1, sizeof d1, "%s/.katana", home);
        snprintf(d2, sizeof d2, "%s/.katana/cache", home);
        if (mk) MKDIR(d1);
    }
    if (mk) MKDIR(d2);
    snprintf(out, cap, "%s/pfx-%016llx.kv", d2, (unsigned long long)key);
    return 1;
}

/* le (dir=0) ou escreve (dir=1) o K ou V de uma camada: n posicoes; devolve 0 se falhar */
static int pfx_layer(Model *m, int l, int vv, int n, int ngl, int esz, int backend, void *buf, int dir) {
    const Config *c = &m->cfg;
    const size_t kvd = (size_t)c->n_kv * c->hd, ctx = c->ctx;
    if (l >= ngl) {
        const size_t cs = m->kh ? 2 : 4;        /* elemento do cache da CPU */
        if (c->g4) {                            /* gemma4: bytes por camada (anel) */
            const size_t gb = pfx_lbytes(m, l, n, ngl, esz);
            char *gp = m->kh ? (char *)((vv ? m->vh : m->kh) + m->layers[l].kv_off) : (char *)((vv ? m->vcache : m->kcache) + m->layers[l].kv_off);
            if (dir) memcpy(buf, gp, gb); else memcpy(gp, buf, gb);
            return 1;
        }
        if (m->layers[l].lin) { if (dir) memset(buf, 0, (size_t)n * kvd * cs); return 1; }   /* DeltaNet: sem cache KV */
        char *base = m->kh ? (char *)((vv ? m->vh : m->kh) + m->layers[l].kv_off) : (char *)((vv ? m->vcache : m->kcache) + m->layers[l].kv_off);
        if (dir) memcpy(buf, base, (size_t)n * kvd * cs); else memcpy(base, buf, (size_t)n * kvd * cs);
        return 1;
    }
    const size_t off = (size_t)l * ctx * kvd * (size_t)esz, bytes = (size_t)n * kvd * (size_t)esz;
    if (backend == 1) {
        const KvkBuf b = vv ? vkg.vc[l] : vkg.kc[l];
        return dir ? kvk_buf_read(b, 0, buf, bytes) : kvk_buf_write(b, 0, buf, bytes);
    }
    clh b = vv ? gpu.bvc : gpu.bkc;
    if (dir) return cl.EnqueueReadBuffer(gpu.queue, b, 1, off, bytes, buf, 0, NULL, NULL) == 0;
    return cl.EnqueueWriteBuffer(gpu.queue, b, 1, off, bytes, buf, 0, NULL, NULL) == 0;
}

static int pfx_save(Model *m, uint64_t key, int n, int ngl, int esz, int backend) {
    const Config *c = &m->cfg;
    const size_t kvd = (size_t)c->n_kv * c->hd;
    char path[1000], tmp[1010];
    pfx_path(path, sizeof path, key, 1);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    PfxHdr h; memset(&h, 0, sizeof h);
    h.magic = PFX_MAGIC; h.key = key; h.n = n; h.L = c->n_layers; h.kvd = (int32_t)kvd; h.ctx = c->ctx;
    h.ngl = ngl; h.esz = esz; h.backend = backend;
    if (c->ssm_inner) h.ssm_bytes = (size_t)m->n_lin * dn_state_floats(c) * sizeof(float);
    int ok = fwrite(&h, sizeof h, 1, f) == 1;
    size_t bmax = (size_t)n * kvd * 4;
    for (int l = 0; l < c->n_layers; l++) { const size_t b = pfx_lbytes(m, l, n, ngl, esz); if (b > bmax) bmax = b; }
    void *buf = xalloc(bmax + 64, 1);
    if (backend == 1) kvk_wait();
    if (backend == 2) cl.Finish(gpu.queue);
    for (int l = 0; l < c->n_layers && ok; l++)
        for (int vv = 0; vv < 2 && ok; vv++) {
            const size_t bytes = pfx_lbytes(m, l, n, ngl, esz);
            ok = pfx_layer(m, l, vv, n, ngl, esz, backend, buf, 1) && fwrite(buf, 1, bytes, f) == bytes;
        }
    if (ok && h.ssm_bytes) ok = fwrite(m->ssm, 1, (size_t)h.ssm_bytes, f) == h.ssm_bytes;
    free(buf);
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return 0; }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
#ifndef _WIN32
    /* mantem so os PFX_KEEP prefixos mais recentes */
    char dir[900]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct { char name[300]; long t; } e[256]; int ne = 0;
        struct dirent *de;
        while ((de = readdir(d)) && ne < 256) {
            if (strncmp(de->d_name, "pfx-", 4) != 0) continue;
            char fp[1300]; struct stat st;
            snprintf(fp, sizeof fp, "%s/%s", dir, de->d_name);
            if (stat(fp, &st) != 0) continue;
            snprintf(e[ne].name, sizeof e[ne].name, "%s", fp); e[ne].t = (long)st.st_mtime; ne++;
        }
        closedir(d);
        while (ne > PFX_KEEP) {
            int old = 0;
            for (int i = 1; i < ne; i++) if (e[i].t < e[old].t) old = i;
            remove(e[old].name);
            e[old] = e[--ne];
        }
    }
#endif
    return 1;
}

static int pfx_load(Model *m, uint64_t key, int n, int ngl, int esz, int backend) {
    const Config *c = &m->cfg;
    const size_t kvd = (size_t)c->n_kv * c->hd;
    char path[1000];
    pfx_path(path, sizeof path, key, 0);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    PfxHdr h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == PFX_MAGIC && h.key == key && h.n == n && h.L == c->n_layers &&
             h.kvd == (int32_t)kvd && h.ctx == c->ctx && h.ngl == ngl && h.esz == esz && h.backend == backend;
    const uint64_t ssm_bytes = c->ssm_inner ? (size_t)m->n_lin * dn_state_floats(c) * sizeof(float) : 0;
    ok = ok && h.ssm_bytes == ssm_bytes;
    if (ok) {       /* confere o tamanho do arquivo antes de tocar no cache */
        size_t want = sizeof h + ssm_bytes;
        for (int l = 0; l < c->n_layers; l++) want += 2 * pfx_lbytes(m, l, n, ngl, esz);
        fseek(f, 0, SEEK_END);
        ok = (size_t)ftell(f) == want;
        fseek(f, (long)sizeof h, SEEK_SET);
    }
    size_t bmax = (size_t)n * kvd * 4;
    for (int l = 0; l < c->n_layers; l++) { const size_t b = pfx_lbytes(m, l, n, ngl, esz); if (b > bmax) bmax = b; }
    void *buf = ok ? xalloc(bmax + 64, 1) : NULL;
    for (int l = 0; l < c->n_layers && ok; l++)
        for (int vv = 0; vv < 2 && ok; vv++) {
            const size_t bytes = pfx_lbytes(m, l, n, ngl, esz);
            ok = fread(buf, 1, bytes, f) == bytes && pfx_layer(m, l, vv, n, ngl, esz, backend, buf, 0);
        }
    if (ok && ssm_bytes) ok = fread(m->ssm, 1, (size_t)ssm_bytes, f) == ssm_bytes;
    free(buf);
    fclose(f);
    if (ok && backend == 2) cl.Finish(gpu.queue);
    return ok;
}

/* garante que as primeiras posicoes do cache KV guardam o prefixo de sistema do agente; devolve quantos tokens
 * do prompt completo (full) ja estao no cache (0 = nada aproveitado, processar tudo) */
static int pfx_prepare(Model *m, const Ids *full, const char *sys) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const char *e = getenv("KATANA_PFX");
    if (e && atoi(e) == 0) return 0;
    int ngl, esz, backend;
    if (!pfx_geom(m, &ngl, &esz, &backend)) return 0;
    static char ptext[26000];
    if (c->g4) snprintf(ptext, sizeof ptext, "<|turn>system\n%s%s<turn|>\n", g4_think() ? "<|think|>\n" : "", sys);
    else snprintf(ptext, sizeof ptext, "<|im_start|>system\n%s<|im_end|>\n", sys);
    Ids p = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&p, c->bos);
    encode(tk, ptext, &p);
    if (p.n < 32 || p.n >= full->n || memcmp(p.ids, full->ids, sizeof(int) * (size_t)p.n) != 0) { free(p.ids); return 0; }
    const uint64_t key = pfx_key(m, p.ids, p.n, ngl, esz, backend);
    const int n = p.n;
    const double t0 = now_sec();
    int r = n;
    if (pfx_load(m, key, n, ngl, esz, backend)) {
        fprintf(stderr, "[prefixo do cache: %d tokens carregados em %.2f s]\n", n, now_sec() - t0);
    } else {
        forward_seq(m, p.ids, n, 0, 0);
        const double t1 = now_sec();
        const int saved = pfx_save(m, key, n, ngl, esz, backend);
        fprintf(stderr, "[prefixo do cache: %d tokens calculados em %.2f s%s]\n", n, t1 - t0, saved ? ", gravados em ~/.katana/cache" : " (nao foi possivel gravar)");
    }
    free(p.ids);
    return r;
}

/* aquece o prefixo de sistema do agente ao iniciar, antes da primeira mensagem: carrega o KV gravado em disco
 * (milissegundos) ou calcula e grava uma vez (so muda se o prompt mudar: pasta, memoria, ferramentas MCP) */
static void pfx_warm(Model *m, const char *sys) {
    const Config *c = &m->cfg;
    const char *e = getenv("KATANA_PFX");
    if (e && atoi(e) == 0) return;
    int ngl, esz, backend;
    if (!pfx_geom(m, &ngl, &esz, &backend)) return;
    static char ptext[26000];
    if (c->g4) snprintf(ptext, sizeof ptext, "<|turn>system\n%s%s<turn|>\n", g4_think() ? "<|think|>\n" : "", sys);
    else snprintf(ptext, sizeof ptext, "<|im_start|>system\n%s<|im_end|>\n", sys);
    Ids p = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&p, c->bos);
    encode(&m->tk, ptext, &p);
    if (p.n < 32 || p.n + 64 >= c->ctx) { free(p.ids); return; }
    const uint64_t key = pfx_key(m, p.ids, p.n, ngl, esz, backend);
    const double t0 = now_sec();
    if (pfx_load(m, key, p.n, ngl, esz, backend)) { if (!getenv("KATANA_UI")) fprintf(stderr, "[agent warm: system prompt (%d tokens) loaded from the cache in %.2f s]\n", p.n, now_sec() - t0); }
    else {
        fprintf(stderr, "[agent warm: computing the system prompt (%d tokens), once; the next starts load it from the cache]\n", p.n);
        forward_seq(m, p.ids, p.n, 0, 0);
        const int saved = pfx_save(m, key, p.n, ngl, esz, backend);
        fprintf(stderr, "[agent warm: %d tokens computed in %.1f s%s]\n", p.n, now_sec() - t0, saved ? ", saved in ~/.katana/cache" : " (could not save)");
    }
    free(p.ids);
}

