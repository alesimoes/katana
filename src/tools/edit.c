/* ---------- ferramenta Edit (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * Troca exata de texto. Exige ter lido o arquivo inteiro (ct_seen); aceita arquivo mudado no disco se o trecho ainda casa. */
static const char CT_EDIT_DESC[] =
    "Performs an exact string replacement in a file (no regex). Read the whole file with Read first, or the edit is refused. "
    "old_string must match the file text exactly, including spaces, tabs and line breaks (do not include the line number prefix that Read shows), "
    "and must be unique in the file: otherwise add more surrounding lines to make it unique, or set replace_all to replace every occurrence. "
    "new_string must differ from old_string. To create a new file with Edit, give an empty old_string for a path that does not exist (Write is better for that). "
    "Files that are not valid UTF-8 are refused. After the edit a few lines around the change are shown, and a syntax check runs for common languages.";
static const char CT_EDIT_PROPS[] =
    "\"file_path\": {\"type\": \"string\", \"description\": \"The absolute path of the file to modify\"}, "
    "\"old_string\": {\"type\": \"string\", \"description\": \"The text to replace, exactly as it is in the file\"}, "
    "\"new_string\": {\"type\": \"string\", \"description\": \"The text to replace it with (must be different from old_string)\"}, "
    "\"replace_all\": {\"type\": \"boolean\", \"description\": \"Replace every occurrence of old_string (default false)\"}";
static const char CT_EDIT_REQ[] = "\"file_path\", \"old_string\", \"new_string\"";

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
    if (fclose(f) != 0 || w != n) { snprintf(out, cap, "error: write to %s failed (%s)", abs, strerror(errno)); return 0; }
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

static void ct_edit(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], err[1400];
    char *fp = ct_str(js, end, "file_path"), *os = ct_str(js, end, "old_string"), *ns = ct_str(js, end, "new_string"), *b = NULL, *nb = NULL;
    do {
        if (!fp) { snprintf(out, cap, "error: missing file_path"); break; }
        if (!os) { snprintf(out, cap, "error: missing old_string"); break; }
        if (!ns) { snprintf(out, cap, "error: missing new_string (pass an empty string to delete the text)"); break; }
        const int all = ct_bool(js, end, "replace_all");
        if (strcmp(os, ns) == 0) { snprintf(out, cap, "error: old_string and new_string are identical; nothing to change"); break; }
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        const int exists = stat(abs, &st) == 0;
        if (exists && S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory; give the path of a file", abs); break; }
        const size_t ol = strlen(os), nl = strlen(ns);
        size_t n = 0;
        if (!exists) {
            if (ol) { snprintf(out, cap, "error: %s does not exist. To create a file use Write (or Edit with an empty old_string).", abs); break; }
            char what[4400];
            snprintf(what, sizeof what, "criar o arquivo %s (%zu bytes)", abs, nl);
            if (!agent_act(what, ns)) { snprintf(out, cap, "error: the user denied this action"); break; }
            if (!ct_f_save(abs, ns, nl, out, cap)) break;
            ct_seen_mark(abs, 0);
            snprintf(out, cap, "File created successfully at: %s (%zu bytes, %lld lines)", abs, nl, ct_count_lines(ns, nl));
            code_check(abs, 0, out, cap);
            break;
        }
        const int sn = ct_seen(abs);
        if (sn == 0 || sn == 1) {
            snprintf(out, cap, "error: you have %s %s. Read the whole file with Read first, then Edit.", sn == 0 ? "not read" : "only read part of", abs);
            break;
        }
        b = ct_f_slurp(abs, &n, err, sizeof err);
        if (!b) { snprintf(out, cap, "%s", err); break; }
        if (memchr(b, 0, n) || !ct_utf8_ok((const unsigned char *)b, n)) { snprintf(out, cap, "error: %s is not a valid UTF-8 text file; editing would corrupt it. Use Bash for this file.", abs); break; }
        if (!ol && n) { snprintf(out, cap, "error: old_string is empty but %s already has content. Give the exact text to replace, or use Write to replace the whole file.", abs); break; }
        int cnt = 0;
        const char *first = b;
        if (ol) {
            first = NULL;
            for (const char *p = strstr(b, os); p; p = strstr(p + ol, os)) { if (!cnt) first = p; cnt++; }
        } else cnt = 1;
        if (cnt == 0) { snprintf(out, cap, "error: old_string was not found in %s (0 matches). It must match exactly, including spaces, tabs and line breaks, and without the line number prefix from Read. Read the file again if it may have changed.", abs); break; }
        if (cnt > 1 && !all) { snprintf(out, cap, "error: old_string matches %d places in %s; it must be unique. Add more surrounding lines to old_string to make it unique, or set replace_all to true to replace all %d.", cnt, abs, cnt); break; }
        const int reps = all ? cnt : 1;
        long long line = 1;
        for (const char *p = b; p < first; p++) if (*p == '\n') line++;
        char what[4400];
        snprintf(what, sizeof what, "editar o arquivo %s (%d troca%s)", abs, reps, reps > 1 ? "s" : "");
        if (!agent_act(what, os)) { snprintf(out, cap, "error: the user denied this action"); break; }
        nb = (char *)malloc(n + (size_t)reps * (nl > ol ? nl - ol : 0) + nl + 1);
        if (!nb) { snprintf(out, cap, "error: out of memory"); break; }
        size_t o = 0;
        if (!ol) { memcpy(nb, ns, nl); o = nl; }
        else {
            const char *p = b;
            int done = 0;
            for (const char *q = strstr(b, os); q && done < reps; q = strstr(p, os)) {
                memcpy(nb + o, p, (size_t)(q - p)); o += (size_t)(q - p);
                memcpy(nb + o, ns, nl); o += nl;
                p = q + ol; done++;
            }
            const size_t rest = n - (size_t)(p - b);
            memcpy(nb + o, p, rest); o += rest;
        }
        if (!ct_f_save(abs, nb, o, out, cap)) break;
        ct_seen_mark(abs, 0);
        long long nlines = 1;
        for (size_t i = 0; i < nl; i++) if (ns[i] == '\n') nlines++;
        long long from = line > 3 ? line - 3 : 1, to = line + nlines - 1 + 3;
        if (to - from > 24) to = from + 24;
        size_t w = (size_t)snprintf(out, cap, "The file %s has been updated (%d replacement%s, first at line %lld).%s\n", abs, reps, reps > 1 ? "s" : "", line,
                                    sn == 3 ? "\n[note: the file had other changes on disk since you last read it; this edit was applied to its current content. Read it again to see them.]" : "");
        if (w < cap) w += ct_ctx_lines(nb, o, from, to, out + w, cap - w);
        if (w > 0 && w < cap && out[w - 1] == '\n') out[w - 1] = 0;
        code_check(abs, 0, out, cap);
    } while (0);
    free(fp); free(os); free(ns); free(b); free(nb);
}
