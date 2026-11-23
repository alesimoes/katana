/* ---------- coder/flow.c: ligacoes por CHAVE DE TEXTO (katana coder flow) ----------
 * API estruturada (fase U2 do UIOE; ver o bloco "API estruturada" de symquery.c): fl_q_key(v, chave, limit, out, meta), fl_q_file(v, arquivo, linha, limit, out, meta),
 * fl_q_links(v, limit, out, meta) e fl_q_arg(v, arg, limit, out, meta) (decide se arg e arquivo[:linha] ou chave, como `katana coder flow`). Preenchem SyItems e FlQ e devolvem
 * o total; nao imprimem e nao usam estado global. fl_render_text / fl_render_json produzem a saida de `katana coder flow` (a ligacao produtor -> consumidor traz os dois
 * lados: path/line do produtor e peer_path/peer_line/peer_where do consumidor); `meta->brief = 1` (depois da consulta) encurta a explicacao da confianca, como a ferramenta Symbols.
 * katana coder flow <chave> [pasta] [--json] [--limit N]          quem produz (envia, emite, escreve, chama a rota) e quem consome (trata, le) a chave
 * katana coder flow <arquivo>[:<linha>] [pasta] [--json]           as chaves que o arquivo (ou o simbolo que contem a linha) produz/consome e para onde vao
 * katana coder flow --links [pasta] [--json] [--limit N]           o grafo de ligacoes por chave do projeto: arquivo -> arquivo, com as chaves
 * As chaves vem de keys.c (secoes SY_T_K* do symindex.c). A ligacao e TEXTO IGUAL em arquivos diferentes; nao ha resolucao.
 * CONTRATO como terceiro lado: ocorrencia de papel DEFINE (classe contract, sub CKS_*) de um arquivo de contrato; `fl_pair_contract` da a confianca de contrato -> codigo (rota igual com
 * parametros cortados e mesmo metodo, nome de operacao em cliente gerado ou chamado pelo nome, canal/mensagem/valor de enum igual a uma chave msg/event, arquivo e campo GraphQL citados);
 * o par produtor -> consumidor sustentado por um contrato ganha +6 e mostra `contract: arquivo:linha`. Secoes novas do texto: CONTRACT e CONTRACT LINKS; itens kind define e clink.
 * Mesma cascata das outras consultas: nome exato da chave, depois derivado (caixa/sublinhado/parte), depois parecido (distancia de edicao <= 2),
 * com `confidence` 0-100, nivel e `how`. A confianca de uma LIGACAO produtor->consumidor vem de:
 *   base 60 (mesma chave literal em arquivos diferentes), +4 mesma linguagem, +10 se a chave aparece em 2 arquivos (+6 ate 4, -1 por arquivo acima de 8, ate -20:
 *   chave comum), classe (env +14, msg/evento/rota +8, config +4, comando 0, comparacao qualquer -8), rota: +10 se o metodo casa e -25 se discorda,
 *   chave curta e sem separador -6, chave composta (_ . /) +4, muitos produtores x consumidores -3 por par extra (ate -12), mesmo arquivo -20,
 *   a linha foi relida e contem a chave +4, arquivo mudou desde o `index` -25. */
typedef struct { const char *path; uint32_t plen; const char *key; uint32_t kl; int seg; uint32_t doc, line, role, cls, meth, lang, sub; int byname; } KyOcc;          /* sub = CKS_* (so classe contract); byname = veio de uma chamada pelo NOME da operacao (indice de referencias), nao de um literal */
typedef struct { KyOcc *o; int n, cap; } KyOccs;
static const char *const FL_CLS[] = {"other", "msg", "event", "route", "env", "config", "cmd", "contract"};
static const char *const FL_SUBN[] = {"", "route", "rpc", "operation", "message", "schema", "enum value", "file", "graphql field"};
static const char *const FL_METH[] = {"", "GET", "POST", "PUT", "DELETE", "PATCH", "OTHER"};
static const char *const FL_PV[] = {"uses", "sends", "emits", "calls", "sets", "defines", "invokes", "specifies"};              /* verbo do produtor, por classe */
static const char *const FL_PL[] = {"compared in", "handled in", "handled in", "served in", "read in", "read in", "dispatched in", "specified in"};
static const char *const FL_CV[] = {"compares", "handles", "handles", "serves", "reads", "reads", "dispatches", "implements"};   /* verbo do consumidor */
static const char *const FL_CL[] = {"used in", "sent from", "emitted from", "called from", "set in", "defined in", "invoked from", "specified in"};

static void ky_occ_at(const SyView *v, int seg, uint32_t row, KyOcc *o) {
    const KySec *x = &v->s[seg].ky; const uint8_t *q = store_table_at(&x->occ, row);
    o->seg = seg; o->key = store_strs_get(&x->names, st_get_u32(q), &o->kl); uint32_t pk = st_get_u32(q + 4);
    o->role = pk & 0xFF; o->cls = (pk >> 8) & 0xFF; if (o->cls >= CKC_N) o->cls = 0; o->meth = (pk >> 16) & 0xFF; if (o->meth >= CKM_N) o->meth = 0;
    o->sub = (pk >> 24) & 0xFF; if (o->cls != CKC_CONTRACT || o->sub >= CKS_N) o->sub = 0; o->byname = 0;          /* role/cls desconhecidos (armazenamento de versao futura): ky_push descarta a ocorrencia */
    o->line = st_get_u32(q + 8); o->doc = st_get_u32(q + 12);
    sy_doc_path(v, seg, o->doc, &o->path, &o->plen, &o->lang);
    if (!o->lang) { const char *dot = NULL; for (uint32_t i = 0; i < o->plen; i++) if (o->path[i] == '.') dot = o->path + i; if (dot) { char e[8]; size_t el = (size_t)(o->path + o->plen - dot - 1); if (el < 7) { memcpy(e, dot + 1, el); e[el] = 0; o->lang = (uint32_t)coder_lang_of(o->path); } } }
}
static void ky_push(KyOccs *a, const KyOcc *o) { if (o->role < CKR_PRODUCE || o->role > CKR_DEFINE) return; if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->o = (KyOcc *)realloc(a->o, sizeof(KyOcc) * (size_t)a->cap); } a->o[a->n++] = *o; }
static int ky_cmp(const void *a, const void *b) { const KyOcc *x = (const KyOcc *)a, *y = (const KyOcc *)b; return sy_cmp_path_line(x->path, x->plen, (int)x->line, y->path, y->plen, (int)y->line); }
static void ky_occs_of(const SyView *v, const char *key, size_t kl, KyOccs *out) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
        int64_t id = store_strs_find(&c->ky.names, key, kl); uint32_t lo, hi; if (id < 0 || !sy_pair_range(&c->ky.bykey, (uint32_t)id, &lo, &hi)) continue;
        for (uint32_t i = lo; i < hi; i++) { uint32_t row = st_get_u32(store_table_at(&c->ky.bykey, i) + 4); if (!c->live[st_get_u32(store_table_at(&c->ky.occ, row) + 12)]) continue;
            KyOcc o; ky_occ_at(v, s, row, &o); ky_push(out, &o); } }
    qsort(out->o, (size_t)out->n, sizeof(KyOcc), ky_cmp);
}
static void ky_all(const SyView *v, KyOccs *out) {
    for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
        for (uint32_t row = 0; row < c->ky.occ.n; row++) { if (!c->live[st_get_u32(store_table_at(&c->ky.occ, row) + 12)]) continue; KyOcc o; ky_occ_at(v, s, row, &o); ky_push(out, &o); } }
}
static int ky_same_file(const KyOcc *a, const KyOcc *b) { return a->plen == b->plen && !memcmp(a->path, b->path, a->plen); }
static int ky_count_files(const KyOcc *o, int n, int role) {
    int f = 0; for (int i = 0; i < n; i++) { if ((role && (int)o[i].role != role) || (!role && o[i].role == CKR_DEFINE)) continue; int dup = 0; for (int j = 0; j < i && !dup; j++) if (((!role && o[j].role != CKR_DEFINE) || (role && (int)o[j].role == role)) && ky_same_file(&o[i], &o[j])) dup = 1; if (!dup) f++; }          /* role 0 = produtores e consumidores (o contrato nao conta como arquivo que usa a chave) */
    return f;
}
/* innermost symbol containing a line: name, range. 0 = none */
static int64_t fl_encl_k(const SyView *v, int seg, uint32_t doc, uint32_t line, char *nm, size_t cap, uint32_t *l0, uint32_t *l1) {
    const SySec *x = &v->s[seg].x; if (!x->has || doc >= x->docs.n) return -1;
    const uint8_t *dr = store_table_at(&x->docs, doc); uint32_t s0 = st_get_u32(dr), n = st_get_u32(dr + 4); if (!n) return -1;
    uint32_t lo = 0, hi = n; while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if (st_get_u32(store_table_at(&x->syms, s0 + m) + 12) <= line) lo = m + 1; else hi = m; }
    int64_t k = (int64_t)lo - 1;
    while (k >= 0 && st_get_u32(store_table_at(&x->ival, s0 + (uint32_t)k)) >= line) {
        const uint8_t *y = store_table_at(&x->syms, s0 + (uint32_t)k);
        if (st_get_u32(y + 16) >= line && !((st_get_u32(y + 40) >> 16) & CSF_DECL)) { uint32_t nl; const char *p = store_strs_get(&x->names, st_get_u32(y), &nl); sy_cpy(nm, cap, p, nl); *l0 = st_get_u32(y + 12); *l1 = st_get_u32(y + 16); return k; }
        k--;
    }
    return -1;
}
static int fl_encl(const SyView *v, int seg, uint32_t doc, uint32_t line, char *nm, size_t cap, uint32_t *l0, uint32_t *l1) { return fl_encl_k(v, seg, doc, line, nm, cap, l0, l1) >= 0; }

/* ---- confianca de uma ligacao produtor -> consumidor ---- */
typedef struct { int conf; char how[170]; } FlLink;
/* o arquivo de ref tambem tem uma ocorrencia com este papel? (um consumidor que ele mesmo define a chave nao recebe nada de fora) */
static int ky_file_has(const KyOcc *o, int n, int role, const KyOcc *ref) { for (int i = 0; i < n; i++) if ((int)o[i].role == role && ky_same_file(&o[i], ref)) return 1; return 0; }
/* ---- confianca de uma ligacao contrato -> codigo (d = ocorrencia DEFINE, u = produtor/consumidor da mesma chave em outro arquivo) ---- */
static void fl_pair_contract(const KyOcc *d, const KyOcc *u, int nfiles, FlLink *L) {
    int conf = 56; char h[170]; size_t o = 0; const int same = ky_same_file(d, u);
    o += (size_t)snprintf(h + o, sizeof h - o, "contract %s key, same literal", FL_SUBN[d->sub]);
    if (same) { conf -= 25; o += (size_t)snprintf(h + o, sizeof h - o, " in the SAME file"); }
    switch (d->sub) {
    case CKS_ROUTE: case CKS_RPC:
        if (u->cls == CKC_ROUTE) { conf += 16; o += (size_t)snprintf(h + o, sizeof h - o, "; route matches (parameters cut: {id} and :id)");
            if (d->meth && u->meth) { if (d->meth == u->meth) { conf += 12; o += (size_t)snprintf(h + o, sizeof h - o, "; method %s matches", FL_METH[d->meth]); } else { conf -= 25; o += (size_t)snprintf(h + o, sizeof h - o, "; method differs (%s vs %s)", FL_METH[d->meth], FL_METH[u->meth]); } } }
        else if (u->cls == CKC_MSG || u->cls == CKC_EVENT || u->cls == CKC_OTHER) { conf -= 6; o += (size_t)snprintf(h + o, sizeof h - o, "; used as plain text, not as a route"); }
        else { conf -= 22; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s)", FL_CLS[u->cls]); }
        break;
    case CKS_OP:
        if (u->cls == CKC_CONTRACT && u->sub == CKS_OP) { conf += 10; o += (size_t)snprintf(h + o, sizeof h - o, u->byname == 1 ? "; a call to a function with the operation name" : u->byname == 2 ? "; a function or method with the operation name (implementation or stub)" : "; generated client function with the operation name"); if (u->byname) conf -= 16; }
        else { conf -= 18; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s)", FL_CLS[u->cls]); }
        break;
    case CKS_MSG: case CKS_ENUM:
        if (u->cls == CKC_MSG || u->cls == CKC_EVENT) { conf += 14; o += (size_t)snprintf(h + o, sizeof h - o, "; %s key (message/event name)", FL_CLS[u->cls]); }
        else if (u->cls == CKC_OTHER) { conf += 2; o += (size_t)snprintf(h + o, sizeof h - o, "; compared text, field unknown"); }
        else if (u->cls == CKC_ROUTE) { conf -= 12; o += (size_t)snprintf(h + o, sizeof h - o, "; used as a route"); }
        else { conf -= 22; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s)", FL_CLS[u->cls]); }
        break;
    case CKS_SCHEMA:
        conf -= 4; if (u->cls == CKC_MSG) o += (size_t)snprintf(h + o, sizeof h - o, "; schema name used as a message key");
        else if (u->cls == CKC_OTHER) { conf -= 6; o += (size_t)snprintf(h + o, sizeof h - o, "; schema name as plain text"); } else { conf -= 22; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s)", FL_CLS[u->cls]); }
        break;
    case CKS_FILE: case CKS_GQL:
        if (u->cls == CKC_CONTRACT && u->sub == d->sub) { conf += 26; o += (size_t)snprintf(h + o, sizeof h - o, d->sub == CKS_FILE ? "; the file is named by the code or by another contract" : "; GraphQL root field used by a client query"); }
        else { conf -= 30; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s)", FL_CLS[u->cls]); }
        break;
    default: break;
    }
    if (d->lang && u->lang && sy_lfam(d->lang) != sy_lfam(u->lang)) o += (size_t)snprintf(h + o, sizeof h - o, "; %s -> %s", sy_langname(d->lang), sy_langname(u->lang));
    if (nfiles <= 2) { conf += 8; o += (size_t)snprintf(h + o, sizeof h - o, "; rare key (%d files)", nfiles); } else if (nfiles <= 4) { conf += 4; o += (size_t)snprintf(h + o, sizeof h - o, "; %d files use the key", nfiles); }
    else if (nfiles > 8) { int pen = nfiles - 8; if (pen > 20) pen = 20; conf -= pen; o += (size_t)snprintf(h + o, sizeof h - o, "; common key (%d files)", nfiles); }
    { int plain = 1; for (uint32_t i = 0; i < d->kl; i++) if (!isalpha((unsigned char)d->key[i])) plain = 0; if (plain && d->kl <= 4 && d->sub != CKS_FILE && d->sub != CKS_GQL) { conf -= 8; o += (size_t)snprintf(h + o, sizeof h - o, "; short plain key"); } }
    L->conf = conf < 1 ? 1 : conf > 99 ? 99 : conf; snprintf(L->how, sizeof L->how, "%s", h);
}
static void fl_pair(const KyOcc *p, const KyOcc *c, int nfiles, int np, int nc, int localdef, FlLink *L) {
    if (p->role == CKR_DEFINE || c->role == CKR_DEFINE) {          /* um dos lados e o contrato */
        const KyOcc *d = p->role == CKR_DEFINE ? p : c, *u = d == p ? c : p;
        if (u->role == CKR_DEFINE) { L->conf = 1; snprintf(L->how, sizeof L->how, "two contracts define the same key"); return; }
        fl_pair_contract(d, u, nfiles, L); return;
    }
    int conf = 60; char h[170]; size_t o = 0; const int same = ky_same_file(p, c);
    o += (size_t)snprintf(h + o, sizeof h - o, "same literal key");
    if (localdef && !same) { conf -= (c->cls == CKC_ENV || c->cls == CKC_CONFIG) ? 30 : 10; o += (size_t)snprintf(h + o, sizeof h - o, "; the consumer file also defines the key itself"); }
    if (same) { conf -= 20; o += (size_t)snprintf(h + o, sizeof h - o, " in the SAME file (it both sends and handles it)"); } else o += (size_t)snprintf(h + o, sizeof h - o, " in different files");
    const int samelang = p->lang && sy_lfam(p->lang) == sy_lfam(c->lang);
    if (samelang) { conf += 4; o += (size_t)snprintf(h + o, sizeof h - o, "; same language"); } else if (p->lang && c->lang) o += (size_t)snprintf(h + o, sizeof h - o, "; cross-language (%s -> %s)", sy_langname(p->lang), sy_langname(c->lang));
    if (nfiles <= 2) { conf += 10; o += (size_t)snprintf(h + o, sizeof h - o, "; rare key (%d files)", nfiles); } else if (nfiles <= 4) { conf += 6; o += (size_t)snprintf(h + o, sizeof h - o, "; %d files use the key", nfiles); }
    else if (nfiles > 8) { int pen = nfiles - 8; if (pen > 20) pen = 20; conf -= pen; o += (size_t)snprintf(h + o, sizeof h - o, "; common key (%d files)", nfiles); }
    static const int bonus[CKC_N] = {-8, 8, 8, 8, 14, 4, 0, 0};
    int cls = c->cls == p->cls ? (int)c->cls : -1;
    if (cls < 0) { conf -= 8; o += (size_t)snprintf(h + o, sizeof h - o, "; kinds differ (%s vs %s)", FL_CLS[p->cls], FL_CLS[c->cls]); }
    else { conf += bonus[cls]; o += (size_t)snprintf(h + o, sizeof h - o, "; %s key", FL_CLS[cls]); }
    if (p->cls == CKC_ROUTE && c->cls == CKC_ROUTE && p->meth && c->meth) {
        if (p->meth == c->meth) { conf += 10; o += (size_t)snprintf(h + o, sizeof h - o, "; method %s matches", FL_METH[p->meth]); } else { conf -= 25; o += (size_t)snprintf(h + o, sizeof h - o, "; method differs (%s vs %s)", FL_METH[p->meth], FL_METH[c->meth]); } }
    int plain = 1; for (uint32_t i = 0; i < p->kl; i++) if (!isalpha((unsigned char)p->key[i])) plain = 0;
    if (p->kl <= 4 && plain) conf -= 6; else if (!plain && p->cls != CKC_ENV) conf += 4;
    { int extra = np * nc - 1; if (extra > 0) { int pen = extra * 3; conf -= pen > 12 ? 12 : pen; o += (size_t)snprintf(h + o, sizeof h - o, "; %d sender file%s x %d handler file%s", np, np == 1 ? "" : "s", nc, nc == 1 ? "" : "s"); } }
    L->conf = conf < 1 ? 1 : conf > 99 ? 99 : conf; snprintf(L->how, sizeof L->how, "%s", h);
}
static int fl_verify(SyView *v, const KyOcc *o) {
    if (!sy_file_load(v, o->path, o->plen) || v->fstale) return -1;
    char t[600], k[100]; snprintf(k, sizeof k, "%.*s", o->kl < 99 ? (int)o->kl : 99, o->key);
    for (int l = (int)o->line; l <= (int)o->line + 1; l++) if (sy_line_text(v, l, t, sizeof t) && strstr(t, k)) return 1;
    if (o->role == CKR_DEFINE && o->sub == CKS_FILE) return 1;          /* o proprio arquivo de contrato: a chave e o nome dele, nao um texto da linha */
    if (o->cls == CKC_CONTRACT && o->sub == CKS_ROUTE && o->meth) {          /* rota de OpenAPI: a linha do metodo ("post:") nao repete o caminho, que esta na linha do caminho */
        char mw[12]; snprintf(mw, sizeof mw, "%s", FL_METH[o->meth]); for (char *q = mw; *q; q++) *q = (char)tolower((unsigned char)*q);
        for (int l = (int)o->line; l <= (int)o->line + 1; l++) { char *tt = t; if (sy_line_text(v, l, t, sizeof t)) { for (char *q = tt; *q; q++) *q = (char)tolower((unsigned char)*q); if (strstr(tt, mw)) return 1; } }
    }
    if (o->cls == CKC_CONTRACT || o->role == CKR_DEFINE) {          /* chave de contrato: a linha tem a ultima parte da chave, sem caixa nem _ - (variantes de nome, prefixo de servidor, "Servico/Metodo") */
        const char *a = strrchr(k, '/'), *b = strrchr(k, '.'); const char *tail = a && (!b || a > b) ? a + 1 : b ? b + 1 : k; if (!*tail) tail = k;
        char fk[100]; int w = 0; for (const char *q = tail; *q && w < 98; q++) if (*q != '_' && *q != '-') fk[w++] = (char)tolower((unsigned char)*q); fk[w] = 0;
        for (int l = (int)o->line; l <= (int)o->line + 1; l++) if (sy_line_text(v, l, t, sizeof t)) { w = 0; char *big = (char *)malloc(strlen(t) + 1); for (const char *q = t; *q; q++) if (*q != '_' && *q != '-') big[w++] = (char)tolower((unsigned char)*q); big[w] = 0; int hit = fk[0] && strstr(big, fk) != NULL; free(big); if (hit) return 1; }
        return -1;
    }
    if (o->cls == CKC_ROUTE) { const char *sl = strrchr(k, '/'); if (sl && sl[1]) for (int l = (int)o->line; l <= (int)o->line + 1; l++) if (sy_line_text(v, l, t, sizeof t) && strstr(t, sl)) return 1; }
    return -1;
}
/* arquivos com cara de contrato (nome) analisados antes do extrator de contratos: o indice nao tem o lado do contrato deles */
static int fl_old_contracts(const SyView *v) {
    int n = 0;
    for (int i = 0; i < v->L.n; i++) {
        const StoreLiveDoc *e = &v->L.e[i]; if (e->dead) continue; const SyVSeg *c = &v->s[e->seg];
        if (!c->ky.has || e->idx >= c->ky.docs.n) continue; uint32_t w = st_get_u32(store_table_at(&c->ky.docs, e->idx) + 4); if (!(w & SY_KF1) || (w & SY_KF2)) continue;
        const char *p; uint32_t l; sy_doc_path(v, e->seg, e->idx, &p, &l, NULL);
        const char *b = p; for (uint32_t k = 0; k < l; k++) if (p[k] == '/') b = p + k + 1; uint32_t bl = (uint32_t)(p + l - b); if (bl < 5 || bl > 80) continue;
        char lo[84]; for (uint32_t k = 0; k < bl; k++) lo[k] = (char)tolower((unsigned char)b[k]); lo[bl] = 0;
        int ext = (bl > 6 && !strcmp(lo + bl - 6, ".proto")) || (bl > 8 && !strcmp(lo + bl - 8, ".graphql")) || (bl > 4 && !strcmp(lo + bl - 4, ".gql")) || (bl > 9 && !strcmp(lo + bl - 9, ".graphqls"));
        int yj = (bl > 5 && !strcmp(lo + bl - 5, ".yaml")) || (bl > 4 && !strcmp(lo + bl - 4, ".yml")) || (bl > 5 && !strcmp(lo + bl - 5, ".json"));
        if (ext || (yj && (strstr(lo, "openapi") || strstr(lo, "swagger") || strstr(lo, "asyncapi") || strstr(lo, "schema") || strstr(lo, "api")))) n++;
    }
    return n;
}
static void fl_limits(const SyView *v, char *o, size_t cap) {
    size_t n = (size_t)snprintf(o, cap, "Indexed set: %u files analysed for text keys. A link is the SAME literal text in two files (event/message names, routes, env variables, config keys, subcommands, contract names: OpenAPI, AsyncAPI, .proto, GraphQL, JSON Schema, typed enums); keys built at run time, "
        "dispatch tables, reflection and other projects are not seen.", v->nkeyflag);
    if (v->nkeyold && n < cap) { snprintf(o + n, cap - n, " %u indexed files were analysed before text keys existed and have none: delete %s and run `katana coder index` again.", v->nkeyold, v->dir); n = strlen(o); }
    { int oc = fl_old_contracts(v); if (oc && n < cap) snprintf(o + n, cap - n, " %d contract-like file%s (OpenAPI, AsyncAPI, .proto, GraphQL, JSON Schema) %s analysed before contract keys existed, so the contract side of a link may be missing: delete %s and run `katana coder index` again.", oc, oc == 1 ? "" : "s", oc == 1 ? "was" : "were", v->dir); }
}
/* "in nome (l0-l1)" do simbolo que contem a linha */
static void fl_where(const SyView *v, const KyOcc *o, char *out, size_t cap) {
    char nm[100]; uint32_t a, b; out[0] = 0;
    if (fl_encl(v, o->seg, o->doc, o->line, nm, sizeof nm, &a, &b)) snprintf(out, cap, "in %s (%u-%u)", nm, a, b);
}
static void fl_set_line(SyView *v, SyItem *it, const KyOcc *o) {
    char t[240]; if (sy_file_load(v, o->path, o->plen) && sy_line_text(v, (int)o->line, t, sizeof t)) { it->snip = (char *)malloc(strlen(t) + 16); sprintf(it->snip, "%u: %s", o->line, t); }
}
static void fl_key_label(const KyOcc *o, char *out, size_t cap) {
    if ((o->cls == CKC_ROUTE || (o->cls == CKC_CONTRACT && (o->sub == CKS_ROUTE || o->sub == CKS_RPC))) && o->meth) snprintf(out, cap, "%s %.*s", FL_METH[o->meth], (int)(o->kl < 80 ? o->kl : 80), o->key);
    else snprintf(out, cap, "%.*s", (int)(o->kl < 90 ? o->kl : 90), o->key);
}

/* ---- argumentos e abertura ---- */
/* brief: texto enxuto para o modelo (explicacao da confianca encurtada) */
static void fl_brief_how(const char *how, char *o, size_t cap, int brief) {
    snprintf(o, cap, "%s", how); if (!brief) return;
    int semi = 0; for (char *p = o; *p; p++) if (*p == ';' && ++semi == 3) { *p = 0; break; }
}
static int fl_start(const char *use, int argc, char **argv, SyArgs *a, SyView *v, int *links, int *brief) {
    char *av[64]; int ac = 0; *links = 0; *brief = 0;
    for (int i = 0; i < argc && ac < 62; i++) { if (!strcmp(argv[i], "--links")) *links = 1; else if (!strcmp(argv[i], "--brief")) *brief = 1; else av[ac++] = argv[i]; }
    SyArgs probe; sy_args(ac, av, &probe, 12);
    if (probe.npos < 1 && *links) av[ac++] = (char *)".";
    if (sy_args(ac, av, a, 12) != 0) { fprintf(stderr, "uso: katana coder %s\n", use); return 2; }
    const char *folder = *links ? a->pos[0] : (a->npos >= 2 ? a->pos[1] : ".");
    char err[300] = ""; if (sy_view_open(folder, v, err, sizeof err) != 0) { fprintf(stderr, "flow: %s\n", err); return 1; }
    if (!v->anykeys) { fprintf(stderr, "flow: the index has no text-key sections (it was built before they existed); delete %s and run `katana coder index` again\n", v->dir); sy_view_close(v); return 1; }
    return 0;
}
typedef struct {
    const char *query, *arg;          /* "flow" e o argumento como o chamador o deu */
    int mode, brief, total, step;     /* mode: 0 chave, 1 arquivo[:linha], 2 --links; brief: o chamador liga depois da consulta; total de resultados; passo da cascata */
    char resolved[200], cascade[160]; SyCand cand[8]; int ncand;      /* cascata: nomes resolvidos, motivo e a cadeia completa */
    char hint[900], next[400];        /* nota (vai no JSON como "note") e o "Next: Read ..." do texto (modo chave) */
    char head[4700];                  /* cabecalho do texto (modo arquivo), so com resultado */
    char notfound[704], limits[900];  /* NOT FOUND e o limite do conjunto analisado */
    uint32_t nlive, nkeys;            /* arquivos do conjunto e arquivos analisados para chaves */
    char argbuf[4500];
} FlQ;
static void fl_q_begin(FlQ *o, const char *arg, int mode) { memset(o, 0, sizeof *o); o->query = "flow"; o->arg = arg; o->mode = mode; }
static int fl_q_end(const SyView *v, FlQ *o, int total, const char *notfound) {
    o->total = total; snprintf(o->notfound, sizeof o->notfound, "%s", notfound); fl_limits(v, o->limits, sizeof o->limits); o->nlive = v->nlive; o->nkeys = v->nkeyflag; return total;
}
static void fl_render_json(CtSb *b, const FlQ *o, const SyItems *a) {
    sy_sbf(b, "{\"query\":"); sy_sb_jstr(b, o->query); sy_sbf(b, ",\"arg\":"); sy_sb_jstr(b, o->arg);
    if (o->resolved[0]) { sy_sbf(b, ",\"resolved\":"); sy_sb_jstr(b, o->resolved); }
    if (o->step) { sy_sbf(b, ",\"cascade_step\":%d,\"cascade\":", o->step); sy_sb_jstr(b, o->cascade); }
    sy_sbf(b, ",\"count\":%d,\"shown\":%d,\"items\":[", o->total, a->n);
    for (int i = 0; i < a->n; i++) { if (i) sy_sbf(b, ","); sy_json_item(b, &a->v[i]); }
    sy_sbf(b, "]"); if (o->hint[0]) { sy_sbf(b, ",\"note\":"); sy_sb_jstr(b, o->hint); }
    if (!o->total) { char nf[1400]; snprintf(nf, sizeof nf, "NOT FOUND: %s. Cascade tried: exact key, same key ignoring case or as a part, edit distance <= 2. %s", o->notfound, o->limits); sy_sbf(b, ",\"not_found\":"); sy_sb_jstr(b, nf); }
    sy_sbf(b, ",\"index\":{\"files\":%u,\"with_keys\":%u}}\n", o->nlive, o->nkeys);
}
static void fl_print_item(CtSb *b, const SyItem *it, int brief) {
    if (!strcmp(it->kind, "link") || !strcmp(it->kind, "clink")) { char hb[200]; fl_brief_how(it->how, hb, sizeof hb, brief); sy_sbf(b, "  %s:%d  %s  [confidence %d %s; %s]\n", it->path, it->line0, it->name, it->conf, sy_level(it->conf), hb); return; }
    sy_sbf(b, "  %s:%d  %s  %s  [%s; confidence %d %s]\n", it->path, it->line0, it->name, it->encl, it->vis, it->conf, sy_level(it->conf));
    if (it->snip) sy_sbf(b, "      %s\n", it->snip);
}
static void fl_render_text(CtSb *b, const FlQ *o, const SyItems *a) {
    if (!o->total) { sy_sbf(b, "NOT FOUND: %s.\nCascade tried: exact key, same key ignoring case or as a part, edit distance <= 2.\n%s\n", o->notfound, o->limits); return; }
    if (o->step > 1) sy_sbf(b, "No exact key '%s'; %s\n", o->arg, o->cascade);
    const int brief = o->brief;
    if (o->mode == 0) {
        for (int pass = 0; pass < 5; pass++) {
            const char *kind = pass == 0 ? "produce" : pass == 1 ? "consume" : pass == 2 ? "define" : pass == 3 ? "link" : "clink"; int cnt = 0; for (int i = 0; i < a->n; i++) if (!strcmp(a->v[i].kind, kind)) cnt++;
            if (!cnt) continue;
            sy_sbf(b, "%s (%d):\n", pass == 0 ? "PRODUCE (sends / emits / writes / calls)" : pass == 1 ? "CONSUME (handles / reads / serves)" : pass == 2 ? "CONTRACT (defines the key)" : pass == 3 ? "LINKS producer -> consumer" : "CONTRACT LINKS contract -> code", cnt);
            for (int i = 0; i < a->n; i++) if (!strcmp(a->v[i].kind, kind)) {
                const SyItem *it = &a->v[i];
                if (pass >= 3) { char hb[200]; fl_brief_how(it->how, hb, sizeof hb, brief); sy_sbf(b, "  %s:%d %s %s  [confidence %d %s; %s]\n", it->path, it->line0, it->name, it->sig, it->conf, sy_level(it->conf), hb); if (!brief) sy_sbf(b, "      %s\n", it->encl); }
                else fl_print_item(b, it, brief);
            }
        }
        char h[900]; snprintf(h, sizeof h, "%s", o->hint);
        if (o->next[0]) { size_t l = strlen(h); snprintf(h + l, sizeof h - l, "%s%s", l ? "\n" : "", o->next); }
        if (h[0]) sy_sbf(b, "%s\n", h);
    } else if (o->mode == 1) {
        sy_sbf(b, "%s\n", o->head);
        for (int pass = 0; pass < 3; pass++) {
            const char *pk = pass == 0 ? "produce" : pass == 1 ? "consume" : "define";
            int cnt = 0; for (int i = 0; i < a->n; i++) if (!strcmp(a->v[i].kind, pk)) cnt++;
            if (!cnt) continue; sy_sbf(b, "%s\n", pass == 1 ? "RECEIVES (reads / handles)" : pass == 0 ? "SENDS (produces / writes / calls)" : "DEFINES (this contract specifies)");
            for (int i = 0; i < a->n; i++) { const SyItem *it = &a->v[i]; if (strcmp(it->kind, pk)) continue;
                int cls = 0; for (int k = 0; k < CKC_N; k++) if (!strncmp(it->vis, FL_CLS[k], strlen(FL_CLS[k]))) cls = k;
                if (it->flags & SYI_NOCONF) sy_sbf(b, "  %d  %s [%s]  -- %s", it->line0, it->name, it->vis, it->sig);
                else sy_sbf(b, "  %d  %s [%s]  %s %s   [confidence %d %s]", it->line0, it->name, it->vis, pass == 2 ? "used by" : pass ? FL_CL[cls] : FL_PL[cls], it->sig, it->conf, sy_level(it->conf));
                sy_sbf(b, "\n"); }
        }
        sy_sbf(b, "%s\n", o->hint);
        for (int i = 0; i < a->n; i++) if (!(a->v[i].flags & SYI_NOCONF) && !strcmp(a->v[i].kind, "produce")) { sy_sbf(b, "Next: flow %s for the full list of a key; Read the peer range above.\n", a->v[i].name); break; }
    } else {
        for (int i = 0; i < a->n; i++) { const SyItem *it = &a->v[i]; sy_sbf(b, "%s -> %s   %s   [confidence %d %s]\n", it->path, it->sig, it->name, it->conf, sy_level(it->conf)); }
        if (o->total > a->n) sy_sbf(b, "(%d more not shown; raise --limit)\n", o->total - a->n);
        sy_sbf(b, "%s\n", o->hint);
    }
}

/* ---- cascata de nomes de chave ---- */
static int fl_nocc(const SyView *v, const char *k, size_t kl) { KyOccs o = {0}; ky_occs_of(v, k, kl, &o); int n = o.n; free(o.o); return n; }
static int fl_cascade(const SyView *v, const char *q, SyCand *out, int max, int *stepused) {
    size_t ql = strlen(q); int n = 0; *stepused = 0;
    if (fl_nocc(v, q, ql) > 0) { snprintf(out[0].name, sizeof out[0].name, "%s", q); out[0].step = 1; snprintf(out[0].how, sizeof out[0].how, "exact key match"); *stepused = 1; return 1; }
    char qf[128]; sy_fold(q, ql, qf, sizeof qf); char qt[16][32]; int qn = sy_tokens(q, ql, qt, 16);
    SyNameHit *hits = NULL; int nh = 0, hc = 0;
    for (int step = 2; step <= 3 && !n; step++) {
        nh = 0;
        for (int s = 0; s < v->ns; s++) { const SyVSeg *c = &v->s[s]; if (!c->ky.has) continue;
            for (uint32_t i = 0; i < c->ky.names.n; i++) {
                uint32_t l; const char *p = store_strs_get(&c->ky.names, i, &l); if (l == 0 || l > 120 || (l == ql && !memcmp(p, q, l))) continue;
                int score = -1;
                if (step == 2) {
                    char nf[128]; sy_fold(p, l, nf, sizeof nf);
                    if (!strcmp(nf, qf) && qf[0]) score = 0;
                    else if (qn) { char nt[16][32]; int nn = sy_tokens(p, l, nt, 16);
                        for (int a = 0; a + qn <= nn && score < 0; a++) { int ok = 1; for (int b = 0; b < qn; b++) if (strcmp(nt[a + b], qt[b])) { ok = 0; break; } if (ok) score = 1 + (nn - qn); } }
                } else if (ql >= 4 && (int)l >= (int)ql - 2 && (int)l <= (int)ql + 2) {
                    char lo[128], lq[128]; for (uint32_t k = 0; k < l; k++) lo[k] = (char)tolower((unsigned char)p[k]); for (size_t k = 0; k < ql; k++) lq[k] = (char)tolower((unsigned char)q[k]);
                    int d = sy_lev(lo, (int)l, lq, (int)ql); if (d <= 2) score = d;
                }
                if (score < 0) continue;
                int dup = 0; for (int k = 0; k < nh; k++) if (!strncmp(hits[k].name, p, l) && hits[k].name[l] == 0) { dup = 1; break; }
                if (dup) continue;
                if (nh == hc) { hc = hc ? hc * 2 : 64; hits = (SyNameHit *)realloc(hits, sizeof *hits * (size_t)hc); }
                memcpy(hits[nh].name, p, l); hits[nh].name[l] = 0; hits[nh++].score = score;
            } }
        qsort(hits, (size_t)nh, sizeof *hits, sy_hit_cmp);
        for (int k = 0; k < nh && n < max && n < 6; k++) {
            if (fl_nocc(v, hits[k].name, strlen(hits[k].name)) <= 0) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", hits[k].name); out[n].step = step;
            if (step == 2) snprintf(out[n].how, sizeof out[n].how, hits[k].score == 0 ? "no exact key; same text ignoring case and punctuation" : "no exact key; '%s' is part of the key", q);
            else snprintf(out[n].how, sizeof out[n].how, "no exact or derived key; within edit distance %d", hits[k].score);
            n++;
        }
        if (n) *stepused = step;
    }
    free(hits); return n;
}
/* "7" -> numero; texto -> chave. Devolve 1 se arg e arquivo:linha de um arquivo do indice */
static int fl_is_file(const SyView *v, const char *arg, char *spec, size_t cap, int *line) {
    snprintf(spec, cap, "%s", arg); *line = 0;
    char *colon = strrchr(spec, ':'); int hasline = 0;
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) { *line = atoi(colon + 1); *colon = 0; hasline = 1; }
    if (!hasline && fl_nocc(v, arg, strlen(arg)) > 0) {          /* e uma chave... a menos que so exista porque um contrato com este nome de arquivo esta no indice (o proprio arquivo e uma chave de contrato, e o codigo que o cita tambem) */
        KyOccs o = {0}; ky_occs_of(v, arg, strlen(arg), &o); int onlyfile = o.n > 0; for (int i = 0; i < o.n; i++) if (!(o.o[i].cls == CKC_CONTRACT && o.o[i].sub == CKS_FILE)) onlyfile = 0; free(o.o);
        if (!onlyfile) return 0;
    }
    if (!hasline && !strchr(spec, '/') && !strchr(spec, '.')) return 0;
    char rel[4400]; int seg, ns; uint32_t idx; return sy_find_doc(v, spec, rel, sizeof rel, &seg, &idx, &ns);
}

/* classe de uma ocorrencia para o texto: "route", "msg"... ou "contract route", "contract operation (call by name)" */
static void fl_vis(const KyOcc *x, char *b, size_t cap) { snprintf(b, cap, "%s", FL_CLS[x->cls]); }
/* nome do item: a chave (rota com metodo) e, para contrato, o tipo na frente: "operation: createOrder", "route: GET /v1/orders", "enum value: order.created" */
static void fl_item_name(const KyOcc *x, char *b, size_t cap) {
    char l[100]; fl_key_label(x, l, sizeof l);
    if (x->cls == CKC_CONTRACT) snprintf(b, cap, "%s%s: %s", FL_SUBN[x->sub], x->byname == 1 ? " (call by name)" : x->byname == 2 ? " (function by name)" : "", l); else snprintf(b, cap, "%s", l);
}
/* um contrato define o nome de uma operacao ("createOrder"); o cliente escrito a mao chama stub.createOrder(...) sem literal nenhum. As chamadas pelo NOME (indice de referencias)
 * entram no conjunto como CONSUME contract/operation com byname = 1 (confianca menor). So para nomes de 5+ letras e ate 12 arquivos; arquivo que ja tem a chave fica como esta. */
static void fl_add_byname(const SyView *v, const char *key, size_t kl, KyOccs *oc) {
    int hasdef = 0; for (int i = 0; i < oc->n; i++) if (oc->o[i].role == CKR_DEFINE && oc->o[i].cls == CKC_CONTRACT && oc->o[i].sub == CKS_OP) hasdef = 1;
    if (!hasdef || kl < 5) return;
    SyRefs r = {0}; sy_refs_of(v, key, kl, 1u << CR_CALL, &r); int added = 0, n0 = oc->n;
    for (int i = 0; i < r.n && added < 12; i++) {
        const SyRef *f = &r.r[i]; int skip = 0;
        for (int z = 0; z < oc->n && !skip; z++) if (oc->o[z].plen == f->plen && !memcmp(oc->o[z].path, f->path, f->plen)) skip = 1;
        if (skip) continue;
        KyOcc x; memset(&x, 0, sizeof x); x.path = f->path; x.plen = f->plen; x.key = key; x.kl = (uint32_t)kl; x.seg = f->seg; x.doc = f->doc; x.line = f->line;
        x.role = CKR_CONSUME; x.cls = CKC_CONTRACT; x.sub = CKS_OP; x.lang = f->lang; x.byname = 1; ky_push(oc, &x); added++;
    }
    free(r.r);
    /* e as DEFINICOES de funcao/metodo com o nome da operacao (o servidor Go que implementa `func (s *srv) CreateOrder`): byname = 2 */
    SySyms d = {0}; sy_defs_of(v, key, kl, &d);
    for (int i = 0; i < d.n && added < 12; i++) {
        const SySym *f = &d.s[i]; if ((f->kind != CK_FUNC && f->kind != CK_METHOD) || ((f->pack >> 16) & CSF_DECL)) continue;
        int skip = 0; for (int z = 0; z < oc->n && !skip; z++) if (oc->o[z].plen == f->plen && !memcmp(oc->o[z].path, f->path, f->plen)) skip = 1;
        if (skip) continue;
        KyOcc x; memset(&x, 0, sizeof x); x.path = f->path; x.plen = f->plen; x.key = key; x.kl = (uint32_t)kl; x.seg = f->seg; x.doc = f->doc; x.line = f->line0;
        x.role = CKR_CONSUME; x.cls = CKC_CONTRACT; x.sub = CKS_OP; x.lang = f->lang; x.byname = 2; ky_push(oc, &x); added++;
    }
    free(d.s); if (oc->n > n0) qsort(oc->o, (size_t)oc->n, sizeof(KyOcc), ky_cmp);
}
/* a ocorrencia de contrato que sustenta o par produtor -> consumidor (rota/rpc; o metodo nao pode discordar), ou -1 */
static int fl_backing(const KyOccs *oc, const KyOcc *p, const KyOcc *c) {
    int best = -1;
    for (int i = 0; i < oc->n; i++) { const KyOcc *d = &oc->o[i]; if (d->role != CKR_DEFINE || ky_same_file(d, p) || ky_same_file(d, c)) continue;
        if (d->sub == CKS_ROUTE || d->sub == CKS_RPC) { if (d->meth && ((p->meth && p->meth != d->meth) || (c->meth && c->meth != d->meth))) continue; if (best < 0 || (d->meth && (p->meth == d->meth || c->meth == d->meth))) best = i; }
        else if (best < 0) best = i; }
    return best;
}
/* ---- flow <chave> ---- */
typedef struct { int p, c; FlLink L; } FlPair;
static int fl_pair_cmp(const void *a, const void *b) { const FlPair *x = (const FlPair *)a, *y = (const FlPair *)b; return y->L.conf - x->L.conf; }
static int fl_q_key(SyView *v, const char *q, int limit, SyItems *out, FlQ *o) {
    fl_q_begin(o, q, 0);
    SyCand cand[8]; int step, nc = fl_cascade(v, q, cand, 8, &step); int total = 0;
    if (nc) { o->ncand = nc < 8 ? nc : 8; memcpy(o->cand, cand, sizeof(SyCand) * (size_t)o->ncand); o->step = step; for (int i = 0; i < nc && i < 4; i++) { size_t l = strlen(o->resolved); snprintf(o->resolved + l, sizeof o->resolved - l, "%s%s", i ? ", " : "", cand[i].name); } snprintf(o->cascade, sizeof o->cascade, "showing %s (%s)", o->resolved, cand[0].how); }
    char nextbuf[400] = ""; int nlink = 0;
    for (int ci = 0; ci < nc; ci++) {
        KyOccs oc = {0}; ky_occs_of(v, cand[ci].name, strlen(cand[ci].name), &oc); fl_add_byname(v, cand[ci].name, strlen(cand[ci].name), &oc);
        int np = 0, ncn = 0, nd = 0; for (int i = 0; i < oc.n; i++) { if (oc.o[i].role == CKR_PRODUCE) np++; else if (oc.o[i].role == CKR_CONSUME) ncn++; else nd++; }
        const int nfiles = ky_count_files(oc.o, oc.n, 0), npf = ky_count_files(oc.o, oc.n, CKR_PRODUCE), ncf = ky_count_files(oc.o, oc.n, CKR_CONSUME);
        int cls = 0, best = -1; { int cnt[CKC_N] = {0}; for (int i = 0; i < oc.n; i++) cnt[oc.o[i].cls]++; for (int k = 0; k < CKC_N; k++) if (cnt[k] > best) { best = cnt[k]; cls = k; } }
        total += oc.n;
        static const char *const KINDN[] = {"", "produce", "consume", "define"};
        for (int role = CKR_PRODUCE; role <= CKR_DEFINE; role++) {
            int shown = 0;
            for (int i = 0; i < oc.n; i++) { const KyOcc *x = &oc.o[i]; if ((int)x->role != role) continue;
                if (shown >= limit) break; shown++;
                SyItem *it = sy_item_new(out); sy_set_path(it, x->path, x->plen); it->line0 = it->line1 = (int)x->line;
                snprintf(it->kind, sizeof it->kind, "%s", KINDN[role]); fl_item_name(x, it->name, sizeof it->name); fl_where(v, x, it->encl, sizeof it->encl);
                fl_vis(x, it->vis, sizeof it->vis);
                it->ver = fl_verify(v, x); it->conf = sy_conf(cand[ci].step, 1, it->ver, x->cls == CKC_OTHER ? -10 : x->byname ? -12 : 0);
                snprintf(it->how, sizeof it->how, "%s; %s%s", cand[ci].how, it->ver > 0 ? "line verified" : "line NOT verified", x->byname ? "; found by the operation NAME in the symbol index, not by a literal" : "");
                fl_set_line(v, it, x); }
        }
        /* ligacoes produtor -> consumidor (o contrato, quando ha, e o terceiro lado) */
        FlPair *pr = (FlPair *)malloc(sizeof *pr * (size_t)(np * ncn + 1)); int npr = 0;
        if (np && ncn && np * ncn <= 4000) for (int i = 0; i < oc.n; i++) { if (oc.o[i].role != CKR_PRODUCE) continue; for (int j = 0; j < oc.n; j++) { if (oc.o[j].role != CKR_CONSUME) continue;
            pr[npr].p = i; pr[npr].c = j; fl_pair(&oc.o[i], &oc.o[j], nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, &oc.o[j]), &pr[npr].L); npr++; } }
        for (int k = 0; k < npr && nd; k++) if (fl_backing(&oc, &oc.o[pr[k].p], &oc.o[pr[k].c]) >= 0 && !ky_same_file(&oc.o[pr[k].p], &oc.o[pr[k].c])) pr[k].L.conf += 6;          /* o contrato sustenta a ligacao */
        qsort(pr, (size_t)npr, sizeof *pr, fl_pair_cmp);
        int top = npr ? pr[0].L.conf : 0, lshown = 0; (void)cls;
        for (int k = 0; k < npr && lshown < 5; k++) {
            if (pr[k].L.conf < 45 && top >= 45) break;
            if (k > 0 && pr[k].L.conf < top - 30) break;
            const KyOcc *p = &oc.o[pr[k].p], *c = &oc.o[pr[k].c];
            /* so um par por arquivo de cada lado (o primeiro) */
            int dup = 0; for (int z = 0; z < k && !dup; z++) { const KyOcc *p2 = &oc.o[pr[z].p], *c2 = &oc.o[pr[z].c]; if (ky_same_file(p, p2) && ky_same_file(c, c2)) dup = 1; }
            if (dup) continue;
            SyItem *it = sy_item_new(out); sy_set_path(it, p->path, p->plen); it->line0 = it->line1 = (int)p->line; snprintf(it->kind, sizeof it->kind, "link");
            char pw[160], cw[160], cwl[100], lbl[100]; fl_where(v, p, pw, sizeof pw); fl_where(v, c, cw, sizeof cw); (void)cwl; fl_key_label(p, lbl, sizeof lbl);
            snprintf(it->name, sizeof it->name, "%s %s -> %s", FL_PV[p->cls], lbl, FL_PL[c->cls]);
            snprintf(it->sig, sizeof it->sig, "%.*s:%u", (int)c->plen, c->path, c->line); sy_cpy(it->peer_path, sizeof it->peer_path, c->path, c->plen); it->peer_line = (int)c->line; snprintf(it->peer_where, sizeof it->peer_where, "%s", cw);
            snprintf(it->encl, sizeof it->encl, "producer %.*s%s%s; consumer %s", (int)p->plen, p->path, pw[0] ? " " : "", pw, cw[0] ? cw : "(file level)");
            { int bk = nd ? fl_backing(&oc, p, c) : -1; if (bk >= 0) { char dl[100]; fl_key_label(&oc.o[bk], dl, sizeof dl); size_t l = strlen(it->encl); snprintf(it->encl + l, sizeof it->encl - l, "; contract: %.*s:%u %s", (int)oc.o[bk].plen, oc.o[bk].path, oc.o[bk].line, dl); } }
            it->ver = fl_verify(v, p) > 0 && fl_verify(v, c) > 0 ? 1 : -1;
            it->conf = pr[k].L.conf + (it->ver > 0 ? 4 : -25); if (it->conf < 1) it->conf = 1; if (it->conf > 99) it->conf = 99;
            snprintf(it->how, sizeof it->how, "%s; %s", pr[k].L.how, it->ver > 0 ? "both lines verified" : "a line NOT verified");
            if (!nlink) { uint32_t a0, a1; char nm[100]; int hasr = fl_encl(v, c->seg, c->doc, c->line, nm, sizeof nm, &a0, &a1); uint32_t r0 = hasr ? a0 : (c->line > 3 ? c->line - 3 : 1), r1 = hasr ? a1 : c->line + 25; if (r1 - r0 > 60) r1 = r0 + 60;
                snprintf(nextbuf, sizeof nextbuf, "Next: Read %.*s offset=%u limit=%u (the %s side)%s%s", (int)c->plen, c->path, r0, r1 - r0 + 1, FL_CLS[c->cls] == FL_CLS[CKC_ROUTE] ? "serving" : "consuming", hasr ? ", symbol " : "", hasr ? nm : ""); }
            nlink++; lshown++;
        }
        free(pr);
        /* ligacoes contrato -> codigo: cada DEFINE com o que usa a mesma chave em outro arquivo */
        if (nd && np + ncn && (long)nd * (np + ncn) <= 4000) {
            FlPair *cp = (FlPair *)malloc(sizeof *cp * (size_t)(nd * (np + ncn) + 1)); int ncp = 0;
            for (int i = 0; i < oc.n; i++) { if (oc.o[i].role != CKR_DEFINE) continue; for (int j = 0; j < oc.n; j++) { if (oc.o[j].role == CKR_DEFINE || ky_same_file(&oc.o[i], &oc.o[j])) continue;
                cp[ncp].p = i; cp[ncp].c = j; fl_pair_contract(&oc.o[i], &oc.o[j], nfiles, &cp[ncp].L); ncp++; } }
            qsort(cp, (size_t)ncp, sizeof *cp, fl_pair_cmp);
            int ctop = ncp ? cp[0].L.conf : 0, cshown = 0;
            for (int k = 0; k < ncp && cshown < 6; k++) {
                if (cp[k].L.conf < 40 && ctop >= 40) break;
                if (k > 0 && cp[k].L.conf < ctop - 30) break;
                const KyOcc *d = &oc.o[cp[k].p], *u = &oc.o[cp[k].c];
                int dup = 0; for (int z = 0; z < k && !dup; z++) if (ky_same_file(d, &oc.o[cp[z].p]) && ky_same_file(u, &oc.o[cp[z].c])) dup = 1;
                if (dup) continue;
                SyItem *it = sy_item_new(out); sy_set_path(it, d->path, d->plen); it->line0 = it->line1 = (int)d->line; snprintf(it->kind, sizeof it->kind, "clink");
                char dw[160], uw[160], lbl[100]; fl_where(v, d, dw, sizeof dw); fl_where(v, u, uw, sizeof uw); fl_key_label(d, lbl, sizeof lbl);
                snprintf(it->name, sizeof it->name, "specifies %s -> %s", lbl, u->byname == 1 ? "called by name in" : u->byname == 2 ? "implemented by name in" : (u->cls == CKC_CONTRACT && d->sub == CKS_FILE) ? "named in" : (u->cls == CKC_CONTRACT && d->sub == CKS_GQL) ? "queried in" : u->cls == CKC_CONTRACT ? "client code in" : u->role == CKR_CONSUME ? "handled in" : "sent / called from");
                snprintf(it->sig, sizeof it->sig, "%.*s:%u", (int)u->plen, u->path, u->line); sy_cpy(it->peer_path, sizeof it->peer_path, u->path, u->plen); it->peer_line = (int)u->line; snprintf(it->peer_where, sizeof it->peer_where, "%s", uw);
                snprintf(it->encl, sizeof it->encl, "contract %.*s%s%s; %s %.*s%s%s", (int)d->plen, d->path, dw[0] ? " " : "", dw, u->role == CKR_CONSUME ? "consumer" : "producer", (int)u->plen, u->path, uw[0] ? " " : "", uw);
                it->ver = fl_verify(v, d) > 0 && fl_verify(v, u) > 0 ? 1 : -1;
                it->conf = cp[k].L.conf + (it->ver > 0 ? 4 : -25); if (it->conf < 1) it->conf = 1; if (it->conf > 99) it->conf = 99;
                snprintf(it->how, sizeof it->how, "%s; %s", cp[k].L.how, it->ver > 0 ? "both lines verified" : "a line NOT verified");
                cshown++;
            }
            free(cp);
        }
        if (nd) { for (int i = 0; i < oc.n; i++) if (oc.o[i].role == CKR_DEFINE) { char dl[100]; fl_key_label(&oc.o[i], dl, sizeof dl); size_t l = strlen(o->hint);
                snprintf(o->hint + l, sizeof o->hint - l, "%sContract side of '%s': %.*s:%u (%s%s%s).%s", l ? " " : "", cand[ci].name, (int)oc.o[i].plen, oc.o[i].path, oc.o[i].line, FL_SUBN[oc.o[i].sub], strcmp(dl, cand[ci].name) ? " " : "", strcmp(dl, cand[ci].name) ? dl : "",
                    !np && !ncn ? " It defines the key but nobody in the indexed set sends, calls, handles or implements it by literal text." : ""); break; } }
        if (np && !ncn && !nd) { size_t l = strlen(o->hint); snprintf(o->hint + l, sizeof o->hint - l, "%sNobody handles '%s' in the indexed set (%u files analysed for text keys): %d place%s send%s it, none consumes it. A handler built at run time, in another project or reached by other means is not seen.", l ? " " : "", cand[ci].name, v->nkeyflag, np, np == 1 ? "" : "s", np == 1 ? "s" : ""); }
        else if (ncn && !np && !nd) { size_t l = strlen(o->hint); snprintf(o->hint + l, sizeof o->hint - l, "%sNobody sends '%s' in the indexed set (%u files analysed for text keys): %d place%s read%s it, none produces it. The sender may build the text at run time, live in another project or be external (user input, a protocol).", l ? " " : "", cand[ci].name, v->nkeyflag, ncn, ncn == 1 ? "" : "s", ncn == 1 ? "s" : ""); }
        free(oc.o);
    }
    if (nextbuf[0]) snprintf(o->next, sizeof o->next, "%s", nextbuf);      /* so o texto mostra; o JSON leva a nota sem ele */
    char nf[300]; snprintf(nf, sizeof nf, "no text key '%s' in the indexed set (nobody sends, emits, writes, handles or reads it)", q);
    return fl_q_end(v, o, total, nf);
}

/* ---- flow <arquivo>[:<linha>] ---- */
typedef struct { KyOcc o; int n; int conf; char peer[200], how[170]; char encl[100]; char ppath[400]; int pline; char pwhere[160]; } FlRow;
static int fl_row_cmp(const void *a, const void *b) { const FlRow *x = (const FlRow *)a, *y = (const FlRow *)b; if (x->o.role != y->o.role) return (int)x->o.role - (int)y->o.role; if ((x->conf > 0) != (y->conf > 0)) return y->conf > 0 ? 1 : -1; if (x->conf != y->conf) return y->conf - x->conf; return (int)x->o.line - (int)y->o.line; }
static int fl_q_file(SyView *v, const char *spec, int line, int limit, SyItems *out, FlQ *o) {
    char rel[4400]; int seg, ns; uint32_t idx; char nf[700];
    fl_q_begin(o, NULL, 1); if (line > 0) snprintf(o->argbuf, sizeof o->argbuf, "%s:%d", spec, line); else snprintf(o->argbuf, sizeof o->argbuf, "%s", spec); o->arg = o->argbuf;
    if (!sy_find_doc(v, spec, rel, sizeof rel, &seg, &idx, &ns)) { snprintf(nf, sizeof nf, "'%s' is not in the indexed set", spec); return fl_q_end(v, o, 0, nf); }
    const KySec *ky = &v->s[seg].ky;
    if (!ky->has || idx >= ky->docs.n) { snprintf(nf, sizeof nf, "'%s' has no text-key data (indexed before keys existed: delete %s and run `katana coder index` again)", rel, v->dir); return fl_q_end(v, o, 0, nf); }
    const uint8_t *dr = store_table_at(&ky->docs, idx); uint32_t o0 = st_get_u32(dr), no = st_get_u32(dr + 4) & ~SY_KFLAG;
    uint32_t lo = 0, hi = 0xFFFFFFFFu; char rng[200] = "";
    if (line > 0) {
        char nm[100]; uint32_t a0, a1;
        if (fl_encl(v, seg, idx, (uint32_t)line, nm, sizeof nm, &a0, &a1)) { lo = a0; hi = a1; snprintf(rng, sizeof rng, " in %s (%u-%u)", nm, a0, a1); }
        else { lo = line > 10 ? (uint32_t)line - 10 : 1; hi = (uint32_t)line + 10; snprintf(rng, sizeof rng, " around line %d", line); }
    }
    FlRow *rows = (FlRow *)calloc((size_t)no + 1, sizeof *rows); int nr = 0, npro = 0, ncon = 0, ndef = 0;
    for (uint32_t k = 0; k < no; k++) {
        KyOcc x; ky_occ_at(v, seg, o0 + k, &x); if (x.line < lo || x.line > hi) continue;
        if (x.role < CKR_PRODUCE || x.role > CKR_DEFINE) continue;          /* papel desconhecido (armazenamento de versao futura) */
        int dup = -1; for (int i = 0; i < nr; i++) if (rows[i].o.role == x.role && rows[i].o.kl == x.kl && !memcmp(rows[i].o.key, x.key, x.kl) && rows[i].o.meth == x.meth) dup = i;
        if (dup >= 0) { rows[dup].n++; continue; }
        rows[nr].o = x; rows[nr].n = 1; nr++; if (x.role == CKR_PRODUCE) npro++; else if (x.role == CKR_CONSUME) ncon++; else ndef++;
    }
    int linked = 0;
    for (int r = 0; r < nr; r++) {
        FlRow *R = &rows[r]; KyOccs oc = {0}; ky_occs_of(v, R->o.key, R->o.kl, &oc); fl_add_byname(v, R->o.key, R->o.kl, &oc);
        const int nfiles = ky_count_files(oc.o, oc.n, 0), npf = ky_count_files(oc.o, oc.n, CKR_PRODUCE), ncf = ky_count_files(oc.o, oc.n, CKR_CONSUME);
        int bi = -1, bc = 0; FlLink bl; memset(&bl, 0, sizeof bl); int peers = 0;
        for (int i = 0; i < oc.n; i++) { const KyOcc *z = &oc.o[i]; if (z->role == R->o.role || ky_same_file(z, &R->o)) continue; peers++;
            FlLink L; if (R->o.role == CKR_PRODUCE) fl_pair(&R->o, z, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, z), &L); else fl_pair(z, &R->o, nfiles, npf, ncf, ky_file_has(oc.o, oc.n, CKR_PRODUCE, &R->o), &L);
            if (L.conf > bc) { bc = L.conf; bi = i; bl = L; } }
        if (bi >= 0) { const KyOcc *z = &oc.o[bi]; char w[140]; fl_where(v, z, w, sizeof w); snprintf(R->peer, sizeof R->peer, "%.*s:%u%s%s", (int)z->plen, z->path, z->line, w[0] ? " " : "", w); sy_cpy(R->ppath, sizeof R->ppath, z->path, z->plen); R->pline = (int)z->line; snprintf(R->pwhere, sizeof R->pwhere, "%s", w);
            R->conf = bc; snprintf(R->how, sizeof R->how, "%s; %d peer%s elsewhere", bl.how, peers, peers == 1 ? "" : "s"); linked++; }
        else { R->conf = 0; /* nenhum par em outro arquivo */ int same = 0; for (int i = 0; i < oc.n; i++) if (oc.o[i].role != R->o.role && ky_same_file(&oc.o[i], &R->o)) same = 1;
            snprintf(R->peer, sizeof R->peer, "%s", same ? "only in this same file" : R->o.role == CKR_PRODUCE ? "nobody handles it in the indexed set" : R->o.role == CKR_CONSUME ? "nobody sends it in the indexed set" : "no code in the indexed set sends, calls, handles or implements it by this text"); R->how[0] = 0; }
        free(oc.o);
    }
    qsort(rows, (size_t)nr, sizeof *rows, fl_row_cmp);
    int total = nr;
    for (int r = 0; r < nr && out->n < limit; r++) { const FlRow *R = &rows[r];
        SyItem *it = sy_item_new(out); sy_set_path(it, R->o.path, R->o.plen); it->line0 = it->line1 = (int)R->o.line;
        snprintf(it->kind, sizeof it->kind, "%s", R->o.role == CKR_PRODUCE ? "produce" : R->o.role == CKR_CONSUME ? "consume" : "define"); fl_item_name(&R->o, it->name, sizeof it->name); fl_vis(&R->o, it->vis, sizeof it->vis);
        snprintf(it->sig, sizeof it->sig, "%s", R->peer); snprintf(it->peer_path, sizeof it->peer_path, "%s", R->ppath); it->peer_line = R->pline; snprintf(it->peer_where, sizeof it->peer_where, "%s", R->pwhere); it->conf = R->conf ? R->conf : 1; if (!R->conf) it->flags |= SYI_NOCONF;
        snprintf(it->how, sizeof it->how, "%s", R->how[0] ? R->how : "no peer in other files");
        if (R->n > 1) snprintf(it->encl, sizeof it->encl, "%d occurrences", R->n);
        else fl_where(v, &R->o, it->encl, sizeof it->encl);
    }
    snprintf(o->hint, sizeof o->hint, "%s: %d key%s sent, %d received; %d with a peer in another file.", rel, npro, npro == 1 ? "" : "s", ncon, linked);
    if (ndef) { size_t l = strlen(o->hint); snprintf(o->hint + l, sizeof o->hint - l, " %d contract key%s defined in this file.", ndef, ndef == 1 ? "" : "s"); }
    if (total > out->n) { size_t l = strlen(o->hint); snprintf(o->hint + l, sizeof o->hint - l, " (%d more not shown; raise --limit)", total - out->n); }
    snprintf(nf, sizeof nf, "'%s'%s has no text keys (nothing it sends, emits, writes, handles or reads by literal name)", rel, rng);
    if (total) snprintf(o->head, sizeof o->head, "%s%s  [%s]", rel, rng, sy_langname(rows[0].o.lang));
    free(rows);
    return fl_q_end(v, o, total, nf);
}

/* ---- flow --links ---- */
typedef struct { const char *pp, *cp; uint32_t pl, cl; const char *key; uint32_t kl; int conf, cls; } FlEdge;
static int fl_edge_cmp(const void *a, const void *b) { const FlEdge *x = (const FlEdge *)a, *y = (const FlEdge *)b; int c = st_cmp_bytes(x->pp, x->pl, y->pp, y->pl); if (c) return c; c = st_cmp_bytes(x->cp, x->cl, y->cp, y->cl); if (c) return c; return y->conf - x->conf; }
static int fl_key_run_cmp(const void *a, const void *b) { const KyOcc *x = (const KyOcc *)a, *y = (const KyOcc *)b; int c = st_cmp_bytes(x->key, x->kl, y->key, y->kl); if (c) return c; if (x->cls != y->cls) return (int)x->cls - (int)y->cls; return sy_cmp_path_line(x->path, x->plen, (int)x->line, y->path, y->plen, (int)y->line); }
typedef struct { const char *pp, *cp; uint32_t pl, cl; int nk, best, first; char keys[4][70]; int cls; } FlGroup;
static int fl_group_cmp(const void *a, const void *b) { const FlGroup *x = (const FlGroup *)a, *y = (const FlGroup *)b; if (x->nk != y->nk) return y->nk - x->nk; return y->best - x->best; }
static int fl_q_links(SyView *v, int limit, SyItems *out, FlQ *o) {
    fl_q_begin(o, "--links", 2);
    KyOccs all = {0}; ky_all(v, &all); qsort(all.o, (size_t)all.n, sizeof(KyOcc), fl_key_run_cmp);
    FlEdge *ed = NULL; int ne = 0, ec = 0, nkeys = 0, nlinked = 0;
    for (int i = 0; i < all.n; ) {
        int j = i; while (j < all.n && all.o[j].kl == all.o[i].kl && !memcmp(all.o[j].key, all.o[i].key, all.o[i].kl)) j++;          /* todas as classes da mesma chave */
        const KyOcc *kg = all.o + i; const int kn = j - i;
        for (int a = 0; a < kn; ) {          /* sub-grupo por classe: produtor x consumidor, como antes */
            int b = a; while (b < kn && kg[b].cls == kg[a].cls) b++;
            nkeys++;
            const KyOcc *g = kg + a; int n = b - a, np = 0, nc = 0; for (int k = 0; k < n; k++) { if (g[k].role == CKR_PRODUCE) np++; else if (g[k].role == CKR_CONSUME) nc++; }
            if (np && nc && (long)np * nc <= 3000) {
                const int nfiles = ky_count_files(g, n, 0), npf = ky_count_files(g, n, CKR_PRODUCE), ncf = ky_count_files(g, n, CKR_CONSUME); int any = 0;
                for (int p = 0; p < n; p++) { if (g[p].role != CKR_PRODUCE) continue; for (int c = 0; c < n; c++) { if (g[c].role != CKR_CONSUME || ky_same_file(&g[p], &g[c])) continue;
                    FlLink L; fl_pair(&g[p], &g[c], nfiles, npf, ncf, ky_file_has(g, n, CKR_PRODUCE, &g[c]), &L); if (L.conf < 45) continue;
                    if (ne == ec) { ec = ec ? ec * 2 : 256; ed = (FlEdge *)realloc(ed, sizeof(FlEdge) * (size_t)ec); }
                    ed[ne].pp = g[p].path; ed[ne].pl = g[p].plen; ed[ne].cp = g[c].path; ed[ne].cl = g[c].plen; ed[ne].key = g[p].key; ed[ne].kl = g[p].kl; ed[ne].conf = L.conf; ed[ne].cls = (int)g[p].cls; ne++; any = 1; } }
                nlinked += any;
            }
            a = b;
        }
        { int nd = 0, nu = 0; for (int k = 0; k < kn; k++) { if (kg[k].role == CKR_DEFINE) nd++; else nu++; }
          if (nd && nu && (long)nd * nu <= 3000) {          /* o contrato e um lado: contrato -> arquivo que usa a chave */
              const int nfiles = ky_count_files(kg, kn, 0); int any = 0;
              for (int d = 0; d < kn; d++) { if (kg[d].role != CKR_DEFINE) continue; for (int u = 0; u < kn; u++) { if (kg[u].role == CKR_DEFINE || ky_same_file(&kg[d], &kg[u])) continue;
                  FlLink L; fl_pair_contract(&kg[d], &kg[u], nfiles, &L); if (L.conf < 45) continue;
                  if (ne == ec) { ec = ec ? ec * 2 : 256; ed = (FlEdge *)realloc(ed, sizeof(FlEdge) * (size_t)ec); }
                  ed[ne].pp = kg[d].path; ed[ne].pl = kg[d].plen; ed[ne].cp = kg[u].path; ed[ne].cl = kg[u].plen; ed[ne].key = kg[d].key; ed[ne].kl = kg[d].kl; ed[ne].conf = L.conf; ed[ne].cls = CKC_CONTRACT; ne++; any = 1; } }
              nlinked += any; } }
        i = j;
    }
    qsort(ed, (size_t)ne, sizeof *ed, fl_edge_cmp);
    FlGroup *gr = (FlGroup *)calloc((size_t)ne + 1, sizeof *gr); int ng = 0;
    for (int i = 0; i < ne; i++) {
        FlGroup *G = ng ? &gr[ng - 1] : NULL;
        if (!G || G->pl != ed[i].pl || G->cl != ed[i].cl || memcmp(G->pp, ed[i].pp, ed[i].pl) || memcmp(G->cp, ed[i].cp, ed[i].cl)) { G = &gr[ng++]; G->pp = ed[i].pp; G->pl = ed[i].pl; G->cp = ed[i].cp; G->cl = ed[i].cl; G->first = i; }
        int dup = 0; for (int k = G->first; k < i && !dup; k++) if (ed[k].kl == ed[i].kl && !memcmp(ed[k].key, ed[i].key, ed[i].kl)) dup = 1;      /* a mesma chave em varias linhas conta uma vez */
        if (!dup) { if (G->nk < 4) { snprintf(G->keys[G->nk], sizeof G->keys[0], "%.*s", ed[i].kl < 68 ? (int)ed[i].kl : 68, ed[i].key); } G->nk++; }
        if (ed[i].conf > G->best) { G->best = ed[i].conf; G->cls = ed[i].cls; }
    }
    qsort(gr, (size_t)ng, sizeof *gr, fl_group_cmp);
    for (int i = 0; i < ng && out->n < limit; i++) { const FlGroup *G = &gr[i];
        SyItem *it = sy_item_new(out); sy_set_path(it, G->pp, G->pl); it->line0 = it->line1 = 1; snprintf(it->kind, sizeof it->kind, "link");
        char ks[260] = ""; for (int k = 0; k < G->nk && k < 4; k++) { size_t l = strlen(ks); snprintf(ks + l, sizeof ks - l, "%s%s", k ? ", " : "", G->keys[k]); } if (G->nk > 4) { size_t l = strlen(ks); snprintf(ks + l, sizeof ks - l, " (+%d)", G->nk - 4); }
        snprintf(it->name, sizeof it->name, "%.*s", (int)(sizeof it->name - 1), ks); snprintf(it->sig, sizeof it->sig, "%.*s", (int)G->cl, G->cp); sy_cpy(it->peer_path, sizeof it->peer_path, G->cp, G->cl);
        it->conf = G->best; snprintf(it->how, sizeof it->how, "%d key%s link%s them (best: %s key; line text not reread in this summary)", G->nk, G->nk == 1 ? "" : "s", G->nk == 1 ? "s" : "", FL_CLS[G->cls]); }
    snprintf(o->hint, sizeof o->hint, "%d file pair%s linked by %d shared text key%s (of %d distinct keys with occurrences); only links at confidence >= 45 are counted. Next: flow <key> for the lines, flow <file> for one file.", ng, ng == 1 ? "" : "s", nlinked, nlinked == 1 ? "" : "s", nkeys);
    free(gr); free(ed); free(all.o);
    return fl_q_end(v, o, ng, "no text-key links between files in the indexed set (no key is produced in one file and consumed in another)");
}

/* arg e arquivo[:linha] do indice ou chave de texto? */
static int fl_q_arg(SyView *v, const char *arg, int limit, SyItems *out, FlQ *o) {
    char spec[4400]; int line;
    if (fl_is_file(v, arg, spec, sizeof spec, &line)) { fl_q_file(v, spec, line, limit, out, o); o->arg = arg; return o->total; }
    return fl_q_key(v, arg, limit, out, o);
}
static int coder_flow_main(int argc, char **argv) {
    SyArgs a; SyView v; int links = 0, brief = 0; int rc = fl_start("flow <chave|arquivo[:linha]> [pasta] [--json] [--limit N]  |  flow --links [pasta]", argc, argv, &a, &v, &links, &brief); if (rc) return rc;
    SyItems items = {0}; FlQ q; CtSb sb = {0};
    if (links) fl_q_links(&v, a.limit, &items, &q); else fl_q_arg(&v, a.pos[0], a.limit, &items, &q);
    q.brief = brief;
    if (a.json) fl_render_json(&sb, &q, &items); else fl_render_text(&sb, &q, &items);
    sy_put(&sb); sy_items_free(&items); sy_view_close(&v); return q.total ? 0 : 1;
}
