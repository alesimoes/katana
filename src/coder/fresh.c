/* ---------- coder/fresh.c: confere um arquivo citado numa resposta contra o catalogo (fase 4 de docs/code-index-lifecycle.md, C.4) ----------
 * Todo arquivo que uma resposta cita e relido do disco para a evidencia (cs_read_doc, fd_text, sy_file_load). Aqui se compara o arquivo vivo com o que o
 * catalogo diz: tamanho e mtime iguais = como indexado; so o mtime difere e o conteudo (hash) e o mesmo = nao mudou; faltando, ou tamanho/conteudo
 * diferentes = mudou depois de indexado. Mudou: conta (sem repetir o mesmo arquivo), manda o caminho ao vigia da sessao (vira TOUCH no rastreador) e a
 * resposta mostra uma linha `[index: N files changed since indexed; shown from the files]` (fresh_note_take). A linha do simbolo que veio do indice sai
 * como "line NOT verified" (sy_file_load ja marcava; ver symquery.c). O catalogo consultado fica em cache e e reaberto quando o MANIFEST muda.
 * fresh_check(root, rel, size, mtime): size == 0 e mtime == 0 significam "procure no catalogo"; a raiz pode ser uma subpasta da raiz do indice.
 * Devolve 1 = o arquivo mudou (ou sumiu), 0 = como o catalogo diz ou sem como saber. Nao toca em sessao sem vigia (so conta). */
#ifndef _WIN32
static void index_touch(const char *path);                     /* indexctl.c: dica ao vigia e a revalidacao */
typedef struct { char root[4096], iroot[4096], dir[4400], prefix[2048]; StoreReader *rd; StoreLive L; int live; } FrCache;
static FrCache g_fr;
static struct { uint64_t seen[64]; int n; unsigned long total; double sent_t[64]; uint64_t sent[64]; int ns; } g_fr_st;
static void fr_drop(void) { if (g_fr.live) { store_live_free(&g_fr.L); g_fr.live = 0; } if (g_fr.rd) { store_close(g_fr.rd); g_fr.rd = NULL; } }
static StoreLiveDoc *fr_lookup(const char *root, const char *rel, char *iroot, size_t icap) {
    if (strcmp(g_fr.root, root) != 0) {
        fr_drop(); IndexLoc loc; index_resolve(root, 0, &loc);
        snprintf(g_fr.root, sizeof g_fr.root, "%s", root); snprintf(g_fr.iroot, sizeof g_fr.iroot, "%s", loc.root); snprintf(g_fr.prefix, sizeof g_fr.prefix, "%s", loc.prefix);
        store_locate(loc.root, "coder", g_fr.dir, sizeof g_fr.dir, 0);
    }
    if (g_fr.rd && store_stale(g_fr.rd)) fr_drop();
    if (!g_fr.rd) {
        char err[200]; if (store_open(g_fr.dir, &g_fr.rd, err, sizeof err) != 0) { g_fr.rd = NULL; return NULL; }
        const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(g_fr.rd->nseg + 1)); for (int i = 0; i < g_fr.rd->nseg; i++) sg[i] = &g_fr.rd->seg[i];
        store_live_build(&g_fr.L, sg, g_fr.rd->nseg); free(sg); g_fr.live = 1;
    }
    char key[4400]; if (g_fr.prefix[0]) snprintf(key, sizeof key, "%s/%s", g_fr.prefix, rel); else snprintf(key, sizeof key, "%s", rel);
    snprintf(iroot, icap, "%s", g_fr.iroot);
    StoreLiveDoc *e = store_live_find(&g_fr.L, key, strlen(key)); return e && !e->dead ? e : NULL;
}
static int fresh_check(const char *root, const char *rel, uint64_t size, int64_t mtime) {
    if (!root || !rel || !*rel) return 0;
    char iroot[4096] = ""; uint64_t chash = 0; uint32_t cflags = 0; int known = 1;
    if (size == 0 && mtime == 0) {
        StoreLiveDoc *e = fr_lookup(root, rel, iroot, sizeof iroot);
        if (g_fr.live == 0) return 0;                                   /* sem indice legivel: nada a comparar */
        if (e) { size = e->d.size; mtime = e->d.mtime; chash = e->d.hash; cflags = e->d.flags; } else known = 0;       /* arquivo que o catalogo nao tem: e novo */
    }
    char abs[8800]; snprintf(abs, sizeof abs, "%s/%s", root, rel);
    struct stat st; int changed = 0;
    if (stat(abs, &st) != 0) changed = 1;
    else if (!known) changed = 1;
    else {
        const int64_t lm = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
        if ((uint64_t)st.st_size != size) changed = 1;
        else if (lm != mtime) {                                          /* so a data difere: o conteudo decide (git checkout e touch regravam sem mudar) */
            if (chash && !(cflags & SDF_SKIPMASK) && st.st_size <= (8 << 20)) {
                uint8_t *b = (uint8_t *)malloc((size_t)st.st_size + 1); const int64_t got = b ? st_slurp(abs, b, (size_t)st.st_size) : -1;
                changed = got < 0 || store_hash64(b, (size_t)got, 1) != chash; free(b);
            } else changed = 1;
        }
    }
    if (!changed) return 0;
    const uint64_t h = store_hash64(rel, strlen(rel), 9) | 1;
    int seen = 0; for (int i = 0; i < g_fr_st.n; i++) if (g_fr_st.seen[i] == h) { seen = 1; break; }
    if (!seen) { if (g_fr_st.n < 64) g_fr_st.seen[g_fr_st.n++] = h; g_fr_st.total++; }
    const double now = now_sec(); int idx = -1;                           /* o mesmo caminho vai ao rastreador no maximo uma vez a cada 2 s */
    for (int i = 0; i < g_fr_st.ns; i++) if (g_fr_st.sent[i] == h) { idx = i; break; }
    if (idx < 0) { idx = g_fr_st.ns < 64 ? g_fr_st.ns++ : (int)(h & 63); g_fr_st.sent[idx] = h; g_fr_st.sent_t[idx] = 0; }
    if (now - g_fr_st.sent_t[idx] >= 2.0) { g_fr_st.sent_t[idx] = now; index_touch(abs); }
    return 1;
}
/* a linha de aviso da resposta, uma vez por consulta: devolve 1 e a escreve se algum arquivo citado mudou desde a ultima chamada */
static int fresh_note_take(char *out, size_t cap) {
    if (g_fr_st.n == 0) { if (cap) out[0] = 0; return 0; }
    snprintf(out, cap, "[index: %d file%s changed since indexed; shown from the files]\n", g_fr_st.n, g_fr_st.n == 1 ? "" : "s");
    g_fr_st.n = 0; return 1;
}
#else
static int fresh_check(const char *root, const char *rel, uint64_t size, int64_t mtime) { (void)root; (void)rel; (void)size; (void)mtime; return 0; }
static int fresh_note_take(char *out, size_t cap) { if (cap) out[0] = 0; return 0; }
#endif
