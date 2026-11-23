/* ---------- uioe/interpret.c: Interpretar (docs/uioe.md 3.3, fase U1) ----------
 * O pedido em texto vira um UioeAsk: idioma, entidades de codigo e de arquivos, lugares citados, bits de relacao, restricoes,
 * termos informativos (com o conceito da base de conhecimento), palavras descartadas e ambiguidade. Deterministico: regras, listas,
 * o tokenizador de coder/text.c e a base de conhecimento de coder/kb.c. Nenhum modelo, nenhum embedding, nenhuma chamada de rede.
 * O que NAO faz em U1: consultar o indice (entidade "resolvida" aqui quer dizer reconhecida pela forma ou achada no disco; a
 * resolucao pela cascata exata/derivada/provavel e de U3), datas normalizadas para intervalo, valores em dinheiro alem da forma.
 * Decisoes do implementador (o documento deixava abertas):
 *   - tudo e feito sobre o texto com os acentos dobrados (ct_fold) e as aspas curvas trocadas por retas; o texto mostrado ao dono
 *     fica sem acentos por isso.
 *   - as listas de pistas usam "*" para prefixo ("envi*" casa envia, enviar, enviado) e "..." para "a antes de b, ate 14 palavras depois".
 *   - texto citado entre aspas retas, aspas curvas ou crases: uma palavra so entre crases e simbolo; duas ou mais palavras e literal
 *     (ou texto de erro, se tiver a forma). Aspa simples nao conta (apostrofo).
 *   - texto de erro: a partir de `error:`, `erro:`, `exception`, `traceback`, `panic`... ate o fim do pedido (no maximo 190 bytes);
 *     se a palavra-chave vem depois de um determinante ("este erro: <log>") o literal e so o que vem depois dos dois pontos.
 *   - nome proprio: palavra capitalizada que nao abre frase, nao e palavra funcional e nao tem forma de identificador; sempre
 *     marcada "guess" (reconhecida so pela forma). */
#define UW_MAX 240
typedef struct { char w[UW_MAX][32]; int n; } UioeWords;

/* ---- listas de extensoes (sem ponto, minusculas) ---- */
static const char *const g_ui_ext_code[] = {"c", "h", "cc", "cpp", "hpp", "cxx", "cs", "java", "kt", "swift", "py", "rb", "php", "js", "mjs", "cjs", "ts", "tsx", "jsx", "go", "rs",
    "lua", "sh", "bash", "zsh", "ps1", "bat", "sql", "dart", "scala", "pl", "ex", "exs", "erl", "hs", "clj", "lisp", "qml", "vue", "svelte", "glsl", "comp", "vert", "frag", "mk", "cmake", "gradle", NULL};
static const char *const g_ui_ext_doc[] = {"md", "txt", "rst", "json", "yaml", "yml", "toml", "ini", "cfg", "conf", "xml", "html", "css", "scss", "csv", "tsv", "pdf", "docx", "xlsx", "pptx", "odt",
    "png", "jpg", "jpeg", "gif", "svg", "zip", "tar", "gz", "log", "env", "kb", "lock", "gguf", NULL};
/* 1 codigo, 2 documento ou dado, 0 desconhecida */
static int ui_ext_class(const char *e) {
    for (int i = 0; g_ui_ext_code[i]; i++) if (!strcmp(e, g_ui_ext_code[i])) return 1;
    for (int i = 0; g_ui_ext_doc[i]; i++) if (!strcmp(e, g_ui_ext_doc[i])) return 2;
    return 0;
}
/* extensao conhecida do ultimo componente de c; devolve a classe (0 = nenhuma) e a extensao em ext */
static int ui_path_ext(const char *c, char *ext, size_t cap) {
    const char *sl = strrchr(c, '/'); const char *base = sl ? sl + 1 : c;
    const char *dot = strrchr(base, '.'); ext[0] = 0;
    if (!dot || dot == base || !dot[1]) return 0;
    size_t n = strlen(dot + 1); if (n >= cap || n > 6) return 0;
    for (size_t i = 0; i < n; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[n] = 0;
    return ui_ext_class(ext);
}

/* ---- texto: aspas curvas -> retas, acentos dobrados; devolve o tamanho. f guarda a caixa; lf e a copia minuscula ---- */
static size_t ui_prep(const char *in, char *f, size_t cap) {
    char *t = (char *)malloc(strlen(in) + 1); if (!t) { f[0] = 0; return 0; }
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ) {
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) { t[o++] = '"'; p += 3; }
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) { t[o++] = '\''; p += 3; }
        else t[o++] = (char)*p++;
    }
    t[o] = 0;
    int *src = (int *)malloc(sizeof(int) * (cap + 2)); if (!src) { free(t); f[0] = 0; return 0; }
    const int k = ct_fold(t, (int)o, f, src, (int)cap - 1);
    f[k] = 0; free(src); free(t);
    return (size_t)k;
}
static void ui_lower(const char *f, char *lf, size_t n) { for (size_t i = 0; i <= n; i++) lf[i] = (char)tolower((unsigned char)f[i]); }

/* ---- palavras e pistas ---- */
static void uw_build(UioeWords *W, const char *lf) {
    W->n = 0;
    for (const char *p = lf; *p && W->n < UW_MAX; ) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) { p++; continue; }
        int l = 0; char *w = W->w[W->n];
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) { if (l < 31) w[l++] = *p; p++; }
        w[l] = 0; W->n++;
    }
}
/* uma palavra da pista ("envi*" ou "dia") contra a palavra w */
static int uw_wordeq(const char *cue, int cl, const char *w) {
    if (cl > 0 && cue[cl - 1] == '*') return strncmp(w, cue, (size_t)cl - 1) == 0;
    return (int)strlen(w) == cl && strncmp(w, cue, (size_t)cl) == 0;
}
/* a frase (palavras separadas por espaco) comeca na palavra i? devolve o indice depois dela ou -1 */
static int uw_phrase_at(const UioeWords *W, int i, const char *ph) {
    const char *p = ph;
    while (*p) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ') e++;
        if (i >= W->n || !uw_wordeq(p, (int)(e - p), W->w[i])) return -1;
        i++; p = e;
    }
    return i;
}
/* primeira posicao >= from onde a frase casa; *end recebe o fim */
static int uw_find(const UioeWords *W, int from, int to, const char *ph, int *end) {
    for (int i = from; i < W->n && i <= to; i++) { const int e = uw_phrase_at(W, i, ph); if (e >= 0) { if (end) *end = e; return i; } }
    return -1;
}
/* pista com "..." entre partes: a segunda ate 14 palavras depois do fim da primeira; devolve a posicao da primeira palavra ou -1 */
static int uw_cue(const UioeWords *W, const char *cue) {
    const char *gap = strstr(cue, "...");
    if (!gap) return uw_find(W, 0, W->n, cue, NULL);
    char a[96], b[96]; snprintf(a, sizeof a, "%.*s", (int)(gap - cue), cue); snprintf(b, sizeof b, "%s", gap + 3);
    for (char *q = a + strlen(a); q > a && q[-1] == ' '; ) *--q = 0;
    const char *bp = b; while (*bp == ' ') bp++;
    int from = 0;
    for (;;) {
        int e; const int s = uw_find(W, from, W->n, a, &e);
        if (s < 0) return -1;
        if (uw_find(W, e, e + 14, bp, NULL) >= 0) return s;
        from = s + 1;
    }
}
/* alguma das pistas da lista (terminada em NULL) casa? devolve a primeira que casou em *which; o retorno e a posicao */
static int uw_any(const UioeWords *W, const char *const *list, const char **which) {
    int best = -1;
    for (int i = 0; list[i]; i++) { const int p = uw_cue(W, list[i]); if (p >= 0 && (best < 0 || p < best)) { best = p; if (which) *which = list[i]; } }
    return best;
}

/* ---- entidades ---- */
static UioeEntity *ui_ent_add(UioeAsk *a, int kind, const char *text, const char *how, int conf, int guess) {
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == kind && !strcmp(a->ent[i].text, text)) return &a->ent[i];
    if (a->nent >= UIOE_NENT) return NULL;
    UioeEntity *e = &a->ent[a->nent++]; memset(e, 0, sizeof *e);
    e->kind = (char)kind; snprintf(e->text, sizeof e->text, "%s", text); snprintf(e->how, sizeof e->how, "%s", how); e->conf = conf; e->guess = (uint8_t)guess;
    return e;
}
static int ui_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
/* a pasta e um projeto? marcador de projeto ou arquivos de codigo no primeiro nivel */
static int ui_is_project(const char *dir) {
    static const char *const mk[] = {".git", ".katana", "package.json", "Makefile", "build.sh", "build.bat", "CMakeLists.txt", "Cargo.toml", "go.mod", "pyproject.toml", "setup.py", "pom.xml", "KATANA.md", "AGENTS.md", NULL};
    char p[1400];
    for (int i = 0; mk[i]; i++) { snprintf(p, sizeof p, "%s/%s", dir, mk[i]); if (uioe_exists(p)) return 1; }
    DIR *d = opendir(dir); if (!d) return 0;
    int n = 0, code = 0; struct dirent *de; char ext[12];
    while ((de = readdir(d)) && n < 300) { n++; if (ui_path_ext(de->d_name, ext, sizeof ext) == 1) code++; }
    closedir(d);
    return code >= 2;
}
/* resolve o texto de um caminho no disco: absoluto, ~/, relativo a raiz ou a pasta de trabalho */
static int ui_resolve_path(const UioeCtx *c, const char *t, char *out, size_t cap) {
    char tmp[2400];
    if (t[0] == '/') snprintf(tmp, sizeof tmp, "%s", t);
    else if (t[0] == '~') { char h[1100]; agent_home(h, sizeof h); snprintf(tmp, sizeof tmp, "%s%s", h, t + 1); }
    else { snprintf(tmp, sizeof tmp, "%.1090s/%.1000s", c->root, t); if (!uioe_exists(tmp)) snprintf(tmp, sizeof tmp, "%.1090s/%.1000s", c->cwd, t); }
    if (!uioe_exists(tmp)) return 0;
    char rp[4200]; if (!realpath(tmp, rp)) snprintf(rp, sizeof rp, "%s", tmp);
    snprintf(out, cap, "%s", rp); return 1;
}
/* texto de erro: a partir da palavra-chave ate o fim; devolve o byte de inicio do trecho (no folded) ou -1 */
static int ui_find_errtext(const char *f, const char *lf, int *kwpos) {
    static const char *const kw[] = {"error:", "erro:", "fatal:", "panic:", "exception", "traceback", "segmentation fault", "undefined reference", "stack trace", "stacktrace", NULL};
    int best = -1;
    for (int i = 0; kw[i]; i++) { const char *p = strstr(lf, kw[i]); if (p && (best < 0 || p - lf < best)) best = (int)(p - lf); }
    (void)f; if (kwpos) *kwpos = best;
    return best;
}
static int ui_chunk_is_ident(const char *c) { if (!*c) return 0; for (; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '_')) return 0; return 1; }
static int ui_has_lower_then_upper(const char *c) { for (; c[0] && c[1]; c++) if (islower((unsigned char)c[0]) && isupper((unsigned char)c[1])) return 1; return 0; }

/* classifica um pedaco (ja sem pontuacao nas pontas); devolve 1 se virou entidade */
static int ui_chunk(const UioeCtx *cx, UioeAsk *a, const char *c, int sentence_initial) {
    const size_t n = strlen(c); char ext[12], rs[1100];
    if (n < 2 || n > 190) return 0;
    if (!strncmp(c, "http://", 7) || !strncmp(c, "https://", 8) || !strncmp(c, "www.", 4)) { ui_ent_add(a, UK_URL, c, "scheme", 95, 0); return 1; }
    /* AAAA-MM-DD */
    if (n == 10 && isdigit((unsigned char)c[0]) && c[4] == '-' && c[7] == '-') { ui_ent_add(a, UK_DATE, c, "iso date", 95, 0); return 1; }
    if (((c[0] == 'R' || c[0] == 'U') && c[1] == '$' && isdigit((unsigned char)c[2])) || (c[0] == '$' && isdigit((unsigned char)c[1]))) { ui_ent_add(a, UK_AMOUNT, c, "currency sign", 80, 0); return 1; }
    char buf[200]; snprintf(buf, sizeof buf, "%s", c);
    /* arquivo:linha */
    { char *col = strrchr(buf, ':'); if (col && col[1] && strspn(col + 1, "0123456789") == strlen(col + 1) && col > buf) *col = 0; }
    int call = 0; { size_t bl = strlen(buf); if (bl > 2 && !strcmp(buf + bl - 2, "()")) { buf[bl - 2] = 0; call = 1; } }
    if (strchr(buf, '/')) {
        const int slashes = (int)(strchr(buf, '/') ? 1 : 0) + (int)(strchr(strchr(buf, '/') + 1, '/') ? 1 : 0);
        const int xc = ui_path_ext(buf, ext, sizeof ext);
        const int lead = buf[0] == '/' || buf[0] == '~' || (buf[0] == '.' && (buf[1] == '/' || (buf[1] == '.' && buf[2] == '/')));
        if (!(xc || slashes >= 2 || lead)) return 0;      /* "pt/en", "and/or" nao sao caminhos */
        UioeEntity *e = ui_ent_add(a, xc ? UK_PATH : UK_FOLDER, buf, xc ? "path shape with a known extension" : "path shape", 50, 0);
        if (e && ui_resolve_path(cx, buf, rs, sizeof rs)) { snprintf(e->resolved, sizeof e->resolved, "%s", rs); e->conf = 90; snprintf(e->how, sizeof e->how, "exists on disk"); }
        else if (e && xc) { e->conf = 40; snprintf(e->how, sizeof e->how, "file path not found from the working folder (suffix match in U3)"); }
        return 1;
    }
    { const int xc = ui_path_ext(buf, ext, sizeof ext);
      if (xc && ui_chunk_is_ident(buf) == 0 && !strchr(buf, ' ')) {
          UioeEntity *e = ui_ent_add(a, UK_PATH, buf, "file name with a known extension", 55, 0);
          if (e && ui_resolve_path(cx, buf, rs, sizeof rs)) { snprintf(e->resolved, sizeof e->resolved, "%s", rs); e->conf = 90; snprintf(e->how, sizeof e->how, "exists on disk"); }
          return 1;
      } }
    /* MAIUSCULAS_COM_SUBLINHADO */
    { int up = 1, us = 0, has = 0; for (const char *p = buf; *p; p++) { if (*p == '_') us++; else if (isupper((unsigned char)*p)) has = 1; else if (!isdigit((unsigned char)*p)) up = 0; }
      if (up && us && has && strlen(buf) >= 4 && buf[0] != '_') { ui_ent_add(a, UK_ENV, buf, "ALL_CAPS_WITH_UNDERSCORE", !strncmp(buf, "KATANA_", 7) ? 85 : 70, 0); return 1; } }
    if (ui_chunk_is_ident(buf) && strlen(buf) >= 3) {
        int us = 0, lo = 0; for (const char *p = buf; *p; p++) { if (*p == '_') us++; if (islower((unsigned char)*p)) lo = 1; }
        if ((us && lo && buf[0] != '_') || ui_has_lower_then_upper(buf) || call) { ui_ent_add(a, UK_SYMBOL, buf, call ? "call parentheses" : us ? "snake_case identifier shape" : "camelCase identifier shape", call ? 75 : 70, 1); return 1; }
    }
    if (strstr(buf, "::") || strstr(buf, "->")) { ui_ent_add(a, UK_SYMBOL, buf, "scope operator shape", 70, 1); return 1; }
    if (call && strlen(buf) >= 2) { ui_ent_add(a, UK_SYMBOL, buf, "call parentheses", 70, 1); return 1; }
    /* nome proprio: Capitalizada, so letras, nao abre frase, nao e palavra funcional */
    if (!sentence_initial && n >= 3 && isupper((unsigned char)buf[0]) && strlen(buf) == n) {
        int ok = 1; for (const char *p = buf + 1; *p; p++) if (!islower((unsigned char)*p)) ok = 0;
        if (ok) { char lw[48]; int l = 0; for (const char *p = buf; *p && l < 47; p++) lw[l++] = (char)tolower((unsigned char)*p); lw[l] = 0; ct_init();
            if (!ctm_get(&g_ct_words, lw, l)) { ui_ent_add(a, UK_NAME, buf, "capitalized word, not sentence-initial (shape only)", 35, 1); return 1; } }
    }
    return 0;
}

/* constantes de pistas usadas por interpret e classify */
static const char *const g_ui_c_sendcue[] = {"envi*", "mand*", "emit*", "publica*", "transmit*", "send*", "sent", "post*", "push*", "dispatch*", "notifica*", NULL};
static const char *const g_ui_c_recvcue[] = {"receb*", "recebe*", "chega*", "chegam", "trata*", "escuta*", "ouve", "handl*", "receiv*", "arriv*", "listen*", "reach*", "consome*", "consum*", NULL};
static const char *const g_ui_c_change[] = {"mud*", "alter*", "troc*", "remov*", "remova*", "apag*", "renome*", "corrij*", "corrig*", "implement*", "adicion*", "cri*", "refator*", "reescrev*", "atualiz*", "edit*",
    "change", "modify", "rename", "remove", "delete", "fix", "implement", "add", "create", "refactor", "rewrite", "update", "write", "make", NULL};
static const char *const g_ui_c_hypo[] = {"se eu", "caso eu", "se a gente", "if i", "if we", "what if", "quando eu", "se for", NULL};
static const char *const g_ui_c_firstp[] = {"como eu", "como faco", "como posso", "como se", "como instal*", "como us*", "como rod*", "como execut*", "como configur*", "como compil*",
    "how do i", "how can i", "how to", "how should i", "how do you", "how can we", NULL};
static const char *const g_ui_generic[] = {"projeto", "projetos", "pasta", "folder", "project", "repo", "repositorio", "repository", "diretorio", "chamado", "chamada", "named", "called", "sabe", "saber", "tenho",
    "tem", "tenha", "quero", "queria", "gostaria", "existe", "arquivo", "arquivos", "file", "files", "favor", "please", NULL};

static void ui_add_drop(UioeAsk *a, const char *w) {
    for (int i = 0; i < a->ndropped; i++) if (!strcmp(a->dropped[i], w)) return;
    if (a->ndropped < 24) snprintf(a->dropped[a->ndropped++], sizeof a->dropped[0], "%s", w);
}

/* U3: uma palavra em ingles/codigo de um conceito do termo k (para casar com o codigo), sem repetir */
static void ui_xadd(UioeAsk *a, int k, const char *w) {
    char lw[24]; size_t l = strlen(w); if (a->nxw >= 64 || l < 3 || l > 23) return;
    for (size_t i = 0; i < l; i++) { lw[i] = (char)tolower((unsigned char)w[i]); if (!isalnum((unsigned char)lw[i]) && lw[i] != '_') return; }
    lw[l] = 0;
    for (int i = 0; i < a->nxw; i++) if (!strcmp(a->xw[i], lw)) return;
    snprintf(a->xw[a->nxw], sizeof a->xw[0], "%s", lw); a->xw_of[a->nxw++] = k;
}
/* termos informativos e conceitos, a partir do texto sem as entidades */
static void ui_terms(UioeAsk *a, const char *work, const CoderKb *kb) {
    CoderTok *tk = (CoderTok *)malloc(sizeof(CoderTok) * 160); if (!tk) return;
    const int nt = coder_tokens(work, tk, 160);
    int pt = 0, en = 0;
    for (int i = 0; i < nt; i++) {
        const CoderTok *t = &tk[i];
        if (t->kind == 2) continue;
        if (t->lang == CT_L_PT) pt++; else if (t->lang == CT_L_EN) en++;
        if (t->kind == 1) continue;
        if (t->stop == 1) continue;
        char lw[48]; int l = 0; for (const char *p = t->orig; *p && l < 47; p++) lw[l++] = (char)tolower((unsigned char)*p); lw[l] = 0;
        int gen = t->stop == 2; for (int g = 0; !gen && g_ui_generic[g]; g++) if (!strcmp(lw, g_ui_generic[g])) gen = 1;
        if (gen) { ui_add_drop(a, lw); continue; }
        if (strlen(lw) < 3 && !isdigit((unsigned char)lw[0])) continue;
        int dup = 0; for (int k = 0; k < a->nterms; k++) if (!strcmp(a->norm[k], t->norm)) dup = 1;
        if (dup || a->nterms >= 24) continue;
        const int k = a->nterms++;
        snprintf(a->terms[k], sizeof a->terms[0], "%s", lw); snprintf(a->norm[k], sizeof a->norm[0], "%s", t->norm); a->concept[k][0] = 0;
        if (kb && kb->nterm) {
            int ix[4]; int nc = coder_kb_term_concepts(kb, lw, ix, 4); if (!nc) nc = coder_kb_term_concepts(kb, t->norm, ix, 4);
            if (nc) {
                snprintf(a->concept[k], sizeof a->concept[0], "%s", kb->con[kb->term[ix[0]].con].id);
                const KbConcept *cc = &kb->con[kb->term[ix[0]].con];
                for (int pass = 0; pass < 4; pass++) {       /* as palavras nao portuguesas de maior peso do conceito, uma por passo */
                    int bq = -1; float bw = -1;
                    for (int q = cc->t0; q < cc->t0 + cc->nt && q < kb->nterm; q++) { if (kb->term[q].lang == 0 || kb->term[q].w <= bw) continue;
                        int dup = 0; for (int z = 0; z < a->nxw; z++) if (!strcasecmp(a->xw[z], kb->term[q].term)) dup = 1; if (dup || strlen(kb->term[q].term) < 3 || strlen(kb->term[q].term) > 23 || strchr(kb->term[q].term, ' ')) continue;
                        bq = q; bw = kb->term[q].w; }
                    if (bq < 0) break; ui_xadd(a, k, kb->term[bq].term);
                }
            } else { int vx[4]; int nv = coder_kb_verb_ops(kb, t->norm, vx, 4); if (!nv) nv = coder_kb_verb_ops(kb, lw, vx, 4);
                   if (nv) { snprintf(a->concept[k], sizeof a->concept[0], "op:%s", kb->op[kb->verb[vx[0]].op].name);
                             const KbOp *oo = &kb->op[kb->verb[vx[0]].op]; int added = 0;
                             for (int q = oo->v0; q < oo->v0 + oo->nv && q < kb->nverb && added < 6; q++) if (kb->verb[q].lang != 0) { const int before = a->nxw; ui_xadd(a, k, kb->verb[q].w); if (a->nxw > before) added++; } } }
        }
    }
    snprintf(a->lang, sizeof a->lang, "%s", pt > en ? "pt" : en > pt ? "en" : "?");
    free(tk);
}

/* a funcao principal: texto -> UioeAsk. cx traz a pasta de trabalho e a raiz. */
static void uioe_interpret(UioeCtx *cx, const char *text_in, UioeAsk *a) {
    memset(a, 0, sizeof *a);
    a->origin = UO_CLI; snprintf(a->lang, sizeof a->lang, "?");
    while (text_in[0] == '[' && strncmp(text_in, "[katana:", 8) == 0) { const char *nl = strchr(text_in, '\n'); if (!nl) break; text_in = nl + 1; }
    snprintf(a->text, sizeof a->text, "%s", text_in);
    char *f = (char *)malloc(sizeof a->text + 8), *lf = (char *)malloc(sizeof a->text + 8), *work = (char *)malloc(sizeof a->text + 8);
    if (!f || !lf || !work) { free(f); free(lf); free(work); return; }
    const size_t n = ui_prep(a->text, f, sizeof a->text);
    ui_lower(f, lf, n); memcpy(work, f, n + 1);
    snprintf(a->text, sizeof a->text, "%s", f);       /* o texto mostrado e o dobrado */

    /* 1. texto de erro: do indicador ate o fim */
    { int kp; const int ep = ui_find_errtext(f, lf, &kp);
      if (ep >= 0) {
          int s = ep; const char *col = strchr(f + ep, ':');
          const char *kwc = f + ep; int kwlen = 0; while (kwc[kwlen] && kwc[kwlen] != ':' && kwc[kwlen] != ' ') kwlen++;
          int det = 0; { int q = ep - 1; while (q >= 0 && f[q] == ' ') q--; int e2 = q; while (q >= 0 && f[q] != ' ') q--; char pw[16]; int pl = e2 - q; if (pl > 0 && pl < 15) { memcpy(pw, f + q + 1, (size_t)pl); pw[pl] = 0; for (int i = 0; pw[i]; i++) pw[i] = (char)tolower((unsigned char)pw[i]);
              det = !strcmp(pw, "este") || !strcmp(pw, "esse") || !strcmp(pw, "this") || !strcmp(pw, "o") || !strcmp(pw, "um") || !strcmp(pw, "the") || !strcmp(pw, "seguinte") || !strcmp(pw, "a") || !strcmp(pw, "esta") || !strcmp(pw, "essa"); } }
          (void)kwlen;
          if (det && col && col[1]) { s = (int)(col - f) + 1; while (f[s] == ' ') s++; }
          if (f[s]) { char lit[200]; snprintf(lit, sizeof lit, "%.190s", f + s); for (char *q = lit + strlen(lit); q > lit && (q[-1] == '\n' || q[-1] == ' '); ) *--q = 0;
              if (lit[0]) ui_ent_add(a, UK_ERRTEXT, lit, det && col ? "text after the error cue" : "error cue and the text after it", 85, 0); }
          memset(work + ep, ' ', n - (size_t)ep);
      } }
    /* 2. texto citado */
    for (size_t i = 0; i < n; i++) {
        const char q = work[i];
        if (q != '"' && q != '`') continue;
        size_t j = i + 1; while (j < n && work[j] != q) j++;
        if (j >= n) break;
        char in[200]; const size_t l = j - i - 1 < 190 ? j - i - 1 : 190; memcpy(in, work + i + 1, l); in[l] = 0;
        int words = 0, sp = 1; for (const char *p = in; *p; p++) { if (*p == ' ') sp = 1; else if (sp) { words++; sp = 0; } }
        if (l >= 2) {
            if (q == '`' && words == 1) ui_ent_add(a, UK_SYMBOL, in, "between backticks", 80, 1);
            else if (words >= 2 || strchr(in, ':')) ui_ent_add(a, UK_LITERAL, in, q == '`' ? "backtick text, two or more words" : "quoted text, two or more words", 85, 0);
            else if (words == 1) { UioeAsk tmp; (void)tmp; if (!ui_chunk(cx, a, in, 0)) ui_ent_add(a, UK_LITERAL, in, "quoted word", 60, 0); }
        }
        memset(work + i, ' ', j - i + 1); i = j;
    }
    /* 3. pedacos */
    int sent_start = 1;
    for (size_t i = 0; i < n; ) {
        if (isspace((unsigned char)work[i])) { i++; continue; }
        size_t j = i; while (j < n && !isspace((unsigned char)work[j])) j++;
        size_t s = i, e = j;
        while (s < e && strchr("([{<\"'`", work[s])) s++;
        while (e > s && strchr(")]}>\"'`,;:?!.", work[e - 1])) e--;
        const int endsent = j > 0 && strchr(".?!:", work[j - 1]) != NULL;
        if (e > s) {
            char c[200]; const size_t l = e - s < 199 ? e - s : 199; memcpy(c, work + s, l); c[l] = 0;
            if (ui_chunk(cx, a, c, sent_start)) memset(work + s, ' ', e - s);
        }
        sent_start = endsent; i = j;
    }
    /* 4. lugares citados (place_note de agent/locate.c, um dos extratores); a nota e so lida, nunca anexada */
#ifndef _WIN32
    { char note[4096]; note[0] = 0; const int had = place_note(a->text, note, sizeof note);
      a->nplace_cited = 0;
      for (const char *p = note; had && (p = strstr(p, "you mention \"")) != NULL && a->nplace_cited < 4; ) {
          p += 13; const char *e = strchr(p, '"'); if (!e) break;
          snprintf(a->place_cited[a->nplace_cited++], sizeof a->place_cited[0], "%.*s", (int)(e - p) < 199 ? (int)(e - p) : 199, p); p = e;
      }
      for (int i = 0; i < g_place_nd && a->nplaces < 4; i++) snprintf(a->places[a->nplaces++], sizeof a->places[0], "%s", g_place_dirs[i]);
      a->place_note = had != 0;
      for (int i = 0; i < a->nplaces; i++) {
          const char *pd = a->places[i]; const char *cited = NULL;
          size_t pl = strlen(pd);
          for (int k = 0; k < a->nplace_cited; k++) { const char *ct = a->place_cited[k]; const char *sl = strrchr(ct, '/'); const char *b = sl ? sl + 1 : ct; size_t bl = strlen(b);
              if (bl && pl >= bl && !strcmp(pd + pl - bl, b)) { cited = ct; break; } }
          const char *shown = cited; char bn[200];
          if (!shown) { const char *sl = strrchr(pd, '/'); snprintf(bn, sizeof bn, "%s", sl ? sl + 1 : pd); shown = bn; }
          /* a entidade-pasta que o pedaco ja tinha e a mesma coisa: reaproveita */
          UioeEntity *e = NULL;
          for (int k = 0; k < a->nent; k++) if (a->ent[k].kind == UK_FOLDER && !a->ent[k].resolved[0]) { const size_t tl = strlen(a->ent[k].text); if (tl && pl >= tl && !strcmp(pd + pl - tl, a->ent[k].text)) { e = &a->ent[k]; break; } }
          if (!e) e = ui_ent_add(a, UK_FOLDER, shown, "located", 80, 0);
          if (e) { snprintf(e->resolved, sizeof e->resolved, "%s", pd); e->conf = cited ? 80 : 90; snprintf(e->how, sizeof e->how, "%s (place_note, src/agent/locate.c)", cited ? "cited folder located on this machine" : "cited folder exists"); }
      } }
#endif
    /* pastas de forma de caminho que nada resolveu */
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_FOLDER && !a->ent[i].resolved[0]) {
        const char *sl = strrchr(a->ent[i].text, '/'); const char *bn = sl ? sl + 1 : a->ent[i].text; int dup = 0;
        for (int k = 0; k < a->nent; k++) if (k != i && a->ent[k].kind == UK_FOLDER && a->ent[k].resolved[0]) { const char *s2 = strrchr(a->ent[k].resolved, '/'); if (s2 && !strcmp(s2 + 1, bn)) dup = 1; }
        if (dup) { memmove(&a->ent[i], &a->ent[i + 1], sizeof(UioeEntity) * (size_t)(a->nent - i - 1)); a->nent--; i--; continue; }      /* o mesmo lugar, escrito como caminho: ja esta na entidade localizada */
        a->ent[i].conf = 25; snprintf(a->ent[i].how, sizeof a->ent[i].how, "folder shape; not found on this machine");
    }
    /* 5. raiz */
    snprintf(a->root, sizeof a->root, "%s", cx->root);
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_FOLDER && a->ent[i].resolved[0] && ui_isdir(a->ent[i].resolved)) { snprintf(a->root, sizeof a->root, "%.1090s", a->ent[i].resolved); break; }
    { char w[160]; a->index_ready = (uint8_t)uioe_index_present(a->root, w, sizeof w); snprintf(a->index_note, sizeof a->index_note, "%s", w); }
    /* 6. termos, conceitos, idioma */
    { CoderKb kb; memset(&kb, 0, sizeof kb); kb.quiet = 1;       /* base de conhecimento: so leitura, sem avisos; sem ela os conceitos ficam vazios */
      coder_kb_load(coder_kb_default_dir(), &kb);
      ui_terms(a, work, &kb); }
    if (!strcmp(a->lang, "?")) {      /* poucas palavras: cumprimentos e respostas curtas que o tokenizador nao classifica */
        static const char *const ptw[] = {"bom", "dia", "boa", "tarde", "noite", "oi", "ola", "obrigado", "obrigada", "valeu", "sim", "nao", "pode", "continua", "beleza", NULL};
        static const char *const enw[] = {"good", "morning", "evening", "afternoon", "hello", "hi", "hey", "thanks", "thank", "yes", "continue", "okay", "ok", NULL};
        int pt = 0, en = 0; UioeWords *W = (UioeWords *)malloc(sizeof *W);
        if (W) { uw_build(W, lf); for (int i = 0; i < W->n; i++) { for (int g = 0; ptw[g]; g++) if (!strcmp(W->w[i], ptw[g])) pt++; for (int g = 0; enw[g]; g++) if (!strcmp(W->w[i], enw[g])) en++; } free(W); }
        if (pt > en) snprintf(a->lang, sizeof a->lang, "pt"); else if (en > pt) snprintf(a->lang, sizeof a->lang, "en");
    }
    /* 7. relacao, restricoes, mudanca (sobre as palavras do pedido sem as aspas e o texto de erro) */
    { UioeWords *W = (UioeWords *)malloc(sizeof *W); char *wl = (char *)malloc(n + 2);
      if (W && wl) {
          ui_lower(work, wl, n); uw_build(W, wl);
          static const char *const callers[] = {"quem chama", "who calls", "quem chamou", "callers", "chamadores", "quem invoca", "who invokes", "quem referencia", NULL};
          static const char *const defd[] = {"onde...definid*", "onde...defin*", "where...defined", "where...defin*", "definicao", "definition", "quem define", "who defines", NULL};
          static const char *const uses[] = {"usad*", "usa", "usam", "usage*", "uses", "used", "refer*", "depend*", "quem usa", "who uses", NULL};
          static const char *const callees[] = {"o que...chama", "what...calls", "callees", "chama o que", NULL};
          static const char *const workfail[] = {"funciona...mas...nao", "works...but...not", "funciona...porem...nao", "funciona em...nao em", "works in...not in", NULL};
          static const char *const neg[] = {"nao", "not", "never", "nunca", "sem", "without", "nem", "no", NULL};
          if (uw_any(W, callers, NULL) >= 0) a->rel |= UREL_CALLERS;
          if (uw_any(W, defd, NULL) >= 0) a->rel |= UREL_DEFINED;
          if (uw_any(W, uses, NULL) >= 0) a->rel |= UREL_USES;
          if (uw_any(W, callees, NULL) >= 0) a->rel |= UREL_CALLEES;
          const int ps = uw_any(W, g_ui_c_sendcue, NULL), pr = uw_any(W, g_ui_c_recvcue, NULL);
          if (ps >= 0) a->rel |= UREL_SEND;
          if (pr >= 0) a->rel |= UREL_RECV;
          if (ps >= 0 && (pr < 0 || ps <= pr)) snprintf(a->dir, sizeof a->dir, "send"); else if (pr >= 0) snprintf(a->dir, sizeof a->dir, "recv");
          if (uw_any(W, workfail, NULL) >= 0) a->rel |= UREL_WORKFAIL;
          if (uw_any(W, neg, NULL) >= 0) a->rel |= UREL_NEG;
          /* restricoes */
          static const char *const per[] = {"hoje", "ontem", "amanha", "today", "yesterday", "tomorrow", "semana passada", "esta semana", "last week", "this week", "mes passado", "ultimo mes", "last month", "ultimo trimestre",
              "last quarter", "ano passado", "este ano", "last year", "this year", NULL};
          const char *which = NULL;
          if (uw_any(W, per, &which) >= 0 && a->ncon < 8) { snprintf(a->con[a->ncon].what, sizeof a->con[0].what, "period"); snprintf(a->con[a->ncon].text, sizeof a->con[0].text, "%s", which); a->ncon++; }
          static const char *const ro[] = {"sem alterar", "sem modificar", "nao altere", "nao mude", "nao modifique", "read only", "readonly", "apenas leitura", "somente leitura", "without changing", "do not change", "dont change", NULL};
          if (uw_any(W, ro, &which) >= 0 && a->ncon < 8) { snprintf(a->con[a->ncon].what, sizeof a->con[0].what, "read-only"); snprintf(a->con[a->ncon].text, sizeof a->con[0].text, "%s", which); a->ncon++; }
          static const char *const only[] = {"so em", "apenas em", "somente em", "only in", "only inside", NULL};
          { int pp = uw_any(W, only, &which); if (pp >= 0 && a->ncon < 8) { int e2 = 0; uw_find(W, pp, pp + 1, which, &e2); snprintf(a->con[a->ncon].what, sizeof a->con[0].what, "scope"); snprintf(a->con[a->ncon].text, sizeof a->con[0].text, "%s %s", which, e2 < W->n ? W->w[e2] : ""); a->ncon++; } }
          /* mudanca: verbo de alteracao e nenhuma hipotese ("se eu mudar") */
          const int ch = uw_any(W, g_ui_c_change, NULL) >= 0, hy = uw_any(W, g_ui_c_hypo, NULL) >= 0;
          a->change = (uint8_t)(ch && !hy);
          /* ambiguidade: referencia sem alvo */
          static const char *const refs[] = {"isso", "isto", "aquilo", "esse", "essa", "aquele", "aquela", "o outro", "a outra", "that", "this one", "the other one", "the other", "it", NULL};
          if (uw_any(W, refs, &which) >= 0 && a->nent == 0 && a->nterms < 2) { a->ambiguous = 1; snprintf(a->amb, sizeof a->amb, "reference \"%s\" with no target in the request and no open investigation", which); }
      }
      free(W); free(wl); }
    if (!a->ambiguous && a->nplaces >= 2) {
        int distinct = 0; const char *b0 = strrchr(a->places[0], '/');
        for (int i = 1; i < a->nplaces; i++) { const char *b = strrchr(a->places[i], '/'); if (b0 && b && !strcmp(b0, b) && strcmp(a->places[0], a->places[i])) distinct = 1; }
        if (distinct) { a->ambiguous = 1; snprintf(a->amb, sizeof a->amb, "%d folders with the same name were found (%.60s, %.60s)", a->nplaces, a->places[0], a->places[1]); }
    }
    free(f); free(lf); free(work);
}
