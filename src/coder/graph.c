/* ---------- coder/graph.c: o grafo de arquivos derivado dos niveis N2 e N3 (fase 7, docs/code-index-lifecycle.md E) ----------
 * Secao STS_GRAPH (versao 1), numero STS_SYM_BASE + 11. E um CACHE DERIVADO: nenhum arquivo e lido para monta-la. Quem a monta (fs_graph_derive, em
 * coder/findstore.c) junta as referencias e as definicoes do indice de simbolos (aresta sym), os imports resolvidos a arquivos (aresta path), os nomes
 * raros compartilhados (aresta rare) e, se houver git, as mudancas juntas (aresta co). Quem monta e o rastreador, quando as filas esvaziam e ficam
 * quietas (graph_on_quiet, chamado por crawl.c), ou o Find, na primeira consulta de uma pasta cujo indice acabou de ser criado em processo (nao ha
 * rastreador ainda); o Find so reconstroi sozinho ate KATANA_FIND_GRAPH_MAX arquivos (30000), acima disso usa o grafo que existe, mesmo velho.
 * Formato (little-endian, tudo alinhado a 8 porque a secao comeca alinhada): cabecalho de 40 bytes {u32 magic "KGRF", u32 versao, u32 nn (nos), u32 nnz
 * (entradas dirigidas), u32 nfiles (arquivos do indice na montagem), u32 reservado, u64 assinatura dos documentos, u64 reservado}; depois
 * hash[nn] (u64, ordenado: store_hash64 do caminho completo relativo a raiz do armazenamento, semente 11), rp[nn+1] (u32), dst[nnz] (u32, indice de no),
 * w[nnz] (f32, peso normalizado por linha), ty[nnz] (u8, mascara: 1 sym, 2 path, 4 rare, 8 co), preenchimento a 8, poff[nn+1] (u32) e o pool de
 * caminhos (para imprimir um vizinho sem abrir o catalogo). So os arquivos que tem alguma aresta sao nos. Os ids de documento sao locais ao
 * segmento, por isso os nos sao chaveados pelo hash do caminho.
 * VALIDADE: a assinatura resume os documentos vigentes (hash de conteudo, tamanho do caminho, nivel); a compactacao nao a muda, uma edicao muda. Grafo
 * com assinatura velha ainda serve (no sem aresta e no que mudou so perdem vizinhos ate a proxima montagem). Fusor: fica o grafo do segmento mais novo.
 * KATANA_FIND_GRAPH=0 desliga a montagem pelo rastreador. */
#ifndef _WIN32
#define STS_GRAPH (STS_SYM_BASE + 11)
#define FGS_MAGIC 0x4652474Bu       /* "KGRF" */
#define FGS_HDR 40
#define FGS_SEED 11
#define FGS_ALGO 2          /* sobe quando a regra de derivacao (findstore.c) muda */

static uint64_t fgs_hash(const char *p, size_t l) { return store_hash64(p, l, FGS_SEED); }

/* grafo aberto: ponteiros para a secao mapeada (mem == NULL) ou para memoria propria (mem != NULL, liberada por fgs_free) */
static void fgs_free(FgsGraph *g) { free(g->mem); memset(g, 0, sizeof *g); }
/* abre o grafo nos bytes de uma secao; valida tamanhos. Devolve 1 ou 0 */
static int fgs_parse(const uint8_t *d, uint64_t size, FgsGraph *g) {
    memset(g, 0, sizeof *g);
    if (size < FGS_HDR || st_get_u32(d) != FGS_MAGIC || st_get_u32(d + 4) != 1) return 0;
    const uint64_t nn = st_get_u32(d + 8), nnz = st_get_u32(d + 12);
    uint64_t o = FGS_HDR;
    const uint64_t need = o + 8 * nn + 4 * (nn + 1) + 9 * nnz;
    if (size < need || (((uintptr_t)d) & 7)) return 0;
    g->nn = (uint32_t)nn; g->nnz = (uint32_t)nnz; g->nfiles = st_get_u32(d + 16); g->sig = st_get_u64(d + 24);
    g->hash = (const uint64_t *)(d + o); o += 8 * nn;
    g->rp = (const uint32_t *)(d + o); o += 4 * (nn + 1);
    g->dst = (const uint32_t *)(d + o); o += 4 * nnz;
    g->w = (const float *)(d + o); o += 4 * nnz;
    g->ty = d + o; o += nnz; o = (o + 7) & ~7ull;
    if (o + 4 * (nn + 1) > size) { memset(g, 0, sizeof *g); return 0; }
    g->poff = (const uint32_t *)(d + o); o += 4 * (nn + 1);
    g->pool = (const char *)(d + o); g->poolsz = size - o;
    for (uint32_t i = 0; i < g->nn; i++) if (g->rp[i] > g->rp[i + 1] || g->rp[i + 1] > g->nnz || g->poff[i] > g->poff[i + 1] || g->poff[i + 1] > g->poolsz) { memset(g, 0, sizeof *g); return 0; }
    return 1;
}
/* o grafo mais novo do leitor (a secao do segmento mais recente que a tem); aponta para o mapeamento do leitor */
static int graph_open(const StoreReader *r, FgsGraph *g) {
    for (int s = r->nseg - 1; s >= 0; s--) { const StoreSection *x = store_seg_section(&r->seg[s], STS_GRAPH); if (x) return fgs_parse(x->data, x->size, g); }
    memset(g, 0, sizeof *g);
    return 0;
}
/* assinatura dos documentos vigentes (ver acima). Cache por manifesto: a montagem da vista vigente e linear */
static uint64_t graph_doc_sig(const StoreReader *r) {
    static char cdir[4400]; static uint64_t cino, cmt, csz, csig;
    if (!strcmp(cdir, r->dir) && cino == r->man_ino && cmt == r->man_mtime && csz == r->man_size) return csig;
    const StoreSeg **segs = (const StoreSeg **)malloc(sizeof *segs * (size_t)(r->nseg + 1));
    for (int i = 0; i < r->nseg; i++) segs[i] = &r->seg[i];
    StoreLive L; store_live_build(&L, segs, r->nseg); free(segs);
    uint64_t sig = 0x9E3779B97F4A7C15ull + FGS_ALGO;                 /* a versao da derivacao entra: grafo de outra regra de derivacao nao vale */
    for (int i = 0; i < L.n; i++) {
        const StoreLiveDoc *e = &L.e[i]; if (e->dead) continue;
        sig += (e->d.hash ^ ((uint64_t)e->d.plen * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)(e->d.flags & 0xF1F) << 40)) * 0x100000001B3ull + 1;
    }
    store_live_free(&L);
    snprintf(cdir, sizeof cdir, "%s", r->dir); cino = r->man_ino; cmt = r->man_mtime; csz = r->man_size; csig = sig;
    return sig;
}
static int graph_merge(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec) {
    *nrec = 0;
    for (int i = c->nseg - 1; i >= 0; i--) { const StoreSection *x = store_seg_section(c->seg[i], STS_GRAPH); if (x) { sb_bytes(out, x->data, (size_t)x->size); *nrec = x->nrec; return 0; } }
    return 0;
}
static void graph_register(void) { store_register_merger(STS_GRAPH, 1, graph_merge); }

static int fs_graph_derive(const char *root, uint64_t sig, StoreBuf *out, uint32_t *nn);       /* coder/findstore.c */
static int fgs_env_graph(void) { const char *e = getenv("KATANA_FIND_GRAPH"); return !(e && *e == '0'); }
/* grava o grafo derivado como um segmento so dele; w e o escritor do chamador (que ja tem o lock). 0 = nada feito ou gravado, -1 = falhou */
static int graph_write(StoreWriter *w, const char *root) {
    char err[300] = "";
    const uint64_t sig = graph_doc_sig(w->base);
    StoreBuf b = {0}; uint32_t nn = 0;
    if (!fs_graph_derive(root, sig, &b, &nn)) { sb_free(&b); return 0; }
    store_writer_add_section_buf(w, STS_GRAPH, 1, nn, &b);
    return store_writer_commit(w, 0, err, sizeof err) < 0 ? -1 : 1;
}
/* chamado por crawl.c no fim da rodada (filas vazias): se o grafo guardado nao vale para os documentos de agora, deriva e grava */
static void graph_on_quiet(StoreReader *r, StoreWriter *w) {
    if (!fgs_env_graph() || !r || !r->man.root[0]) return;
    FgsGraph g;
    if (graph_open(r, &g) && g.sig == graph_doc_sig(r)) return;
    graph_write(w, r->man.root);
}
#else
static void graph_register(void) {}
static void graph_on_quiet(StoreReader *r, StoreWriter *w) { (void)r; (void)w; }
#endif
