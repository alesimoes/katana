/* ---------- curriculo v7: Symbols, Find novo, lugar citado, cursor de leitura, linhas [katana: ...] ----------
 * Extensao do v6 (as familias de v1 a v6 ficam como estao; o v7 as gera de novo com o binario e o prompt de agora). Cinco familias novas,
 * todas na mesma maquina de scenario.c: o projeto e de verdade, toda chamada passa pelo despacho do motor, nenhuma saida e escrita
 * a mao, e o check refaz a conversa e compara byte a byte.
 *   x-symbols      navegar pelo indice de simbolos (Symbols: callers, def, callees, impact, at, deps, outline) em projetos de sete
 *                  linguagens; chamadas independentes na mesma fala; Read so da faixa que a saida indicou; NOT FOUND e nome parecido
 *                  tratados sem inventar.
 *   x-find7        o Find de agora: identificador (Found by name or text), texto entre aspas (Literal text found), nome com erro de
 *                  digitacao, descricao em palavras, NOT FOUND e "No match", mode=refs pelo indice de simbolos.
 *   x-place        o usuario cita um projeto que esta em outro lugar: a nota [katana: you mention ...] do motor, path desde a primeira
 *                  chamada, Read com "Did you mean", recusa de Find/Glob na pasta home.
 *   x-cursor       arquivo grande lido em partes: o cursor do Read ("lines 1-N ... already given") e a nota de repeticao.
 *   x-katananotes  seguir qualquer linha [katana: ...]: repeticao, "plan first", lista de todo em aberto, indice criado.
 * Cerca de 10% dos pedidos saem em portugues, sobre codigo em ingles (x7_lang). */

/* ---- 10% dos pedidos em portugues; o resto em ingles (decidido pelo sorteio da conversa, entao o check refaz igual) ---- */
static void x7_lang(void) { g_cu_lang = cu_pct(10) ? 1 : 0; }

/* x_call, x_run e x_user ja passam pelo controle de repeticao do laco (rr_check / rr_after / rr_reset, em scenario.c): estes nomes ficam por clareza */
static const char *x7_call(const char *say, const char *name, const char *args, int flags) { return x_call(say, name, args, flags); }
static void x7_user(const char *t) { x_user(t); }
/* o aviso de tempo do indice ("[symbol index created in 0.1 s]") muda de uma execucao para outra: o texto gravado leva sempre 0.0 */
static void x7_idx_norm(const char *r) {
    if (strncmp(r, "[symbol index ", 14)) return;
    const char *in = strstr(r, " in "), *e = in ? strstr(in, " s]") : NULL;
    if (!in || !e || e - in > 14 || g_x.nsub >= 4) return;
    const char *from = cu_fmt("%.*s", (int)(e + 3 - in), in);
    if (!strcmp(from, " in 0.0 s]")) return;
    g_x.sfrom[g_x.nsub] = from; g_x.sto[g_x.nsub++] = " in 0.0 s]";
}
static const char *x7_sym(const char *say, const char *args) { const char *r = x7_call(say, "Symbols", args, 0); x7_idx_norm(r); return r; }
static const char *x7_find(const char *say, const char *args) { return x7_call(say, "Find", args, 0); }
static const char *x7_read(const Scn *s, const char *rel, int off, int lim, int flags) {
    return x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": %d}", cu_q(x_abs(s, rel)), off, lim), flags);
}
static const char *x7_say(void) { return cu_pct(30) ? CU_PA(((const char *const[]){L("Asking the symbol index.", "Pergunto ao indice de simbolos."), L("The index answers this exactly.", "O indice responde isso com exatidao."), L("Checking the call graph.", "Vejo o grafo de chamadas.")})) : NULL; }

/* ======================= projetos de navegacao: sete linguagens, grafo de chamadas de verdade ======================= */
enum { N7_PY, N7_JS, N7_TS, N7_C, N7_GO, N7_RS, N7_JV, N7_NL };
static const char *const g_n7_ext[N7_NL] = {"py", "js", "ts", "c", "go", "rs", "java"};
static const char *const g_n7_lname[N7_NL] = {"Python", "JavaScript", "TypeScript", "C", "Go", "Rust", "Java"};
static const char *const g_n7_verb[] = {"parse", "load", "build", "check", "apply", "merge", "format", "scale", "round", "clamp", "split", "join", "count", "rank", "pick", "mark", "trim", "fetch"};
static const char *const g_n7_noun[] = {"header", "config", "invoice", "tax", "quota", "token", "route", "order", "price", "label", "batch", "record", "window", "limit", "score", "ticket", "cache", "stock"};
static const char *const g_n7_msg[] = {"invoice not found", "quota exceeded", "retry limit reached", "session expired", "payment declined", "route is unavailable", "token has expired", "batch is empty", "stock is below the minimum", "ticket already closed"};
static const char *const g_n7_msgid[] = {"INVOICE_MISSING", "QUOTA_FULL", "RETRY_LIMIT", "SESSION_GONE", "PAYMENT_DENIED", "ROUTE_DOWN", "TOKEN_OLD", "BATCH_EMPTY", "STOCK_LOW", "TICKET_CLOSED"};
typedef struct { char name[48]; int layer, nc, call[3], flip[3], k, mod, line; } X7F;
typedef struct { char name[24], cls[24]; int nf, f[16]; char text[14000]; } X7M;
typedef struct {
    int lang, nf, nm, hub, big, nmsg, msgi[2];
    X7F f[20]; X7M m[4];
    char dir[8];
} X7N;
static X7N g_x7n;
static const char *x7n_style(int lang, const char *v, const char *n) {
    if (lang == N7_PY || lang == N7_C || lang == N7_RS) return cu_fmt("%s_%s", v, n);
    char *o = cu_fmt("%s%s", v, n);
    if (lang != N7_GO) o[strlen(v)] = (char)toupper((unsigned char)o[strlen(v)]);
    else { o[0] = (char)toupper((unsigned char)o[0]); o[strlen(v)] = (char)toupper((unsigned char)o[strlen(v)]); }
    return o;
}
static int x7n_idx(const X7N *n, const char *name) { for (int i = 0; i < n->nf; i++) if (!strcmp(n->f[i].name, name)) return i; return -1; }
/* uma chamada: Java chama as de outro modulo com a classe na frente */
static const char *x7n_callx(const X7N *n, int from, int to, const char *a1, const char *a2) {
    if (n->lang == N7_JV && n->f[from].mod != n->f[to].mod) return cu_fmt("%s.%s(%s, %s)", n->m[n->f[to].mod].cls, n->f[to].name, a1, a2);
    return cu_fmt("%s(%s, %s)", n->f[to].name, a1, a2);
}
static const char *x7n_expr(const X7N *n, int i) {
    const X7F *f = &n->f[i]; const char *A = n->big ? "t" : "a", *B = n->big ? "u" : "b";
    Buf b = {0}; buf_str(&b, "");
    if (!f->nc) {
        if (n->big) return cu_fmt("u + %d", f->k);
        const int sh = f->k % 4;
        return sh == 0 ? cu_fmt("a * %d + b", f->k) : sh == 1 ? cu_fmt("a + b * %d", f->k) : sh == 2 ? cu_fmt("(a + b) * %d", f->k) : cu_fmt("a * b + %d", f->k);
    }
    for (int c = 0; c < f->nc; c++) buf_str(&b, cu_fmt("%s%s", c ? " + " : "", x7n_callx(n, i, f->call[c], f->flip[c] ? B : A, f->flip[c] ? A : B)));
    buf_str(&b, cu_fmt(" + %d", f->k));
    return cu_keep(b.p);
}
static const char *x7n_cmt(int lang, const char *t) { return lang == N7_PY ? cu_fmt("# %s\n", t) : cu_fmt("// %s\n", t); }
static const char *x7n_def_head(const X7N *n, int i) {
    const char *nm = n->f[i].name;
    switch (n->lang) {
    case N7_PY: return cu_fmt("def %s(a, b):\n", nm);
    case N7_JS: return cu_fmt("function %s(a, b) {\n", nm);
    case N7_TS: return cu_fmt("export function %s(a: number, b: number): number {\n", nm);
    case N7_C:  return cu_fmt("int %s(int a, int b) {\n", nm);
    case N7_GO: return cu_fmt("func %s(a, b int) int {\n", nm);
    case N7_RS: return cu_fmt("pub fn %s(a: i32, b: i32) -> i32 {\n", nm);
    default:    return cu_fmt("    public static int %s(int a, int b) {\n", nm);
    }
}
static const char *x7n_def(const X7N *n, int i) {
    Buf b = {0}; buf_str(&b, "");
    const X7F *f = &n->f[i]; const char *e = x7n_expr(n, i);
    if (n->big) {
        buf_str(&b, x7n_cmt(n->lang, cu_fmt("%s %s: step %d of the %s module", g_n7_verb[(f->k * 3 + i) % 18], g_n7_noun[(f->k + i * 5) % 18], i + 1, n->m[f->mod].name)));
        static const char *const cm[] = {"keeps whole numbers only; no rounding", "the two arguments are never negative", "callers pass the amount first and the limit second", "the result is used by the nightly report", "changing the order of the arguments breaks the report",
            "no state is kept between calls", "values come from the settings loaded at start", "the tests cover the small cases only", "kept simple on purpose; see the module notes", "the same rule is applied in the invoice export"};
        for (int c = 0; c < 7; c++) buf_str(&b, x7n_cmt(n->lang, cm[(i * 3 + c * 7 + f->k) % 10]));
    }
    buf_str(&b, x7n_def_head(n, i));
    const char *loc1 = n->big ? (n->lang == N7_PY ? cu_fmt("    t = a * %d\n    u = t + b\n", f->k) : n->lang == N7_JS || n->lang == N7_TS ? cu_fmt("  const t = a * %d;\n  const u = t + b;\n", f->k) : n->lang == N7_C ? cu_fmt("    int t = a * %d;\n    int u = t + b;\n", f->k)
                  : n->lang == N7_GO ? cu_fmt("\tt := a * %d\n\tu := t + b\n", f->k) : n->lang == N7_RS ? cu_fmt("    let t = a * %d;\n    let u = t + b;\n", f->k) : cu_fmt("        int t = a * %d;\n        int u = t + b;\n", f->k)) : "";
    buf_str(&b, loc1);
    switch (n->lang) {
    case N7_PY: buf_str(&b, cu_fmt("    return %s\n", e)); break;
    case N7_JS: case N7_TS: buf_str(&b, cu_fmt("  return %s;\n}\n", e)); break;
    case N7_C: buf_str(&b, cu_fmt("    return %s;\n}\n", e)); break;
    case N7_GO: buf_str(&b, cu_fmt("\treturn %s\n}\n", e)); break;
    case N7_RS: buf_str(&b, cu_fmt("    %s\n}\n", e)); break;
    default: buf_str(&b, cu_fmt("        return %s;\n    }\n", e)); break;
    }
    return cu_keep(b.p);
}
static const char *x7n_file(const X7N *n, int mi) {
    return n->lang == N7_JV ? cu_fmt("src/%s.java", n->m[mi].cls) : cu_fmt("src/%s.%s", n->m[mi].name, g_n7_ext[n->lang]);
}
static const char *x7n_hdr(const X7N *n, int mi) { return cu_fmt("src/%s.h", n->m[mi].name); }
/* funcoes de outros modulos que as do modulo mi chamam, por modulo */
static int x7n_uses(const X7N *n, int mi, int om, const char **names) {      /* names: ate 16 */
    int k = 0;
    for (int q = 0; q < n->m[mi].nf; q++) { const X7F *f = &n->f[n->m[mi].f[q]]; for (int c = 0; c < f->nc; c++) {
        const X7F *g = &n->f[f->call[c]]; if (g->mod != om) continue;
        int dup = 0; for (int j = 0; j < k; j++) dup |= !strcmp(names[j], g->name);
        if (!dup) names[k++] = g->name;
    } }
    return k;
}
static void x7n_build(X7N *n, int mi) {
    X7M *m = &n->m[mi]; Buf b = {0};
    const int L = n->lang;
    buf_str(&b, L == N7_GO ? "package main\n\n" : L == N7_JV ? "package shop;\n\n" : L == N7_JS ? "'use strict';\n" : "");
    if (L == N7_C) buf_str(&b, cu_fmt("#include \"%s.h\"\n", m->name));
    for (int om = 0; om < n->nm; om++) {
        const char *nm[16]; if (om == mi) continue;
        const int k = x7n_uses(n, mi, om, nm); if (!k) continue;
        Buf l = {0}; buf_str(&l, ""); for (int j = 0; j < k; j++) buf_str(&l, cu_fmt("%s%s", j ? ", " : "", nm[j]));
        if (L == N7_PY) buf_str(&b, cu_fmt("from .%s import %s\n", n->m[om].name, l.p));
        else if (L == N7_JS) buf_str(&b, cu_fmt("const { %s } = require('./%s');\n", l.p, n->m[om].name));
        else if (L == N7_TS) buf_str(&b, cu_fmt("import { %s } from './%s';\n", l.p, n->m[om].name));
        else if (L == N7_C) buf_str(&b, cu_fmt("#include \"%s.h\"\n", n->m[om].name));
        else if (L == N7_RS) buf_str(&b, cu_fmt("use crate::%s::{%s};\n", n->m[om].name, l.p));
        free(l.p);
    }
    if (L == N7_JV) buf_str(&b, cu_fmt("public class %s {\n", m->cls));
    for (int q = 0; q < n->nmsg && mi == 0; q++) {
        const char *id = g_n7_msgid[n->msgi[q]], *tx = g_n7_msg[n->msgi[q]];
        buf_str(&b, L == N7_PY ? cu_fmt("\n%s = \"%s\"\n", id, tx) : L == N7_JS ? cu_fmt("\nconst %s = \"%s\";\n", id, tx) : L == N7_TS ? cu_fmt("\nexport const %s = \"%s\";\n", id, tx) : L == N7_C ? cu_fmt("\nconst char *%s = \"%s\";\n", id, tx)
                   : L == N7_GO ? cu_fmt("\nconst %s = \"%s\"\n", id, tx) : L == N7_RS ? cu_fmt("\npub const %s: &str = \"%s\";\n", id, tx) : cu_fmt("    public static final String %s = \"%s\";\n", id, tx));
    }
    for (int q = 0; q < m->nf; q++) { buf_str(&b, L == N7_PY ? "\n\n" : "\n"); buf_str(&b, x7n_def(n, m->f[q])); }
    if (L == N7_JS) { buf_str(&b, "\nmodule.exports = { "); for (int q = 0; q < m->nf; q++) buf_str(&b, cu_fmt("%s%s", q ? ", " : "", n->f[m->f[q]].name)); buf_str(&b, " };\n"); }
    if (L == N7_JV) buf_str(&b, "}\n");
    snprintf(m->text, sizeof m->text, "%s", b.p); free(b.p);
}
/* o projeto: nf funcoes em tres camadas (folhas, intermediarias, de cima); a folha hub e chamada por tres delas */
static void x7n_gen(X7N *n, const Scn *s, int lang, int nmod, int nleaf, int nmid, int ntop, int nmsg, int big) {
    memset(n, 0, sizeof *n); n->lang = lang; n->nm = nmod; n->big = big; n->nmsg = nmsg;
    snprintf(n->dir, sizeof n->dir, "src/");
    int used[16] = {0}; n->nf = nleaf + nmid + ntop;
    for (int i = 0; i < n->nf; i++) {
        X7F *f = &n->f[i]; f->layer = i < nleaf ? 0 : i < nleaf + nmid ? 1 : 2; f->k = 2 + cu_n(8);
        for (int t = 0; ; t++) {
            const char *nm = x7n_style(lang, CU_PA(g_n7_verb), CU_PA(g_n7_noun));
            int dup = 0; for (int j = 0; j < i; j++) dup |= !strcmp(n->f[j].name, nm);
            if (!dup) { snprintf(f->name, sizeof f->name, "%s", nm); break; }
            if (t > 200) cu_die("sem nome de funcao");
        }
    }
    int mu[16] = {0}; (void)used;
    for (int m = 0; m < nmod; m++) {
        int mi; do mi = cu_n(CU_N(g_x_modnames)); while (!strcmp(g_x_modnames[mi], s->name) || mu[mi]);
        mu[mi] = 1; snprintf(n->m[m].name, sizeof n->m[m].name, "%s", g_x_modnames[mi]);
        snprintf(n->m[m].cls, sizeof n->m[m].cls, "%s", g_x_modnames[mi]); n->m[m].cls[0] = (char)toupper((unsigned char)n->m[m].cls[0]);
    }
    if (big) { n->nm = nmod = 1; }
    for (int i = 0; i < n->nf; i++) {      /* cada modulo recebe funcoes; todo modulo tem pelo menos uma */
        n->f[i].mod = i < nmod ? (i + cu_n(nmod)) % nmod : cu_n(nmod);
    }
    { int has[4] = {0}; for (int i = 0; i < n->nf; i++) has[n->f[i].mod] = 1; for (int m = 0; m < nmod; m++) if (!has[m]) { n->f[n->nf - 1 - m].mod = m; has[m] = 1; } }
    n->hub = 0;
    for (int i = nleaf; i < n->nf; i++) {
        X7F *f = &n->f[i];
        const int lo = f->layer == 1 ? 0 : nleaf, hi = f->layer == 1 ? nleaf : nleaf + nmid;      /* a camada de cima chama as intermediarias */
        f->nc = 1 + cu_n(2);
        for (int c = 0; c < f->nc; c++) {
            int t; for (t = 0; t < 50; t++) { const int g = lo + cu_n(hi - lo); int dup = 0; for (int j = 0; j < c; j++) dup |= f->call[j] == g; if (!dup) { f->call[c] = g; break; } }
            if (t == 50) f->nc = c;
            f->flip[c] = cu_n(2);
        }
        if (!f->nc) cu_die("funcao sem chamada");
    }
    int hc = 0;      /* o hub (folha 0) tem ao menos tres chamadores diretos */
    for (int i = nleaf; i < n->nf && hc < 3; i++) {
        X7F *f = &n->f[i]; if (f->layer != 1) continue;
        int has = 0; for (int c = 0; c < f->nc; c++) has |= f->call[c] == 0;
        if (!has) { if (f->nc < 2) { f->call[f->nc] = 0; f->flip[f->nc++] = cu_n(2); } else f->call[0] = 0; }
        hc++;
    }
    for (int i = 0; i < n->nf; i++) if (n->f[i].nc && n->f[i].layer == 2) { int ok = 0; for (int c = 0; c < n->f[i].nc; c++) ok |= n->f[n->f[i].call[c]].layer == 1; if (!ok) cu_die("camada de cima sem intermediaria"); }
    for (int i = 0; i < n->nf; i++) { X7M *m = &n->m[n->f[i].mod]; m->f[m->nf++] = i; if (m->nf > 15) cu_die("modulo grande demais"); }
    for (int m = 0; m < nmod; m++) x7n_build(n, m);
    for (int q = 0; q < 2; q++) if (q < nmsg) { int t; do t = cu_n(CU_N(g_n7_msg)); while (q && t == n->msgi[0]); n->msgi[q] = t; }
    if (nmsg) { for (int m = 0; m < nmod; m++) x7n_build(n, m); }
}
/* o texto das mensagens foi sorteado depois da primeira montagem: monta de novo antes de gravar */
static void x7n_write(const X7N *n, const Scn *s) {
    if (n->lang == N7_PY) x_put(s, "src/__init__.py", "", 0);
    if (n->lang == N7_RS) x_put(s, "Cargo.toml", "[package]\nname = \"shop\"\nversion = \"0.1.0\"\nedition = \"2021\"\n", 0);
    if (n->lang == N7_GO) x_put(s, "go.mod", "module shop\n\ngo 1.21\n", 0);
    g_x_mt = 0;
    for (int m = 0; m < n->nm; m++) {
        x_put(s, x7n_file(n, m), n->m[m].text, 0);
        if (n->lang == N7_C) { Buf h = {0}; buf_str(&h, "#ifndef H_"); buf_str(&h, n->m[m].name); buf_str(&h, "\n#define H_"); buf_str(&h, n->m[m].name); buf_str(&h, "\n\n");
            for (int q = 0; q < n->m[m].nf; q++) buf_str(&h, cu_fmt("int %s(int a, int b);\n", n->f[n->m[m].f[q]].name));
            buf_str(&h, "\n#endif\n"); x_put(s, x7n_hdr(n, m), h.p, 0); free(h.p); }
    }
}
static void x7_prehook(const Scn *s) { x7n_write(&g_x7n, s); }
/* linha (1..) em que a funcao e definida no texto do modulo */
static int x7n_defline(const X7N *n, int i) {
    const char *t = n->m[n->f[i].mod].text; const char *h = x7n_def_head(n, i); char first[200]; snprintf(first, sizeof first, "%.*s", (int)strcspn(h, "\n"), h);
    int ln = 1;
    for (const char *p = t; *p; ) { const size_t k = strcspn(p, "\n"); if (k == strlen(first) && !strncmp(p, first, k)) return ln; p += k; if (*p) p++; ln++; }
    cu_die("definicao de %s nao achada", n->f[i].name); return 0;
}
static int x7n_nlines(const char *t) { int n = 0; for (; *t; t++) n += *t == '\n'; return n; }
/* quem chama i, direto */
static int x7n_callers(const X7N *n, int i, int *out) { int k = 0; for (int j = 0; j < n->nf; j++) for (int c = 0; c < n->f[j].nc; c++) if (n->f[j].call[c] == i) { out[k++] = j; break; } return k; }
/* fecho dos chamadores ate depth; devolve quantos, com o nivel de cada */
static int x7n_closure(const X7N *n, int i, int depth, int *ids, int *lvl) {
    int k = 0, from = 0, cur[16], nc = 0; cur[nc++] = i;
    for (int d = 1; d <= depth; d++) {
        int next[16], nn = 0;
        for (int a = 0; a < nc; a++) { int cs[16]; const int m = x7n_callers(n, cur[a], cs); for (int q = 0; q < m; q++) { int seen = 0; for (int z = 0; z < k; z++) seen |= ids[z] == cs[q]; if (!seen && cs[q] != i) { ids[k] = cs[q]; lvl[k++] = d; next[nn++] = cs[q]; } } }
        memcpy(cur, next, sizeof cur); nc = nn; (void)from;
    }
    return k;
}
static const char *x7n_rel(const X7N *n, int i) { return x7n_file(n, n->f[i].mod); }

/* ---- leitura da saida do Symbols: uma linha de resultado comeca (depois do recuo de 2 por nivel) com caminho:linha ---- */
typedef struct { char path[100]; int a, b, depth; const char *rest; } X7Hit;
static int x7_hits(const char *out, X7Hit *h, int max) {
    int n = 0;
    for (const char *l = out; *l; ) {
        const size_t k = strcspn(l, "\n"); int ind = 0; while (l[ind] == ' ') ind++;
        const char *p = l + ind; const size_t tk = strcspn(p, " \n");
        const char *co = memchr(p, ':', tk);
        if (!isdigit((unsigned char)*p) && *p != '[' && co && co > p && isdigit((unsigned char)co[1]) && n < max && ind % 2 == 0 && ind < 8) {
            X7Hit *x = &h[n]; memset(x, 0, sizeof *x);
            snprintf(x->path, sizeof x->path, "%.*s", (int)(co - p), p);
            x->a = atoi(co + 1); const char *q = co + 1; while (isdigit((unsigned char)*q)) q++;
            x->b = *q == '-' ? atoi(q + 1) : x->a; x->depth = ind / 2;
            const char *sp = p + tk; while (*sp == ' ') sp++; x->rest = cu_fmt("%.*s", (int)((l + k) - sp), sp);
            n++;
        }
        l += k; if (*l) l++;
    }
    return n;
}
/* o nome depois de uma palavra-chave ("call NAME  in FN (a-b)"): copia ate o proximo espaco */
static const char *x7_after(const char *rest, const char *key) { const char *q = strstr(rest, key); if (!q) return NULL; q += strlen(key); return cu_fmt("%.*s", (int)strcspn(q, " ("), q); }
/* faixa "(a-b)" depois de " in FN " numa linha de callers; 0 se nao ha */
static int x7_range_in(const char *rest, int *a, int *b) { const char *q = strstr(rest, " ("); if (!q) return 0; return sscanf(q, " (%d-%d)", a, b) == 2; }
static const char *x7_callers_text(const X7Hit *h, int n, const char *callee) {
    Buf b = {0}; buf_str(&b, "");
    for (int i = 0; i < n; i++) { const char *fn = x7_after(h[i].rest, " in "); buf_str(&b, cu_fmt("%s`%s` (%s:%d)", i ? (i + 1 == n ? L(" and ", " e ") : ", ") : "", fn ? fn : "?", h[i].path, h[i].a)); }
    (void)callee; return cu_keep(b.p);
}
static int x7_noccur(const char *out) { int k = 0; const char *q = strstr(out, " occurrence"); if (!q) return -1; while (q > out && isdigit((unsigned char)q[-1])) q--; sscanf(q, "%d", &k); return k; }

/* ======================= x-symbols ======================= */
enum { S7_CALLERS, S7_DEF, S7_CALLEES, S7_IMPACT, S7_IMPACT0, S7_AT, S7_DEPS, S7_PAR, S7_OUTREAD, S7_TYPO, S7_MISSING, S7_N };
static const char *const g_s7_names[S7_N] = {"callers", "def", "callees", "impact", "impact-none", "at", "deps", "parallel", "outline-read", "typo", "missing"};
static const int g_s7_w[S7_N] = {4, 3, 3, 3, 1, 2, 2, 3, 2, 2, 1};
static int x7_pick(const int *w, int n);
static const char *x7_typo(const X7N *n, const char *nm) {
    for (int t = 0; t < 40; t++) {
        char *o = cu_fmt("%s", nm); const size_t l = strlen(o); const int i = 2 + cu_n((int)l - 4);
        if (cu_n(2) && o[i] != o[i + 1] && isalpha((unsigned char)o[i]) && isalpha((unsigned char)o[i + 1])) { const char c = o[i]; o[i] = o[i + 1]; o[i + 1] = c; }
        else if (isalpha((unsigned char)o[i])) memmove(o + i, o + i + 1, l - (size_t)i);
        else continue;
        if (strcmp(o, nm) && x7n_idx(n, o) < 0) return o;
    }
    cu_die("sem erro de digitacao para %s", nm); return NULL;
}
static void fam_x_symbols(FILE *f, Scn *s) {
    x7_lang();
    int sit, v, lang, fu;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de symbols");
        sit = x7_pick(g_s7_w, S7_N); v = cu_n(3); lang = cu_n(N7_NL); fu = cu_pct(35) ? 1 : 0;
        if (sit == S7_DEPS && lang != N7_JS && lang != N7_TS && lang != N7_C) continue;
        if (x_held(210 + sit, v, lang, fu, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
    g_x_prehook = x7_prehook;
    x_begin("x-symbols", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x_cell("symbols/%s/v%d/%s/fu%d", g_s7_names[sit], v, g_n7_ext[lang], fu);
    char sdone[2] = {0};
    int ti = 0;      /* a funcao de que se trata */
    const char *tn;
    if (sit == S7_CALLERS || sit == S7_TYPO) { int cs[16]; do ti = cu_n(n->nf); while (x7n_callers(n, ti, cs) < 1 || (sit == S7_CALLERS && ti != 0 && cu_pct(40) && x7n_callers(n, ti, cs) < 2)); }
    else if (sit == S7_CALLEES) { do ti = cu_n(n->nf); while (!n->f[ti].nc); }
    else if (sit == S7_IMPACT) { int ids[16], lv[16]; int t; for (t = 0; t < 200; t++) { ti = cu_n(n->nf); if (x7n_closure(n, ti, 3, ids, lv) >= 3) { int d2 = 0; for (int q = 0; q < 16 && q < x7n_closure(n, ti, 3, ids, lv); q++) d2 |= lv[q] >= 2; if (d2) break; } } if (t == 200) cu_die("sem impacto de dois niveis"); }
    else if (sit == S7_IMPACT0) { int cs[16]; do ti = cu_n(n->nf); while (x7n_callers(n, ti, cs) != 0 || n->f[ti].layer < 2); }
    else ti = cu_n(n->nf);
    tn = n->f[ti].name; (void)sdone;
    const char *rel = x7n_rel(n, ti); int dl = x7n_defline(n, ti);
    const char *ask;
    if (sit == S7_CALLERS) {
        ask = cu_fmt(v == 0 ? L("who calls `%s`?", "quem chama `%s`?") : v == 1 ? L("which functions call `%s`?", "quais funcoes chamam `%s`?") : L("I am about to change `%s`: where is it called from?", "vou mexer em `%s`: de onde ela e chamada?"), tn);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
        X7Hit h[16]; const int nh = x7_hits(r, h, 16); int cs[16]; const int ne = x7n_callers(n, ti, cs);
        if (nh != ne || x7_noccur(r) != ne) cu_die("callers de %s: esperava %d, o indice deu %d (%s)", tn, ne, nh, g_x.cell);
        for (int i = 0; i < nh; i++) { const char *fn = x7_after(h[i].rest, " in "); int ok = 0; for (int j = 0; j < ne; j++) ok |= fn && !strcmp(fn, n->f[cs[j]].name); if (!ok) cu_die("chamador inesperado %s", fn ? fn : "?"); }
        if (cu_pct(45)) {      /* ler so a faixa do primeiro chamador, que a saida deu */
            int a, b; if (!x7_range_in(h[0].rest, &a, &b)) cu_die("sem faixa no chamador");
            const char *rd = x7_read(s, h[0].path, a, b - a + 1, 0);
            const char *fn0 = x7_after(h[0].rest, " in ");
            if (!strstr(rd, tn)) cu_die("a leitura nao mostra a chamada");
            x_final(cu_fmt(L("`%s` is called by %s. The first one, `%s` in %s:%d, uses it in its return expression, as the lines I read show.", "`%s` e chamada por %s. A primeira, `%s` em %s:%d, usa ela na expressao de retorno, como mostram as linhas que li."), tn, x7_callers_text(h, nh, tn), fn0, h[0].path, h[0].a));
        } else x_final(cu_fmt(L("`%s` is called by %s. Nothing else in the indexed files calls it.", "`%s` e chamada por %s. Nada mais nos arquivos indexados a chama."), tn, x7_callers_text(h, nh, tn)));
        if (fu) {
            x_dep(); x7_user(XV(L("and what breaks if I change it?", "e o que quebra se eu mudar ela?"), L("what is affected if I change it?", "o que e afetado se eu mudar ela?")));
            const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"impact\", \"name\": %s}", cu_q(tn)));
            X7Hit h2[24]; const int n2 = x7_hits(r2, h2, 24); int ids[16], lv[16]; const int ne2 = x7n_closure(n, ti, 3, ids, lv);
            if (n2 != ne2) cu_die("impact: esperava %d, deu %d", ne2, n2);
            Buf bd = {0}, bi = {0}; buf_str(&bd, ""); buf_str(&bi, ""); int nd = 0, ni = 0;
            for (int i = 0; i < n2; i++) { const char *fn = x7_after(h2[i].rest, "caller "); if (!fn) cu_die("sem caller no impact"); if (h2[i].depth == 0) buf_str(&bd, cu_fmt("%s`%s`", nd++ ? ", " : "", fn)); else buf_str(&bi, cu_fmt("%s`%s`", ni++ ? ", " : "", fn)); }
            x_final(ni ? cu_fmt(L("Directly: %s. Through them, also %s. The index matches calls by name only, so run the tests after the change.", "Direto: %s. Por meio delas, tambem %s. O indice casa as chamadas so pelo nome; rode os testes depois da mudanca."), bd.p, bi.p)
                       : cu_fmt(L("Only %s, and nothing calls them in turn.", "So %s, e nada chama elas por sua vez."), bd.p));
            free(bd.p); free(bi.p);
        }
    } else if (sit == S7_DEF) {
        ask = cu_fmt(v == 0 ? L("where is `%s` defined?", "onde `%s` e definida?") : v == 1 ? L("which file has `%s`, and on which lines?", "em que arquivo esta `%s`, e em quais linhas?") : L("show me the definition of `%s`", "me mostra a definicao de `%s`"), tn);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn)));
        X7Hit h[8]; const int nh = x7_hits(r, h, 8); int hit = -1;
        for (int i = 0; i < nh; i++) if (!strstr(h[i].rest, "(decl)") && strstr(h[i].rest, cu_fmt(" %s ", tn)) && !strcmp(h[i].path, rel)) hit = i;
        if (hit < 0 || h[hit].a != dl) cu_die("def de %s nao bate (%s)", tn, g_x.cell);
        if (cu_pct(50)) {
            const char *rd = x7_read(s, rel, h[hit].a, h[hit].b - h[hit].a + 1, 0);
            if (!strstr(rd, tn)) cu_die("leitura da definicao");
            x_final(cu_fmt(L("`%s` is defined in %s, lines %d-%d. I read them: %s", "`%s` esta definida em %s, linhas %d-%d. Li essas linhas: %s"), tn, rel, h[hit].a, h[hit].b,
                           cu_fmt(L("it combines its two arguments%s.", "ela combina os dois argumentos%s."), n->f[ti].nc ? L(" and calls other functions of the project", " e chama outras funcoes do projeto") : "")));
        } else x_final(cu_fmt(L("`%s` is defined in %s, lines %d-%d.", "`%s` esta definida em %s, linhas %d-%d."), tn, rel, h[hit].a, h[hit].b));
        if (fu) {
            x_dep(); x7_user(XV(L("and who calls it?", "e quem chama ela?"), L("which functions use it?", "quais funcoes usam ela?")));
            const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
            X7Hit h2[16]; const int n2 = x7_hits(r2, h2, 16); int cs[16]; const int ne = x7n_callers(n, ti, cs);
            if (ne == 0) { if (!strstr(r2, "NOT FOUND")) cu_die("sem chamadores e sem NOT FOUND"); x_final(cu_fmt(L("Nothing in the indexed files calls `%s`: it is an entry point, or it is called in a way the index does not see (through a pointer or by name).", "Nada nos arquivos indexados chama `%s`: e um ponto de entrada, ou e chamada de um jeito que o indice nao ve (por ponteiro ou pelo nome)."), tn)); }
            else { if (n2 != ne) cu_die("callers do follow-up"); x_final(cu_fmt(L("`%s` is called by %s.", "`%s` e chamada por %s."), tn, x7_callers_text(h2, n2, tn))); }
        }
    } else if (sit == S7_CALLEES) {
        ask = cu_fmt(v == 0 ? L("what does `%s` call?", "o que `%s` chama?") : v == 1 ? L("list the functions that `%s` calls, in order", "lista as funcoes que `%s` chama, em ordem") : L("what does `%s` depend on?", "do que `%s` depende?"), tn);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"callees\", \"name\": %s}", cu_q(tn)));
        X7Hit h[8]; const int nh = x7_hits(r, h, 8);
        if (nh != n->f[ti].nc) cu_die("callees de %s: esperava %d, deu %d", tn, n->f[ti].nc, nh);
        Buf b = {0}; buf_str(&b, "");
        for (int i = 0; i < nh; i++) { const char *c = x7_after(h[i].rest, "call "); if (!c || strcmp(c, n->f[n->f[ti].call[i]].name)) cu_die("ordem das chamadas de %s", tn);
            const char *tg = strstr(h[i].rest, "-> "); char tp[100]; int ta = 0; if (!tg || sscanf(tg + 3, "%99[^:]:%d", tp, &ta) != 2) cu_die("sem destino no callee");
            buf_str(&b, cu_fmt("%s`%s` (%s:%d)", i ? (i + 1 == nh ? L(" and then ", " e depois ") : ", ") : "", c, tp, ta)); }
        x_final(cu_fmt(L("`%s` calls, in this order: %s.", "`%s` chama, nesta ordem: %s."), tn, b.p)); free(b.p);
        if (fu) {
            x_dep(); x7_user(XV(L("show me the first one", "me mostra a primeira"), L("let me see the first of them", "quero ver a primeira delas")));
            const X7F *c0 = &n->f[n->f[ti].call[0]]; const int c0i = n->f[ti].call[0];
            const char *tg = strstr(h[0].rest, "-> "); char tp[100]; int ta = 0, tb = 0; sscanf(tg + 3, "%99[^:]:%d-%d", tp, &ta, &tb);
            const char *rd = x7_read(s, tp, ta, tb ? tb - ta + 1 : 4, 0);
            if (!strstr(rd, c0->name)) cu_die("leitura do callee");
            x_final(cu_fmt(L("`%s` is at %s:%d-%d; it is the lines I just read.", "`%s` esta em %s:%d-%d; sao as linhas que acabei de ler."), c0->name, tp, ta, tb ? tb : ta + 3)); (void)c0i;
        }
    } else if (sit == S7_IMPACT || sit == S7_IMPACT0) {
        const int dp = sit == S7_IMPACT && cu_pct(35) ? 2 : 0;
        ask = cu_fmt(v == 0 ? L("what breaks if I change `%s`?", "o que quebra se eu mudar `%s`?") : v == 1 ? L("how far does a change to `%s` reach?", "ate onde uma mudanca em `%s` chega?") : L("I want to change the signature of `%s`: who is affected?", "quero mudar a assinatura de `%s`: quem e afetado?"), tn);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), dp ? cu_fmt("{\"action\": \"impact\", \"name\": %s, \"depth\": %d}", cu_q(tn), dp) : cu_fmt("{\"action\": \"impact\", \"name\": %s}", cu_q(tn)));
        if (sit == S7_IMPACT0) {
            if (!strstr(r, "NOT FOUND")) cu_die("impact sem chamadores devia dar NOT FOUND (%s)", g_x.cell);
            x_final(cu_fmt(L("Nothing in the indexed files calls `%s`, so a change to it cannot break another function there. The index sees calls by name only: a call through a pointer, a string or another program would not show.", "Nada nos arquivos indexados chama `%s`, entao mudar ela nao quebra outra funcao ali. O indice ve so chamadas pelo nome: uma chamada por ponteiro, texto ou outro programa nao aparece."), tn));
        } else {
            X7Hit h[24]; const int nh = x7_hits(r, h, 24); int ids[16], lv[16]; const int ne = x7n_closure(n, ti, dp ? dp : 3, ids, lv);
            if (nh != ne) cu_die("impact de %s: esperava %d, deu %d", tn, ne, nh);
            Buf bd = {0}, bi = {0}; buf_str(&bd, ""); buf_str(&bi, ""); int nd = 0, ni = 0;
            for (int i = 0; i < nh; i++) { const char *fn = x7_after(h[i].rest, "caller "); if (!fn) cu_die("sem caller"); if (h[i].depth == 0) buf_str(&bd, cu_fmt("%s`%s` (%s:%d)", nd++ ? ", " : "", fn, h[i].path, h[i].a)); else buf_str(&bi, cu_fmt("%s`%s`", ni++ ? ", " : "", fn)); }
            x_final(ni ? cu_fmt(L("Changing `%s` directly affects %s. Those are called in turn by %s, so the change can reach them too. Calls are matched by name, so check the tests.", "Mudar `%s` afeta direto %s. Essas sao chamadas por %s, entao a mudanca pode chegar nelas tambem. As chamadas sao casadas pelo nome; confira os testes."), tn, bd.p, bi.p)
                       : cu_fmt(L("Changing `%s` directly affects %s, and nothing calls those in turn.", "Mudar `%s` afeta direto %s, e nada chama essas por sua vez."), tn, bd.p));
            free(bd.p); free(bi.p);
        }
    } else if (sit == S7_AT) {
        const int ln = dl + 1 + (n->lang == N7_JV ? 1 : 0);
        ask = cu_fmt(v == 0 ? L("which function contains line %d of %s?", "qual funcao contem a linha %d de %s?") : v == 1 ? L("line %d of %s: what function is that in?", "linha %d de %s: em que funcao fica?") : L("what is at line %d of %s?", "o que tem na linha %d de %s?"), ln, rel);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"at\", \"file\": %s, \"line\": %d}", cu_q(rel), ln));
        X7Hit h[8]; const int nh = x7_hits(r, h, 8); int ok = 0, a = 0, b = 0;
        for (int i = 0; i < nh; i++) if (strstr(h[i].rest, cu_fmt(" %s ", tn)) && (strstr(h[i].rest, "func") || strstr(h[i].rest, "method")))      /* o indice diz "method" em Java */ { ok = 1; a = h[i].a; b = h[i].b; }
        if (!ok) cu_die("at nao deu %s (%s)", tn, g_x.cell);
        x_final(cu_fmt(L("Line %d of %s is inside `%s`, which spans lines %d-%d.", "A linha %d de %s fica dentro de `%s`, que vai das linhas %d a %d."), ln, rel, tn, a, b));
    } else if (sit == S7_DEPS) {
        int mi; { int best = -1; for (int m = 0; m < n->nm; m++) { const char *nm[16]; int uses = 0; for (int om = 0; om < n->nm; om++) if (om != m) uses += x7n_uses(n, m, om, nm) > 0; if (uses && (best < 0 || cu_n(2))) best = m; } if (best < 0) cu_die("sem modulo que importa"); mi = best; }
        const char *mf = x7n_file(n, mi);
        ask = cu_fmt(v == 0 ? L("what does %s import, and who imports it?", "o que %s importa, e quem importa ele?") : v == 1 ? L("map the imports of %s", "mapeia os imports de %s") : L("which files does %s depend on?", "de quais arquivos %s depende?"), mf);
        x7_user(ask);
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"deps\", \"file\": %s}", cu_q(mf)));
        X7Hit h[16]; const int nh = x7_hits(r, h, 16); Buf bo = {0}, bi = {0}; buf_str(&bo, ""); buf_str(&bi, ""); int no = 0, ni = 0;
        for (int i = 0; i < nh; i++) { const char *t = strstr(h[i].rest, "-> "), *u = strstr(h[i].rest, "<- ");
            if (t) buf_str(&bo, cu_fmt("%s%s", no++ ? ", " : "", cu_fmt("%.*s", (int)strcspn(t + 3, " "), t + 3)));
            else if (u) buf_str(&bi, cu_fmt("%s%s", ni++ ? ", " : "", cu_fmt("%.*s", (int)strcspn(u + 3, " "), u + 3))); }
        if (!no && !ni) cu_die("deps sem relacao (%s)", g_x.cell);
        x_final(cu_fmt(L("%s imports %s; it is imported by %s.", "%s importa %s; e importado por %s."), mf, no ? bo.p : L("no other file of the project", "nenhum outro arquivo do projeto"), ni ? bi.p : L("no file", "nenhum arquivo")));
        free(bo.p); free(bi.p);
    } else if (sit == S7_PAR) {
        int tj; do tj = cu_n(n->nf); while (tj == ti);
        const int kind = cu_n(3);
        if (kind == 0) {      /* a mesma funcao vista dos dois lados: quem chama e o que chama */
            int t2; do t2 = cu_n(n->nf); while (!n->f[t2].nc || !x7n_callers(n, t2, (int[16]){0}));
            ti = t2; tn = n->f[ti].name;
            x7_user(cu_fmt(L("give me the picture of `%s`: who calls it and what it calls", "me da o quadro de `%s`: quem chama e o que ela chama"), tn));
            const char *nm[2] = {"Symbols", "Symbols"}, *ar[2] = {cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)), cu_fmt("{\"action\": \"callees\", \"name\": %s}", cu_q(tn))}, *res[2];
            x_calls(L("The two queries are independent: one turn.", "As duas consultas sao independentes: uma fala so."), 2, nm, ar, res); x7_idx_norm(res[0]);
            X7Hit h1[16], h2[8]; const int n1 = x7_hits(res[0], h1, 16), n2 = x7_hits(res[1], h2, 8); int cs[16];
            if (n1 != x7n_callers(n, ti, cs) || n2 != n->f[ti].nc) cu_die("quadro de %s", tn);
            Buf b = {0}; buf_str(&b, ""); for (int i = 0; i < n2; i++) buf_str(&b, cu_fmt("%s`%s`", i ? ", " : "", x7_after(h2[i].rest, "call ")));
            x_final(cu_fmt(L("`%s` is called by %s, and it calls %s.", "`%s` e chamada por %s, e chama %s."), tn, x7_callers_text(h1, n1, tn), b.p)); free(b.p);
        } else if (kind == 1) {      /* duas definicoes */
            const char *tj2 = n->f[tj].name;
            x7_user(cu_fmt(L("where are `%s` and `%s` defined?", "onde `%s` e `%s` estao definidas?"), tn, tj2));
            const char *nm[2] = {"Symbols", "Symbols"}, *ar[2] = {cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn)), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tj2))}, *res[2];
            x_calls(L("Two independent definitions: asking both at once.", "Duas definicoes independentes: pergunto as duas juntas."), 2, nm, ar, res); x7_idx_norm(res[0]);
            int la = 0, lb = 0; X7Hit h[8]; int nh = x7_hits(res[0], h, 8); for (int i = 0; i < nh; i++) if (!strstr(h[i].rest, "(decl)") && strstr(h[i].rest, cu_fmt(" %s ", tn))) la = h[i].a;
            nh = x7_hits(res[1], h, 8); for (int i = 0; i < nh; i++) if (!strstr(h[i].rest, "(decl)") && strstr(h[i].rest, cu_fmt(" %s ", tj2))) lb = h[i].a;
            if (la != x7n_defline(n, ti) || lb != x7n_defline(n, tj)) cu_die("duas definicoes");
            x_final(cu_fmt(L("`%s` is in %s:%d and `%s` is in %s:%d.", "`%s` esta em %s:%d e `%s` esta em %s:%d."), tn, x7n_rel(n, ti), la, tj2, x7n_rel(n, tj), lb));
        } else {      /* o mapa de um arquivo e o que o importa, ou a mesma coisa em dois arquivos */
            const int mj = n->f[tj].mod; const int mi2 = n->f[ti].mod; if (mi2 == mj) cu_die("dois modulos iguais");
            const char *fa = x7n_file(n, mi2), *fb = x7n_file(n, mj);
            x7_user(cu_fmt(L("outline %s and %s for me", "me da o mapa de %s e de %s"), fa, fb));
            const char *nm[2] = {"Symbols", "Symbols"}, *ar[2] = {cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(fa)), cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(fb))}, *res[2];
            x_calls(L("Both outlines are independent.", "Os dois mapas sao independentes."), 2, nm, ar, res); x7_idx_norm(res[0]);
            Buf o = {0}; buf_str(&o, "");
            for (int k = 0; k < 2; k++) { const int m = k ? mj : mi2; Buf fl = {0}; buf_str(&fl, "");
                for (int q = 0; q < n->m[m].nf; q++) { if (!strstr(res[k], n->f[n->m[m].f[q]].name)) cu_die("mapa sem %s", n->f[n->m[m].f[q]].name); buf_str(&fl, cu_fmt("%s`%s`", q ? ", " : "", n->f[n->m[m].f[q]].name)); }
                buf_str(&o, cu_fmt("%s%s: %s.", k ? " " : "", k ? fb : fa, fl.p)); free(fl.p); }
            x_final(o.p); free(o.p);
        }
    } else if (sit == S7_OUTREAD) {
        const int mi = n->f[ti].mod; const char *mf = x7n_file(n, mi);
        x7_user(cu_fmt(v == 0 ? L("show me `%s` from %s", "me mostra a `%s` de %s") : v == 1 ? L("I need to read `%s` in %s, but not the whole file", "preciso ler a `%s` em %s, mas nao o arquivo todo") : L("open `%s` (it is in %s)", "abre a `%s` (ela esta em %s)"), tn, mf));
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(mf)));
        const char *ln0 = strstr(r, cu_fmt(" %s", tn)); int a = 0, b = 0;
        if (!ln0) cu_die("outline sem %s", tn);
        { const char *ls = ln0; while (ls > r && ls[-1] != '\n') ls--; if (sscanf(ls, " %d-%d", &a, &b) < 1) cu_die("faixa do outline de %s", tn); if (!b) b = a; }
        const char *rd = x7_read(s, mf, a, b - a + 1, 0);
        if (!strstr(rd, tn)) cu_die("leitura do trecho do outline");
        x_final(cu_fmt(L("`%s` is at %s, lines %d-%d, which is what I read: %s", "`%s` esta em %s, linhas %d-%d, que e o que li: %s"), tn, mf, a, b, cu_fmt(L("%d call%s inside it.", "%d chamada%s dentro dela."), n->f[ti].nc, n->f[ti].nc == 1 ? "" : L("s", "s"))));
    } else if (sit == S7_TYPO) {
        const char *bad = x7_typo(n, tn);
        x7_user(cu_fmt(L("who calls `%s`?", "quem chama `%s`?"), bad));
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(bad)));
        if (!strstr(r, cu_fmt("No exact match for '%s'; showing %s", bad, tn))) cu_die("o indice nao sugeriu %s para %s (%s)", tn, bad, g_x.cell);
        X7Hit h[16]; const int nh = x7_hits(r, h, 16);
        x_final(cu_fmt(L("There is no `%s` in the project. The closest name is `%s`, and the index shows its callers with low confidence: %s. If you meant another function, tell me its name.", "Nao existe `%s` no projeto. O nome mais parecido e `%s`, e o indice mostra os chamadores dela com confianca baixa: %s. Se voce quis outra funcao, me diz o nome."), bad, tn, x7_callers_text(h, nh, tn)));
    } else {      /* missing: o nome nao existe e nada se parece com ele */
        const char *bad = NULL;
        for (int t = 0; t < 40 && !bad; t++) { const char *c = x7n_style(lang, "recalculate", CU_PA(((const char *const[]){"payroll", "rebate", "forecast", "mileage"}))); if (x7n_idx(n, c) < 0) bad = c; }
        if (!bad) cu_die("sem nome inexistente");
        x7_user(cu_fmt(v == 0 ? L("who calls `%s`?", "quem chama `%s`?") : v == 1 ? L("where is `%s` used?", "onde `%s` e usada?") : L("what breaks if I remove `%s`?", "o que quebra se eu remover `%s`?"), bad));
        const char *act = v == 2 ? "impact" : "callers";
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"%s\", \"name\": %s}", act, cu_q(bad)));
        if (!strstr(r, "NOT FOUND")) cu_die("o nome inexistente %s achou algo (%s)", bad, g_x.cell);
        x_final(cu_fmt(L("The index has no `%s`: it indexed the project files and found no function with that name or a close spelling, so I cannot say who calls it. It may be a name from another branch, or one built at runtime. Tell me where you saw it and I will look there.", "O indice nao tem `%s`: ele indexou os arquivos do projeto e nao achou funcao com esse nome nem com grafia parecida, entao nao sei dizer quem chama. Pode ser nome de outro branch, ou montado em tempo de execucao. Me diz onde voce viu e eu olho la."), bad));
    }
    (void)dl; (void)rel;
    x_end(f);
}
/* FIM x-symbols */

/* ======================= x-find7 ======================= */
enum { F7_IDENT, F7_QUOTED, F7_QMISS, F7_TYPO, F7_CONCEPT, F7_NOMATCH, F7_N };
static const char *const g_f7_names[F7_N] = {"ident", "quoted", "quoted-missing", "typo", "concept", "no-match"};
static const int g_f7_w[F7_N] = {3, 2, 2, 2, 2, 1};
static int x7_pick(const int *w, int n) { int t = 0; for (int i = 0; i < n; i++) t += w[i]; int r = cu_n(t); for (int i = 0; i < n; i++) { if (r < w[i]) return i; r -= w[i]; } return 0; }
/* a palavra (5 letras ou mais) do texto que nao aparece em nenhum modulo do projeto; NULL se nao ha */
static const char *x7_rare_word(const X7N *n, const char *msg) {
    for (const char *w = msg; *w; ) {
        const size_t k = strcspn(w, " ");
        if (k >= 5) { const char *t = cu_fmt("%.*s", (int)k, w); int hit = 0; for (int m = 0; m < n->nm; m++) hit |= strcasestr(n->m[m].text, t) != NULL; if (!hit) return t; }
        w += k; while (*w == ' ') w++;
    }
    return NULL;
}
static void fam_x_find7(FILE *f, Scn *s) {
    x7_lang();
    int sit, v, lang, fu;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de find7");
        sit = x7_pick(g_f7_w, F7_N); v = cu_n(3); lang = cu_n(N7_NL); fu = sit == F7_IDENT && cu_pct(45) ? 1 : sit == F7_CONCEPT && cu_pct(35) ? 1 : 0;
        if (sit == F7_CONCEPT || sit == F7_NOMATCH) lang = CU_PA(((const int[]){N7_PY, N7_JS, N7_C}));
        if (x_held(220 + sit, v, lang, fu, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n; static XProj P; XProj *p = &P;
    const int xp = sit == F7_CONCEPT || sit == F7_NOMATCH;
    int ci = 0, mi = 0; XFi *fi = NULL;
    if (xp) {
        x6_base(p, s, lang == N7_PY ? X_PY : lang == N7_JS ? X_JS : X_C, 2 + cu_n(2));
        ci = cu_ix(X6_NC); fi = x6_place(p, g_x6_c[ci].fn, -1, &mi);
        x_begin("x-find7", s, p, cu_pct(15) ? 2 : 0, cu_pct(12) ? 1 : 0);
    } else {
        x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, sit == F7_QUOTED || sit == F7_QMISS ? 1 + cu_n(2) : 0, 0);
        g_x_prehook = x7_prehook;
        x_begin("x-find7", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
        g_x_prehook = NULL;
    }
    rr_reset();
    x_cell("find7/%s/v%d/%s/fu%d", g_f7_names[sit], v, xp ? g_x_stid[p->st] : g_n7_ext[lang], fu);
    if (sit == F7_IDENT || sit == F7_TYPO) {
        int ti; int cs[16]; do ti = cu_n(n->nf); while (sit == F7_TYPO ? 0 : fu && x7n_callers(n, ti, cs) < 1);
        const char *tn = n->f[ti].name, *rel = x7n_rel(n, ti); const int dl = x7n_defline(n, ti);
        const char *q = tn;
        if (sit == F7_TYPO) {
            q = x7_typo(n, tn);
            x7_user(cu_fmt(v == 0 ? L("where is `%s` defined?", "onde `%s` e definida?") : v == 1 ? L("find `%s` for me", "acha `%s` pra mim") : L("which file has the function `%s`?", "qual arquivo tem a funcao `%s`?"), q));
        } else x7_user(cu_fmt(v == 0 ? L("where is `%s` defined?", "onde `%s` e definida?") : v == 1 ? L("find `%s` for me", "acha `%s` pra mim") : L("which file has the function `%s`?", "qual arquivo tem a funcao `%s`?"), tn));
        const char *r = x7_find(cu_pct(25) ? L("A name: Find checks it in the files.", "Um nome: o Find confere nos arquivos.") : NULL, cu_fmt("{\"query\": %s}", cu_q(q)));
        if (sit == F7_IDENT) {
            if (strncmp(r, "Found by name or text (checked in the files):", 45) || !strstr(r, cu_fmt("[confidence high] %s:%d:", rel, dl))) cu_die("o Find nao achou %s pelo nome (%s)", tn, g_x.cell);
            if (cu_pct(40)) {
                const char *rd = x7_read(s, rel, dl, 3, 0);
                if (!strstr(rd, tn)) cu_die("leitura da definicao");
                x_final(cu_fmt(L("`%s` is defined in %s, line %d. The Find checked the file, and the lines I read show the definition.", "`%s` esta definida em %s, linha %d. O Find conferiu no arquivo, e as linhas que li mostram a definicao."), tn, rel, dl));
            } else x_final(cu_fmt(L("`%s` is defined in %s, line %d (found by name and checked in the file).", "`%s` esta definida em %s, linha %d (achada pelo nome e conferida no arquivo)."), tn, rel, dl));
            if (fu) {
                x_dep(); x7_user(XV(L("and who uses it?", "e quem usa ela?"), L("where is it called from?", "de onde ela e chamada?")));
                const char *rf = x7_find(L("Following the name with refs.", "Sigo o nome com refs."), cu_fmt("{\"query\": %s, \"mode\": \"refs\"}", cu_q(tn)));
                int nu = 0, nf = 0; const char *u = strstr(rf, "Used ");
                if (!strstr(rf, "Defined (") || !u || sscanf(u, "Used %d times in %d files", &nu, &nf) != 2) cu_die("o refs de %s nao deu os usos (%s)", tn, g_x.cell);
                Buf fl = {0}; buf_str(&fl, ""); int k = 0;
                for (const char *l = u; *l; ) { const size_t m = strcspn(l, "\n"); const char *pa = memchr(l, ' ', m); if (m > 4 && l[m - 1] == ':' && l[0] != ' ' && pa && l[m - 2] == ')') buf_str(&fl, cu_fmt("%s%.*s", k++ ? ", " : "", (int)(pa - l), l)); l += m; if (*l) l++; }
                if (!k) cu_die("o refs nao listou arquivos");
                x_final(cu_fmt(L("`%s` is used %d time%s, in %s.", "A `%s` e usada %d vez%s, em %s."), tn, nu, nu == 1 ? "" : L("s", "es"), fl.p)); free(fl.p);
            }
        } else {
            if (strncmp(r, cu_fmt("NOT FOUND: nothing is named \"%s\"", q), 20) || !strstr(r, cu_fmt("nothing is named \"%s\"", q)) || !strstr(r, "(confidence low)") || !strstr(r, cu_fmt(":%d: ", dl))) cu_die("o Find nao deu o nome parecido de %s (%s)", tn, g_x.cell);
            const char *d = x7_sym(L("Nothing is named that; the closest name looks like the right one. Checking it exactly.", "Nada tem esse nome; o mais parecido parece o certo. Confiro com exatidao."), cu_fmt("{\"action\": \"def\", \"name\": %s}", cu_q(tn)));
            X7Hit h[8]; const int nh = x7_hits(d, h, 8); int ok = 0; for (int i = 0; i < nh; i++) ok |= !strstr(h[i].rest, "(decl)") && !strcmp(h[i].path, rel) && h[i].a == dl;
            if (!ok) cu_die("o def de %s nao bate (%s)", tn, g_x.cell);
            x_final(cu_fmt(L("There is no `%s`. The Find listed `%s` as the closest spelling, and the symbol index confirms it: it is defined in %s, line %d. I am assuming you meant that one.", "Nao existe `%s`. O Find listou `%s` como a grafia mais proxima, e o indice de simbolos confirma: esta definida em %s, linha %d. Estou supondo que voce quis essa."), q, tn, rel, dl));
        }
    } else if (sit == F7_QUOTED) {
        const int qi = cu_n(n->nmsg); const char *tx = g_n7_msg[n->msgi[qi]], *id = g_n7_msgid[n->msgi[qi]];
        x7_user(cu_fmt(v == 0 ? L("which file has the message \"%s\"?", "qual arquivo tem a mensagem \"%s\"?") : v == 1 ? L("a user saw \"%s\". Where does it come from?", "um usuario viu \"%s\". De onde vem?") : L("find the text \"%s\" in the code", "acha o texto \"%s\" no codigo"), tx));
        const char *r = x7_find(NULL, cu_fmt("{\"query\": %s}", cu_q(cu_fmt("\"%s\"", tx))));
        const char *hit = cu_fmt("[confidence high] src/%s", ""); (void)hit;
        if (strncmp(r, "Literal text found (checked in the files, not guessed):", 55) || !strstr(r, "[confidence high] ") || !strstr(r, id)) cu_die("o Find nao achou o texto literal (%s)", g_x.cell);
        const char *ln = strstr(r, "[confidence high] ") + 18; char path[100]; int line = 0;
        if (sscanf(ln, "%99[^:]:%d:", path, &line) != 2 || strcmp(path, x7n_file(n, 0))) cu_die("literal em outro arquivo");
        x_final(cu_fmt(L("The text \"%s\" is in %s, line %d, in the constant `%s`. The Find checked it in the file, not just in its index.", "O texto \"%s\" esta em %s, linha %d, na constante `%s`. O Find conferiu no arquivo, nao so no indice."), tx, path, line, id));
    } else if (sit == F7_QMISS) {
        const char *tx; do tx = g_n7_msg[cu_n(CU_N(g_n7_msg))]; while (tx == g_n7_msg[n->msgi[0]] || (n->nmsg > 1 && tx == g_n7_msg[n->msgi[1]]));
        x7_user(cu_fmt(v == 0 ? L("which file has the message \"%s\"?", "qual arquivo tem a mensagem \"%s\"?") : v == 1 ? L("a user saw \"%s\". Where does it come from?", "um usuario viu \"%s\". De onde vem?") : L("find the text \"%s\" in the code", "acha o texto \"%s\" no codigo"), tx));
        const char *r = x7_find(NULL, cu_fmt("{\"query\": %s}", cu_q(cu_fmt("\"%s\"", tx))));
        if (strncmp(r, "NOT FOUND (literal):", 20)) cu_die("o texto inexistente foi achado (%s)", g_x.cell);
        const char *rw = x7_rare_word(n, tx);
        if (rw && cu_pct(55)) {
            const char *g = x7_call(L("The exact text is nowhere; one word of it, in any case.", "O texto exato nao esta em lugar nenhum; uma palavra dele, sem ligar para maiusculas."), "Grep", cu_fmt("{\"pattern\": %s, \"-i\": true, \"output_mode\": \"content\"}", cu_q(rw)), 0);
            if (!strstr(g, "No matches")) cu_die("o Grep achou %s (%s)", rw, g_x.cell);
            x_final(cu_fmt(L("No file of the project has \"%s\", and not even the word \"%s\" appears in it. The message probably comes from another service or a library, or it is built at run time. Tell me where you saw it and I will search there.", "Nenhum arquivo do projeto tem \"%s\", e nem a palavra \"%s\" aparece nele. A mensagem provavelmente vem de outro servico ou de uma biblioteca, ou e montada em tempo de execucao. Me diz onde voce viu e eu procuro la."), tx, rw));
        } else x_final(cu_fmt(L("The text \"%s\" is not in any indexed file of the project, so I cannot point to a place. It may come from a library or another service, or be built at run time. Where did you see it?", "O texto \"%s\" nao esta em nenhum arquivo indexado do projeto, entao nao tenho como apontar um lugar. Pode vir de uma biblioteca ou de outro servico, ou ser montado em tempo de execucao. Onde voce viu?"), tx));
    } else if (sit == F7_CONCEPT) {
        int dl = 0; x_user(x6_ask(ci, v));
        const char *src = x6_locate_by_concept(p, fi, mi, ci, &dl); const char *what = L(fi->f->en, fi->f->pt);
        x_final(cu_fmt(L("It is `%s`, at %s:%d: it computes %s.", "E a `%s`, em %s:%d: ela calcula %s."), fi->name, src, dl, what));
        if (fu) {
            x_dep(); x_user(XV(L("and who uses it?", "e quem usa ela?"), L("where else is it called from?", "de onde mais ela e chamada?")));
            const char *rf = cu_fmt("%s", x6_find(L("Following the name with refs.", "Sigo o nome com refs."), fi->name, NULL, 1));
            int nu = 0, nf = 0; const char *u = strstr(rf, "Used ");
            if (!u) { if (!strstr(rf, "No uses") && !strstr(rf, "Defined")) cu_die("refs sem resposta"); x_final(cu_fmt(L("Nothing else calls `%s` in the indexed files.", "Nada mais chama a `%s` nos arquivos indexados."), fi->name)); }
            else { if (sscanf(u, "Used %d times in %d files", &nu, &nf) != 2) cu_die("refs ilegivel"); x_final(cu_fmt(L("`%s` is used %d time%s, in %d file%s.", "A `%s` e usada %d vez%s, em %d arquivo%s."), fi->name, nu, nu == 1 ? "" : L("s", "es"), nf, nf == 1 ? "" : "s")); }
        }
    } else {      /* no-match: as palavras do usuario nao estao no codigo */
        x_user(x6_ask(ci, v));
        const char *askn = L(g_x6_c[ci].ask_en, g_x6_c[ci].ask_pt);
        const char *r = cu_fmt("%s", x6_find(NULL, x6_foreign(p, askn), NULL, 0));
        if (strncmp(r, "No match for", 12)) cu_die("o Find nao deu No match com a dica (%s)", g_x.cell);
        const char *gr = x_call(L("Nothing for those words; the code term instead.", "Nada para essas palavras; o termo do codigo no lugar."), "Grep", cu_fmt("{\"pattern\": %s, \"-i\": true, \"output_mode\": \"content\"}", cu_q(g_x6_c[ci].g)), 0);
        const char *pre = cu_fmt("%s:", x_src(p, mi)), *lit = x_deflit(p, fi->name); int dl = 0;
        for (const char *l = gr; *l && !dl; ) { const size_t k = strcspn(l, "\n"); if (!strncmp(l, pre, strlen(pre)) && memmem(l, k, lit, strlen(lit))) dl = atoi(l + strlen(pre)); l += k; if (*l) l++; }
        if (!dl) cu_die("o Grep nao mostrou a definicao");
        x_final(cu_fmt(L("It is `%s`, at %s:%d: it computes %s. The Find had nothing for your words (they are not in the code), and the search for `%s` found it.", "E a `%s`, em %s:%d: ela calcula %s. O Find nao teve nada para as suas palavras (elas nao estao no codigo), e a busca por `%s` achou."), fi->name, x_src(p, mi), dl, L(fi->f->en, fi->f->pt), g_x6_c[ci].g));
    }
    x_end(f);
}
/* FIM x-find7 */

/* ======================= x-place ======================= */
enum { P7_SYM, P7_FIND, P7_GLOB, P7_GREP, P7_ABS, P7_DYM, P7_FOLDER, P7_HOME, P7_N };
static const char *const g_p7_names[P7_N] = {"symbols", "find", "glob", "grep", "abs-read", "did-you-mean", "folder-exists", "home-refusal"};
static const int g_p7_w[P7_N] = {3, 3, 2, 2, 2, 2, 2, 2};
static Scn g_x7_other;      /* a "pasta" do projeto citado: a copia do cenario com o cwd dele */
static void x7_prehook2(const Scn *s) {
    x7n_write(&g_x7n, &g_x7_other);
    if (strcmp(s->cwd, s->home)) x_put(s, "notes.md", "# scratch notes\n\n- call the vendor about the invoice export\n- renew the staging certificate\n", 0);
}
/* a nota que o motor poe no fim do pedido quando o projeto citado nao existe daqui */
static void x7_place_note(int must) {
    XMsg *m = &g_x.m[g_x.n - 1]; static char pn[2400];
    if (!place_note(m->text, pn, sizeof pn)) { if (must) cu_die("o motor nao deu a nota de lugar (%s)", g_x.cell); return; }
    if (!must) cu_die("a nota de lugar apareceu onde nao devia (%s)", g_x.cell);
    if (!strstr(pn, g_x7_other.cwd) || strstr(pn, "], ") ) cu_die("a nota aponta para outro lugar (%s): %.200s", g_x.cell, pn);
    m->text = cu_fmt("%s\n\n%s", m->text, pn);
}
static void fam_x_place(FILE *f, Scn *s) {
    x7_lang();
    int sit, v, lang, inhome;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de place");
        sit = x7_pick(g_p7_w, P7_N); v = cu_n(3); lang = cu_n(N7_NL); inhome = sit == P7_HOME || cu_pct(40);
        if (x_held(230 + sit, v, lang, inhome, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3, 4, 3, 2, sit == P7_FIND && cu_pct(50) ? 1 : 0, 0);
    const char *d1 = CU_PA(((const char *const[]){"work", "code", "repos", "clients"})), *d2 = CU_PA(((const char *const[]){"greenhouse", "lighthouse", "quarry", "orchard", "harbor", "meadow"}));
    const char *name = s->name, *P = sit == P7_HOME ? cu_fmt("%s/%s", s->home, name) : cu_fmt("%s/%s/%s/%s", s->home, d1, d2, name);
    const char *rel_in_home = sit == P7_HOME ? name : cu_fmt("%s/%s/%s", d1, d2, name);
    g_x7_other = *s; g_x7_other.cwd = P;
    s->cwd = inhome ? s->home : cu_fmt("%s/dev/%s", s->home, cu_word());
    g_x_prehook = x7_prehook2;
    x_begin("x-place", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x_cell("place/%s/v%d/%s/%s", g_p7_names[sit], v, g_n7_ext[lang], inhome ? "home" : "other");
    int ti; int cs[16]; do ti = cu_n(n->nf); while ((sit == P7_SYM) && x7n_callers(n, ti, cs) < 1);
    const char *tn = n->f[ti].name, *rel = x7n_rel(n, ti); const int dl = x7n_defline(n, ti);
    /* o lugar como o usuario o cita: com a pasta (so quando nao existe a partir da pasta de trabalho) ou so o nome do projeto */
    const char *where = !inhome && cu_pct(60) ? cu_fmt(L("the project %s (it is in %s)", "o projeto %s (fica em %s)"), name, rel_in_home) : cu_fmt(L("the project %s", "o projeto %s"), name);
    const char *Pabs = P;
    if (sit == P7_SYM) {
        x7_user(cu_fmt(v == 0 ? L("in %s, who calls `%s`?", "em %s, quem chama `%s`?") : v == 1 ? L("%s: which functions call `%s`?", "%s: quais funcoes chamam `%s`?") : L("in %s, where is `%s` called from?", "em %s, de onde `%s` e chamada?"), where, tn));
        x7_place_note(1);
        const char *r = x7_sym(cu_pct(40) ? L("The note says where the project is: passing it as path.", "A nota diz onde o projeto esta: passo como path.") : NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s, \"path\": %s}", cu_q(tn), cu_q(Pabs)));
        X7Hit h[16]; const int nh = x7_hits(r, h, 16); const int ne = x7n_callers(n, ti, cs);
        if (nh != ne) cu_die("callers em place: esperava %d, deu %d (%s)", ne, nh, g_x.cell);
        x_final(cu_fmt(L("In %s, `%s` is called by %s.", "Em %s, `%s` e chamada por %s."), name, tn, x7_callers_text(h, nh, tn)));
    } else if (sit == P7_FIND) {
        const int msg = n->nmsg > 0;
        if (msg) {
            const char *tx = g_n7_msg[n->msgi[0]], *id = g_n7_msgid[n->msgi[0]];
            x7_user(cu_fmt(v == 0 ? L("in %s, which file has the message \"%s\"?", "em %s, qual arquivo tem a mensagem \"%s\"?") : v == 1 ? L("%s: where does the text \"%s\" come from?", "%s: de onde vem o texto \"%s\"?") : L("find \"%s\" in %s", "acha \"%s\" em %s"), v == 2 ? tx : where, v == 2 ? where : tx));
            x7_place_note(1);
            const char *r = x7_find(L("Searching in the project folder from the note.", "Procuro na pasta do projeto que a nota deu."), cu_fmt("{\"query\": %s, \"path\": %s}", cu_q(cu_fmt("\"%s\"", tx)), cu_q(Pabs)));
            if (strncmp(r, "Literal text found", 18) || !strstr(r, id)) cu_die("o Find com path nao achou o texto (%s): %.300s", g_x.cell, r);
            x_final(cu_fmt(L("In %s, the text \"%s\" is in %s, in the constant `%s`.", "Em %s, o texto \"%s\" esta em %s, na constante `%s`."), name, tx, x7n_file(n, 0), id));
        } else {
            x7_user(cu_fmt(v == 0 ? L("in %s, where is `%s` defined?", "em %s, onde `%s` e definida?") : v == 1 ? L("%s: find the function `%s`", "%s: acha a funcao `%s`") : L("which file of %s has `%s`?", "qual arquivo de %s tem `%s`?"), where, tn));
            x7_place_note(1);
            const char *r = x7_find(L("Searching in the project folder from the note.", "Procuro na pasta do projeto que a nota deu."), cu_fmt("{\"query\": %s, \"path\": %s}", cu_q(tn), cu_q(Pabs)));
            if (strncmp(r, "Found by name or text", 21) || !strstr(r, cu_fmt("%s:%d:", rel, dl))) cu_die("o Find com path nao achou %s (%s): %.300s", tn, g_x.cell, r);
            x_final(cu_fmt(L("In %s, `%s` is defined in %s, line %d.", "Em %s, `%s` esta definida em %s, linha %d."), name, tn, rel, dl));
        }
    } else if (sit == P7_GLOB) {
        x7_user(cu_fmt(v == 0 ? L("list the %s files of %s", "lista os arquivos %s de %s") : v == 1 ? L("which %s files does %s have?", "quais arquivos %s tem %s?") : L("show the %s sources in %s", "mostra os fontes %s em %s"), g_n7_lname[lang], where));
        x7_place_note(1);
        const char *r = x7_call(NULL, "Bash", cu_fmt("{\"command\": %s, \"description\": \"List the files by name\"}", cu_q(cu_fmt("find %s -type f -name '%s' -not -path '*/.g" "it/*' | sort", Pabs, cu_fmt("*.%s", g_n7_ext[lang])))), 0);
        for (int m = 0; m < n->nm; m++) if (!strstr(r, x7n_file(n, m))) cu_die("o Glob nao listou %s (%s)", x7n_file(n, m), g_x.cell);
        Buf b = {0}; buf_str(&b, ""); for (int m = 0; m < n->nm; m++) buf_str(&b, cu_fmt("%s%s", m ? ", " : "", x7n_file(n, m)));
        x_final(cu_fmt(L("The %s sources of %s are: %s.", "Os fontes %s de %s sao: %s."), g_n7_lname[lang], name, b.p)); free(b.p);
    } else if (sit == P7_GREP) {
        x7_user(cu_fmt(v == 0 ? L("in %s, every line that mentions `%s`", "em %s, todas as linhas que citam `%s`") : v == 1 ? L("grep `%s` in %s", "faz um grep de `%s` em %s") : L("show me the uses of the text `%s` in %s", "me mostra os usos do texto `%s` em %s"), v == 1 || v == 2 ? tn : where, v == 1 || v == 2 ? where : tn));
        x7_place_note(1);
        const char *r = x7_call(NULL, "Grep", cu_fmt("{\"pattern\": %s, \"path\": %s, \"output_mode\": \"content\", \"-n\": true}", cu_q(tn), cu_q(Pabs)), 0);
        int nl = 0; for (const char *l = r; *l; ) { const size_t k = strcspn(l, "\n"); if (k && memmem(l, k, tn, strlen(tn))) nl++; l += k; if (*l) l++; }
        if (nl < 1 || !strstr(r, rel)) cu_die("o Grep com path nao achou %s (%s)", tn, g_x.cell);
        x_final(cu_fmt(L("In %s, `%s` shows up on %d lines; the definition is in %s, line %d.", "Em %s, `%s` aparece em %d linhas; a definicao esta em %s, linha %d."), name, tn, nl, rel, dl));
    } else if (sit == P7_ABS) {
        const int mi = cu_n(n->nm); const char *mf = x7n_file(n, mi);
        x7_user(cu_fmt(v == 0 ? L("read %s of %s and tell me what it defines", "le o %s de %s e diz o que ele define") : v == 1 ? L("what is in %s, in %s?", "o que tem em %s, em %s?") : L("open %s of %s", "abre o %s de %s"), mf, where));
        x7_place_note(1);
        const char *r = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(cu_fmt("%s/%s", Pabs, mf))), 0);
        Buf b = {0}; buf_str(&b, "");
        for (int q = 0; q < n->m[mi].nf; q++) { if (!strstr(r, n->f[n->m[mi].f[q]].name)) cu_die("a leitura nao tem %s", n->f[n->m[mi].f[q]].name); buf_str(&b, cu_fmt("%s`%s`", q ? ", " : "", n->f[n->m[mi].f[q]].name)); }
        x_final(cu_fmt(L("%s of %s defines %s.", "%s de %s define %s."), mf, name, b.p)); free(b.p);
    } else if (sit == P7_DYM) {
        const int mi = cu_n(n->nm); const char *mf = x7n_file(n, mi);
        x7_user(cu_fmt(v == 0 ? L("look at %s/%s and tell me which functions it defines", "olha %s/%s e diz quais funcoes ele define") : v == 1 ? L("what does %s/%s define?", "o que %s/%s define?") : L("read %s/%s for me", "le %s/%s pra mim"), name, mf));
        x7_place_note(0);
        const char *r1 = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, cu_fmt("%s/%s", name, mf)))), XF_ERR);
        if (!strstr(r1, "Did you mean: ") || !strstr(r1, cu_fmt("%s/%s", Pabs, mf))) cu_die("o Read nao sugeriu o caminho (%s): %.300s", g_x.cell, r1);
        const char *r = x7_call(L("The error suggests where the file really is; reading it there.", "O erro sugere onde o arquivo esta de verdade; leio la."), "Read", cu_fmt("{\"file_path\": %s}", cu_q(cu_fmt("%s/%s", Pabs, mf))), 0);
        Buf b = {0}; buf_str(&b, "");
        for (int q = 0; q < n->m[mi].nf; q++) { if (!strstr(r, n->f[n->m[mi].f[q]].name)) cu_die("a leitura nao tem %s", n->f[n->m[mi].f[q]].name); buf_str(&b, cu_fmt("%s`%s`", q ? ", " : "", n->f[n->m[mi].f[q]].name)); }
        x_final(cu_fmt(L("The file is not at %s/%s from here: the project is in %s. There, %s defines %s.", "O arquivo nao esta em %s/%s a partir daqui: o projeto fica em %s. La, %s define %s."), name, mf, Pabs, mf, b.p)); free(b.p);
    } else if (sit == P7_FOLDER) {
        const int mi = cu_n(n->nm); const char *mf = x7n_file(n, mi); const char *base = strrchr(mf, '/') + 1;
        x7_user(cu_fmt(v == 0 ? L("what is in %s/lib/%s?", "o que tem em %s/lib/%s?") : v == 1 ? L("read %s/lib/%s and summarize it", "le %s/lib/%s e resume") : L("open %s/lib/%s", "abre %s/lib/%s"), name, base));
        x7_place_note(0);
        const char *r1 = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, cu_fmt("%s/lib/%s", name, base)))), XF_ERR);
        if (!strstr(r1, cu_fmt("The folder \"%s\" exists at %s, but lib/%s is not in it", name, Pabs, base))) cu_die("o Read nao disse que a pasta existe (%s): %.300s", g_x.cell, r1);
        const char *g = x7_call(L("The folder exists but has no lib/: looking for the file inside it.", "A pasta existe mas nao tem lib/: procuro o arquivo dentro dela."), "Bash", cu_fmt("{\"command\": %s, \"description\": \"List the files by name\"}", cu_q(cu_fmt("find %s -type f -name '%s' -not -path '*/.g" "it/*' | sort", Pabs, base))), 0);
        if (!strstr(g, mf)) cu_die("o Glob nao achou %s (%s): %.200s", mf, g_x.cell, g);
        const char *r = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(cu_fmt("%s/%s", Pabs, mf))), 0);
        Buf b = {0}; buf_str(&b, "");
        for (int q = 0; q < n->m[mi].nf; q++) { if (!strstr(r, n->f[n->m[mi].f[q]].name)) cu_die("a leitura nao tem %s", n->f[n->m[mi].f[q]].name); buf_str(&b, cu_fmt("%s`%s`", q ? ", " : "", n->f[n->m[mi].f[q]].name)); }
        x_final(cu_fmt(L("There is no lib/ folder in %s; the file is %s. It defines %s.", "Nao existe pasta lib/ em %s; o arquivo e %s. Ele define %s."), name, mf, b.p)); free(b.p);
    } else {      /* home-refusal: a pasta de trabalho e a home; o primeiro Find sem path e recusado (a fala errada fica so como contexto) */
        const int useglob = 0;      /* sem Glob na lista enxuta: so o Find e recusado na home */
        x7_user(useglob ? cu_fmt(v == 0 ? L("list the %s files of the %s project", "lista os arquivos %s do projeto %s") : v == 1 ? L("which %s files does the %s project have?", "quais arquivos %s o projeto %s tem?") : L("show the %s sources of the %s project", "mostra os fontes %s do projeto %s"), g_n7_lname[lang], name)
                        : cu_fmt(v == 0 ? L("in the %s project, where is `%s` defined?", "no projeto %s, onde `%s` e definida?") : v == 1 ? L("find the function `%s` in the %s project", "acha a funcao `%s` no projeto %s") : L("which file of the %s project has `%s`?", "qual arquivo do projeto %s tem `%s`?"), v == 1 ? tn : name, v == 1 ? name : tn));
        x7_place_note(0);
        const char *bad = x7_call(NULL, useglob ? "Glob" : "Find", useglob ? cu_fmt("{\"pattern\": %s}", cu_q(cu_fmt("**/*.%s", g_n7_ext[lang]))) : cu_fmt("{\"query\": %s}", cu_q(tn)), XF_NOTRAIN | XF_ERR);
        if (!strstr(bad, "the working folder is your home folder")) cu_die("a recusa da home nao veio (%s): %.200s", g_x.cell, bad);
        if (useglob) {
            const char *r = x7_call(L("The working folder is my home folder; the project folder goes in path.", "A pasta de trabalho e a home; a pasta do projeto vai em path."), "Glob", cu_fmt("{\"pattern\": %s, \"path\": %s}", cu_q(cu_fmt("**/*.%s", g_n7_ext[lang])), cu_q(Pabs)), 0);
            for (int m = 0; m < n->nm; m++) if (!strstr(r, x7n_file(n, m))) cu_die("o Glob nao listou %s", x7n_file(n, m));
            Buf b = {0}; buf_str(&b, ""); for (int m = 0; m < n->nm; m++) buf_str(&b, cu_fmt("%s%s", m ? ", " : "", x7n_file(n, m)));
            x_final(cu_fmt(L("The %s sources of %s are: %s.", "Os fontes %s de %s sao: %s."), g_n7_lname[lang], name, b.p)); free(b.p);
        } else {
            const char *r = x7_find(L("The working folder is my home folder; the project folder goes in path.", "A pasta de trabalho e a home; a pasta do projeto vai em path."), cu_fmt("{\"query\": %s, \"path\": %s}", cu_q(tn), cu_q(Pabs)));
            if (strncmp(r, "Found by name or text", 21) || !strstr(r, cu_fmt("%s:%d:", rel, dl))) cu_die("o Find com path nao achou %s (%s)", tn, g_x.cell);
            x_final(cu_fmt(L("In the %s project, `%s` is defined in %s, line %d.", "No projeto %s, `%s` esta definida em %s, linha %d."), name, tn, rel, dl));
        }
    }
    x_end(f);
}
/* FIM x-place */

/* ======================= x-cursor ======================= */
enum { C7_REST, C7_CHUNKS, C7_SAMEOFF, C7_SWITCH, C7_ANSWER, C7_STUCK, C7_MAP, C7_N };
static const char *const g_c7_names[C7_N] = {"rest", "chunks", "same-offset", "repeat-switch", "repeat-answer", "stuck", "map"};
static const int g_c7_w[C7_N] = {3, 2, 2, 2, 1, 1, 3};
static const char *x7_fnlist(const X7N *n) {      /* todas as funcoes do arquivo com a linha da definicao */
    Buf b = {0}; buf_str(&b, ""); const X7M *m = &n->m[0];
    for (int q = 0; q < m->nf; q++) buf_str(&b, cu_fmt("%s`%s` (%d)", q ? ", " : "", n->f[m->f[q]].name, x7n_defline(n, m->f[q])));
    return cu_keep(b.p);
}
static void fam_x_cursor(FILE *f, Scn *s) {
    x7_lang();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de cursor");
        sit = x7_pick(g_c7_w, C7_N); v = cu_n(3); lang = CU_PA(((const int[]){N7_PY, N7_JS, N7_TS, N7_C, N7_GO, N7_RS, N7_JV}));
        if (x_held(240 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 1, 5, 5, 5, 0, 1);
    g_x_prehook = x7_prehook;
    x_begin("x-cursor", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    const char *file = x7n_file(n, 0); const int total = x7n_nlines(n->m[0].text);
    if (total <= 200) cu_die("arquivo pequeno demais para o mapa (%d linhas)", total);
    x_cell("cursor/%s/v%d/%s", g_c7_names[sit], v, g_n7_ext[lang]);
    const int ti = cu_n(n->nf); const char *tn = n->f[ti].name; const int dl = x7n_defline(n, ti);
    if (sit == C7_REST || sit == C7_CHUNKS || sit == C7_SAMEOFF) {
        x7_user(cu_fmt(v == 0 ? L("list every function of %s with its line number", "lista todas as funcoes de %s com o numero da linha") : v == 1 ? L("which functions does %s define, and where?", "quais funcoes %s define, e onde?") : L("I need the line of each function in %s", "preciso da linha de cada funcao em %s"), file));
        const int first = sit == C7_SAMEOFF ? 40 : sit == C7_CHUNKS ? 70 : 60;
        const char *r1 = x7_call(L("A big file: the first part.", "Um arquivo grande: a primeira parte."), "Read", cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": %d}", cu_q(x_abs(s, file)), first), 0);
        if (!strstr(r1, cu_fmt("[PARTIAL view: returned lines 1-%d of %d total", first, total))) cu_die("o Read parcial nao disse o total (%s): %.200s", g_x.cell, strstr(r1, "[PARTIAL") ? strstr(r1, "[PARTIAL") : "(sem aviso)");
        const char *rest = NULL;
        if (sit == C7_CHUNKS) {
            Buf all = {0}; buf_str(&all, r1); int off = first + 1;
            while (off <= total) {
                const char *r = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": %d}", cu_q(x_abs(s, file)), off, first), 0);
                if (strstr(r, "[katana: lines")) cu_die("o cursor apareceu num Read com offset novo");
                buf_str(&all, r); off += first;
            }
            rest = all.p;
        } else {
            const char *r2 = sit == C7_REST ? x7_call(L("The rest of the file, without a range.", "O resto do arquivo, sem faixa."), "Read", cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, file))), 0)
                                            : x7_call(L("The whole file from the start; the part I have is the beginning.", "O arquivo todo desde o comeco; a parte que tenho e o inicio."), "Read", cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": 400}", cu_q(x_abs(s, file))), 0);
            if (!strstr(r2, cu_fmt("[katana: lines 1-%d of ", first)) || !strstr(r2, cu_fmt("here is the rest, from line %d]", first + 1))) cu_die("o cursor nao apareceu (%s): %.200s", g_x.cell, r2);
            rest = r2;
        }
        const X7M *m = &n->m[0];
        for (int q = 0; q < m->nf; q++) if (!strstr(rest, n->f[m->f[q]].name) && !strstr(r1, n->f[m->f[q]].name)) cu_die("faltou %s nas leituras", n->f[m->f[q]].name);
        x_final(cu_fmt(L("%s defines %d functions (name and line): %s.", "%s define %d funcoes (nome e linha): %s."), file, m->nf, x7_fnlist(n)));
    } else if (sit == C7_SWITCH || sit == C7_ANSWER || sit == C7_STUCK) {
        const int lim = sit == C7_ANSWER ? 30 : 40;
        const int first_fn = n->m[0].f[0]; const char *fn0 = n->f[first_fn].name;
        const int lastf = n->m[0].f[n->m[0].nf - 1];
        if (sit == C7_SWITCH) x7_user(cu_fmt(L("list every function of %s", "lista todas as funcoes de %s"), file));
        else if (sit == C7_ANSWER) x7_user(cu_fmt(L("what is the first function of %s, and what does it return?", "qual e a primeira funcao de %s, e o que ela devolve?"), file));
        else x7_user(cu_fmt(L("what does the last function of %s return?", "o que a ultima funcao de %s devolve?"), file));
        const char *args = cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": %d}", cu_q(x_abs(s, file)), lim);
        const char *r1 = x7_call(NULL, "Read", args, 0);
        const char *r2 = x7_call(NULL, "Read", args, XF_NOTRAIN);
        if (!strstr(r2, "you already made this exact call at step 1")) cu_die("sem a nota de repeticao (%s): %.200s", g_x.cell, r2);
        if (sit == C7_SWITCH) {
            const char *o = x7_sym(L("The note says not to repeat it: the outline of the file lists all the functions at once.", "A nota diz para nao repetir: o mapa do arquivo lista todas as funcoes de uma vez."), cu_fmt("{\"action\": \"outline\", \"file\": %s}", cu_q(file)));
            for (int q = 0; q < n->m[0].nf; q++) if (!strstr(o, n->f[n->m[0].f[q]].name)) cu_die("outline sem %s", n->f[n->m[0].f[q]].name);
            x_final(cu_fmt(L("%s defines %d functions (name and line): %s.", "%s define %d funcoes (nome e linha): %s."), file, n->m[0].nf, x7_fnlist(n)));
        } else if (sit == C7_ANSWER) {
            const char *ex = x7n_expr(n, first_fn);
            if (!strstr(r1, ex)) cu_die("a primeira leitura nao mostra o retorno de %s", fn0);
            x_final(cu_fmt(L("The first function is `%s` (line %d of %s). It returns `%s`, as the lines I already read show.", "A primeira funcao e `%s` (linha %d de %s). Ela devolve `%s`, como mostram as linhas que ja li."), fn0, x7n_defline(n, first_fn), file, ex));
        } else {
            for (int k = 0; k < 2; k++) { const char *r3 = x7_call(NULL, "Read", args, XF_NOTRAIN); if (k == 1 && !strstr(r3, "time you make this exact call")) cu_die("sem a nota da terceira repeticao: %.300s", r3); }
            if (strstr(r1, n->f[lastf].name)) cu_die("a ultima funcao ja estava na primeira parte");
            x_final(cu_fmt(L("I could not tell: I only read lines 1-%d of %s, which has %d lines, and the last function (`%s`, line %d) is further down. Ask me again and I will read that part with a new offset.", "Nao consegui dizer: so li as linhas 1-%d de %s, que tem %d linhas, e a ultima funcao (`%s`, linha %d) fica mais adiante. Pergunte de novo e eu leio essa parte com outro offset."), lim, file, total, n->f[lastf].name, x7n_defline(n, lastf)));
        }
    } else {      /* map: Read sem faixa de arquivo grande devolve o mapa e o comeco; depois so a faixa da funcao */
        x7_user(cu_fmt(v == 0 ? L("show me `%s` in %s", "me mostra a `%s` em %s") : v == 1 ? L("what does `%s` return? it is in %s", "o que `%s` devolve? ela esta em %s") : L("open `%s` of %s", "abre a `%s` de %s"), tn, file));
        const char *r1 = x7_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, file))), 0);
        if (!strstr(r1, "this is its map and its start") || !strstr(r1, tn)) cu_die("o Read sem faixa nao deu o mapa com %s (%s): %.200s", tn, g_x.cell, r1);
        const int off = dl > 8 ? dl - 8 : 1; const int len = 16;
        const char *r2 = x7_call(L("The map has it; reading only its part.", "O mapa tem ela; leio so o trecho dela."), "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": %d}", cu_q(x_abs(s, file)), off, len), 0);
        const char *ex = x7n_expr(n, ti);
        if (!strstr(r2, ex)) cu_die("o trecho de %s nao mostra o retorno (%s)", tn, g_x.cell);
        x_final(cu_fmt(L("`%s` starts at line %d of %s and returns `%s`. The file has %d lines, so I read only that part after the map.", "`%s` comeca na linha %d de %s e devolve `%s`. O arquivo tem %d linhas, entao li so esse trecho depois do mapa."), tn, dl, file, ex, total));
    }
    x_end(f);
}
/* FIM x-cursor */

/* ======================= x-katananotes ======================= */
enum { K7_REPEAT, K7_PLAN, K7_TODO, K7_N };
static const char *const g_k7_names[K7_N] = {"repeat", "plan-first", "todo-open"};
static const int g_k7_w[K7_N] = {4, 3, 3};
/* o comando de checagem de sintaxe da linguagem (so as que o gerador consegue rodar de verdade) */
static const char *x7_syntax_cmd(int lang, const X7N *n) {
    Buf b = {0}; buf_str(&b, "");
    if (lang == N7_PY) { for (int m = 0; m < n->nm; m++) buf_str(&b, cu_fmt("%s%s", m ? " " : "", x7n_file(n, m))); return cu_fmt("python3 -c \"import ast,sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]; print('syntax ok')\" %s", b.p); }
    if (lang == N7_JS) return "for f in src/*.js; do node --check \"$f\" && echo \"ok $f\"; done";
    return "gcc -fsyntax-only -I src src/*.c && echo 'syntax ok'";
}
static void fam_x_katananotes(FILE *f, Scn *s) {
    x7_lang();
    int sit, v, lang;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de katananotes");
        sit = x7_pick(g_k7_w, K7_N); v = cu_n(3); lang = cu_n(N7_NL);
        if (sit == K7_PLAN) lang = CU_PA(((const int[]){N7_PY, N7_JS, N7_C}));
        if (x_held(250 + sit, v, lang, 0, 0) == (g_cu_hold != 0)) break;
    }
    X7N *n = &g_x7n;
    x7n_gen(n, s, lang, 3 + cu_n(2), 4 + cu_n(2), 3 + cu_n(2), 2, 0, 0);
    g_x_prehook = x7_prehook;
    x_begin("x-katananotes", s, NULL, cu_pct(12) ? 2 : 0, cu_pct(10) ? 1 : 0);
    g_x_prehook = NULL; rr_reset();
    x_cell("katananotes/%s/v%d/%s", g_k7_names[sit], v, g_n7_ext[lang]);
    int cs[16];
    if (sit == K7_REPEAT) {
        int ti, c1 = -1, ids[16], lv[16];
        for (int t = 0; ; t++) { if (t > 300) cu_die("sem cadeia de chamadores"); ti = cu_n(n->nf); const int k = x7n_callers(n, ti, cs); if (!k) continue; for (int q = 0; q < k; q++) if (x7n_callers(n, cs[q], ids) > 0) { c1 = cs[q]; } if (c1 >= 0) break; }
        (void)lv;
        const char *tn = n->f[ti].name, *cn = n->f[c1].name; const int tool = v;      /* 0 Symbols, 1 Grep, 2 Find */
        x7_user(cu_fmt(L("who calls `%s`, and who calls those callers? I need the chain.", "quem chama `%s`, e quem chama esses chamadores? preciso da cadeia."), tn));
        const char *a1 = tool == 0 ? cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)) : tool == 1 ? cu_fmt("{\"pattern\": %s, \"output_mode\": \"content\", \"-n\": true}", cu_q(tn)) : cu_fmt("{\"query\": %s, \"mode\": \"refs\"}", cu_q(tn));
        const char *tl = tool == 0 ? "Symbols" : tool == 1 ? "Grep" : "Find";
        const char *r1 = x7_call(NULL, tl, a1, 0); x7_idx_norm(r1);
        if (tool != 1 && !strstr(r1, cn)) cu_die("a primeira consulta nao mostrou %s (%s)", cn, g_x.cell);
        const char *r2 = x7_call(NULL, tl, a1, XF_NOTRAIN);
        if (!strstr(r2, "you already made this exact call at step 1")) cu_die("sem a nota de repeticao: %.200s", r2);
        const char *r3 = x7_sym(L("The note says to look somewhere else: the callers of the caller.", "A nota diz para olhar em outro lugar: os chamadores do chamador."), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(cn)));
        X7Hit h[16]; const int nh = x7_hits(r3, h, 16); int ec[16]; const int ne = x7n_callers(n, c1, ec);
        if (nh != ne) cu_die("callers de %s: esperava %d, deu %d", cn, ne, nh);
        x_final(cu_fmt(L("`%s` is called by `%s`, and `%s` is called by %s. So the chain goes up from `%s` through `%s` to those callers.", "`%s` e chamada por `%s`, e `%s` e chamada por %s. A cadeia sobe de `%s` por `%s` ate esses chamadores."), tn, cn, cn, x7_callers_text(h, nh, cn), tn, cn));
    } else if (sit == K7_PLAN) {
        int hubc = x7n_callers(n, 0, cs); if (hubc != 3) cu_die("o hub tem %d chamadores", hubc);
        const char *tn = n->f[0].name;
        x7_user(cu_fmt(v == 0 ? L("remove every call to `%s`; the callers do not need it any more", "remove toda chamada a `%s`; os chamadores nao precisam mais dela") : v == 1 ? L("`%s` is retired: take its calls out of the functions that use it", "`%s` foi aposentada: tira as chamadas dela das funcoes que a usam") : L("drop the calls to `%s` from its callers", "tira as chamadas a `%s` dos chamadores dela"), tn));
        const char *r = x7_sym(x7_say(), cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
        X7Hit h[8]; const int nh = x7_hits(r, h, 8); if (nh != 3) cu_die("callers do hub");
        g_x_planok = 1;
        { int rd[4], nr = 0; for (int i = 0; i < 3; i++) { const int md = n->f[cs[i]].mod; int dup = 0; for (int j = 0; j < nr; j++) dup |= rd[j] == md; if (!dup) { rd[nr++] = md; x_read(s, x7n_file(n, md), 0); } } }
        for (int i = 0; i < 3; i++) {
            X7F *c = &n->f[cs[i]]; int ci = -1; for (int q = 0; q < c->nc; q++) if (c->call[q] == 0) ci = q;
            const char *old = x7n_expr(n, cs[i]), *txt = n->m[c->mod].text;
            X7F sv = *c; for (int q = ci; q + 1 < c->nc; q++) { c->call[q] = c->call[q + 1]; c->flip[q] = c->flip[q + 1]; } c->nc--;
            const char *nw = c->nc ? x7n_expr(n, cs[i]) : cu_fmt("%d", c->k); *c = sv;
            int cnt = 0; for (const char *q = txt; (q = strstr(q, old)); q++) cnt++;
            if (cnt != 1) cu_die("a expressao nao e unica no arquivo (%d)", cnt);
            const char *rr = x_edit(s, x7n_file(n, c->mod), old, nw, 0, 0);
            if (i == 2 && !strstr(rr, "[katana: plan first")) cu_die("o motor nao pediu o plano na terceira mudanca: %.300s", rr);
            if (i < 2 && strstr(rr, "[katana: plan first")) cu_die("o plano foi pedido cedo demais");
        }
        const char *it[3] = {L("Check that nothing calls the function any more", "Conferir que nada mais chama a funcao"), L("Run the syntax check on the changed files", "Rodar a checagem de sintaxe nos arquivos mudados"), L("Report what changed", "Relatar o que mudou")};
        x_plan(it, 3); x_done(1);
        const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn)));
        if (!strstr(r2, "NOT FOUND")) cu_die("o hub ainda tem chamadores: %.300s", r2);
        x_done(2);
        const char *sc = x_bash(NULL, x7_syntax_cmd(lang, n), L("Syntax check of the changed files", "Checagem de sintaxe dos arquivos mudados"), 0);
        if (strstr(sc, "rror")) cu_die("a checagem de sintaxe falhou: %.300s", sc);
        x_final(cu_fmt(L("Done: the calls to `%s` are gone from %s, %s and %s. The index now finds no caller, and the syntax check passes. The function itself is still defined and unused.", "Pronto: as chamadas a `%s` sairam de %s, %s e %s. O indice nao acha mais chamador, e a checagem de sintaxe passa. A funcao em si continua definida e sem uso."),
                       tn, n->f[cs[0]].name, n->f[cs[1]].name, n->f[cs[2]].name));
    } else {      /* todo-open: o modelo responde antes de fechar o plano; o motor lembra, o modelo fecha o plano e responde */
        int ti; do ti = cu_n(n->nf); while (!n->f[ti].nc || x7n_callers(n, ti, cs) < 1);
        const char *tn = n->f[ti].name;
        x7_user(cu_fmt(L("audit `%s`: who calls it, what it calls, and what a change to it reaches. Answer all three.", "audita a `%s`: quem chama, o que ela chama e ate onde uma mudanca nela chega. Responde as tres."), tn));
        const char *it[4] = {L("Find who calls the function", "Achar quem chama a funcao"), L("List what the function calls", "Listar o que a funcao chama"), L("Check what a change reaches", "Ver ate onde uma mudanca chega"), L("Write the three answers", "Escrever as tres respostas")};
        x_plan(it, 4);
        const char *r1 = x7_sym(NULL, cu_fmt("{\"action\": \"callers\", \"name\": %s}", cu_q(tn))); X7Hit h1[16]; const int n1 = x7_hits(r1, h1, 16);
        x_done(1);
        const char *r2 = x7_sym(NULL, cu_fmt("{\"action\": \"callees\", \"name\": %s}", cu_q(tn))); X7Hit h2[8]; const int n2 = x7_hits(r2, h2, 8);
        x_done(2);
        const char *r3 = x7_sym(NULL, cu_fmt("{\"action\": \"impact\", \"name\": %s}", cu_q(tn))); X7Hit h3[24]; const int n3 = x7_hits(r3, h3, 24);
        int ec[16]; if (n1 != x7n_callers(n, ti, ec) || n2 != n->f[ti].nc) cu_die("auditoria de %s", tn);
        Buf b2 = {0}; buf_str(&b2, ""); for (int i = 0; i < n2; i++) buf_str(&b2, cu_fmt("%s`%s`", i ? ", " : "", x7_after(h2[i].rest, "call ")));
        const char *ans = cu_fmt(L("`%s` is called by %s. It calls %s. A change to it reaches %d function%s up the call graph.", "`%s` e chamada por %s. Ela chama %s. Uma mudanca nela chega a %d funcao%s no grafo de chamadas."), tn, x7_callers_text(h1, n1, tn), b2.p, n3, n3 == 1 ? "" : L("s", "es"));
        x_push('a', ans, 0);      /* resposta antes de fechar o plano: so contexto */
        static char nm[3600]; if (!loop_todo_should_nudge()) cu_die("o motor nao lembraria do plano");
        loop_todo_nudge(nm, sizeof nm); x_push('e', nm, 1);
        x7_call(L("The list still has open steps; marking them done.", "A lista ainda tem passos abertos; marco como feitos."), "TodoWrite", "{\"done\": 4}", 0);
        x_final(ans); free(b2.p);
    }
    x_end(f);
}
/* FIM x-katananotes */
