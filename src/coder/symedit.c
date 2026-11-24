/* ---------- coder/symedit.c: edicao por simbolo com validacao (Edit symbol=..., ver docs/tools.md e docs/coder.md) ----------
 * O modelo diz QUAL simbolo (nome, Classe.nome ou nome:linha) e o texto novo; nao reescreve nem repete o arquivo. O simbolo e achado por um
 * parse novo do arquivo (coder_parse, o mesmo scanner por familia do indice: o disco manda, nunca um indice velho). Operacoes (where):
 *   replace (padrao) = a definicao inteira, cabecalho incluido (decoradores e anotacoes fazem parte); body = so o que fica entre as chaves (familias
 *   de chaves) ou o bloco indentado (Python); before / after = insere ao lado, depois dos comentarios que pertencem ao simbolo; delete.
 * new_string e escrito SEM a indentacao do simbolo (coluna 0 = a coluna do simbolo): o motor indenta. Se o texto veio com indentacao comum, ela e tirada.
 *
 * VALIDACAO ANTES DE GRAVAR (nada vai ao disco se falhar: e o desfazer automatico, sem precisar desfazer; o undo de ~/.katana/undo guarda a versao
 * anterior so quando grava). Cada achado leva o grau de certeza, nunca "exato" (decisao do dono, 2026-11-23):
 *   [structural] fato do scanner da familia sobre ESTE arquivo: chaves/parenteses/colchetes balanceados (pela mascara sem comentarios nem strings;
 *                so a regiao nova e o arquivo todo comparado com o antes: um desbalanceamento que ja existia nao reprova), o simbolo continua existindo
 *                e do mesmo tipo, assinatura mudou, simbolos que sumiram ou apareceram; a ferramenta de sintaxe da linguagem (python ast, node --check,
 *                bash -n, php -l, ruby -c, json) so reprova o que o arquivo ANTES nao tinha;
 *   [derived]    chamadores (e usos) por NOME IGUAL: no mesmo arquivo pela referencia do parse novo; nos outros pelo indice de simbolos (symquery.c),
 *                com o "how" da consulta (quantas definicoes tem o nome). Nao e resolucao de nomes de verdade;
 *   [lsp NOME]   so com servidor de linguagem instalado: diagnosticos de ANTES e de DEPOIS, so os erros introduzidos pela edicao; prazo curto
 *                (KATANA_EDIT_LSP_MS, 2500; KATANA_EDIT_LSP=0 desliga), nunca bloqueia nem reprova.
 * Linguagens: toda familia que o parse.c/parse2.c cobre tem simbolo e extensao (replace, before, after, delete); body so em familia de chaves e
 * Python; balanceamento nas familias de chaves, Python, shell, PowerShell, JSON e CSS; sintaxe externa onde a ferramenta existe. Nas demais o ciclo
 * valida so o que o scanner enxerga (o simbolo existe, extensao). Falhas conhecidas: macros que geram chaves, #if com ramos desbalanceados (a regiao
 * nova com #if so e comparada no arquivo inteiro), linguagens de palavra-fim (Ruby, Lua) sem balanceamento proprio. */
#ifndef _WIN32
static int lsp_diag_get(const char *abs, double secs, int resync, char **diag, char *srv, size_t scap, char *why, size_t wcap);

typedef struct { char err[500]; } SeErr;

/* a mascara sem comentarios nem conteudo de strings (a "c" do coder_parse; a outra copia, com as strings, nao serve para contar delimitadores);
   memoria nova, terminada em zero, mesmos offsets do texto */
static char *se_mask(int L, const char *t, size_t n) {
    char *cl = (char *)malloc(n + 1), *wt = (char *)malloc(n + 1);
    if (!cl || !wt) { free(cl); free(wt); return NULL; }
    if (L == CL_MD) { memcpy(cl, t, n); memcpy(wt, t, n); }
    else if (c2_isline(L) || c2_istok(L)) c2_mask(L, t, n, cl, wt);
    else cp_mask(L, t, n, cl, wt);
    cl[n] = wt[n] = 0; free(wt);
    return cl;
}
/* quais delimitadores a familia balanceia; NULL = nao confere */
static const char *se_brk(int L) {
    switch (L) {
        case CL_C: case CL_CPP: case CL_JS: case CL_TS: case CL_KT: case CL_JAVA: case CL_GO: case CL_RS: case CL_QML: case CL_CS: case CL_SWIFT:
        case CL_PHP: case CL_DART: case CL_SCALA: case CL_GROOVY: case CL_OBJC: case CL_ZIG: case CL_PY: case CL_PS1: case CL_VUE: return "(){}[]";
        case CL_SH: return "{}[]";
        case CL_JSON: return "{}[]";
        case CL_CSS: return "{}()";
        default: return NULL;
    }
}
static int se_line_of(const char *t, size_t off) { int ln = 1; for (size_t i = 0; i < off; i++) if (t[i] == '\n') ln++; return ln; }
/* balanceamento de nc[from,to): 0 ok; 1 com o motivo em e. whole: o limite e o fim do arquivo (so muda a frase) */
static int se_balance(const char *nc, size_t n, size_t from, size_t to, const char *brk, int whole, SeErr *e) {
    if (to > n) to = n;
    size_t *st = (size_t *)malloc(sizeof(size_t) * (to - from + 1));
    if (!st) return 0;
    int sp = 0, ln = se_line_of(nc, from), rc = 0;
    for (size_t i = from; i < to && !rc; i++) {
        const char c = nc[i];
        if (c == '\n') { ln++; continue; }
        const char *k = strchr(brk, c);
        if (!c || !k) continue;
        const int idx = (int)(k - brk);
        if (idx % 2 == 0) { st[sp++] = i; continue; }
        const char op = brk[idx - 1];
        if (!sp) { snprintf(e->err, sizeof e->err, "line %d: unexpected `%c`, nothing is open here", ln, c); rc = 1; }
        else if (nc[st[sp - 1]] != op) {
            const char o = nc[st[sp - 1]]; const char *q = strchr(brk, o);
            snprintf(e->err, sizeof e->err, "line %d: unexpected `%c`, expected `%c` to close the `%c` opened at line %d", ln, c, q ? q[1] : '?', o, se_line_of(nc, st[sp - 1])); rc = 1;
        } else sp--;
    }
    if (!rc && sp) {
        const char o = nc[st[sp - 1]]; const char *q = strchr(brk, o);
        snprintf(e->err, sizeof e->err, "line %d: the `%c` opened here is never closed (expected `%c` before %s)", se_line_of(nc, st[sp - 1]), o, q ? q[1] : '?', whole ? "the end of the file" : "the end of your text"); rc = 1;
    }
    free(st);
    return rc;
}
/* a linha n (1-based) de t, sem a quebra, no maximo 160 bytes, em o */
static void se_line_text(const char *t, size_t tn, int ln, char *o, size_t cap) {
    size_t s = 0; int l = 1;
    while (l < ln && s < tn) { const char *nl = (const char *)memchr(t + s, '\n', tn - s); if (!nl) { s = tn; break; } s = (size_t)(nl - t) + 1; l++; }
    size_t e = s; while (e < tn && t[e] != '\n') e++;
    if (e > s && t[e - 1] == '\r') e--;
    size_t k = e - s; if (k > 160) k = 160; if (k + 1 > cap) k = cap - 1;
    memcpy(o, t + s, k); o[k] = 0;
}
static size_t se_ls(const char *b, size_t off) { while (off > 0 && b[off - 1] != '\n') off--; return off; }                  /* inicio da linha de off */
static size_t se_le(const char *b, size_t n, size_t off) { while (off < n && b[off] != '\n') off++; return off; }          /* fim da linha (a quebra) */
static int se_blank(const char *b, size_t a, size_t e) { for (; a < e; a++) if (b[a] != ' ' && b[a] != '\t' && b[a] != '\r') return 0; return 1; }
/* os espacos e tabs do comeco da linha que comeca em ls */
static size_t se_indlen(const char *b, size_t n, size_t ls) { size_t k = 0; while (ls + k < n && (b[ls + k] == ' ' || b[ls + k] == '\t')) k++; return k; }

/* texto sem a indentacao comum (a menor entre as linhas nao vazias) e com `ind` no comeco de cada linha nao vazia; espacos do fim da linha ficam; sem quebra final */
static void se_reindent(Buf *o, const char *s, const char *ind, size_t il) {
    size_t sn = strlen(s);
    while (sn && (s[sn - 1] == '\n' || s[sn - 1] == '\r' || s[sn - 1] == ' ' || s[sn - 1] == '\t')) sn--;
    size_t a = 0; while (a < sn && (s[a] == '\n' || s[a] == '\r')) a++;                 /* linhas vazias do comeco */
    size_t common = (size_t)-1;
    for (size_t p = a; p < sn;) {
        size_t e = p; while (e < sn && s[e] != '\n') e++;
        if (!se_blank(s, p, e)) { size_t k = 0; while (p + k < e && (s[p + k] == ' ' || s[p + k] == '\t')) k++; if (k < common) common = k; }
        p = e + 1;
    }
    if (common == (size_t)-1) common = 0;
    for (size_t p = a; p <= sn;) {
        size_t e = p; while (e < sn && s[e] != '\n') e++;
        size_t le = e; if (le > p && s[le - 1] == '\r') le--;
        if (!se_blank(s, p, le)) { buf_add(o, ind, il); buf_add(o, s + p + (le - p >= common ? common : 0), le - p - (le - p >= common ? common : 0)); }
        if (e >= sn) break;
        buf_add(o, "\n", 1);
        p = e + 1;
    }
}
static void se_norm_sig(const char *s, char *o, size_t cap) {
    size_t k = 0; int sp = 0;
    for (; s && *s && k + 2 < cap; s++) {
        if (isspace((unsigned char)*s)) { sp = k > 0; continue; }
        if (sp) o[k++] = ' ';
        sp = 0; o[k++] = *s;
    }
    o[k] = 0;
}
static int se_nparams(const char *sig) {
    const char *p = strchr(sig, '('); if (!p) return -1;
    int d = 0, n = 0, any = 0;
    for (p++; *p; p++) {
        if (*p == '(' || *p == '[' || *p == '{' || *p == '<') d++;
        else if ((*p == ')' || *p == ']' || *p == '}' || *p == '>') && d > 0) d--;
        else if (*p == ')') break;
        else if (*p == ',' && d == 0) n++;
        else if (!isspace((unsigned char)*p)) any = 1;
    }
    return any ? n + 1 : 0;
}
static int se_callable(int k) { return k == CK_FUNC || k == CK_METHOD || k == CK_CTOR; }
static int se_kind_eq(int a, int b) { return a == b || (se_callable(a) && se_callable(b)); }
static int se_hash_lang(int L) { return L == CL_PY || L == CL_SH || L == CL_PS1 || L == CL_RB || L == CL_PERL || L == CL_R || L == CL_JL || L == CL_NIM || L == CL_CMAKE || L == CL_MAKE || L == CL_YAML || L == CL_TOML || L == CL_EX; }
static int se_slash_lang(int L) { return se_brk(L) && !se_hash_lang(L) && L != CL_JSON && L != CL_CSS; }
/* inicio da linha onde comecam os comentarios colados acima do simbolo (a linha que comeca em ls) */
static size_t se_attach_top(const char *b, size_t n, size_t ls, int L) {
    if (!se_slash_lang(L) && !se_hash_lang(L)) return ls;
    while (ls > 0) {
        size_t ps = se_ls(b, ls - 1), k = ps; while (k < ls && (b[k] == ' ' || b[k] == '\t')) k++;
        const int hit = se_hash_lang(L) ? b[k] == '#' : (b[k] == '/' && (b[k + 1] == '/' || b[k + 1] == '*')) || (b[k] == '*' && b[k + 1] != '=') ;
        if (k >= ls - 1 || !hit) break;
        ls = ps;
    }
    (void)n;
    return ls;
}
/* o texto comeca (depois de linhas vazias) com um comentario da linguagem */
static int se_starts_comment(const char *s, int L) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (se_hash_lang(L)) return *s == '#';
    return se_slash_lang(L) && s[0] == '/' && (s[1] == '/' || s[1] == '*');
}
/* nome qualificado de um simbolo para comparar antes e depois: Pai.Avo.nome|tipo */
static void se_qname(const CoderFile *f, int i, char *o, size_t cap) {
    char t[400]; t[0] = 0; size_t k = 0;
    for (int p = f->sym[i].parent, d = 0; p >= 0 && d < 8; p = f->sym[p].parent, d++) k += (size_t)snprintf(t + k, sizeof t - k > 0 ? sizeof t - k : 0, "%s.", f->sym[p].name);
    snprintf(o, cap, "%s%s|%d", t, f->sym[i].name, se_callable(f->sym[i].kind) ? CK_FUNC : f->sym[i].kind);
}
static int se_str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* nomes (sem o tipo) que existem em a e nao em b, sem repetir, ate max; devolve quantos no total */
static int se_set_minus(const CoderFile *a, const CoderFile *bb, char names[][120], int max) {
    char **kb = (char **)malloc(sizeof(char *) * (size_t)(bb->nsym + 1));
    for (int i = 0; i < bb->nsym; i++) { kb[i] = (char *)malloc(520); se_qname(bb, i, kb[i], 520); }
    qsort(kb, (size_t)bb->nsym, sizeof(char *), se_str_cmp);
    int tot = 0;
    for (int i = 0; i < a->nsym; i++) {
        char q[520]; se_qname(a, i, q, sizeof q);
        if (bsearch(&(char *){q}, kb, (size_t)bb->nsym, sizeof(char *), se_str_cmp)) continue;
        if (a->sym[i].kind == CK_FILE) continue;
        if (tot < max) { char q2[520]; se_qname(a, i, q2, sizeof q2); char *bar = strrchr(q2, '|'); if (bar) *bar = 0; snprintf(names[tot], 120, "%s", q2); }
        tot++;
    }
    for (int i = 0; i < bb->nsym; i++) free(kb[i]);
    free(kb);
    return tot;
}
static const char *se_kindn(int k) { return k >= 0 && k < CK_N ? CP_KINDN[k] : "?"; }

/* ---- achar o simbolo ---- */
/* spec: nome, Pai.nome, Pai::nome, Pai#nome, com :linha no fim. Devolve o indice, ou -1 com a mensagem (candidatos) em err */
static int se_resolve(const CoderFile *f, const char *spec, char *err, size_t ecap) {
    char sp[300]; snprintf(sp, sizeof sp, "%s", spec);
    int line = 0;
    { char *c = strrchr(sp, ':'); if (c && c[1] && c > sp && c[-1] != ':') { char *q = c + 1; while (isdigit((unsigned char)*q)) q++; if (!*q) { line = atoi(c + 1); *c = 0; } } }
    char *qual = NULL, *name = sp;
    { char *s1 = strstr(sp, "::"), *s2 = strrchr(sp, '.'), *s3 = strrchr(sp, '#'); char *last = NULL; size_t sl = 1;
      for (char *p = sp; (p = strstr(p, "::")); p += 2) s1 = p;
      if (s1) { last = s1; sl = 2; }
      if (s2 && (!last || s2 > last)) { last = s2; sl = 1; }
      if (s3 && (!last || s3 > last)) { last = s3; sl = 1; }
      if (last && last > sp && last[sl]) { *last = 0; qual = sp; name = last + sl; } }
    int cand[64], nc = 0, anydef = 0;
    for (int i = 0; i < f->nsym; i++) {
        const CoderSym *y = &f->sym[i];
        if (y->kind == CK_FILE || strcmp(y->name, name) != 0) continue;
        if (line && y->line0 != line) continue;
        if (qual) {
            int ok = 0;
            for (int p = y->parent, d = 0; p >= 0 && d < 12; p = f->sym[p].parent, d++) if (!strcmp(f->sym[p].name, qual)) ok = 1;
            if (!ok && y->sig) { char t[330]; snprintf(t, sizeof t, "%s::%s", qual, name); const char *q = strstr(y->sig, t); if (q) ok = 1; }
            if (!ok) continue;
        }
        if (nc < 64) cand[nc++] = i;
        if (!(y->flags & CSF_DECL)) anydef = 1;
    }
    if (anydef) { int k = 0; for (int i = 0; i < nc; i++) if (!(f->sym[cand[i]].flags & CSF_DECL)) cand[k++] = cand[i]; nc = k; }
    if (nc == 1) return cand[0];
    if (nc > 1) {
        size_t o = (size_t)snprintf(err, ecap, "error: symbol `%s` matches %d definitions in this file; say which with Class.name or name:LINE: ", spec, nc);
        for (int i = 0; i < nc && i < 8 && o + 120 < ecap; i++) {
            const CoderSym *y = &f->sym[cand[i]];
            o += (size_t)snprintf(err + o, ecap - o, "%s%s%s%s %s (lines %d-%d)", i ? "; " : "", y->parent >= 0 ? f->sym[y->parent].name : "", y->parent >= 0 ? "." : "", y->name, se_kindn(y->kind), y->line0, y->line1);
        }
        return -1;
    }
    size_t o = (size_t)snprintf(err, ecap, "error: no symbol `%s` in this file%s. Symbols here: ", spec, line ? " at that line" : "");
    int shown = 0;
    for (int i = 0; i < f->nsym && shown < 14 && o + 60 < ecap; i++) if (f->sym[i].kind != CK_FILE && f->sym[i].parent < 0) o += (size_t)snprintf(err + o, ecap - o, "%s%s", shown++ ? ", " : "", f->sym[i].name);
    for (int i = 0; i < f->nsym && shown < 14 && o + 60 < ecap; i++) if (f->sym[i].kind != CK_FILE && f->sym[i].parent >= 0) o += (size_t)snprintf(err + o, ecap - o, "%s%s.%s", shown++ ? ", " : "", f->sym[f->sym[i].parent].name, f->sym[i].name);
    if (!shown) snprintf(err + o, ecap - o, "(none: the scanner found no symbols in this file)");
    return -1;
}

/* ---- Python: o corpo e o que vem depois dos dois pontos do cabecalho (fora de parenteses) ---- */
static int se_py_head(const char *nc, size_t n, size_t b0, size_t b1, size_t *colon) {
    int d = 0;
    for (size_t i = b0; i < b1 && i < n; i++) {
        const char c = nc[i];
        if (c == '(' || c == '[' || c == '{') d++;
        else if ((c == ')' || c == ']' || c == '}') && d > 0) d--;
        else if (c == ':' && d == 0) { *colon = i; return 1; }
    }
    return 0;
}

/* ---- chamadores pelo indice (derivado) ---- */
typedef struct { char where[420]; char how[100]; } SeCall;
/* quem chama `name` em OUTROS arquivos do projeto, pelo indice de simbolos; devolve quantos achou (ate max em c) e o total em *tot; -1 se nao deu para consultar */
static int se_index_callers(const char *root, const char *rel, const char *name, SeCall *c, int max, int *tot, int *ndefs) {
    double ms; int created; *tot = 0; *ndefs = 0;
    if (sym_ensure_index(root, &ms, &created) != 0) return -1;
    SyView v; char err[300] = "";
    if (sy_view_open(root, &v, err, sizeof err) != 0 || !v.any) { if (v.r) sy_view_close(&v); return -1; }
    SyItems items = {0}; SyQ q;
    *ndefs = sy_ndefs(&v, name, strlen(name), NULL);
    sy_q_callers(&v, name, 60, &items, &q);
    int n = 0;
    for (int i = 0; i < items.n; i++) {
        const SyItem *it = &items.v[i];
        if (strcmp(it->name, name) != 0 || !strcmp(it->path, rel)) continue;
        (*tot)++;
        if (n < max) {
            const char *en = strncmp(it->encl, "in ", 3) == 0 ? it->encl + 3 : it->encl; char fn[100]; size_t k = 0;
            while (en[k] && en[k] != ' ' && k + 1 < sizeof fn) { fn[k] = en[k]; k++; } fn[k] = 0;
            snprintf(c[n].where, sizeof c[n].where, "%s:%d%s%s", it->path, it->line0, k ? " in " : "", fn);
            c[n].how[0] = 0;
            n++;
        }
    }
    sy_items_free(&items); sy_view_close(&v);
    return n;
}
/* chamadas a name no arquivo novo (mesmo arquivo), uma por simbolo que chama; ate max */
static int se_same_file_callers(const CoderFile *f, const char *name, const char *rel, SeCall *c, int max) {
    int tot = 0;
    for (int r = 0; r < f->nref; r++) {
        const CoderRef *y = &f->ref[r];
        if (y->kind != CR_CALL || strcmp(y->name, name) != 0) continue;
        if (y->from >= 0 && !strcmp(f->sym[y->from].name, name)) continue;            /* recursao no proprio simbolo */
        if (tot < max) { snprintf(c[tot].where, sizeof c[tot].where, "%s:%d%s%s", rel, y->line, y->from >= 0 ? " in " : "", y->from >= 0 ? f->sym[y->from].name : ""); snprintf(c[tot].how, sizeof c[tot].how, "same file"); }
        tot++;
    }
    return tot;
}

/* ---- diagnosticos do servidor de linguagem: antes e depois ---- */
typedef struct { int sev, line; char msg[200]; } SeDiag;
static int se_diag_parse(const char *arr, SeDiag *d, int max) {
    int n = 0; const char *v, *p;
    if (!arr || *arr != '[') return 0;
    for (p = arr + 1; n < max && (p = mcp_elem(p, &v)) != NULL;) {
        const char *st = mcp_get(mcp_get(v, "range"), "start"), *mv = mcp_get(v, "message"), *sv = mcp_get(v, "severity");
        d[n].sev = sv ? atoi(sv) : 1; if (d[n].sev < 1 || d[n].sev > 4) d[n].sev = 1;
        d[n].line = st && mcp_get(st, "line") ? atoi(mcp_get(st, "line")) + 1 : 0;
        d[n].msg[0] = 0; if (mv) mcp_dec(mv, d[n].msg, sizeof d[n].msg);
        for (char *q = d[n].msg; *q; q++) if (*q == '\n') *q = ' ';
        n++;
    }
    return n;
}

/* ---- sintaxe pela ferramenta da linguagem, em arquivo temporario (antes de gravar). 1 ok, 0 falhou (msg), -1 sem verificador ---- */
static int se_ext_syntax(const char *path, const char *data, size_t n, char *msg, size_t mcap) {
    const char *ext = strrchr(path, '.'), *base = strrchr(path, '/');
    if (!ext || (base && ext < base)) return -1;
    const char *td = getenv("TMPDIR"); if (!td || !*td) td = "/tmp";
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s/katana-se-XXXXXX%s", td, ext);
    const int fd = mkstemps(tmp, (int)strlen(ext));
    if (fd < 0) return -1;
    size_t off = 0; while (off < n) { ssize_t w = write(fd, data + off, n - off); if (w <= 0) break; off += (size_t)w; }
    close(fd);
    static char res[2000]; res[0] = 0;
    if (off == n) code_check(tmp, 0, res, sizeof res);
    unlink(tmp);
    msg[0] = 0;
    if (!res[0]) return -1;
    if (strstr(res, "[syntax check: ok]")) return 1;
    const char *nl = strstr(res, "]\n");
    snprintf(msg, mcap, "%s", nl ? nl + 2 : res);
    for (char *q; (q = strstr(msg, tmp)) != NULL;) { size_t tl = strlen(tmp), bl = base ? strlen(base + 1) : 0; if (bl > tl) break; memcpy(q, base ? base + 1 : "", bl); memmove(q + bl, q + tl, strlen(q + tl) + 1); }
    return 0;
}

static int se_apply_fail(char *out, size_t cap, const char *kind, const char *why, const char *ctx) {
    snprintf(out, cap, "error: edit refused, the file was NOT changed. [%s] %s%s%s", kind, why, ctx && *ctx ? "\n" : "", ctx ? ctx : "");
    return 0;
}

static void se_add(Buf *b, const char *s) { buf_str(b, s); }

static void ct_edit_symbol(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], err[1400];
    char *fp = ct_str(js, end, "file_path"), *spec = ct_str(js, end, "symbol"), *where = ct_str(js, end, "where"), *ns = ct_str(js, end, "new_string");
    char *b = NULL, *nb = NULL, *nmask = NULL, *omask = NULL;
    CoderFile of, nf;
    memset(&of, 0, sizeof of); memset(&nf, 0, sizeof nf);
    int have_of = 0, have_nf = 0;
    const double t0 = now_sec();
    do {
        if (!fp || !*fp) { snprintf(out, cap, "error: missing file_path"); break; }
        const char *w = where && *where ? where : "replace";
        enum { W_REPLACE, W_BODY, W_BEFORE, W_AFTER, W_DELETE } wk;
        if (!strcmp(w, "replace")) wk = W_REPLACE; else if (!strcmp(w, "body")) wk = W_BODY; else if (!strcmp(w, "before")) wk = W_BEFORE;
        else if (!strcmp(w, "after")) wk = W_AFTER; else if (!strcmp(w, "delete")) wk = W_DELETE;
        else { snprintf(out, cap, "error: where must be replace, body, before, after or delete (got `%s`)", w); break; }
        if (wk != W_DELETE && (!ns || !*ns)) { snprintf(out, cap, "error: where=%s needs new_string (the new text, without the symbol's own indentation). To remove the symbol use where=delete.", w); break; }
        if (wk == W_DELETE && ns && *ns) { snprintf(out, cap, "error: where=delete takes no new_string"); break; }
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        if (stat(abs, &st) != 0) { snprintf(out, cap, "error: %s does not exist. To create a file use Write.", abs); break; }
        if (S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory; give the path of a file", abs); break; }
        size_t n = 0;
        b = ct_f_slurp(abs, &n, err, sizeof err);
        if (!b) { snprintf(out, cap, "%s", err); break; }
        if (memchr(b, 0, n) || !ct_utf8_ok((const unsigned char *)b, n)) { snprintf(out, cap, "error: %s is not a valid UTF-8 text file; editing would corrupt it. Use Bash for this file.", abs); break; }
        const int seen = ct_seen(abs);
        coder_parse(abs, b, n, &of); have_of = 1;
        if (of.lang == CL_UNKNOWN || of.lang == CL_CONFIG || of.nsym == 0) {
            snprintf(out, cap, "error: the scanner finds no symbols in %s (%s); edit it with old_string and new_string.", abs, of.lang < CL_N ? CP_LANGN[of.lang] : "unknown type");
            break;
        }
        const int L = of.lang;
        const int si = se_resolve(&of, spec ? spec : "", err, sizeof err);
        if (si < 0) { snprintf(out, cap, "%s", err); break; }
        const CoderSym *sy = &of.sym[si];
        if (sy->byte1 > n || sy->byte0 >= sy->byte1) { snprintf(out, cap, "error: the scanner has no usable extent for `%s`; use old_string and new_string.", sy->name); break; }
        omask = se_mask(L, b, n);
        if (!omask) { snprintf(out, cap, "error: out of memory"); break; }
        const char *brk = se_brk(L);

        /* ---- texto novo: troca b[cut0, cut1) pela insercao ins ---- */
        size_t cut0 = sy->byte0, cut1 = sy->byte1, tx0 = 0, tx1 = 0;              /* tx: onde fica o texto do modelo dentro da insercao */
        Buf ins = {0};
        const size_t symls = se_ls(b, sy->byte0), il = se_indlen(b, n, symls);
        const int own_line = sy->byte0 == symls + il;                              /* o simbolo abre a linha (so espacos antes) */
        char ind[256]; const size_t indl = own_line && il < sizeof ind ? il : 0; memcpy(ind, b + symls, indl);
        const size_t eol = se_le(b, n, sy->byte1);
        const int own_end = se_blank(b, sy->byte1, eol) || (b[sy->byte1] == ';' && se_blank(b, sy->byte1 + 1, eol));
        if (wk == W_REPLACE) {
            if (b[sy->byte1 - 1] == '}' && b[sy->byte1] == ';') cut1 = sy->byte1 + 1;         /* struct A { } ; */
            int cmt = 0;
            if (own_line && se_starts_comment(ns, L)) { cut0 = se_attach_top(b, n, symls, L); cmt = 1; }   /* new_string traz o comentario do simbolo: o antigo sai */
            se_reindent(&ins, ns, ind, indl);
            if (!cmt && ins.n >= indl) { memmove(ins.p, ins.p + indl, ins.n - indl + 1); ins.n -= indl; }      /* a primeira linha cai onde o simbolo comecava */
            tx0 = 0; tx1 = ins.n;
        } else if (wk == W_BODY) {
            if (!(sy->kind == CK_FUNC || sy->kind == CK_METHOD || sy->kind == CK_CTOR || sy->kind == CK_CLASS || sy->kind == CK_STRUCT || sy->kind == CK_IFACE || sy->kind == CK_NAMESPACE || sy->kind == CK_ENUM || sy->kind == CK_UNION) || (sy->flags & CSF_DECL)) {
                snprintf(out, cap, "error: `%s` (%s) has no body to replace; use where=replace.", sy->name, se_kindn(sy->kind)); break; }
            char unit[16]; size_t ul = (indl && ind[0] == '\t') ? 1 : 4;
            if (ul == 1) unit[0] = '\t'; else memcpy(unit, "    ", 4);
            char bi[600];
            if (L == CL_PY) {
                size_t colon;
                if (!se_py_head(omask, n, sy->byte0, sy->byte1, &colon)) { snprintf(out, cap, "error: cannot find the `:` that ends the header of `%s`; use where=replace.", sy->name); break; }
                const size_t he = se_le(b, n, colon);
                const int oneliner = !se_blank(omask, colon + 1, he);
                cut0 = oneliner ? colon + 1 : he; cut1 = sy->byte1;
                if (!oneliner) for (size_t p = he + 1; p < sy->byte1;) {          /* a indentacao do corpo que existe */
                    const size_t e = se_le(b, n, p);
                    if (!se_blank(omask, p, e)) { const size_t k = se_indlen(b, n, p); if (k > indl && k - indl <= sizeof unit) { ul = k - indl; memcpy(unit, b + p + indl, ul); } break; }
                    p = e + 1;
                }
                memcpy(bi, ind, indl); memcpy(bi + indl, unit, ul);
                buf_add(&ins, "\n", 1); tx0 = ins.n; se_reindent(&ins, ns, bi, indl + ul); tx1 = ins.n;
            } else {
                const size_t ob = sy->body0;
                if (!ob || ob >= n || omask[ob] != '{') { snprintf(out, cap, "error: cannot locate the `{` of `%s`; use where=replace.", sy->name); break; }
                size_t cl = sy->byte1;
                while (cl > ob && strchr(" \n\t\r;", omask[cl - 1])) cl--;
                if (cl <= ob + 1 || omask[cl - 1] != '}') { snprintf(out, cap, "error: cannot locate the closing `}` of `%s`; use where=replace.", sy->name); break; }
                cl--;                                                        /* indice do '}' */
                SeErr be;
                if (brk && se_balance(omask, n, ob, cl + 1, brk, 0, &be)) { snprintf(out, cap, "error: cannot isolate the body of `%s` (%s); use where=replace.", sy->name, be.err); break; }
                for (size_t p = se_le(b, n, ob) + 1; p < cl;) {
                    size_t e = se_le(b, n, p); if (e > cl) e = cl;
                    if (!se_blank(b, p, e)) { const size_t k = se_indlen(b, n, p); if (k > indl && k - indl <= sizeof unit) { ul = k - indl; memcpy(unit, b + p + indl, ul); } break; }
                    p = e + 1;
                }
                cut0 = ob + 1; cut1 = cl;
                memcpy(bi, ind, indl); memcpy(bi + indl, unit, ul);
                buf_add(&ins, "\n", 1); tx0 = ins.n; se_reindent(&ins, ns, bi, indl + ul); tx1 = ins.n;
                buf_add(&ins, "\n", 1); buf_add(&ins, ind, indl);
            }
        } else if (wk == W_BEFORE || wk == W_AFTER) {
            const char *gap = L == CL_PY && sy->parent < 0 ? "\n\n\n" : "\n\n";
            Buf r2 = {0}; se_reindent(&r2, ns, ind, indl);
            if (wk == W_BEFORE) { cut0 = cut1 = se_attach_top(b, n, symls, L); tx0 = 0; if (r2.p) buf_add(&ins, r2.p, r2.n); tx1 = ins.n; buf_str(&ins, gap); }
            else { cut0 = cut1 = eol; buf_str(&ins, gap); tx0 = ins.n; if (r2.p) buf_add(&ins, r2.p, r2.n); tx1 = ins.n; }
            free(r2.p);
        } else {                                                              /* delete */
            size_t ds = sy->byte0, de = sy->byte1;
            if (own_line) ds = se_attach_top(b, n, symls, L);
            if (own_end && own_line) {
                de = eol < n ? eol + 1 : eol;
                if (ds > 0 && de < n) {                                       /* sobra uma linha vazia de cada lado: tira uma */
                    const size_t pls = se_ls(b, ds - 1), nle = se_le(b, n, de);
                    if (se_blank(b, pls, ds - 1) && se_blank(b, de, nle)) de = nle < n ? nle + 1 : nle;
                }
            } else if (own_end) { de = eol; while (ds > 0 && (b[ds - 1] == ' ' || b[ds - 1] == '\t')) ds--; }
            if (de >= n) while (ds > 0) { const size_t pls = se_ls(b, ds - 1); if (!se_blank(b, pls, ds - 1)) break; ds = pls; }     /* fim do arquivo: sem linhas vazias sobrando */
            cut0 = ds; cut1 = de;
        }
        const size_t inl = ins.p ? ins.n : 0;
        const size_t nn = cut0 + inl + (n - cut1);
        nb = (char *)malloc(nn + 2);
        if (!nb) { snprintf(out, cap, "error: out of memory"); free(ins.p); break; }
        memcpy(nb, b, cut0); if (inl) memcpy(nb + cut0, ins.p, inl); memcpy(nb + cut0 + inl, b + cut1, n - cut1); nb[nn] = 0;
        free(ins.p);
        const size_t rs = cut0 + tx0, re = cut0 + tx1;                        /* o texto do modelo dentro do arquivo novo */
        char ctx[300];

        /* ---- V1: balanceamento da regiao nova e do arquivo todo, comparado com o antes ---- */
        nmask = se_mask(L, nb, nn);
        SeErr e1;
        if (brk && nmask && wk != W_DELETE && re > rs) {
            int pre = 0;
            if (L == CL_C || L == CL_CPP || L == CL_OBJC) for (size_t p = rs; p < re; p++) if (nb[p] == '#' && (p == 0 || nb[p - 1] == '\n') && (!strncmp(nb + p, "#if", 3) || !strncmp(nb + p, "#el", 3))) pre = 1;
            if (!pre && se_balance(nmask, nn, rs, re, brk, 0, &e1)) {
                se_line_text(nb, nn, atoi(e1.err + 5), ctx, sizeof ctx);
                se_apply_fail(out, cap, "structural", e1.err, ctx); break;
            }
        }
        if (brk && nmask) {
            SeErr eo, en;
            const int bo = se_balance(omask, n, 0, n, brk, 1, &eo), bn = se_balance(nmask, nn, 0, nn, brk, 1, &en);
            if (bn && !bo) { se_line_text(nb, nn, atoi(en.err + 5), ctx, sizeof ctx); se_apply_fail(out, cap, "structural", en.err, ctx); break; }
        }
        /* ---- V2: o scanner da familia no arquivo novo: o simbolo existe e e do mesmo tipo ---- */
        coder_parse(abs, nb, nn, &nf); have_nf = 1;
        int mi = -1;                                                           /* o simbolo resultante em nf */
        if (wk == W_REPLACE || wk == W_BODY) {
            const size_t lo = wk == W_BODY ? sy->byte0 : rs, hi = wk == W_BODY ? sy->byte0 + 1 : (re > rs ? re : rs + 1);
            for (int i = 0; i < nf.nsym; i++) if (nf.sym[i].byte0 >= lo && nf.sym[i].byte0 < hi) { mi = i; break; }
            if (mi < 0) {
                snprintf(ctx, sizeof ctx, "expected a %s named `%s` (or its replacement)", se_kindn(sy->kind), sy->name);
                se_apply_fail(out, cap, "structural", wk == W_BODY ? "after the edit the scanner no longer sees this symbol" : "the scanner finds no definition in new_string", ctx); break;
            }
            if (!se_kind_eq(nf.sym[mi].kind, sy->kind)) {
                snprintf(ctx, sizeof ctx, "`%s` was a %s and would become a %s", sy->name, se_kindn(sy->kind), se_kindn(nf.sym[mi].kind));
                se_apply_fail(out, cap, "structural", "the symbol would change kind", ctx); break;
            }
            if (wk == W_BODY && strcmp(nf.sym[mi].name, sy->name) != 0) { se_apply_fail(out, cap, "structural", "the new body changed how the scanner reads the header", ""); break; }
        }
        /* ---- V3: sintaxe pela ferramenta da linguagem; so reprova o que o arquivo antes nao tinha ---- */
        char xm[900] = "";
        int xs = se_ext_syntax(abs, nb, nn, xm, sizeof xm);
        if (xs == 0) {
            char om[300];
            if (se_ext_syntax(abs, b, n, om, sizeof om) != 0) {
                int ln = 0; const char *p = strstr(xm, "line "); if (p) ln = atoi(p + 5);
                ctx[0] = 0; if (ln > 0 && !strchr(xm, '\n')) se_line_text(nb, nn, ln, ctx, sizeof ctx);
                se_apply_fail(out, cap, "structural", xm, ctx); break;
            }
            xs = -2;                                                           /* o arquivo ja nao passava antes */
        }

        /* ---- achados ---- */
        Buf rep = {0};
        char rel[1100], root[1100] = "";
        { char cwd[1100]; rel[0] = 0;
          if (getcwd(cwd, sizeof cwd) && !ct_cwd_is_home()) { const size_t cl = strlen(cwd); if (!strncmp(abs, cwd, cl) && abs[cl] == '/') { snprintf(root, sizeof root, "%s", cwd); snprintf(rel, sizeof rel, "%s", abs + cl + 1); } }
          if (!root[0]) { char r2[1100]; if (index_marker_root(abs, r2, sizeof r2)) { const size_t cl = strlen(r2); if (!strncmp(abs, r2, cl) && abs[cl] == '/') { snprintf(root, sizeof root, "%s", r2); snprintf(rel, sizeof rel, "%s", abs + cl + 1); } } }
          if (!rel[0]) { const char *bs = strrchr(abs, '/'); snprintf(rel, sizeof rel, "%s", bs ? bs + 1 : abs); } }
        char t[1200];
        if (wk == W_DELETE) snprintf(t, sizeof t, "Deleted %s `%s` (was lines %d-%d) from %s.", se_kindn(sy->kind), sy->name, sy->line0, sy->line1, rel);
        else if (wk == W_BEFORE || wk == W_AFTER) snprintf(t, sizeof t, "Inserted your text %s %s `%s` in %s (new lines %d-%d).", w, se_kindn(sy->kind), sy->name, rel, se_line_of(nb, rs), se_line_of(nb, re > rs ? re - 1 : rs));
        else snprintf(t, sizeof t, "Edited %s `%s` in %s: lines %d-%d -> %d-%d (%s).", se_kindn(sy->kind), sy->name, rel, sy->line0, sy->line1, nf.sym[mi].line0, nf.sym[mi].line1, w);
        se_add(&rep, t);
        snprintf(t, sizeof t, "\n[structural] %s%s", brk ? "brackets balanced" : "brackets not checked for this language",
                 xs == 1 ? ", syntax ok" : xs == -2 ? ", the file already failed its syntax check before (not blamed on this edit)" : "");
        se_add(&rep, t);
        if (mi >= 0) {
            if (strcmp(nf.sym[mi].name, sy->name)) snprintf(t, sizeof t, ", symbol renamed `%s` -> `%s`", sy->name, nf.sym[mi].name); else snprintf(t, sizeof t, ", `%s` still a %s", sy->name, se_kindn(nf.sym[mi].kind));
            se_add(&rep, t);
        }
        int sigchg = 0;
        if (wk == W_REPLACE && mi >= 0 && sy->sig && nf.sym[mi].sig && se_callable(sy->kind)) {
            char sa[300], sb2[300]; se_norm_sig(sy->sig, sa, sizeof sa); se_norm_sig(nf.sym[mi].sig, sb2, sizeof sb2);
            if (strcmp(sa, sb2) != 0) {
                const int pa = se_nparams(sa), pb = se_nparams(sb2);
                snprintf(t, sizeof t, "\n[structural] signature changed: `%.200s` -> `%.200s`", sa, sb2); se_add(&rep, t);
                if (pa >= 0 && pb >= 0 && pa != pb) { snprintf(t, sizeof t, " (%d -> %d parameters)", pa, pb); se_add(&rep, t); }
                sigchg = strcmp(nf.sym[mi].name, sy->name) == 0;
            }
        }
        if (wk == W_REPLACE && (b[sy->byte0] == '@' || (b[sy->byte0] == '#' && b[sy->byte0 + 1] == '[')) && nb[rs] != b[sy->byte0])
            se_add(&rep, "\n[structural] warning: the old definition started with decorators/annotations and yours does not, so they are gone");
        char gone[8][120], added[8][120];
        const int ngone = se_set_minus(&of, &nf, gone, 8), nadd = se_set_minus(&nf, &of, added, 8);
        if (nadd > 0) { se_add(&rep, "\n[structural] new in the file: "); for (int i = 0; i < nadd && i < 8; i++) { if (i) se_add(&rep, ", "); se_add(&rep, added[i]); } }
        if (ngone > 0) { se_add(&rep, wk == W_DELETE ? "\n[structural] removed: " : "\n[structural] no longer defined: "); for (int i = 0; i < ngone && i < 8; i++) { if (i) se_add(&rep, ", "); se_add(&rep, gone[i]); } }
        /* chamadores dos nomes que sumiram, e do simbolo cuja assinatura mudou */
        char want[7][120]; int nw = 0;
        for (int i = 0; i < ngone && i < 8 && nw < 6; i++) { const char *dot = strrchr(gone[i], '.'); const char *nm = dot ? dot + 1 : gone[i]; int dup = 0; for (int k = 0; k < nw; k++) if (!strcmp(want[k], nm)) dup = 1; if (!dup) snprintf(want[nw++], 120, "%s", nm); }
        if (sigchg) { int dup = 0; for (int k = 0; k < nw; k++) if (!strcmp(want[k], sy->name)) dup = 1; if (!dup && nw < 6) snprintf(want[nw++], 120, "%s", sy->name); }
        char nocall[6][120]; int nnc = 0, nocall_scope = 0;
        for (int i = 0; i < nw; i++) {
            SeCall cl[4], oc[4]; int tot2 = 0, nd = 0;
            const int tot1 = se_same_file_callers(&nf, want[i], rel, cl, 4);
            const int got = tot1 < 4 ? tot1 : 4;
            const int n2 = root[0] ? se_index_callers(root, rel, want[i], oc, 4, &tot2, &nd) : -1;
            if (tot1 + (tot2 > 0 ? tot2 : 0) == 0) {
                if (nnc < 6) snprintf(nocall[nnc++], 120, "%s", want[i]);
                nocall_scope = n2 >= 0 ? 0 : root[0] ? 1 : 2;
                continue;
            }
            size_t k = (size_t)snprintf(t, sizeof t, "\n[derived] callers of `%s` that may need updating: ", want[i]);
            for (int c = 0; c < got && k + 100 < sizeof t; c++) k += (size_t)snprintf(t + k, sizeof t - k, "%s%s", c ? ", " : "", cl[c].where);
            for (int c = 0; c < n2 && k + 160 < sizeof t; c++) k += (size_t)snprintf(t + k, sizeof t - k, "%s%s", got + c ? ", " : "", oc[c].where);
            const int more = tot1 + (tot2 > 0 ? tot2 : 0) - got - (n2 > 0 ? n2 : 0);
            if (more > 0) k += (size_t)snprintf(t + k, sizeof t - k, ", +%d more", more);
            if (n2 > 0 && nd > 1) snprintf(t + k, sizeof t - k, " (by name; the project has %d definitions of `%s`, so some may call another)", nd, want[i]); else snprintf(t + k, sizeof t - k, " (by name)");
            se_add(&rep, t);
        }

        if (nnc) {
            se_add(&rep, "\n[derived] no callers found by name for: ");
            for (int i = 0; i < nnc; i++) { if (i) se_add(&rep, ", "); se_add(&rep, nocall[i]); }
            se_add(&rep, nocall_scope == 0 ? " (this file and the project index)" : nocall_scope == 1 ? " (this file only: the project index could not be read)" : " (this file only: it is outside the working folder)");
        }
        /* ---- LSP opcional: diagnosticos antes (disco) e depois (so os introduzidos) ---- */
        char *dg0 = NULL, srv[64] = "", lwhy[100] = "";
        const char *lsp_env = getenv("KATANA_EDIT_LSP");
        const double lsecs = (getenv("KATANA_EDIT_LSP_MS") ? atof(getenv("KATANA_EDIT_LSP_MS")) : 2500) / 1000.0;
        const int use_lsp = !(lsp_env && !strcmp(lsp_env, "0"));
        const int lsp0 = use_lsp ? lsp_diag_get(abs, lsecs, 0, &dg0, srv, sizeof srv, lwhy, sizeof lwhy) : 0;

        /* ---- confirma e grava ---- */
        char what[4400];
        snprintf(what, sizeof what, "editar o arquivo %s (simbolo %s, %s)", abs, sy->name, w);
        if (!agent_act(what, ns ? ns : "")) { snprintf(out, cap, "error: the user denied this action"); free(rep.p); free(dg0); break; }
        if (!ct_f_save(abs, nb, nn, out, cap)) { free(rep.p); free(dg0); break; }
        if (seen >= 2) ct_seen_mark(abs, 0);
        if (lsp0) {
            char *dg1 = NULL, srv1[64], why1[100];
            if (lsp_diag_get(abs, lsecs, 1, &dg1, srv1, sizeof srv1, why1, sizeof why1)) {
                static SeDiag d0[200], d1[200];
                const int c0 = se_diag_parse(dg0, d0, 200), c1 = se_diag_parse(dg1, d1, 200);
                int used[200] = {0}, nnew = 0, nwarn = 0; char lt[1000]; size_t k = 0; lt[0] = 0;
                for (int i = 0; i < c1; i++) {
                    int found = 0;
                    for (int j = 0; j < c0; j++) if (!used[j] && d0[j].sev == d1[i].sev && !strcmp(d0[j].msg, d1[i].msg)) { used[j] = 1; found = 1; break; }
                    if (found) continue;
                    if (d1[i].sev == 1) { if (nnew < 5 && k + 260 < sizeof lt) k += (size_t)snprintf(lt + k, sizeof lt - k, "\n  line %d: %.180s", d1[i].line, d1[i].msg); nnew++; }
                    else if (d1[i].sev == 2) nwarn++;
                }
                snprintf(t, sizeof t, "\n[lsp %s] ", srv1); se_add(&rep, t);
                if (!nnew) se_add(&rep, "no new errors");
                else { snprintf(t, sizeof t, "%d new error%s introduced by this edit:", nnew, nnew > 1 ? "s" : ""); se_add(&rep, t); se_add(&rep, lt); }
                if (nwarn) { snprintf(t, sizeof t, "%s%d new warning%s", nnew ? "\n  and " : ", ", nwarn, nwarn > 1 ? "s" : ""); se_add(&rep, t); }
            } else { snprintf(t, sizeof t, "\n[lsp %s] no diagnostics after the edit (%s)", srv, why1); se_add(&rep, t); }
            free(dg1);
        }
        free(dg0);
        if (getenv("KATANA_EDIT_TIME")) { snprintf(t, sizeof t, "\n[edit validated and written in %.0f ms]", (now_sec() - t0) * 1000); se_add(&rep, t); }
        snprintf(out, cap, "%s", rep.p ? rep.p : "Edited.");
        free(rep.p);
    } while (0);
    if (have_of) coder_file_free(&of);
    if (have_nf) coder_file_free(&nf);
    free(fp); free(spec); free(where); free(ns); free(b); free(nb); free(nmask); free(omask);
}
#else
static void ct_edit_symbol(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
#endif
