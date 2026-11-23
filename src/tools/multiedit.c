/* ---------- ferramentas Rename, Replace e Patch (ver docs/tools.md) ----------
 * Mudancas que tocam varios lugares: o motor acha, troca e mostra so o que mudou; o modelo nao escreve cada trecho.
 *   Rename  um simbolo (identificador inteiro) em todo o projeto; os arquivos candidatos vem do indice do Find.
 *   Replace uma expressao regular (ou texto) por outra, com grupos \1..\9, nos arquivos que o Grep listaria.
 *   Patch   um diff unificado (varios arquivos), com tolerancia de deslocamento e de espacos no fim, tudo ou nada.
 * Rename e Replace mostram a previa por padrao (dry_run=true): a regra "ler antes de alterar" do Edit e trocada por ela.
 * Patch aplica por padrao: o contexto do diff prova que o modelo conhece o texto, e nada e gravado se um bloco nao casar.
 * Tudo passa pelo mesmo caminho do Edit: regras never/ask do KATANA.md em cada arquivo (brief_guard_file), agent_act,
 * undo_note (/undo desfaz o pedido inteiro), nota de verificacao (g_me_changed -> brief_track) e caderno de fatos. */
static const char CT_RENAME_DESC[] =
    "Renames a symbol (function, variable, type, constant) across the whole project: every occurrence of the identifier as a whole word "
    "is replaced, in all code, script and config files. Use it when one name must change in several files; for a small change in one "
    "file Edit is the right tool. By default it only shows what it would change (dry_run=true): check the list, then call again with "
    "dry_run=false. It refuses if `to` is already defined somewhere else. It is a text rename, not a language-aware one: members, "
    "fields and local variables with the same name are renamed too, so read the preview. Comments, string literals and prose files are left "
    "alone unless kinds=\"all\". Undo with /undo.";
static const char CT_RENAME_PROPS[] =
    "\"symbol\": {\"type\": \"string\", \"description\": \"The current name (an identifier)\"}, "
    "\"to\": {\"type\": \"string\", \"description\": \"The new name (an identifier)\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"Limit the rename to this subfolder or file (default: the working folder)\"}, "
    "\"dry_run\": {\"type\": \"boolean\", \"description\": \"true (default): only show what would change; false: apply it\"}, "
    "\"kinds\": {\"type\": \"string\", \"enum\": [\"code\", \"all\"], \"description\": \"code (default): code, script and config files, comments and string literals skipped; all: also comments, string literals, docs and text\"}, "
    "\"force\": {\"type\": \"boolean\", \"description\": \"Rename even though `to` is already defined elsewhere (only when the two are in unrelated scopes)\"}";
static const char CT_RENAME_REQ[] = "\"symbol\", \"to\"";

static const char CT_REPLACE_DESC[] =
    "Replaces text in many files at once: every match of `pattern` (a regular expression like Grep's, or plain text with literal=true) "
    "becomes `replacement`, in the files Grep would search (path, glob and type select them). In `replacement` \\1 to \\9 are the "
    "groups of the pattern, \\\\ is a backslash, \\n a line break, \\t a tab (with literal=true the replacement is used as written). "
    "Use it for the same change in many places; for one change in one file Edit is the right tool. By default it only shows what it would "
    "change (dry_run=true): check the list, then call again with dry_run=false. Nothing is changed if more files would change than "
    "limit_files (default 100). Undo with /undo.";
static const char CT_REPLACE_PROPS[] =
    "\"pattern\": {\"type\": \"string\", \"description\": \"Regular expression to find (or plain text with literal=true)\"}, "
    "\"replacement\": {\"type\": \"string\", \"description\": \"The new text; \\\\1 to \\\\9 insert the groups of the pattern\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"Folder or file to change (default: the working folder)\"}, "
    "\"glob\": {\"type\": \"string\", \"description\": \"Only files whose name matches, e.g. *.py\"}, "
    "\"type\": {\"type\": \"string\", \"description\": \"Only files of this type, e.g. c, py, js, md\"}, "
    "\"literal\": {\"type\": \"boolean\", \"description\": \"Treat pattern as plain text and use replacement as written\"}, "
    "\"-i\": {\"type\": \"boolean\", \"description\": \"Ignore case\"}, "
    "\"multiline\": {\"type\": \"boolean\", \"description\": \"Let . match line breaks\"}, "
    "\"dry_run\": {\"type\": \"boolean\", \"description\": \"true (default): only show what would change; false: apply it\"}, "
    "\"limit_files\": {\"type\": \"integer\", \"description\": \"Refuse if more files than this would change (default 100)\"}";
static const char CT_REPLACE_REQ[] = "\"pattern\", \"replacement\"";

static const char CT_PATCH_DESC[] =
    "Applies a unified diff (the output of `diff -u` or `git diff`) to one or more files, creating and deleting files when the diff says so "
    "(--- /dev/null, +++ /dev/null). Use it when you have several changes, or changes in several files, written as a diff; for one small "
    "change in one file Edit is the right tool. Line numbers may be off by up to 30 lines and spaces at the end of lines are ignored, but "
    "the context and removed lines must match the file. All or nothing: if any block does not match, nothing is written and the reply "
    "says which block, why, and shows the closest text of the file so you can correct the diff. Result per file: applied (N blocks) or the "
    "reason. Undo with /undo.";
static const char CT_PATCH_PROPS[] =
    "\"diff\": {\"type\": \"string\", \"description\": \"The unified diff: ---/+++ lines, then @@ -a,b +c,d @@ blocks with lines starting with space, - or +\"}, "
    "\"dry_run\": {\"type\": \"boolean\", \"description\": \"true: only check that every block applies; false (default): apply it\"}";
static const char CT_PATCH_REQ[] = "\"diff\"";

#ifndef _WIN32
static int brief_guard_file(const char *file, char *out, size_t cap);      /* agent/brief.c: never e ask do KATANA.md */
static int g_me_changed;        /* gravou codigo do projeto neste passo: brief_track marca "mudou, rode o test:" */

/* uma troca no texto original: [s, s+l) vira r */
typedef struct { size_t s, l; char *r; size_t rl; } MeEd;
typedef struct {
    char *abs;                  /* caminho final (resolvido), onde gravar */
    char *orig; size_t on;      /* conteudo de antes (NULL: o arquivo nao existia) */
    char *data; size_t n;       /* conteudo de depois */
    int del;                    /* apagar o arquivo */
    int count;                  /* trocas (Rename/Replace) ou blocos (Patch) */
    int shifted;                /* Patch: blocos que casaram em outra linha */
    MeEd *ed; int ned, caped;
    char note[300];             /* Patch: motivo da falha deste arquivo */
} MeFile;
typedef struct { MeFile *v; int n, cap; } MeSet;

static MeFile *me_add(MeSet *S) {
    if (S->n == S->cap) { S->cap = S->cap ? S->cap * 2 : 16; S->v = (MeFile *)realloc(S->v, sizeof(MeFile) * (size_t)S->cap); }
    MeFile *f = &S->v[S->n++];
    memset(f, 0, sizeof *f);
    return f;
}
static void me_free(MeSet *S) {
    for (int i = 0; i < S->n; i++) {
        MeFile *f = &S->v[i];
        for (int k = 0; k < f->ned; k++) free(f->ed[k].r);
        free(f->abs); free(f->orig); free(f->data); free(f->ed);
    }
    free(S->v);
    memset(S, 0, sizeof *S);
}
static void me_push(MeFile *f, size_t s, size_t l, const char *r, size_t rl) {
    if (f->ned == f->caped) { f->caped = f->caped ? f->caped * 2 : 16; f->ed = (MeEd *)realloc(f->ed, sizeof(MeEd) * (size_t)f->caped); }
    MeEd *e = &f->ed[f->ned++];
    e->s = s; e->l = l; e->rl = rl;
    e->r = (char *)malloc(rl + 1);
    memcpy(e->r, r, rl); e->r[rl] = 0;
}
/* texto novo = original com as trocas (em ordem e sem sobrepor) */
static void me_build(MeFile *f) {
    size_t cap = f->on + 1;
    for (int i = 0; i < f->ned; i++) cap += f->ed[i].rl;
    f->data = (char *)malloc(cap + 1);
    size_t o = 0, p = 0;
    for (int i = 0; i < f->ned; i++) {
        memcpy(f->data + o, f->orig + p, f->ed[i].s - p); o += f->ed[i].s - p;
        memcpy(f->data + o, f->ed[i].r, f->ed[i].rl); o += f->ed[i].rl;
        p = f->ed[i].s + f->ed[i].l;
    }
    memcpy(f->data + o, f->orig + p, f->on - p); o += f->on - p;
    f->data[o] = 0; f->n = o;
}

/* texto de um arquivo para trocar: ate 4 MB, UTF-8 valido, sem NUL; NULL se nao serve */
static char *me_read(const char *abs, size_t *n) {
    struct stat st; char err[400];
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (4 << 20)) return NULL;
    char *b = ct_f_slurp(abs, n, err, sizeof err);
    if (!b) return NULL;
    if (memchr(b, 0, *n) || !ct_utf8_ok((const unsigned char *)b, *n)) { free(b); return NULL; }
    return b;
}
/* arquivo de configuracao do proprio agente: nao conta como codigo do projeto (mesma regra de brief_track) */
static int me_conf(const char *p) {
    const char *bn = strrchr(p, '/'); bn = bn ? bn + 1 : p;
    return strstr(p, "/.katana/") || !strncmp(p, ".katana/", 8) || !strcmp(bn, "SKILL.md") || !strcmp(bn, "KATANA.md");
}

/* ---- previa: "arquivo:linha antes -> depois" ---- */
/* trecho da linha em volta do casamento [ms, ms+ml), sem o recuo, com reticencias nas pontas cortadas */
static void me_window(CtSb *sb, const char *ln, size_t ll, size_t ms, size_t ml) {
    size_t a = ms > 24 ? ms - 24 : 0, e = ms + ml + 24;
    if (ml > 70) e = ms + 70;
    if (e > ll) e = ll;
    while (a > 0 && a < ll && ((unsigned char)ln[a] & 0xC0) == 0x80) a--;
    while (e < ll && ((unsigned char)ln[e] & 0xC0) == 0x80) e++;
    if (a == 0) while (a < ms && (ln[a] == ' ' || ln[a] == '\t')) a++;
    if (a > 0) ct_sb_puts(sb, "...");
    ct_sb_add(sb, ln + a, e - a);
    if (e < ll) ct_sb_puts(sb, "...");
}
#define ME_SHOW 40
/* lista curta das linhas que mudam, nos arquivos de S (as trocas ja feitas por me_build) */
static void me_preview(const MeSet *S, const char *cwd, CtSb *sb) {
    int shown = 0, total = 0;
    for (int fi = 0; fi < S->n; fi++) {
        const MeFile *f = &S->v[fi];
        const char *disp = ct_display(f->abs, cwd);
        size_t pos = 0, lastline = 0; long line = 1; long long delta = 0;
        for (int i = 0; i < f->ned; i++) {
            const MeEd *e = &f->ed[i];
            for (; pos < e->s; pos++) if (f->orig[pos] == '\n') line++;
            const size_t ns = (size_t)((long long)e->s + delta);
            delta += (long long)e->rl - (long long)e->l;
            if ((size_t)line == lastline) continue;                      /* varias trocas na mesma linha: mostra uma vez */
            lastline = (size_t)line; total++;
            if (shown >= ME_SHOW) continue;
            size_t bs = e->s, be = e->s;
            while (bs > 0 && f->orig[bs - 1] != '\n') bs--;
            while (be < f->on && f->orig[be] != '\n') be++;
            size_t as = ns, ae = ns;
            while (as > 0 && f->data[as - 1] != '\n') as--;
            while (ae < f->n && f->data[ae] != '\n') ae++;
            if (be > bs && f->orig[be - 1] == '\r') be--;
            if (ae > as && f->data[ae - 1] == '\r') ae--;
            char hd[1200];
            snprintf(hd, sizeof hd, "%s:%ld  ", disp, line);
            ct_sb_puts(sb, hd);
            size_t ml = e->l; if (e->s + ml > be) ml = be - e->s;
            size_t rl = e->rl; { const char *nl = (const char *)memchr(e->r, '\n', rl); if (nl) rl = (size_t)(nl - e->r); }
            if (ns + rl > ae) rl = ae - ns;
            me_window(sb, f->orig + bs, be - bs, e->s - bs, ml);
            ct_sb_puts(sb, "  ->  ");
            me_window(sb, f->data + as, ae - as, ns - as, rl);
            ct_sb_puts(sb, "\n");
            shown++;
        }
    }
    if (total > shown) { char m[80]; snprintf(m, sizeof m, "(+%d more lines)\n", total - shown); ct_sb_puts(sb, m); }
}
/* "12 occurrences in 3 files" e a lista por arquivo */
static void me_summary(const MeSet *S, const char *cwd, const char *unit, CtSb *sb) {
    int total = 0;
    for (int i = 0; i < S->n; i++) total += S->v[i].count;
    char m[200];
    snprintf(m, sizeof m, "%d %s%s in %d file%s", total, unit, total == 1 ? "" : "s", S->n, S->n == 1 ? "" : "s");
    ct_sb_puts(sb, m);
}
static void me_perfile(const MeSet *S, const char *cwd, CtSb *sb) {
    for (int i = 0; i < S->n && i < 12; i++) {
        char m[4600];
        snprintf(m, sizeof m, "%s%s (%d)", i ? ", " : "", ct_display(S->v[i].abs, cwd), S->v[i].count);
        ct_sb_puts(sb, m);
    }
    if (S->n > 12) { char m[40]; snprintf(m, sizeof m, ", +%d more files", S->n - 12); ct_sb_puts(sb, m); }
    ct_sb_puts(sb, "\n");
}

/* grava o conjunto: regras do projeto em cada arquivo, permissao uma vez, /undo, tudo ou desfaz. 1 se gravou; senao a mensagem em out */
static int me_commit(MeSet *S, const char *op, char *out, size_t cap) {
    static char why[1500], det[9000], what[200];
    for (int i = 0; i < S->n; i++)
        if (brief_guard_file(S->v[i].abs, why, sizeof why)) { snprintf(out, cap, "%s Nothing was changed.", why); return 0; }
    size_t o = 0; det[0] = 0;
    for (int i = 0; i < S->n && o + 400 < sizeof det; i++) o += (size_t)snprintf(det + o, sizeof det - o, "%s%s", i ? "\n" : "", S->v[i].abs);
    snprintf(what, sizeof what, "%s: alterar %d arquivo%s", op, S->n, S->n == 1 ? "" : "s");
    if (!agent_act(what, det)) { snprintf(out, cap, "error: the user denied this action"); return 0; }
    int bad = -1;
    char err[1500] = "";
    for (int i = 0; i < S->n; i++) {
        MeFile *f = &S->v[i];
        if (f->del) { undo_note(f->abs); index_touch(f->abs); if (remove(f->abs) != 0) { snprintf(err, sizeof err, "error: cannot delete %s (%s)", f->abs, strerror(errno)); bad = i; break; } }
        else if (!ct_f_save(f->abs, f->data, f->n, err, sizeof err)) { bad = i; break; }
    }
    if (bad >= 0) {                                         /* devolve o que ja foi gravado */
        for (int i = 0; i <= bad; i++) {
            MeFile *f = &S->v[i];
            index_touch(f->abs);
            if (!f->orig) { remove(f->abs); continue; }
            FILE *w = fopen(f->abs, "wb");
            if (w) { fwrite(f->orig, 1, f->on, w); fclose(w); }
        }
        snprintf(out, cap, "%s. Nothing was changed (the files written before it were restored).", err);
        return 0;
    }
    for (int i = 0; i < S->n; i++) {
        MeFile *f = &S->v[i];
        if (!f->del && ct_seen(f->abs) >= 2) ct_seen_mark(f->abs, 0);       /* o modelo ja tinha lido este arquivo inteiro: continua valendo */
        fc_file(f->abs, 1);
        if (!me_conf(f->abs)) g_me_changed = 1;
    }
    return 1;
}

/* ---- Rename ---- */
/* kind de comentario por tipo de arquivo (codigos de fd_kind): 1 // e blocos, 2 #, 3 <!-- -->, 4 --, 5 # com textos de python, 0 nenhum */
static int me_ckind(const char *rel) {
    const int k = fd_kind(rel);
    if (k == FK_C || k == FK_JS || k == FK_JV || k == FK_GO || k == FK_RS || k == FK_QML) return 1;
    if (k == FK_PY) return 5;
    if (k == FK_SH || k == FK_PS) return 2;
    if (k == FK_OTHER) {
        const char *d = strrchr(rel, '.');
        if (d && !strcasecmp(d, ".xml")) return 3;
        if (d && !strcasecmp(d, ".lua")) return 4;
        if (d && !strcasecmp(d, ".txt")) return 0;
        return 2;
    }
    return 0;
}
static int me_isid(unsigned char c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }
/* percorre o texto; cada ocorrencia de w como palavra inteira (fora de comentarios quando lex) vira uma troca por r em f (se f).
   Devolve quantas. lex: ck diz como sao os comentarios; textos entre aspas tambem ficam de fora (so kinds=all troca dentro deles). */
static int me_words(const char *t, size_t n, int ck, int lex, const char *w, size_t wl, MeFile *f, const char *r, size_t rl) {
    int cnt = 0, com = 0; char q = 0;      /* com: 1 ate o fim da linha, 2 bloco */
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)t[i];
        if (lex) {
            if (c == '\n') { if (com == 1) com = 0; q = 0; continue; }
            if (com == 1) continue;
            if (com == 2) {
                if (ck == 1 && c == '*' && i + 1 < n && t[i + 1] == '/') { com = 0; i++; }
                else if (ck == 3 && c == '-' && i + 2 < n && t[i + 1] == '-' && t[i + 2] == '>') { com = 0; i += 2; }
                continue;
            }
            if (q) { if (c == '\\' && i + 1 < n && t[i + 1] != '\n') i++; else if (c == (unsigned char)q) q = 0; continue; }
            if ((ck == 1 || ck == 5) && (c == '"' || c == '\'')) { q = (char)c; continue; }
            if (ck == 1 && c == '/' && i + 1 < n && t[i + 1] == '/') { com = 1; continue; }
            if (ck == 1 && c == '/' && i + 1 < n && t[i + 1] == '*') { com = 2; i++; continue; }
            if ((ck == 2 || ck == 5) && c == '#' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t' || t[i - 1] == '\n')) { com = 1; continue; }
            if (ck == 4 && c == '-' && i + 1 < n && t[i + 1] == '-') { com = 1; continue; }
            if (ck == 3 && c == '<' && i + 3 < n && !memcmp(t + i, "<!--", 4)) { com = 2; i += 3; continue; }
        }
        if (!me_isid(c) || (i > 0 && me_isid((unsigned char)t[i - 1]))) continue;
        size_t j = i;
        while (j < n && me_isid((unsigned char)t[j])) j++;
        if (j - i == wl && memcmp(t + i, w, wl) == 0) { cnt++; if (f) me_push(f, i, wl, r, rl); }
        i = j - 1;
    }
    return cnt;
}
static int me_valid_id(const char *s) {
    if (!s || !*s || !(isalpha((unsigned char)*s) || *s == '_' || *s == '$')) return 0;
    for (const char *p = s; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$')) return 0;
    return strlen(s) < 200;
}

/* previa ou aplicacao: o resultado final de Rename e Replace */
static void me_finish(MeSet *S, int dry, const char *op, const char *unit, const char *what, const char *cwd, CtSb *extra, char *out, size_t cap) {
    CtSb sb = {0};
    for (int i = 0; i < S->n; i++) me_build(&S->v[i]);
    char hd[600];
    if (dry) {
        snprintf(hd, sizeof hd, "%s (dry run, nothing changed): %s: ", op, what);
        ct_sb_puts(&sb, hd); me_summary(S, cwd, unit, &sb); ct_sb_puts(&sb, "\n");
        me_perfile(S, cwd, &sb);
        me_preview(S, cwd, &sb);
        if (extra && extra->p) ct_sb_puts(&sb, extra->p);
        ct_sb_puts(&sb, "To apply it, call again with dry_run=false.");
    } else {
        if (!me_commit(S, op, out, cap)) { free(sb.p); return; }
        snprintf(hd, sizeof hd, "%s done: %s: ", op, what);
        ct_sb_puts(&sb, hd); me_summary(S, cwd, unit, &sb); ct_sb_puts(&sb, ". /undo reverts it.\n");
        me_perfile(S, cwd, &sb);
        me_preview(S, cwd, &sb);
        if (extra && extra->p) ct_sb_puts(&sb, extra->p);
    }
    snprintf(out, cap, "%s", sb.p ? sb.p : "");
    free(sb.p);
}
static int me_dry(const char *js, const char *end) { return ct_has(js, end, "dry_run") ? ct_bool(js, end, "dry_run") : 1; }

static void ct_rename(const char *js, const char *end, char *out, size_t cap) {
    if (cap < 600) { if (cap) out[0] = 0; return; }
    char *sym = ct_str(js, end, "symbol"), *to = ct_str(js, end, "to"), *path = ct_str(js, end, "path"), *kinds = ct_str(js, end, "kinds");
    MeSet S; memset(&S, 0, sizeof S);
    CtSb warn = {0};
    do {
        if (!to) to = ct_str(js, end, "new_name");
        if (!sym || !*sym || !to || !*to) { snprintf(out, cap, "error: Rename needs `symbol` and `to`. Example: {\"symbol\": \"parseCfg\", \"to\": \"parseConfig\"}"); break; }
        if (!me_valid_id(sym)) { snprintf(out, cap, "error: symbol \"%.60s\" is not an identifier (letters, digits, _ and $, not starting with a digit). For other text use Replace.", sym); break; }
        if (!me_valid_id(to)) { snprintf(out, cap, "error: to \"%.60s\" is not a valid identifier (letters, digits, _ and $, not starting with a digit).", to); break; }
        if (!strcmp(sym, to)) { snprintf(out, cap, "error: symbol and to are the same; nothing to rename"); break; }
        const int all = kinds && !strcmp(kinds, "all");
        if (kinds && *kinds && !all && strcmp(kinds, "code") != 0) { snprintf(out, cap, "error: kinds must be \"code\" or \"all\", not \"%s\"", kinds); break; }
        const int dry = me_dry(js, end), force = ct_bool(js, end, "force");
        char cwd[4096], res[4200], root[4200], pre[4200], err[300] = "";
        struct stat st;
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) break;
        const size_t cl = strlen(cwd);                       /* raiz do indice: como no Find */
        if (strcmp(res, cwd) == 0) { snprintf(root, sizeof root, "%s", cwd); pre[0] = 0; }
        else if (strncmp(res, cwd, cl) == 0 && res[cl] == '/' && strcmp(cwd, "/") != 0) { snprintf(root, sizeof root, "%s", cwd); snprintf(pre, sizeof pre, "%s", res + cl + 1); }
        else if (S_ISDIR(st.st_mode)) { snprintf(root, sizeof root, "%s", res); pre[0] = 0; }
        else {
            snprintf(root, sizeof root, "%s", res);
            char *sl = strrchr(root, '/');
            if (sl && sl != root) { *sl = 0; snprintf(pre, sizeof pre, "%s", sl + 1); } else { snprintf(root, sizeof root, "/"); snprintf(pre, sizeof pre, "%s", res + 1); }
        }
        fd_update(&g_fd, root, err, sizeof err);
        FdIndex *X = &g_fd;
        if (!X->nf) { snprintf(out, cap, "No indexable text files under %s; nothing to rename.", root); break; }
        const size_t sl = strlen(sym), tl = strlen(to);
        /* colisao: o novo nome ja e definido em algum lugar */
        int ncol = 0; char col[900] = "";
        for (int s = 0; s < X->ns; s++) {
            if (X->f[X->sf[s]].kind == FK_MD || strcmp(fd_tstr(&X->T, X->sn[s]), to) != 0) continue;
            if (ncol < 3) { const size_t l = strlen(col); snprintf(col + l, sizeof col - l, "%s%s:%d", ncol ? ", " : "", X->f[X->sf[s]].rel, X->sl[s]); }
            ncol++;
        }
        if (ncol && !force) {
            snprintf(out, cap, "error: collision: \"%s\" is already defined at %s%s. Renaming %s to it would merge two things. Pick another name, or rename the other one first; "
                     "if they are in unrelated scopes (two static helpers in different files) call again with force=true. Nothing was changed.", to, col, ncol > 3 ? " (and more)" : "", sym);
            break;
        }
        /* arquivos candidatos: os que o indice liga ao termo (e os que definem o simbolo); cada ocorrencia e conferida lendo o texto */
        char *seen = (char *)calloc((size_t)X->nf + 1, 1);
        const int key = fd_key(X, sym, (int)sl);
        if (key >= 0) {
            int last = -1;
            for (int i = X->poff[2][key]; i < X->poff[2][key + 1]; i++) { const int fi = X->cf[X->pch[2][i]]; if (fi != last) { seen[fi] = 1; last = fi; } }
        }
        int ndef = 0; char defs[500] = "";
        for (int s = 0; s < X->ns; s++) if (!strcmp(fd_tstr(&X->T, X->sn[s]), sym) && X->f[X->sf[s]].kind != FK_MD && fd_in_path(X->f[X->sf[s]].rel, pre)) {
            seen[X->sf[s]] = 1;
            if (ndef < 3) { const size_t l = strlen(defs); snprintf(defs + l, sizeof defs - l, "%s%s:%d", ndef ? ", " : "", X->f[X->sf[s]].rel, X->sl[s]); }
            ndef++;
        }
        int shadow = 0; char shadowf[300] = "";
        for (int fi = 0; fi < X->nf; fi++) {
            if (!seen[fi] || !fd_in_path(X->f[fi].rel, pre)) continue;
            const char *rel = X->f[fi].rel;
            const int ck = me_ckind(rel);
            if (!all && (X->f[fi].kind == FK_MD || (X->f[fi].kind == FK_OTHER && ck == 0))) continue;      /* texto e documentacao: so com kinds=all */
            char ab[8500]; snprintf(ab, sizeof ab, "%s/%s", X->root, rel);
            size_t n = 0; char *t = me_read(ab, &n);
            if (!t) continue;
            MeFile tmp; memset(&tmp, 0, sizeof tmp);
            const int lex = !all && ck != 0;
            const int c = me_words(t, n, ck, lex, sym, sl, &tmp, to, tl);
            if (!c) { free(t); free(tmp.ed); continue; }
            const int sh = me_words(t, n, ck, lex, to, tl, NULL, NULL, 0);
            if (sh) { shadow += sh; if (!shadowf[0]) snprintf(shadowf, sizeof shadowf, "%s", rel); }
            char ferr[1500];
            char fa[8500];
            if (!ct_f_abs(ab, fa, sizeof fa, ferr, sizeof ferr)) { snprintf(out, cap, "%s", ferr); free(t); free(tmp.ed); me_free(&S); free(seen); goto cleanup; }
            MeFile *f = me_add(&S);
            f->abs = strdup(fa); f->orig = t; f->on = n; f->ed = tmp.ed; f->ned = tmp.ned; f->caped = tmp.caped; f->count = c;
        }
        free(seen);
        if (!S.n) {
            snprintf(out, cap, "No occurrence of %s as a whole word found in %s%s. It is looked up in the index of the project (text files up to 300 KB; .git, node_modules, build, dist, vendor and gitignored files are skipped)%s.",
                     sym, path && *path ? path : "the working folder", all ? "" : " (code, script and config files, outside comments and string literals)", all ? "" : "; kinds=\"all\" also searches comments, strings and docs");
            break;
        }
        if (!ndef) ct_sb_puts(&warn, "Note: no definition of the symbol was found in the index (it may come from a library: then the rename breaks the link to it).\n");
        else if (ndef > 1) { char m[300]; snprintf(m, sizeof m, "Note: %s is defined in %d places (%s); all of them are renamed.\n", sym, ndef, defs); ct_sb_puts(&warn, m); }
        if (shadow) { char m[400]; snprintf(m, sizeof m, "Warning: \"%s\" already appears as a word %d time%s in these files (first in %s); check that the renamed symbol does not collide with it.\n", to, shadow, shadow == 1 ? "" : "s", shadowf); ct_sb_puts(&warn, m); }
        if (ncol) { char m[300]; snprintf(m, sizeof m, "Warning: force=true: \"%s\" is also defined at %s.\n", to, col); ct_sb_puts(&warn, m); }
        char what[500]; snprintf(what, sizeof what, "%.200s -> %.200s", sym, to);
        me_finish(&S, dry, "Rename", "occurrence", what, cwd, &warn, out, cap);
    } while (0);
cleanup:
    me_free(&S);
    free(warn.p); free(sym); free(to); free(path); free(kinds);
}

/* ---- Replace ---- */
/* expande a substituicao para um casamento: \1..\9 grupos, \\ \n \t; 0 se cita grupo que nao existe */
static void me_expand(CtSb *o, const char *rep, int literal, const char *t, const long *sp) {
    if (literal) { ct_sb_puts(o, rep); return; }
    for (const char *p = rep; *p; p++) {
        if (*p != '\\' || !p[1]) { ct_sb_add(o, p, 1); continue; }
        p++;
        if (*p >= '0' && *p <= '9') { const int g = *p - '0'; if (sp[2 * g] >= 0 && sp[2 * g + 1] >= sp[2 * g]) ct_sb_add(o, t + sp[2 * g], (size_t)(sp[2 * g + 1] - sp[2 * g])); }
        else if (*p == 'n') ct_sb_add(o, "\n", 1);
        else if (*p == 't') ct_sb_add(o, "\t", 1);
        else if (*p == '\\') ct_sb_add(o, "\\", 1);
        else { ct_sb_add(o, "\\", 1); ct_sb_add(o, p, 1); }
    }
}
static void ct_replace(const char *js, const char *end, char *out, size_t cap) {
    if (cap < 600) { if (cap) out[0] = 0; return; }
    char *pat = ct_str(js, end, "pattern"), *rep = ct_str(js, end, "replacement"), *path = ct_str(js, end, "path"), *glob = ct_str(js, end, "glob");
    char *type = ct_str(js, end, "type");
    MeSet S; memset(&S, 0, sizeof S);
    CtFiles files; memset(&files, 0, sizeof files);
    Rx *re = NULL;
    do {
        if (!rep) rep = ct_str(js, end, "replace");
        if (!glob) glob = ct_str(js, end, "include");
        if (!pat || !*pat || !rep) { snprintf(out, cap, "error: Replace needs `pattern` and `replacement` (an empty replacement deletes the matches). Example: {\"pattern\": \"old_name\\\\((\\\\w+)\\\\)\", \"replacement\": \"new_name(\\\\1)\", \"glob\": \"*.py\"}"); break; }
        const int dry = me_dry(js, end), literal = ct_bool(js, end, "literal"), icase = ct_bool(js, end, "-i") || ct_bool(js, end, "ignore_case");
        const long long lim = ct_int(js, end, "limit_files", 100);
        char err[400], res[4200], cwd[4200];
        struct stat st;
        CtGrepFilter flt; memset(&flt, 0, sizeof flt);
        if (type && *type) {
            flt.exts = ct_type_exts(type);
            if (!flt.exts) { snprintf(out, cap, "error: unknown type \"%s\" (same types as Grep: c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml, toml, txt, xml, sql, rb, php, cs, kt, swift, lua). For anything else use `glob`.", type); break; }
        }
        if (glob && *glob) flt.glob = glob;
        re = rx_compile(pat, strlen(pat), RX_M | (icase ? RX_I : 0) | (ct_bool(js, end, "multiline") ? RX_S : 0) | (literal ? RX_LITERAL : 0), err, sizeof err);
        if (!re) { snprintf(out, cap, "error: pattern is not a valid regular expression (%s). Escape special characters with a backslash, or pass \"literal\": true. Nothing was changed.", err); break; }
        if (!literal) {
            int maxg = 0;
            for (const char *p = rep; *p; p++) if (*p == '\\' && p[1]) { if (p[1] >= '0' && p[1] <= '9' && p[1] - '0' > maxg) maxg = p[1] - '0'; p++; }
            if (maxg > re->ngroups - 1) { snprintf(out, cap, "error: replacement uses \\%d but the pattern has %d group%s (parentheses). Nothing was changed.", maxg, re->ngroups - 1, re->ngroups == 2 ? "" : "s"); break; }
        }
        if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) break;
        if (!agent_resolve(".", cwd, sizeof cwd)) cwd[0] = 0;
        if (S_ISDIR(st.st_mode)) { ct_grep_collect(&files, res, cwd, &flt); qsort(files.v, (size_t)files.n, sizeof(CtFile), ct_cmp_path); }
        else ct_files_add(&files, res, (long long)st.st_mtime);
        long *spans = (long *)malloc(sizeof(long) * 2 * (size_t)re->ngroups + 16);
        int bad = 0;
        for (int fi = 0; fi < files.n && !bad; fi++) {
            size_t n = 0; char *t = me_read(files.v[fi].path, &n);
            if (!t) continue;
            if (re->reqlen && !rx_memmem(t, n, re->req, (size_t)re->reqlen)) { free(t); continue; }
            MeFile tmp; memset(&tmp, 0, sizeof tmp);
            long pos = 0;
            while (pos <= (long)n) {
                const int r = rx_search(re, t, (long)n, pos, spans);
                if (r < 0) { snprintf(out, cap, "error: pattern too slow on %s; simplify it. Nothing was changed.", ct_display(files.v[fi].path, cwd)); bad = 1; break; }
                if (!r) break;
                const long s = spans[0], e = spans[1];
                if (e == s) { pos = s + 1; while (pos < (long)n && ((unsigned char)t[pos] & 0xC0) == 0x80) pos++; continue; }     /* casamento vazio: nao insere */
                CtSb rs = {0};
                me_expand(&rs, rep, literal, t, spans);
                if (!(rs.n == (size_t)(e - s) && memcmp(rs.p ? rs.p : "", t + s, rs.n) == 0)) me_push(&tmp, (size_t)s, (size_t)(e - s), rs.p ? rs.p : "", rs.n);
                free(rs.p);
                pos = e;
                if (tmp.ned > 50000) { snprintf(out, cap, "error: more than 50000 matches in %s; narrow the pattern. Nothing was changed.", ct_display(files.v[fi].path, cwd)); bad = 1; break; }
            }
            if (bad || !tmp.ned) { for (int k = 0; k < tmp.ned; k++) free(tmp.ed[k].r); free(tmp.ed); free(t); continue; }
            char ab[8500], ferr[1500];
            if (!ct_f_abs(files.v[fi].path, ab, sizeof ab, ferr, sizeof ferr)) { snprintf(out, cap, "%s", ferr); bad = 1; for (int k = 0; k < tmp.ned; k++) free(tmp.ed[k].r); free(tmp.ed); free(t); break; }
            MeFile *f = me_add(&S);
            f->abs = strdup(ab); f->orig = t; f->on = n; f->ed = tmp.ed; f->ned = tmp.ned; f->caped = tmp.caped; f->count = tmp.ned;
        }
        free(spans);
        if (bad) break;
        if (!S.n) { snprintf(out, cap, "No match for the pattern in %d file%s searched%s%s. Nothing was changed.", files.n, files.n == 1 ? "" : "s", flt.glob || flt.exts ? " (with the glob/type filter)" : "", literal ? "" : "; check it with Grep"); break; }
        if (S.n > lim) { snprintf(out, cap, "error: the pattern matches in %d files, more than limit_files (%lld). Narrow it with path, glob or type, or raise limit_files. Nothing was changed.", S.n, lim); break; }
        char what[500]; snprintf(what, sizeof what, "%.230s -> %.230s", pat, rep);
        me_finish(&S, dry, "Replace", "replacement", what, cwd, NULL, out, cap);
    } while (0);
    rx_free(re);
    ct_files_free(&files);
    me_free(&S);
    free(pat); free(rep); free(path); free(glob); free(type);
}

/* ---- Patch ---- */
typedef struct { int oa, ol, na, nl; char **t; char *k; int n, nonl_new; char head[120]; } PtHunk;
typedef struct { char *oldp, *newp; PtHunk *h; int nh, caph; } PtFile;
typedef struct { const char *s; size_t l; int cr; } PtLine;      /* uma linha do arquivo, sem o fim de linha */

static int pt_rtrim_eq(const char *a, size_t al, const char *b, size_t bl) {
    while (al && (a[al - 1] == ' ' || a[al - 1] == '\t' || a[al - 1] == '\r')) al--;
    while (bl && (b[bl - 1] == ' ' || b[bl - 1] == '\t' || b[bl - 1] == '\r')) bl--;
    return al == bl && memcmp(a, b, al) == 0;
}
static char *pt_path(const char *s, size_t l) {       /* depois de --- ou +++: corta no tab e tira aspas */
    while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) l--;
    for (size_t i = 0; i < l; i++) if (s[i] == '\t') { l = i; break; }
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') { s++; l -= 2; }
    char *r = (char *)malloc(l + 1); memcpy(r, s, l); r[l] = 0;
    return r;
}
static void pt_free(PtFile *F, int nf) {
    for (int i = 0; i < nf; i++) {
        for (int h = 0; h < F[i].nh; h++) { for (int k = 0; k < F[i].h[h].n; k++) free(F[i].h[h].t[k]); free(F[i].h[h].t); free(F[i].h[h].k); }
        free(F[i].h); free(F[i].oldp); free(F[i].newp);
    }
    free(F);
}
static int pt_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int pt_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
/* caminho que o diff quer dizer: tira a/ ou b/ quando o arquivo existe sem eles (ou, para criar, quando a pasta a/ ou b/ nao existe) */
static char *pt_pick(const char *p, int creating) {
    if ((p[0] == 'a' || p[0] == 'b') && p[1] == '/') {
        if (creating ? !pt_is_dir(p[0] == 'a' ? "a" : "b") : (pt_exists(p + 2) || !pt_exists(p))) return strdup(p + 2);
    }
    return strdup(p);
}
/* "@@ -a,b +c,d @@": 1 se leu */
static int pt_head(const char *s, PtHunk *h) {
    const char *p = s + 2;
    while (*p == ' ') p++;
    if (*p != '-') return 0;
    char *e;
    h->oa = (int)strtol(p + 1, &e, 10); h->ol = 1;
    if (*e == ',') { h->ol = (int)strtol(e + 1, &e, 10); }
    while (*e == ' ') e++;
    if (*e != '+') return 0;
    h->na = (int)strtol(e + 1, &e, 10); h->nl = 1;
    if (*e == ',') h->nl = (int)strtol(e + 1, &e, 10);
    return 1;
}
/* acha a posicao do bloco em L[from..nl): tolera desvio de ate 30 linhas; -1 se nao casa. Em *best a melhor aproximacao (indice, acertos). */
static int pt_find(const PtLine *L, int nl, int from, const PtHunk *h, int want, int *best, int *bestn) {
    int ol = 0;
    for (int i = 0; i < h->n; i++) if (h->k[i] != '+') ol++;
    for (int step = 0; step <= 60; step++) {
        const int at = want + (step & 1 ? (step + 1) / 2 : -(step / 2));      /* 0, +1, -1, +2, -2 ... o mais proximo primeiro */
        if (at < from || at + ol > nl) continue;
        int ok = 1, k = 0;
        for (int i = 0; i < h->n && ok; i++) {
            if (h->k[i] == '+') continue;
            ok = pt_rtrim_eq(L[at + k].s, L[at + k].l, h->t[i], strlen(h->t[i])); k++;
        }
        if (ok) return at;
    }
    /* sem casamento: o ponto do arquivo com mais linhas iguais (o trecho mais proximo para o modelo corrigir) */
    *best = -1; *bestn = -1;
    const long work = (long)nl * (ol ? ol : 1);
    for (int at = from; at + ol <= nl && (work < 4000000L || (at > want - 300 && at < want + 300)); at++) {
        int c = 0, k = 0;
        for (int i = 0; i < h->n; i++) { if (h->k[i] == '+') continue; if (pt_rtrim_eq(L[at + k].s, L[at + k].l, h->t[i], strlen(h->t[i]))) c++; k++; }
        if (c > *bestn || (c == *bestn && abs(at - want) < abs(*best - want))) { *bestn = c; *best = at; }
    }
    return -1;
}
/* o bloco inteiro casa em algum ponto do arquivo (mesmo longe)? onde */
static int pt_far(const PtLine *L, int nl, const PtHunk *h) {
    int ol = 0;
    for (int i = 0; i < h->n; i++) if (h->k[i] != '+') ol++;
    if (!ol) return -1;
    for (int at = 0; at + ol <= nl; at++) {
        int ok = 1, k = 0;
        for (int i = 0; i < h->n && ok; i++) { if (h->k[i] == '+') continue; ok = pt_rtrim_eq(L[at + k].s, L[at + k].l, h->t[i], strlen(h->t[i])); k++; }
        if (ok) return at;
    }
    return -1;
}
/* aplica os blocos de pf ao texto (data, n); novo texto em *res. 0 se um bloco nao casa, com o motivo em why */
static int pt_apply(const char *data, size_t n, const PtFile *pf, char **res, size_t *rn, int *shifted, char *why, size_t wcap) {
    int nl = 0;
    PtLine *L = (PtLine *)malloc(sizeof(PtLine) * (n / 1 + 2));
    int crlf = 0;
    const int final_nl0 = n > 0 && data[n - 1] == '\n';
    for (size_t i = 0; i < n;) {
        const char *q = (const char *)memchr(data + i, '\n', n - i);
        size_t e = q ? (size_t)(q - data) : n, le = e;
        int cr = 0;
        if (le > i && data[le - 1] == '\r') { le--; cr = 1; crlf++; }
        L[nl].s = data + i; L[nl].l = le - i; L[nl].cr = cr; nl++;
        i = q ? e + 1 : n;
    }
    const int defcr = nl && crlf * 2 > nl;
    CtSb o = {0};
    int cursor = 0, final_nl = final_nl0, ok = 1;
    *shifted = 0;
    for (int hi = 0; hi < pf->nh && ok; hi++) {
        const PtHunk *h = &pf->h[hi];
        int ol = 0;
        for (int i = 0; i < h->n; i++) if (h->k[i] != '+') ol++;
        int want = ol == 0 ? h->oa : h->oa - 1;
        if (want < 0) want = 0;
        if (want > nl) want = nl;
        int best = -1, bestn = -1;
        const int at = pt_find(L, nl, cursor, h, want, &best, &bestn);
        if (at < 0) {
            char near[1400] = "", why2[500];
            const int far = pt_far(L, nl, h);
            if (far >= 0) snprintf(why2, sizeof why2, "its text exists at line %d, but the diff says line %d (more than 30 lines apart; fix the line numbers)", far + 1, h->oa);
            else if (bestn <= 0) snprintf(why2, sizeof why2, "none of its context or removed lines was found in the file near line %d", h->oa);
            else snprintf(why2, sizeof why2, "its context or removed lines differ from the file (%d of %d lines match at the closest place)", bestn, ol);
            const int at2 = far >= 0 ? far : best;
            if (at2 >= 0) {
                size_t w = 0; int first = 1, k = 0;
                w += (size_t)snprintf(near + w, sizeof near - w, "\nClosest text of the file, from line %d:\n", at2 + 1);
                for (int i = 0; i < ol + 1 && at2 + i < nl && w + 240 < sizeof near; i++) {
                    const int l = (int)(L[at2 + i].l < 160 ? L[at2 + i].l : 160);
                    w += (size_t)snprintf(near + w, sizeof near - w, "%6d\t%.*s\n", at2 + i + 1, l, L[at2 + i].s);
                }
                for (int i = 0; i < h->n && first; i++) {
                    if (h->k[i] == '+') continue;
                    if (at2 + k >= nl) break;
                    if (!pt_rtrim_eq(L[at2 + k].s, L[at2 + k].l, h->t[i], strlen(h->t[i]))) {
                        const int a = (int)(strlen(h->t[i]) < 120 ? strlen(h->t[i]) : 120), b = (int)(L[at2 + k].l < 120 ? L[at2 + k].l : 120);
                        if (w + 300 < sizeof near) w += (size_t)snprintf(near + w, sizeof near - w, "First difference: the diff has `%.*s` where the file (line %d) has `%.*s`.", a, h->t[i], at2 + k + 1, b, L[at2 + k].s);
                        first = 0;
                    }
                    k++;
                }
            }
            snprintf(why, wcap, "block %d of %d (%s) does not match: %s.%s", hi + 1, pf->nh, h->head, why2, near);
            ok = 0; break;
        }
        if (at != want) (*shifted)++;
        for (int i = cursor; i < at; i++) { ct_sb_add(&o, L[i].s, L[i].l); if (L[i].cr) ct_sb_add(&o, "\r", 1); ct_sb_add(&o, "\n", 1); }
        int k = at;
        for (int i = 0; i < h->n; i++) {
            if (h->k[i] == ' ') { ct_sb_add(&o, L[k].s, L[k].l); if (L[k].cr) ct_sb_add(&o, "\r", 1); ct_sb_add(&o, "\n", 1); k++; }
            else if (h->k[i] == '-') k++;
            else { ct_sb_puts(&o, h->t[i]); if (defcr) ct_sb_add(&o, "\r", 1); ct_sb_add(&o, "\n", 1); }
        }
        cursor = k;
        if (k == nl) {                                            /* o bloco chega ao fim do arquivo: o fim de linha final segue o diff */
            int ch = 0;
            for (int i = 0; i < h->n; i++) if (h->k[i] != ' ') ch = 1;
            if (h->nonl_new) final_nl = 0; else if (ch) final_nl = 1;
        }
    }
    if (ok) {
        for (int i = cursor; i < nl; i++) { ct_sb_add(&o, L[i].s, L[i].l); if (L[i].cr) ct_sb_add(&o, "\r", 1); ct_sb_add(&o, "\n", 1); }
        if (!final_nl && o.n) { o.n--; if (o.n && o.p[o.n - 1] == '\r') o.n--; o.p[o.n] = 0; }       /* o arquivo termina sem quebra de linha */
        *res = o.p ? o.p : strdup(""); *rn = o.n;
    } else free(o.p);
    free(L);
    return ok;
}
static void ct_patch(const char *js, const char *end, char *out, size_t cap) {
    if (cap < 600) { if (cap) out[0] = 0; return; }
    char *diff = ct_str(js, end, "diff");
    if (!diff) diff = ct_str(js, end, "patch");
    PtFile *F = NULL; int nf = 0, capf = 0;
    MeSet S; memset(&S, 0, sizeof S);
    char **ln = NULL; int nln = 0;
    CtSb sb = {0};
    do {
        if (!diff || !*diff) { snprintf(out, cap, "error: Patch needs `diff`, a unified diff: lines `--- a/file` and `+++ b/file`, then blocks `@@ -12,3 +12,4 @@` whose lines start with a space (context), - (remove) or + (add)."); break; }
        const int dry = ct_has(js, end, "dry_run") ? ct_bool(js, end, "dry_run") : 0;
        /* linhas */
        int cl = 1;
        for (const char *p = diff; *p; p++) if (*p == '\n') cl++;
        ln = (char **)calloc((size_t)cl + 1, sizeof(char *));
        for (char *p = diff; p && *p;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            size_t l = strlen(p);
            if (l && p[l - 1] == '\r') p[l - 1] = 0;
            ln[nln++] = p;
            p = e ? e + 1 : NULL;
        }
        while (nln && !ln[nln - 1][0]) nln--;
        char perr[600] = "";
        for (int i = 0; i < nln && !perr[0];) {
            const char *s = ln[i];
            if (!strncmp(s, "--- ", 4) && i + 1 < nln && !strncmp(ln[i + 1], "+++ ", 4)) {
                if (nf == capf) { capf = capf ? capf * 2 : 4; F = (PtFile *)realloc(F, sizeof(PtFile) * (size_t)capf); }
                memset(&F[nf], 0, sizeof F[nf]);
                F[nf].oldp = pt_path(s + 4, strlen(s + 4)); F[nf].newp = pt_path(ln[i + 1] + 4, strlen(ln[i + 1] + 4));
                nf++; i += 2; continue;
            }
            if (!strncmp(s, "@@", 2)) {
                if (!nf) { snprintf(perr, sizeof perr, "error: line %d: a block (@@) comes before any `--- file` / `+++ file` header", i + 1); break; }
                PtFile *pf = &F[nf - 1];
                if (pf->nh == pf->caph) { pf->caph = pf->caph ? pf->caph * 2 : 4; pf->h = (PtHunk *)realloc(pf->h, sizeof(PtHunk) * (size_t)pf->caph); }
                PtHunk *h = &pf->h[pf->nh]; memset(h, 0, sizeof *h);
                if (!pt_head(s, h)) { snprintf(perr, sizeof perr, "error: line %d: cannot read the block header `%.60s`; it must look like @@ -12,3 +12,4 @@", i + 1, s); break; }
                snprintf(h->head, sizeof h->head, "%.100s", s);
                { char *q = strstr(h->head + 2, "@@"); if (q) q[2] = 0; }
                pf->nh++;
                int cap2 = 16; h->t = (char **)malloc(sizeof(char *) * (size_t)cap2); h->k = (char *)malloc((size_t)cap2);
                int ol = h->ol, nl2 = h->nl, j = i + 1;
                for (; j < nln; j++) {
                    const char *b = ln[j];
                    if (ol <= 0 && nl2 <= 0 && b[0] != '\\') break;
                    if (!strncmp(b, "@@", 2) || !strncmp(b, "diff ", 5) || (!strncmp(b, "--- ", 4) && j + 1 < nln && !strncmp(ln[j + 1], "+++ ", 4))) break;
                    if (b[0] == '\\') { if (h->n && h->k[h->n - 1] != '-') h->nonl_new = 1; continue; }
                    if (h->n == cap2) { cap2 *= 2; h->t = (char **)realloc(h->t, sizeof(char *) * (size_t)cap2); h->k = (char *)realloc(h->k, (size_t)cap2); }
                    char kind = b[0] == '-' ? '-' : b[0] == '+' ? '+' : ' ';
                    h->k[h->n] = kind;
                    h->t[h->n] = strdup(b[0] == '-' || b[0] == '+' || b[0] == ' ' ? b + 1 : b);        /* linha vazia ou sem o espaco: contexto */
                    h->n++;
                    if (kind != '+') ol--;
                    if (kind != '-') nl2--;
                }
                i = j; continue;
            }
            if (!strncmp(s, "rename from ", 12) || !strncmp(s, "rename to ", 10)) { snprintf(perr, sizeof perr, "error: line %d: renames are not supported by Patch; use Bash `mv` and then a diff of the contents", i + 1); break; }
            if (!strncmp(s, "Binary files ", 13) || !strncmp(s, "GIT binary patch", 16)) { snprintf(perr, sizeof perr, "error: line %d: binary changes are not supported by Patch", i + 1); break; }
            i++;
        }
        if (perr[0]) { snprintf(out, cap, "%s. Nothing was changed.", perr); break; }
        if (!nf) { snprintf(out, cap, "error: no file header found in the diff (a line `--- a/file` followed by `+++ b/file`). Nothing was changed."); break; }
        /* aplica cada arquivo em memoria */
        int nerr = 0, blocks = 0;
        char cwd[4200];
        if (!agent_resolve(".", cwd, sizeof cwd)) cwd[0] = 0;
        CtSb errs = {0};
        for (int i = 0; i < nf; i++) {
            const int creating = !strcmp(F[i].oldp, "/dev/null"), deleting = !strcmp(F[i].newp, "/dev/null");
            if (creating && deleting) { char m[200]; snprintf(m, sizeof m, "error: diff entry %d has /dev/null on both sides\n", i + 1); ct_sb_puts(&errs, m); nerr++; continue; }
            char *pp = pt_pick(creating ? F[i].newp : deleting ? F[i].oldp : F[i].newp, creating);
            if (!creating && !deleting && !pt_exists(pp) && strcmp(F[i].oldp, F[i].newp) != 0) { free(pp); pp = pt_pick(F[i].oldp, 0); }
            char ab[8500], ferr[1500];
            char m[1900];
            if (!ct_f_abs(pp, ab, sizeof ab, ferr, sizeof ferr)) { snprintf(m, sizeof m, "%s: %s\n", pp, ferr); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
            const char *disp = ct_display(ab, cwd);
            MeFile *f = NULL;
            for (int k = 0; k < S.n; k++) if (!strcmp(S.v[k].abs, ab)) f = &S.v[k];
            if (!f) {
                f = me_add(&S);
                f->abs = strdup(ab);
                struct stat st;
                if (stat(ab, &st) == 0) {
                    if (S_ISDIR(st.st_mode)) { snprintf(m, sizeof m, "%s: error: is a folder\n", disp); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
                    f->orig = me_read(ab, &f->on);
                    if (!f->orig) { snprintf(m, sizeof m, "%s: error: not a UTF-8 text file (or larger than 4 MB); Patch cannot change it\n", disp); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
                    f->data = (char *)malloc(f->on + 1); memcpy(f->data, f->orig, f->on + 1); f->n = f->on;
                } else { f->data = strdup(""); f->n = 0; }
            }
            const int exists = f->orig != NULL || f->count > 0;
            if (creating && f->orig) { snprintf(m, sizeof m, "%s: error: the diff creates this file but it already exists\n", disp); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
            if (!creating && !exists && !(f->count > 0)) { snprintf(m, sizeof m, "%s: error: file does not exist (the diff changes it). Check the path in the --- / +++ lines.\n", disp); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
            char *res = NULL; size_t rn = 0; int sh = 0; char why[2200];
            if (!pt_apply(f->data, f->n, &F[i], &res, &rn, &sh, why, sizeof why)) { snprintf(m, sizeof m, "%s: %.1700s\n", disp, why); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
            free(f->data); f->data = res; f->n = rn;
            f->count += F[i].nh; f->shifted += sh; blocks += F[i].nh;
            if (deleting) {
                if (rn) { snprintf(m, sizeof m, "%s: error: the diff deletes this file but its blocks leave %zu bytes of text; the removed lines must cover the whole file\n", disp, rn); ct_sb_puts(&errs, m); nerr++; free(pp); continue; }
                f->del = 1;
            }
            if (!f->count) f->count = 0;
            free(pp);
        }
        if (nerr) {
            snprintf(out, cap, "error: Patch not applied; nothing was changed (all or nothing). %d of %d file%s failed:\n%s", nerr, nf, nf == 1 ? "" : "s", errs.p ? errs.p : "");
            free(errs.p);
            break;
        }
        free(errs.p);
        if (!dry && !me_commit(&S, "Patch", out, cap)) break;
        char hd[200];
        snprintf(hd, sizeof hd, "Patch %s: %d file%s, %d block%s.\n", dry ? "checks out (dry run, nothing written)" : "applied", S.n, S.n == 1 ? "" : "s", blocks, blocks == 1 ? "" : "s");
        ct_sb_puts(&sb, hd);
        for (int i = 0; i < S.n; i++) {
            const MeFile *f = &S.v[i];
            char m[4800];
            if (f->del) snprintf(m, sizeof m, "%s: %s (%d block%s)\n", ct_display(f->abs, cwd), dry ? "would be deleted" : "deleted", f->count, f->count == 1 ? "" : "s");
            else if (!f->orig) { long long nlines = 0; for (size_t k = 0; k < f->n; k++) if (f->data[k] == '\n') nlines++; snprintf(m, sizeof m, "%s: %s (%lld lines)\n", ct_display(f->abs, cwd), dry ? "would be created" : "created", nlines + (f->n && f->data[f->n - 1] != '\n')); }
            else snprintf(m, sizeof m, "%s: %s (%d block%s%s)\n", ct_display(f->abs, cwd), dry ? "would apply" : "applied", f->count, f->count == 1 ? "" : "s", f->shifted ? ", some matched at other line numbers" : "");
            ct_sb_puts(&sb, m);
            if (sb.n > 6000) { ct_sb_puts(&sb, "(more files not listed)\n"); break; }
        }
        if (!dry) ct_sb_puts(&sb, "/undo reverts it.");
        snprintf(out, cap, "%s", sb.p ? sb.p : "");
    } while (0);
    free(sb.p); free(ln); pt_free(F, nf); me_free(&S); free(diff);
}
#else
static void ct_rename(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
static void ct_replace(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
static void ct_patch(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
static int g_me_changed;
#endif
