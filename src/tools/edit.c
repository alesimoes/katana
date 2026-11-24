/* ---------- ferramenta Edit (ver docs/tools.md) ----------
 * Troca exata de texto, uma (old_string/new_string) ou varias no mesmo arquivo (edits: tudo ou nada, na ordem). Exige ter lido o arquivo inteiro (ct_seen); aceita arquivo mudado no disco se o trecho ainda casa. */
static const char CT_EDIT_DESC[] =
    "Performs an exact string replacement in a file (no regex). Read the whole file with Read first, or the edit is refused. "
    "old_string must match the file text exactly, including spaces, tabs and line breaks (do not include the line number prefix that Read shows), "
    "and must be unique in the file: otherwise add more surrounding lines to make it unique, or set replace_all to replace every occurrence. "
    "new_string must differ from old_string. For several changes in the same file send edits instead of old_string and new_string: they are applied in order, all or none, "
    "each to the text left by the previous one. edits is plain text with one block per change (nothing is escaped): "
    "a line <<<<<<< , the old text, a line ======= , the new text, a line >>>>>>> ; an array of {old_string, new_string, replace_all} is accepted too. To create a new file with Edit, give an empty old_string for a path that does not exist (Write is better for that). "
    "Files that are not valid UTF-8 are refused. After the edit a few lines around the change are shown, and a syntax check runs for common languages. "
    "To change ONE function, class, method or variable without sending the file or the old text (no Read needed), give symbol instead of old_string: its name, Class.name, or name:LINE when the name is ambiguous (the error lists the candidates), "
    "and new_string written WITHOUT the symbol's own indentation (it is indented for you). where: replace (default: the whole definition, header and decorators included), body (only what is inside the braces, or the indented block in Python), "
    "before or after (insert new_string next to the symbol), delete (no new_string). The result is checked BEFORE it is written: unbalanced brackets, a symbol that disappears or changes kind, or a syntax error refuse the edit and leave the file untouched, with the line and what was expected. "
    "A report follows, each finding marked structural (the language scanner), derived (callers found by name, listed when the signature changed or a name disappeared) or lsp (new errors from a language server, when one is installed).";
static const char CT_EDIT_PROPS[] =
    "\"file_path\": {\"type\": \"string\", \"description\": \"The absolute path of the file to modify\"}, "
    "\"old_string\": {\"type\": \"string\", \"description\": \"The text to replace, exactly as it is in the file\"}, "
    "\"new_string\": {\"type\": \"string\", \"description\": \"The text to replace it with (must be different from old_string)\"}, "
    "\"replace_all\": {\"type\": \"boolean\", \"description\": \"Replace every occurrence of old_string (default false)\"}, "
    "\"edits\": {\"type\": \"string\", \"description\": \"Several changes to this file, instead of old_string and new_string: one block per change, "
    "each block being a line <<<<<<<, the old text, a line =======, the new text, a line >>>>>>>\"}, "
    "\"symbol\": {\"type\": \"string\", \"description\": \"Edit one symbol instead of old_string: name, Class.name or name:LINE\"}, "
    "\"where\": {\"type\": \"string\", \"enum\": [\"replace\", \"body\", \"before\", \"after\", \"delete\"], \"description\": \"With symbol: what new_string replaces or where it goes (default replace)\"}";
static const char CT_EDIT_REQ[] = "\"file_path\"";

static void index_touch(const char *path);                   /* src/coder/indexctl.c */
static void ct_edit_symbol(const char *js, const char *end, char *out, size_t cap);   /* src/coder/symedit.c */
/* grava o arquivo (cria ou sobrescreve); 1 se foi, senao a mensagem em out */
static int ct_f_save(const char *abs, const char *data, size_t n, char *out, size_t cap) {
    mkdir_parents(abs);
    undo_note(abs);
    FILE *f = fopen(abs, "wb");
    if (!f) {
        const int perm = errno == EACCES || errno == EPERM || errno == EROFS;
        snprintf(out, cap, "error: cannot write %s (%s).%s", abs, strerror(errno), perm ? " It needs more permission: use Bash with sudo if the user wants this." : "");
        return 0;
    }
    const size_t w = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || w != n) { snprintf(out, cap, "error: write to %s failed (%s)", abs, strerror(errno)); index_touch(abs); return 0; }
    index_touch(abs);          /* o indice do coder revalida a arvore na proxima consulta */
    return 1;
}
/* linhas first..last (1-based) de b, numeradas, para mostrar ao redor de uma mudanca */
static size_t ct_ctx_lines(const char *b, size_t n, long long first, long long last, char *o, size_t cap) {
    size_t w = 0, s = 0;
    long long ln = 1;
    while (ln < first && s < n) { const char *nl = (const char *)memchr(b + s, '\n', n - s); if (!nl) { s = n; break; } s = (size_t)(nl - b) + 1; ln++; }
    for (; ln <= last && s < n; ln++) {
        const char *nl = (const char *)memchr(b + s, '\n', n - s);
        size_t le = nl ? (size_t)(nl - b) : n;
        if (le > s && b[le - 1] == '\r') le--;
        size_t l = le - s;
        if (l > 200) { l = 200; while (l > 0 && ((unsigned char)b[s + l] & 0xC0) == 0x80) l--; }
        if (w + l + 12 >= cap) break;
        w += (size_t)snprintf(o + w, cap - w, "%6lld\t%.*s\n", ln, (int)l, b + s);
        s = nl ? (size_t)(nl - b) + 1 : n;
    }
    return w;
}

/* uma troca; del = bloco de texto sem linhas novas: apaga tambem a quebra de linha que sobraria */
typedef struct { char *os, *ns; int all, del; } CtEd;
#define CT_ED_MAX 64
static void ct_ed_free(CtEd *e, int n) { for (int i = 0; i < n; i++) { free(e[i].os); free(e[i].ns); } }
static char *ct_ed_dup(const char *a, const char *b) {
    const size_t n = b > a ? (size_t)(b - a) : 0;
    char *r = (char *)malloc(n + 1);
    if (r) { memcpy(r, a, n); r[n] = 0; }
    return r;
}
/* linha (sem a quebra) que e so a marca c repetida 7 vezes, com espacos ou um rotulo depois (menos no =) */
static int ct_ed_mark(const char *l, const char *e, char c) {
    if (e > l && e[-1] == '\r') e--;
    for (int i = 0; i < 7; i++) if (l + i >= e || l[i] != c) return 0;
    for (l += 7; c == '=' && l < e; l++) if (*l != ' ' && *l != '\t') return 0;
    return l >= e || *l == ' ' || *l == '\t';
}
/* blocos de texto -> trocas; n, ou -1 com o motivo em out */
static int ct_ed_blocks(const char *t, CtEd *ed, char *out, size_t cap) {
    int n = 0, st = 0;
    const char *a = t, *oe = t, *nb = t;
    for (const char *l = t; *l;) {
        const char *e = strchr(l, '\n'), *nx = e ? e + 1 : l + strlen(l);
        if (!e) e = nx;
        if (st == 0) {
            if (ct_ed_mark(l, e, '<')) { st = 1; a = nx; }
            else for (const char *q = l; q < e; q++) if (!isspace((unsigned char)*q)) { snprintf(out, cap, "error: edits: expected a line <<<<<<< to start block %d, found: %.*s", n + 1, (int)(e - l > 80 ? 80 : e - l), l); return -1 - n; }
        } else if (ct_ed_mark(l, e, '=')) {
            if (st == 2) { snprintf(out, cap, "error: edits: block %d has two ======= lines, so old and new text cannot be told apart; make this change with old_string and new_string", n + 1); return -1 - n; }
            st = 2; oe = l; nb = nx;
        } else if (st == 2 && ct_ed_mark(l, e, '>')) {
            if (n == CT_ED_MAX) { snprintf(out, cap, "error: edits: at most %d changes per call", CT_ED_MAX); return -1 - n; }
            const int cr = oe > a + 1 && oe[-2] == '\r';
            ed[n].os = ct_ed_dup(a, oe > a ? oe - 1 - cr : a);
            const int cn = l > nb + 1 && l[-2] == '\r';
            ed[n].ns = ct_ed_dup(nb, l > nb ? l - 1 - cn : nb);
            ed[n].all = 0; ed[n].del = l == nb; n++; st = 0;
        }
        l = nx;
    }
    if (st) { snprintf(out, cap, "error: edits: block %d is not closed (it needs a line ======= and then a line >>>>>>>)", n + 1); return -1 - n; }
    return n;
}
/* le o argumento edits; n de trocas, 0 = nao veio, negativo = erro em out (-1 - trocas a liberar) */
static int ct_ed_parse(const char *js, const char *end, CtEd *ed, char *out, size_t cap) {
    const char *a, *b;
    if (!ct_find(js, end, "edits", &a, &b)) return 0;
    if (*a == '"') {
        char *t = ct_str(js, end, "edits");
        if (!t) return 0;
        int n = 0; const char *q = t; while (isspace((unsigned char)*q)) q++;
        if (*q == '[') {      /* a lista de trocas veio como texto (chamada em XML): le como lista */
            Buf w = {0}; buf_str(&w, "{\"edits\": "); buf_str(&w, q); buf_str(&w, "}");
            n = ct_ed_parse(w.p, w.p + strlen(w.p), ed, out, cap);
            free(w.p); free(t);
            return n;
        }
        if (*q) { n = ct_ed_blocks(t, ed, out, cap); if (n == 0) { snprintf(out, cap, "error: edits has no blocks"); n = -1; } }
        free(t);
        return n;
    }
    if (*a != '[') { if (!strncmp(a, "null", 4)) return 0; snprintf(out, cap, "error: edits must be an array of {old_string, new_string}, or text blocks (<<<<<<< old ======= new >>>>>>>)"); return -1; }
    int n = 0;
    const char *p = a + 1;
    for (;;) {
        while (p < b && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= b || *p == ']') break;
        const char *e = ct_skip(p, b);
        if (!e || *p != '{') { snprintf(out, cap, "error: edits[%d] must be an object {old_string, new_string}", n); return -1 - n; }
        if (n == CT_ED_MAX) { snprintf(out, cap, "error: edits: at most %d changes per call", CT_ED_MAX); return -1 - n; }
        ed[n].os = ct_str(p, e, "old_string"); ed[n].ns = ct_str(p, e, "new_string"); ed[n].all = ct_bool(p, e, "replace_all"); ed[n].del = 0;
        n++;
        if (!ed[n - 1].os || !ed[n - 1].ns) { snprintf(out, cap, "error: edits[%d] needs old_string and new_string", n - 1); return -1 - n; }
        p = e;
    }
    if (!n) { snprintf(out, cap, "error: edits is empty"); return -1; }
    return n;
}

static void ct_edit(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], err[1400];
    CtEd ed[CT_ED_MAX];
    size_t pos[CT_ED_MAX];              /* onde ficou a primeira troca de cada item, no texto final */
    char *fp = ct_str(js, end, "file_path"), *b = NULL, *det = NULL;
    int ne = 0;
    { char *sym = ct_str(js, end, "symbol"); const int bysym = sym && *sym; free(sym); if (bysym) { free(fp); ct_edit_symbol(js, end, out, cap); return; } }
    do {
        if (!fp) { snprintf(out, cap, "error: missing file_path"); break; }
        ne = ct_ed_parse(js, end, ed, out, cap);
        if (ne < 0) { ne = -1 - ne; break; }
        const int multi = ne > 0;
        char *os1 = ct_str(js, end, "old_string"), *ns1 = ct_str(js, end, "new_string");
        if (multi) {
            const int both = (os1 && *os1) || (ns1 && *ns1);
            free(os1); free(ns1);
            if (both) { snprintf(out, cap, "error: send either edits or old_string and new_string, not both"); break; }
        } else {
            ed[0].os = os1; ed[0].ns = ns1; ed[0].all = ct_bool(js, end, "replace_all"); ed[0].del = 0; ne = 1;
            if (!os1) { snprintf(out, cap, "error: missing old_string"); break; }
            if (!ns1) { snprintf(out, cap, "error: missing new_string (pass an empty string to delete the text)"); break; }
        }
        char pre[80] = "", post[40] = "";
        int bad = 0;
        for (int i = 0; i < ne && !bad; i++) {
            if (multi) { snprintf(pre, sizeof pre, "edit %d of %d: ", i + 1, ne); snprintf(post, sizeof post, " Nothing was changed."); }
            if (strcmp(ed[i].os, ed[i].ns) == 0 && !(ed[i].del && ed[i].os[0])) { snprintf(out, cap, "error: %sold_string and new_string are identical; nothing to change", pre); bad = 1; }
            else if (multi && !ed[i].os[0]) { snprintf(out, cap, "error: %sthe old text is empty.%s", pre, post); bad = 1; }
        }
        if (bad) break;
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        const int exists = stat(abs, &st) == 0;
        if (exists && S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory; give the path of a file", abs); break; }
        size_t n = 0;
        if (!exists) {
            if (ed[0].os[0]) { snprintf(out, cap, "error: %s does not exist. To create a file use Write (or Edit with an empty old_string).", abs); break; }
            const size_t nl = strlen(ed[0].ns);
            char what[4400];
            snprintf(what, sizeof what, "criar o arquivo %s (%zu bytes)", abs, nl);
            if (!agent_act(what, ed[0].ns)) { snprintf(out, cap, "error: the user denied this action"); break; }
            if (!ct_f_save(abs, ed[0].ns, nl, out, cap)) break;
            ct_seen_mark(abs, 0);
            snprintf(out, cap, "File created successfully at: %s (%zu bytes, %lld lines)", abs, nl, ct_count_lines(ed[0].ns, nl));
            code_check(abs, 0, out, cap);
            break;
        }
        const int sn = ct_seen(abs);
        if (sn == 0 || sn == 1) {
            snprintf(out, cap, "error: you have %s %s. Read the whole file with Read first, then Edit (a large file: Read it with offset=1 and a limit as large as its line count).", sn == 0 ? "not read" : "only read part of", abs);
            break;
        }
        b = ct_f_slurp(abs, &n, err, sizeof err);
        if (!b) { snprintf(out, cap, "%s", err); break; }
        if (memchr(b, 0, n) || !ct_utf8_ok((const unsigned char *)b, n)) { snprintf(out, cap, "error: %s is not a valid UTF-8 text file; editing would corrupt it. Use Bash for this file.", abs); break; }
        if (!ed[0].os[0] && n) { snprintf(out, cap, "error: old_string is empty but %s already has content. Give the exact text to replace, or use Write to replace the whole file.", abs); break; }
        int total = 0;
        Buf dt = {0};
        for (int i = 0; i < ne; i++) {                   /* cada troca no texto que a anterior deixou, em memoria: so grava se todas valem */
            if (multi) { snprintf(pre, sizeof pre, "edit %d of %d: ", i + 1, ne); if (i) buf_str(&dt, "\n---\n"); }
            const char *os = ed[i].os, *ns = ed[i].ns;
            const size_t ol = strlen(os), nl = strlen(ns);
            buf_str(&dt, os);
            int cnt = 0;
            if (ol) for (const char *p = strstr(b, os); p; p = strstr(p + ol, os)) cnt++;
            else cnt = 1;
            if (cnt == 0) { snprintf(out, cap, "error: %sold_string was not found in %s (0 matches). It must match exactly, including spaces, tabs and line breaks, and without the line number prefix from Read. Read the file again if it may have changed.%s", pre, abs, post); bad = 1; break; }
            if (cnt > 1 && !ed[i].all) { snprintf(out, cap, "error: %sold_string matches %d places in %s; it must be unique. Add more surrounding lines to old_string to make it unique, or set replace_all to true to replace all %d.%s", pre, cnt, abs, cnt, post); bad = 1; break; }
            char *nb = (char *)malloc(n + (size_t)cnt * nl + 1);
            if (!nb) { snprintf(out, cap, "error: out of memory"); bad = 1; break; }
            size_t o = 0;
            const char *p = b;
            int done = 0;
            for (const char *q = ol ? strstr(b, os) : b; q && done < cnt; q = ol ? strstr(p, os) : NULL) {
                size_t ql = ol;
                if (ed[i].del && (q == b || q[-1] == '\n')) { if (q[ql] == '\r' && q[ql + 1] == '\n') ql += 2; else if (q[ql] == '\n') ql++; }
                memcpy(nb + o, p, (size_t)(q - p)); o += (size_t)(q - p);
                if (!done) pos[i] = o;
                for (int k = 0; k < i; k++) if (pos[k] > o) pos[k] = pos[k] + nl >= ql ? pos[k] + nl - ql : 0;      /* trocas anteriores abaixo desta andam junto */
                memcpy(nb + o, ns, nl); o += nl;
                p = q + ql; done++;
            }
            const size_t rest = n - (size_t)(p - b);
            memcpy(nb + o, p, rest); o += rest; nb[o] = 0;
            free(b); b = nb; n = o; total += done;
        }
        det = dt.p;
        if (bad) break;
        char what[4400];
        snprintf(what, sizeof what, "editar o arquivo %s (%d troca%s)", abs, total, total > 1 ? "s" : "");
        if (!agent_act(what, det ? det : "")) { snprintf(out, cap, "error: the user denied this action"); break; }
        if (!ct_f_save(abs, b, n, out, cap)) break;
        ct_seen_mark(abs, 0);
        const char *note = sn == 3 ? "\n[note: the file had other changes on disk since you last read it; this edit was applied to its current content. Read it again to see them.]" : "";
        size_t w = 0;
        long long shown = 0;
        for (int i = 0; i < ne; i++) {
            long long line = 1, nlines = 1;
            for (size_t k = 0; k < pos[i] && k < n; k++) if (b[k] == '\n') line++;
            for (const char *q = ed[i].ns; *q; q++) if (*q == '\n') nlines++;
            const int around = multi ? 1 : 3, most = multi ? 8 : 24;
            if (!i) {
                if (multi) w = (size_t)snprintf(out, cap, "The file %s has been updated (%d edits, %d replacement%s).%s\n", abs, ne, total, total > 1 ? "s" : "", note);
                else w = (size_t)snprintf(out, cap, "The file %s has been updated (%d replacement%s, first at line %lld).%s\n", abs, total, total > 1 ? "s" : "", line, note);
            }
            long long from = line > around ? line - around : 1, to = line + nlines - 1 + around;
            if (to - from > most) to = from + most;
            if (multi && shown + (to - from) > 120) { if (w < cap) w += (size_t)snprintf(out + w, cap - w, "[the other edits are not shown]\n"); break; }
            shown += to - from + 1;
            if (multi && w < cap) w += (size_t)snprintf(out + w, cap - w, "edit %d, line %lld:\n", i + 1, line);
            if (w < cap) w += ct_ctx_lines(b, n, from, to, out + w, cap - w);
        }
        if (w > 0 && w < cap && out[w - 1] == '\n') out[w - 1] = 0;
        code_check(abs, 0, out, cap);
    } while (0);
    ct_ed_free(ed, ne);
    free(fp); free(b); free(det);
}
