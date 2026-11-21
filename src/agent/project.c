/* ---------- instrucoes do projeto, em camadas ----------
 * Documentos escritos pelo dono para o modelo seguir, do mais geral ao mais especifico:
 *   1. global:   ~/.katana/KATANA.md                       (vale para todos os projetos)
 *   2. projeto:  KATANA.md ou AGENTS.md na pasta de trabalho ou acima, ate a raiz do repositorio
 *   3. local:    KATANA.local.md ao lado do arquivo do projeto (fica fora do controle de versao)
 * Uma linha "@caminho/arquivo.md" dentro de um deles inclui aquele arquivo (relativo ao arquivo que o cita, ate 2 niveis).
 * Entram no prompt de sistema depois do bloco de ambiente (fora da parte estavel do cache de prefixo), com um limite
 * total (KATANA_INSTR_MAX, padrao 5000 bytes); se passar, o mais especifico tem prioridade.
 * Um KATANA.md numa subpasta chega uma vez, junto do resultado de Read/Edit/Write de um arquivo dela (proj_sub_note). */
#define PROJ_MAX 60000          /* tamanho dos buffers dos chamadores; o limite de verdade e proj_max() */
#define PROJ_FILE_MAX 60000     /* maximo lido de um arquivo (com as inclusoes) antes de repartir o limite */
static size_t proj_max(void) {
    const char *e = getenv("KATANA_INSTR_MAX");
    const long v = e && *e ? atol(e) : 0;
    return v >= 200 && v <= PROJ_MAX ? (size_t)v : 5000;
}
static void proj_sub_reset(void);
static void sk_env_line(char *out, size_t cap);     /* tools/skill.c: linha das skills do bloco de ambiente */

typedef struct { char *p; size_t n, cap; } IBuf;
static void ib_put(IBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 256; b->p = (char *)realloc(b->p, b->cap); if (!b->p) die("sem memoria"); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
/* caminho de uma inclusao: ~/ vira a pasta pessoal, absoluto fica, o resto e relativo a pasta do arquivo que a cita */
static void instr_resolve(const char *from, const char *inc, char *out, size_t cap) {
    char home[1024];
    if (inc[0] == '~' && inc[1] == '/') { agent_home(home, sizeof home); snprintf(out, cap, "%s%s", home, inc + 1); return; }
    if (inc[0] == '/') { snprintf(out, cap, "%s", inc); return; }
    char d[2200]; snprintf(d, sizeof d, "%s", from);
    char *sl = strrchr(d, '/');
    if (sl) *sl = 0; else snprintf(d, sizeof d, ".");
    snprintf(out, cap, "%s/%s", d, inc);
}
/* texto do arquivo com as inclusoes expandidas, em memoria nova; NULL se nao abrir, tiver byte zero ou ficar vazio */
static char *instr_read(const char *path, int depth) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char *raw = (char *)malloc(PROJ_FILE_MAX + 1);
    const size_t rn = fread(raw, 1, PROJ_FILE_MAX, fp);
    fclose(fp);
    raw[rn] = 0;
    if (memchr(raw, 0, rn)) { free(raw); return NULL; }
    IBuf o = {0};
    ib_put(&o, "", 0);
    for (const char *l = raw; *l && o.n < PROJ_FILE_MAX;) {
        const size_t len = strcspn(l, "\n");
        const char *nx = l[len] ? l + len + 1 : l + len;
        size_t a = 0, e = len;
        while (a < e && (l[a] == ' ' || l[a] == '\t')) a++;
        while (e > a && (l[e - 1] == ' ' || l[e - 1] == '\t' || l[e - 1] == '\r')) e--;
        int done = 0;
        if (depth < 0 && e - a > 2     /* @include foi trocado por docs: (formato brief); o codigo fica desligado */ && l[a] == '@' && l[a + 1] != '@' && l[a + 1] != ' ') {
            char inc[1100], res[2400];
            const size_t il = e - a - 1;
            if (il < sizeof inc && (memchr(l + a + 1, '/', il) || memchr(l + a + 1, '.', il))) {
                memcpy(inc, l + a + 1, il); inc[il] = 0;
                instr_resolve(path, inc, res, sizeof res);
                char *sub = instr_read(res, depth + 1);
                if (sub) { ib_put(&o, sub, strlen(sub)); ib_put(&o, "\n", 1); free(sub); done = 1; }
            }
        }
        if (!done) { ib_put(&o, l, len); ib_put(&o, "\n", 1); }
        l = nx;
    }
    free(raw);
    while (o.n && (o.p[o.n - 1] == '\n' || o.p[o.n - 1] == ' ' || o.p[o.n - 1] == '\r')) o.p[--o.n] = 0;
    if (!o.n) { free(o.p); return NULL; }
    return o.p;
}
static int proj_isfile(const char *f) { struct stat st; return stat(f, &st) == 0 && S_ISREG(st.st_mode); }
/* o arquivo do projeto numa pasta (KATANA.md antes de AGENTS.md); 0 se nao houver */
static int proj_file_in(const char *d, char *out, size_t cap) {
    static const char *names[] = {"KATANA.md", "AGENTS.md", NULL};
    for (int i = 0; names[i]; i++) { snprintf(out, cap, "%s/%s", d, names[i]); if (proj_isfile(out)) return 1; }
    return 0;
}
/* pasta onde fica o projeto: a primeira, subindo da pasta de trabalho, com KATANA.md, AGENTS.md ou KATANA.local.md
   (para na raiz do repositorio ou na pasta pessoal); 0 se nao houver */
static int proj_dir(char *d, size_t cap) {
    char home[1024], f[4400], g[4400]; struct stat st;
    if (!getcwd(d, cap)) return 0;
    agent_home(home, sizeof home);
    for (int up = 0; up < 12; up++) {
        snprintf(f, sizeof f, "%s/KATANA.local.md", d);
        if (proj_file_in(d, g, sizeof g) || proj_isfile(f)) return 1;
        snprintf(g, sizeof g, "%s/.git", d);
        if (stat(g, &st) == 0 || strcmp(d, home) == 0) break;     /* raiz do repositorio ou a pasta pessoal: nao sobe mais */
        char *sl = strrchr(d, '/');
        if (!sl || sl == d) break;
        *sl = 0;
    }
    return 0;
}
typedef struct { char path[4400]; const char *label; char *txt; size_t n; } InstrDoc;
/* as camadas existentes, do mais geral ao mais especifico; devolve quantas (ate 3) */
static int instr_layers(InstrDoc *dv) {
    int n = 0;
    char home[1024], f[4400], d[4200];
    agent_home(home, sizeof home);
    snprintf(f, sizeof f, "%s/.katana/KATANA.md", home);
    char *t = instr_read(f, 0);
    if (t) { snprintf(dv[n].path, sizeof dv[n].path, "%s", f); dv[n].label = "global"; dv[n].txt = t; dv[n].n = strlen(t); n++; }
    if (proj_dir(d, sizeof d)) {
        if (proj_file_in(d, f, sizeof f) && (t = instr_read(f, 0))) { snprintf(dv[n].path, sizeof dv[n].path, "%s", f); dv[n].label = "project"; dv[n].txt = t; dv[n].n = strlen(t); n++; }
        snprintf(f, sizeof f, "%s/KATANA.local.md", d);
        if ((t = instr_read(f, 0))) { snprintf(dv[n].path, sizeof dv[n].path, "%s", f); dv[n].label = "local"; dv[n].txt = t; dv[n].n = strlen(t); n++; }
    }
    return n;
}
/* instrucoes para o prompt: out recebe o texto (ate o limite total e cap-100), path recebe os arquivos usados, separados por ", " */
static size_t proj_instr(char *out, size_t cap, char *path, size_t pcap) {
    InstrDoc dv[3];
    out[0] = 0; if (pcap) path[0] = 0;
    const int n = instr_layers(dv);
    if (!n) return 0;
    size_t total = proj_max();
    if (cap < total + 100) total = cap > 300 ? cap - 100 : 200;
    size_t room[3], left = total;
    for (int i = n - 1; i >= 0; i--) {      /* o mais especifico escolhe primeiro */
        const size_t head = n > 1 ? strlen(dv[i].label) + strlen(dv[i].path) + 8 : 0;
        const size_t want = dv[i].n + head;
        room[i] = want <= left ? want : left > head + 60 ? left : 0;
        left -= room[i];
    }
    size_t o = 0; int used = 0;
    for (int i = 0; i < n; i++) {
        if (room[i]) {
            if (n > 1) o += (size_t)snprintf(out + o, cap - o, "%s## %s (%s)\n", used ? "\n\n" : "", dv[i].label, dv[i].path);
            else if (used) o += (size_t)snprintf(out + o, cap - o, "\n\n");
            size_t head = n > 1 ? strlen(dv[i].label) + strlen(dv[i].path) + 8 : 0, take = dv[i].n;
            if (take + head > room[i]) take = room[i] > head ? room[i] - head : 0;
            memcpy(out + o, dv[i].txt, take); o += take; out[o] = 0;
            if (take < dv[i].n) {
                while (o && (out[o - 1] != '\n') && take > 0) { o--; take--; }       /* corta na ultima linha inteira */
                out[o] = 0;
                o += (size_t)snprintf(out + o, cap - o, "\n[... the file continues: read %s for the rest]", dv[i].path);
            }
            if (pcap) { const size_t pl = strlen(path); snprintf(path + pl, pcap - pl, "%s%s", pl ? ", " : "", dv[i].path); }
            if (o >= cap) o = cap - 1;
            used++;
        }
        free(dv[i].txt);
    }
    return o;
}

/* ---- KATANA.md de subpasta: entregue uma vez com o resultado de Read/Edit/Write de um arquivo dela ---- */
static char g_sub_done[64][1100];
static int g_nsub_done;
static void proj_sub_reset(void) { g_nsub_done = 0; }
static int proj_under(const char *cwd, const char *d) {     /* d e a pasta de trabalho ou esta acima dela */
    const size_t n = strlen(d);
    if (strncmp(cwd, d, n) != 0) return 0;
    return cwd[n] == 0 || cwd[n] == '/' || n == 1;
}
/* acrescenta a out (ate cap) as instrucoes das pastas entre o arquivo e a raiz do repositorio que ainda nao foram entregues e
   que nao estao no prompt (a pasta de trabalho e as de cima ja estao). Nada acontece se faltar lugar. */
static void proj_sub_note(const char *file, char *out, size_t cap) {
    char cwd[4200], rc[4200], d[4200], home[1024], real[4200];
    if (!file || !*file || !getcwd(cwd, sizeof cwd) || !realpath(cwd, rc)) return;
    agent_home(home, sizeof home);
    if (file[0] == '/') snprintf(d, sizeof d, "%s", file); else snprintf(d, sizeof d, "%s/%s", rc, file);
    for (int k = 0; k < 40; k++) {      /* a pasta do arquivo, ou a mais proxima que existe */
        char *sl = strrchr(d, '/');
        if (!sl) return;
        if (sl == d) { d[1] = 0; } else *sl = 0;
        if (realpath(d, real)) break;
        if (d[1] == 0) return;
    }
    if (!realpath(d, real)) return;
    snprintf(d, sizeof d, "%s", real);
    char dirs[12][4200]; int nd = 0;
    for (int up = 0; up < 12; up++) {
        char g[4400]; struct stat st;
        if (!proj_under(rc, d)) snprintf(dirs[nd++], sizeof dirs[0], "%s", d);
        snprintf(g, sizeof g, "%s/.git", d);
        if (stat(g, &st) == 0 || strcmp(d, home) == 0 || strcmp(d, "/") == 0) break;
        char *sl = strrchr(d, '/');
        if (!sl || sl == d) break;
        *sl = 0;
    }
    size_t o = strlen(out);
    for (int i = nd - 1; i >= 0; i--) {         /* do mais externo ao mais interno */
        char f[4400];
        int dup = 0;
        for (int k = 0; k < g_nsub_done; k++) if (strcmp(g_sub_done[k], dirs[i]) == 0) dup = 1;
        if (dup || !proj_file_in(dirs[i], f, sizeof f)) continue;
        char *t = instr_read(f, 0);
        if (!t) continue;
        size_t tl = strlen(t);
        if (tl > 3000) tl = 3000;
        const size_t need = tl + strlen(f) + 140;
        if (o + need >= cap) { free(t); continue; }         /* sem lugar: fica para a proxima chamada */
        if (g_nsub_done < 64) snprintf(g_sub_done[g_nsub_done++], sizeof g_sub_done[0], "%s", dirs[i]);
        o += (size_t)snprintf(out + o, cap - o, "\n\n[Instructions for the folder %s (file %s, written by the owner of this project: follow them while you work there)]\n%.*s%s",
                              dirs[i], f, (int)tl, t, tl < strlen(t) ? "\n[... truncated: read the file for the rest]" : "");
        free(t);
    }
}
