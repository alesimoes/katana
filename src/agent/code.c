/* ---------- camada de codigo: leitura por trechos e verificacao de sintaxe ----------
 * read_file devolve o arquivo inteiro quando cabe, ou um trecho de linhas inteiras com o aviso de quais linhas
 * vieram e como pedir as proximas (offset/limit). Depois de write_file e edit_file o motor confere a sintaxe do
 * arquivo (python, shell, json, javascript, php, ruby) e junta o resultado ao retorno da ferramenta: o modelo ve
 * o erro na hora, sem gastar uma rodada para descobrir. */
#define READ_MAX 6000       /* bytes de um trecho de read_file */

static int json_int(const char *js, const char *end, const char *key, int def);

/* ---- leitura com posicao guardada ----
 * O motor lembra, por arquivo, ate que linha o agente ja leu nesta conversa: read_file so com o caminho continua de
 * onde parou (com offset ou limit devolve as linhas exatas pedidas, sem mexer na posicao). Cada trecho lido em
 * sequencia pode ganhar uma nota curta (tool_digest, em delegate.c), presa ao trecho: as notas ficam guardadas
 * enquanto o arquivo nao muda (tamanho e data). Pedir de novo um arquivo ja lido inteiro devolve as notas na hora;
 * continuar a leitura depois de uma compactacao do contexto traz junto as notas dos trechos anteriores. */
#define RD_SLOTS 48
#define RD_CHUNKS 64
typedef struct { int from, to, ep; char *note; } RdChunk;
typedef struct { char path[4200]; long long size, mt; int next, nch; unsigned long use; RdChunk ch[RD_CHUNKS]; } RdFile;
static RdFile g_rd[RD_SLOTS];
static int g_rd_n, g_rd_epoch;                  /* epoch: sobe a cada compactacao (o que estava no contexto pode ter saido) */
static unsigned long g_rd_clock;
static struct { RdFile *f; int from, to, seq, notes; } g_rd_last;      /* o que a ultima read_file fez (para a nota) */
static char g_rd_pre[12000];                    /* notas dos trechos anteriores, que entram antes do resultado */
static int g_in_sub;

static void rd_drop(RdFile *f) { for (int i = 0; i < f->nch; i++) free(f->ch[i].note); f->nch = 0; f->next = 1; }
static void rd_reset(void) { for (int i = 0; i < g_rd_n; i++) rd_drop(&g_rd[i]); g_rd_n = 0; g_rd_last.f = NULL; g_rd_pre[0] = 0; }
static RdFile *rd_get(const char *path, long long size, long long mt) {
    RdFile *f = NULL;
    for (int i = 0; i < g_rd_n && !f; i++) if (strcmp(g_rd[i].path, path) == 0) f = &g_rd[i];
    if (f && (f->size != size || f->mt != mt)) rd_drop(f);             /* o arquivo mudou: nada do que foi lido vale */
    if (!f) {
        if (g_rd_n < RD_SLOTS) f = &g_rd[g_rd_n++];
        else { f = &g_rd[0]; for (int i = 1; i < g_rd_n; i++) if (g_rd[i].use < f->use) f = &g_rd[i]; rd_drop(f); }
        f->nch = 0; f->next = 1;
        snprintf(f->path, sizeof f->path, "%s", path);
    }
    f->size = size; f->mt = mt; f->use = ++g_rd_clock;
    return f;
}
/* 1 se os trechos lidos em sequencia cobrem as linhas 1..total e todos tem nota */
static int rd_all_notes(const RdFile *f, int total) {
    int at = 1;
    for (int i = 0; i < f->nch; i++) { if (!f->ch[i].note || f->ch[i].from != at) return 0; at = f->ch[i].to + 1; }
    return f->nch > 0 && at > total;
}
/* notas dos trechos [0, upto) em out; so_old: so as que entraram no contexto antes da ultima compactacao */
static size_t rd_notes(RdFile *f, int upto, int only_old, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < upto && i < f->nch; i++) {
        if (!f->ch[i].note || (only_old && f->ch[i].ep == g_rd_epoch)) continue;
        const size_t need = strlen(f->ch[i].note) + 64;
        if (o + need >= cap) break;
        o += (size_t)snprintf(out + o, cap - o, "[lines %d-%d]\n%s\n", f->ch[i].from, f->ch[i].to, f->ch[i].note);
        f->ch[i].ep = g_rd_epoch;
    }
    return o;
}

static void tool_read_file(const char *js, const char *end, char *out, size_t cap) {
    static char arg[8192];
    char res[4200], err[1300];
    if (!json_str(js, end, "path", arg, sizeof arg)) { snprintf(out, cap, "error: missing path"); return; }
    if (!agent_read_access(arg, 0, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    {   /* PDF e HTML: texto pelo leitor de documentos (read_document saiu do prompt) */
        const char *x = strrchr(res, '.');
        if (x && (ci_ncmp(x, ".pdf", 5) == 0 || ci_ncmp(x, ".html", 6) == 0 || ci_ncmp(x, ".htm", 5) == 0)) { tool_read_document(arg, out, cap); return; }
    }
    int off = json_int(js, end, "offset", 1);
    const int lim = json_int(js, end, "limit", 0);
    const int seq = !g_in_sub && json_int(js, end, "offset", -7) == -7 && json_int(js, end, "limit", -7) == -7;     /* so o caminho: leitura em sequencia */
    g_rd_last.f = NULL; g_rd_last.notes = 0; g_rd_pre[0] = 0;
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
    RdFile *rf = NULL;
    if (!g_in_sub) { struct stat st; if (stat(res, &st) == 0) rf = rd_get(res, (long long)st.st_size, (long long)st.st_mtime); }
    if (seq && rf && rf->next > 1) {
        if (rf->next > total) {             /* arquivo inteiro ja lido e sem mudanca */
            if (rd_all_notes(rf, total)) {
                const size_t h = (size_t)snprintf(out, cap, "[katana: you already read this whole file (%d lines) and it has not changed. These are the notes kept from that reading. "
                                                  "For exact lines call read_file with offset and limit; to find something in it use search_files.]\n", total);
                rd_notes(rf, rf->nch, 0, out + h, cap > h ? cap - h : 0);
                g_rd_last.notes = 1;
                free(b);
                return;
            }
            rd_drop(rf);                    /* sem notas guardadas: le de novo do comeco */
        } else {
            off = rf->next;                 /* continua de onde parou */
            rd_notes(rf, rf->nch, 1, g_rd_pre, sizeof g_rd_pre);
        }
    }
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
    const int last = off + shown - 1;
    if (off > 1 || e < n) {
        if (e < n && seq && rf) snprintf(out + (e - s), cap - (e - s), "\n[lines %d-%d of %d shown; the position is kept: call read_file again with only the path to continue from line %d]", off, last, total, last + 1);
        else if (e < n) snprintf(out + (e - s), cap - (e - s), "\n[lines %d-%d of %d shown; for the next ones call read_file with offset=%d]", off, last, total, last + 1);
        else snprintf(out + (e - s), cap - (e - s), "\n[lines %d-%d of %d shown (end of file)]", off, last, total);
    }
    if (rf && seq) {                        /* trecho lido em sequencia: guarda a posicao e abre a vaga da nota */
        if (rf->nch < RD_CHUNKS) { rf->ch[rf->nch].from = off; rf->ch[rf->nch].to = last; rf->ch[rf->nch].ep = g_rd_epoch; rf->ch[rf->nch].note = NULL; rf->nch++; }
        rf->next = last + 1;
        g_rd_last.f = rf; g_rd_last.from = off; g_rd_last.to = last; g_rd_last.seq = 1;
    } else if (rf && off <= rf->next && last + 1 > rf->next) rf->next = last + 1;      /* trecho pedido por offset que emenda no ja lido */
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
                 : ": fix this now with an edit of the file before doing anything else", (int)(n - (size_t)(t - o) > 700 ? 700 : n - (size_t)(t - o)), t);
    }
    free(o);
#else
    (void)path; (void)partial; (void)out; (void)cap;
#endif
}
