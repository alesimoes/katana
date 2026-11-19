/* ---------- camada de codigo: leitura por trechos e verificacao de sintaxe ----------
 * read_file devolve o arquivo inteiro quando cabe, ou um trecho de linhas inteiras com o aviso de quais linhas
 * vieram e como pedir as proximas (offset/limit). Depois de write_file e edit_file o motor confere a sintaxe do
 * arquivo (python, shell, json, javascript, php, ruby) e junta o resultado ao retorno da ferramenta: o modelo ve
 * o erro na hora, sem gastar uma rodada para descobrir. */
#define READ_MAX 6000       /* bytes de um trecho de read_file */

static int json_int(const char *js, const char *end, const char *key, int def);

static void tool_read_file(const char *js, const char *end, char *out, size_t cap) {
    static char arg[8192];
    char res[4200], err[1300];
    if (!json_str(js, end, "path", arg, sizeof arg)) { snprintf(out, cap, "error: missing path"); return; }
    if (!agent_read_access(arg, 0, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    int off = json_int(js, end, "offset", 1);
    const int lim = json_int(js, end, "limit", 0);
    if (off < 1) off = 1;
    FILE *f = fopen(res, "rb");
    if (!f) { snprintf(out, cap, "error: cannot open %s (%s)", arg, strerror(errno)); return; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 8000000) { fclose(f); snprintf(out, cap, "error: file too large (%ld bytes); use search_files or run_command with head/tail/sed -n", sz); return; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); snprintf(out, cap, "error: out of memory"); return; }
    const size_t n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[n] = 0;
    if (n == 0) { free(b); snprintf(out, cap, "(empty file)"); return; }
    if (memchr(b, 0, n < 4096 ? n : 4096)) { free(b); snprintf(out, cap, "error: %s looks like a binary file (%zu bytes)", arg, n); return; }
    int total = 1;
    for (size_t i = 0; i + 1 < n; i++) if (b[i] == '\n') total++;
    if (off > total) { free(b); snprintf(out, cap, "error: %s has only %d lines", arg, total); return; }
    size_t s = 0;
    for (int ln = 1; ln < off && s < n; s++) if (b[s] == '\n') ln++;
    const size_t room = cap > READ_MAX + 200 ? READ_MAX : cap > 400 ? cap - 200 : 0;
    size_t e = s;
    int shown = 0;
    while (e < n && (!lim || shown < lim)) {
        const char *nl = (const char *)memchr(b + e, '\n', n - e);
        const size_t le = nl ? (size_t)(nl - b) + 1 : n;
        if (le - s > room) { if (!shown) { e = s + room; shown = 1; } break; }     /* linha unica maior que o trecho: corta em bytes */
        e = le; shown++;
    }
    memcpy(out, b + s, e - s);
    out[e - s] = 0;
    if (off > 1 || e < n) {
        const int last = off + shown - 1;
        if (e < n) snprintf(out + (e - s), cap - (e - s), "\n[lines %d-%d of %d shown; for the next ones call read_file with offset=%d]", off, last, total, last + 1);
        else snprintf(out + (e - s), cap - (e - s), "\n[lines %d-%d of %d shown (end of file)]", off, last, total);
    }
    free(b);
}

/* confere a sintaxe do arquivo gravado e junta o resultado em out. partial: gravacao com append (podem faltar partes).
   Sem a ferramenta da linguagem instalada, ou para outras extensoes, nao acrescenta nada. */
static void code_check(const char *path, int partial, char *out, size_t cap) {
#ifndef _WIN32
    const char *base = strrchr(path, '/'), *ext = strrchr(path, '.');
    if (!ext || (base && ext < base)) return;
    static char q[4400], cmd[9400];
    if (!shq(path, q, sizeof q)) return;
    if (strcmp(ext, ".py") == 0)
        snprintf(cmd, sizeof cmd, "timeout 10 python3 -c 'import ast,sys\ntry:\n    ast.parse(open(sys.argv[1],\"rb\").read(), sys.argv[1])\n"
                 "except SyntaxError as e:\n    print(\"line %%s: %%s\\n%%s\" %% (e.lineno, e.msg, (e.text or \"\").rstrip())); sys.exit(1)\n"
                 "except Exception:\n    pass' %s 2>&1", q);
    else if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0) snprintf(cmd, sizeof cmd, "timeout 10 bash -n %s 2>&1", q);
    else if (strcmp(ext, ".json") == 0)
        snprintf(cmd, sizeof cmd, "timeout 10 python3 -c 'import json,sys\ntry:\n    json.load(open(sys.argv[1]))\n"
                 "except ValueError as e:\n    print(e); sys.exit(1)\nexcept Exception:\n    pass' %s 2>&1", q);
    else if (strcmp(ext, ".js") == 0 || strcmp(ext, ".mjs") == 0 || strcmp(ext, ".cjs") == 0) snprintf(cmd, sizeof cmd, "timeout 10 node --check %s 2>&1", q);
    else if (strcmp(ext, ".php") == 0) snprintf(cmd, sizeof cmd, "timeout 10 php -l %s 2>&1", q);
    else if (strcmp(ext, ".rb") == 0) snprintf(cmd, sizeof cmd, "timeout 10 ruby -c %s 2>&1", q);
    else return;
    int rc = 0;
    size_t n = 0;
    char *o = run_capture(cmd, 4000, &rc, &n);
    if (!o) return;
    const size_t l = strlen(out);
    if (rc == 0) snprintf(out + l, cap - l, "\n[syntax check: ok]");
    else if (rc < 124) {
        const char *t = tail_lines(o, n, 8, 700);
        snprintf(out + l, cap - l, "\n[syntax check FAILED%s]\n%.*s", partial ? "; if more parts are still to be appended, finish them, otherwise fix this now"
                 : ": fix this now with edit_file before doing anything else", (int)(n - (size_t)(t - o) > 700 ? 700 : n - (size_t)(t - o)), t);
    }
    free(o);
#else
    (void)path; (void)partial; (void)out; (void)cap;
#endif
}
