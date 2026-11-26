/* katana agentlog [arquivo] [--days N]: le o registro de acoes do agente (~/.katana/agente.log, ou KATANA_LOG) e mostra,
 * por ferramenta, chamadas, falhas e chamadas identicas repetidas; as mensagens de erro mais frequentes (70 primeiros
 * caracteres) e as sequencias mais longas de falhas seguidas, com a hora. Uma linha do registro: data, ferramenta,
 * argumentos, resultado, separados por tabulacao. Os dois conjuntos de ferramentas aparecem com seus proprios nomes
 * (o segundo grava os argumentos como "arguments": {...}). Uma linha que nao comeca com data continua o registro
 * anterior; tabulacoes depois da terceira ficam no resultado. */

/* a linha comeca com "AAAA-MM-DD HH:MM:SS\t"? */
static int al_is_rec(const char *p) {
    for (int i = 0; i < 19; i++) {
        const char c = p[i];
        if (i == 4 || i == 7) { if (c != '-') return 0; }
        else if (i == 10) { if (c != ' ') return 0; }
        else if (i == 13 || i == 16) { if (c != ':') return 0; }
        else if (c < '0' || c > '9') return 0;
    }
    return p[19] == '\t';
}
/* o resultado e de uma chamada que falhou? */
static int al_failed(const char *r) {
    while (*r == ' ') r++;
    if (!strncasecmp(r, "error", 5) || !strncmp(r, "Traceback", 9)) return 1;
    for (const char *e = strstr(r, "xit code "); e; e = strstr(e + 1, "xit code ")) if (e[9] >= '1' && e[9] <= '9') return 1;
    return 0;
}
typedef struct { char name[64]; int calls, fails, reps; } AlTool;
typedef struct { char tool[64]; char msg[72]; int n; } AlErr;
typedef struct { char t0[20], t1[20]; int n; char tools[160]; } AlRun;

static void al_run_close(AlRun *top, int ntop, const AlRun *cur) {
    if (cur->n < 2) return;
    int k = -1;
    for (int i = 0; i < ntop; i++) if (k < 0 ? top[i].n < cur->n : top[i].n < top[k].n) k = i;
    if (k >= 0 && top[k].n < cur->n) top[k] = *cur;
}

static int agentlog_main(int argc, char **argv) {
    char path[1200] = ""; double days = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--days") && i + 1 < argc) days = atof(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: katana agentlog [file] [--days N]\n"); return 2; }
        else snprintf(path, sizeof path, "%s", argv[i]);
    }
    if (!path[0]) {
        const char *e = getenv("KATANA_LOG");
        if (e && *e) snprintf(path, sizeof path, "%s", e);
        else { char h[1024]; agent_home(h, sizeof h); snprintf(path, sizeof path, "%s/.katana/agente.log", h); }
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "agentlog: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 1; }
    const size_t got = fread(buf, 1, (size_t)sz, f); buf[got] = 0; fclose(f);
    char cut[20] = "";
    if (days > 0) { const time_t t = time(NULL) - (time_t)(days * 86400.0); strftime(cut, sizeof cut, "%Y-%m-%d %H:%M:%S", localtime(&t)); }

    /* registros: comeco de cada um (linha com data); as linhas seguintes sem data pertencem a ele */
    int nr = 0, cap = 1024; char **rec = malloc(sizeof(char *) * (size_t)cap);
    for (char *p = buf; *p; ) {
        char *nl = strchr(p, '\n');
        if (al_is_rec(p)) {
            if (nr == cap) { cap *= 2; rec = realloc(rec, sizeof(char *) * (size_t)cap); }
            rec[nr++] = p;
            if (nl && nl > p && nl[-1] == '\r') nl[-1] = ' ';
        } else if (nr && p > buf) p[-1] = ' ';      /* continuacao: a quebra vira espaco */
        if (!nl) break;
        p = nl + 1;
    }
    /* fecha cada registro no comeco do proximo */
    for (int i = 1; i < nr; i++) rec[i][-1] = 0;

    static AlTool tl[128]; int nt = 0;
    static AlErr er[1024]; int ne = 0;
    AlRun top[5]; memset(top, 0, sizeof top);
    AlRun cur; memset(&cur, 0, sizeof cur);
    uint64_t last[20] = {0}; int nl20 = 0;
    int used = 0, fails = 0, reps = 0;
    long tn = 0, tnew = 0, tre = 0, tgt = 0; double tps = 0, tgs = 0;      /* turnos do modelo */
    char first[20] = "", lastt[20] = "";
    for (int i = 0; i < nr; i++) {
        char *ts = rec[i];
        if (cut[0] && strncmp(ts, cut, 19) < 0) continue;
        char *t1 = strchr(ts, '\t'); if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        char *t3 = strchr(t2 + 1, '\t');
        const char *res = t3 ? t3 + 1 : "";
        char name[64]; snprintf(name, sizeof name, "%.*s", (int)(t2 - t1 - 1 < 63 ? t2 - t1 - 1 : 63), t1 + 1);
        if (name[0] == '[') {      /* linha de turno do modelo ("[turn]"): soma o prefill, nao conta como ferramenta */
            int nw = 0, ru = 0, gt = 0; double ps = 0, gs = 0;
            const char *ar = t2 + 1;
            const char *q;
            if ((q = strstr(ar, "new="))) nw = atoi(q + 4);
            if ((q = strstr(ar, "reused="))) ru = atoi(q + 7);
            if ((q = strstr(ar, "prefill_s="))) ps = atof(q + 10);
            if ((q = strstr(ar, "gen_s="))) gs = atof(q + 6);
            if ((q = strstr(ar, "gen_tok="))) gt = atoi(q + 8);
            tn++; tnew += nw; tre += ru; tps += ps; tgs += gs; tgt += gt;
            continue;
        }
        const size_t al = (size_t)((t3 ? t3 : t2 + strlen(t2)) - (t2 + 1));
        uint64_t h = 1469598103934665603ULL;
        for (const char *q = name; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211ULL; }
        h ^= 9; h *= 1099511628211ULL;
        for (size_t k = 0; k < al; k++) { h ^= (unsigned char)t2[1 + k]; h *= 1099511628211ULL; }
        int rep = 0;
        for (int k = 0; k < nl20 && k < 20; k++) if (last[k] == h) { rep = 1; break; }
        last[nl20 % 20] = h; nl20++;
        const int fl = al_failed(res);
        int ti = -1;
        for (int k = 0; k < nt; k++) if (!strcmp(tl[k].name, name)) { ti = k; break; }
        if (ti < 0 && nt < 128) { ti = nt++; snprintf(tl[ti].name, sizeof tl[ti].name, "%s", name); }
        if (ti >= 0) { tl[ti].calls++; tl[ti].fails += fl; tl[ti].reps += rep; }
        used++; fails += fl; reps += rep;
        if (!first[0]) snprintf(first, sizeof first, "%.19s", ts);
        snprintf(lastt, sizeof lastt, "%.19s", ts);
        if (fl) {
            char msg[72]; size_t k = 0;
            const char *q = res; while (*q == ' ') q++;
            for (; *q && *q != '\n' && *q != '\r' && k < 70; q++) msg[k++] = (*q == '\t') ? ' ' : *q;
            msg[k] = 0;
            int ei = -1;
            for (int j = 0; j < ne; j++) if (!strcmp(er[j].tool, name) && !strcmp(er[j].msg, msg)) { ei = j; break; }
            if (ei < 0 && ne < 1024) { ei = ne++; snprintf(er[ei].tool, sizeof er[ei].tool, "%s", name); snprintf(er[ei].msg, sizeof er[ei].msg, "%s", msg); er[ei].n = 0; }
            if (ei >= 0) er[ei].n++;
            if (!cur.n) { snprintf(cur.t0, sizeof cur.t0, "%.19s", ts); cur.tools[0] = 0; }
            snprintf(cur.t1, sizeof cur.t1, "%.19s", ts);
            cur.n++;
            if (!strstr(cur.tools, name) && strlen(cur.tools) + strlen(name) + 3 < sizeof cur.tools) { if (cur.tools[0]) strcat(cur.tools, ", "); strcat(cur.tools, name); }
        } else { al_run_close(top, 5, &cur); cur.n = 0; }
    }
    al_run_close(top, 5, &cur);

    printf("agent log %s: %d calls", path, used);
    if (used) printf(" from %s to %s", first, lastt);
    printf(", %d failed (%.0f%%), %d identical repeats\n\n", fails, used ? 100.0 * fails / used : 0.0, reps);
    if (tn) printf("model turns: %ld, prompt tokens processed %ld, reused from the KV cache %ld (%.0f%%), prefill %.1f s (%.1f tok/s), generation %.1f s for %ld tokens (%.1f tok/s)\n\n",
                   tn, tnew, tre, tnew + tre ? 100.0 * (double)tre / (double)(tnew + tre) : 0.0, tps, tps > 0 ? (double)tnew / tps : 0.0, tgs, tgt, tgs > 0 ? (double)tgt / tgs : 0.0);
    if (!used) { free(rec); free(buf); return 0; }
    /* por ferramenta, mais chamadas primeiro */
    for (int a = 0; a < nt; a++) for (int b = a + 1; b < nt; b++) if (tl[b].calls > tl[a].calls) { AlTool x = tl[a]; tl[a] = tl[b]; tl[b] = x; }
    printf("%-26s %7s %7s %6s %8s\n", "tool", "calls", "failed", "fail%", "repeats");
    for (int a = 0; a < nt; a++)
        printf("%-26s %7d %7d %5.0f%% %8d\n", tl[a].name, tl[a].calls, tl[a].fails, 100.0 * tl[a].fails / tl[a].calls, tl[a].reps);
    for (int a = 0; a < ne; a++) for (int b = a + 1; b < ne; b++) if (er[b].n > er[a].n) { AlErr x = er[a]; er[a] = er[b]; er[b] = x; }
    printf("\nmost frequent errors (first 70 characters):\n");
    for (int a = 0; a < ne && a < 12; a++) printf("%5d  %-18s %s\n", er[a].n, er[a].tool, er[a].msg);
    for (int a = 0; a < 5; a++) for (int b = a + 1; b < 5; b++) if (top[b].n > top[a].n) { AlRun x = top[a]; top[a] = top[b]; top[b] = x; }
    printf("\nlongest runs of consecutive failures:\n");
    int any = 0;
    for (int a = 0; a < 5; a++) if (top[a].n) { any = 1; printf("%5d  %s to %s  %s\n", top[a].n, top[a].t0, top[a].t1 + 11, top[a].tools); }
    if (!any) printf("  none (no two failures in a row)\n");
    free(rec); free(buf);
    return 0;
}
