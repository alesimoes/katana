/* ---------- coder/ignore.c: o que o indice deixa de fora, e por que (fase 8 de docs/code-index-lifecycle.md, D.10 e F.4) ----------
 * Tres tabelas e um arquivo, tudo aqui, para que a varredura sincrona (storedocs.c) e a do rastreador usem as mesmas regras:
 *   1. a lista interna UNICA de pastas pesadas (substitui st_ignored_dir antiga, a lista sem .gitignore de st_scan_walk e fd_skip_rel do Find),
 *      cada grupo com a justificativa; `vendor`, `ref`, `models`, `modelos`, `bin` e `lib` NAO estao nela;
 *   2. as pastas que nunca entram nessa lista por padrao (so uma regra do usuario as esconde) e o aviso "usually worth indexing";
 *   3. a tabela de segredos: nomes de arquivo cujo conteudo nunca e lido (catalogados so pelo nome, motivo "secret");
 *   4. o arquivo `overrides` ao lado do store (<raiz>/.katana/coder/overrides para pasta com init, ~/.katana/coder/<chave>/overrides para raiz da
 *      home): linhas `include <caminho>`, `exclude <caminho>` e `scoped <caminho>`, caminhos relativos a raiz. Uma pasta sem init nunca ganha
 *      arquivo dentro dela. Os overrides vencem qualquer arquivo de regras.
 * Precedencia na varredura: overrides > .katanaignore > .gitignore (regra mais funda, ultima linha) > lista interna > nada. `!padrao` num arquivo de
 * regras reinclui o que a lista interna ignoraria. O grupo "controle de versao e dados do katana" nunca entra, nem por override.
 * Quem usa de fora (fase 6, ferramenta Symbols tree e frase do NOT FOUND): ign_reg_load/ign_reg_largest em maint.c. */
enum { IGR_GITIGNORE = 1, IGR_KATANAIGNORE, IGR_HEAVY, IGR_EXCLUDED, IGR_BINARY, IGR_TOOBIG, IGR_SECRET, IGR_NESTED, IGR_CHECKOUT, IGR_N };
static const char *const g_ign_rname[IGR_N] = {"", "gitignore", "katanaignore", "heavy-folder", "excluded", "binary", "too-big", "secret", "nested-root", "separate-checkout"};

enum { IGG_VCS = 0, IGG_AGENT, IGG_DEPS, IGG_BUILD, IGG_CACHE, IGG_SECRET, IGG_N };
static const char *const g_ign_gwhy[IGG_N] = {
    "version control and katana's own data: not project content (object stores, and the index itself)",
    "agent work areas: whole copies of the project, every symbol duplicated",
    "installed dependencies: third-party code fetched by a tool, 10 to 1000 times the size of the project; floods results",
    "build outputs: derived from sources that are already indexed; duplicates and minified text",
    "tool caches and editor state: machine-generated, no code",
    "folders of keys and credentials: never read"};
typedef struct { const char *name; int group; } IgnHeavy;
static const IgnHeavy g_ign_heavy[] = {
    {".git", IGG_VCS}, {".hg", IGG_VCS}, {".svn", IGG_VCS}, {".katana", IGG_VCS},
    {".claude", IGG_AGENT},
    {"node_modules", IGG_DEPS}, {".venv", IGG_DEPS}, {"venv", IGG_DEPS}, {"site-packages", IGG_DEPS}, {"bower_components", IGG_DEPS}, {".gradle", IGG_DEPS}, {".m2", IGG_DEPS}, {".cargo", IGG_DEPS},
    {"build", IGG_BUILD}, {"dist", IGG_BUILD}, {"out", IGG_BUILD}, {"target", IGG_BUILD}, {"obj", IGG_BUILD}, {".next", IGG_BUILD}, {".nuxt", IGG_BUILD}, {"__pycache__", IGG_BUILD},
    {".pytest_cache", IGG_BUILD}, {".mypy_cache", IGG_BUILD}, {".tox", IGG_BUILD}, {"coverage", IGG_BUILD},
    {".cache", IGG_CACHE}, {".idea", IGG_CACHE}, {".vs", IGG_CACHE}, {".parcel-cache", IGG_CACHE}, {".terraform", IGG_CACHE},
    {".ssh", IGG_SECRET}, {".gnupg", IGG_SECRET}, {".aws", IGG_SECRET},
    {NULL, 0}};
static const IgnHeavy *ign_heavy(const char *nm) { for (int i = 0; g_ign_heavy[i].name; i++) if (!strcmp(nm, g_ign_heavy[i].name)) return &g_ign_heavy[i]; return NULL; }
static int ign_group_includable(int g) { return g != IGG_VCS && g != IGG_SECRET; }
/* pastas que a lista interna jamais contem; so uma regra do usuario as esconde (a saida de `ignored` as marca "usually worth indexing") */
static int ign_never_name(const char *nm) {
    static const char *const nv[] = {"docs", "doc", "test", "tests", "spec", "examples", "samples", "scripts", "tools", "src", "lib", "include", "config", ".github", ".gitlab", "migrations", NULL};
    for (int i = 0; nv[i]; i++) if (!strcmp(nm, nv[i])) return 1;
    return 0;
}
/* o primeiro componente de rel (pasta) que e dessa lista, ou NULL (buffer estatico) */
static const char *ign_never_path(const char *rel) {
    static char c[256]; const char *p = rel;
    while (*p) { size_t n = strcspn(p, "/"); if (n && n < sizeof c) { memcpy(c, p, n); c[n] = 0; if (ign_never_name(c)) return c; } p += n; if (*p == '/') p++; }
    return NULL;
}

/* ---- tabela de segredos: casa o ULTIMO componente (sem diferenciar maiusculas); devolve o padrao da regra ou NULL ---- */
static int ign_ends(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
static const char *ign_secret_name(const char *path) {
    const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
    if (!strncasecmp(nm, ".env", 4) && (!nm[4] || nm[4] == '.')) {
        if (ign_ends(nm, ".example") || ign_ends(nm, ".sample") || ign_ends(nm, ".template")) return NULL;
        return ".env*";
    }
    static const char *const sfx[] = {".pem", ".key", ".p12", ".pfx", ".jks", ".keystore", ".kdbx", ".tfstate", ".tfvars", ".secret", NULL};
    for (int i = 0; sfx[i]; i++) if (ign_ends(nm, sfx[i])) { static char r[8][16]; snprintf(r[i & 7], sizeof r[0], "*%s", sfx[i]); return r[i & 7]; }
    static const char *const pre[] = {"id_rsa", "id_dsa", "id_ecdsa", "id_ed25519", NULL};
    for (int i = 0; pre[i]; i++) if (!strncasecmp(nm, pre[i], strlen(pre[i]))) { if (ign_ends(nm, ".pub")) return NULL; static char r[4][16]; snprintf(r[i], sizeof r[0], "%s*", pre[i]); return r[i]; }
    static const char *const ex[] = {".netrc", ".npmrc", ".pypirc", ".htpasswd", "credentials", NULL};
    for (int i = 0; ex[i]; i++) if (!strcasecmp(nm, ex[i])) return ex[i];
    if (!strncasecmp(nm, "credentials.", 12)) return "credentials.*";
    if (!strncasecmp(nm, "secrets.", 8)) return "secrets.*";
    for (const char *p = path; *p; ) {                                  /* qualquer coisa debaixo de .ssh/, .gnupg/, .aws/ */
        size_t n = strcspn(p, "/"); if (p[n] != '/') break;
        if ((n == 4 && !strncmp(p, ".ssh", 4)) || (n == 6 && !strncmp(p, ".gnupg", 6)) || (n == 4 && !strncmp(p, ".aws", 4))) return "under .ssh/.gnupg/.aws";
        p += n + 1;
    }
    return NULL;
}
/* camada 2: chave privada reconhecida pelo comeco do conteudo ("-----BEGIN" ... "PRIVATE KEY" nos primeiros 200 bytes) */
static int ign_secret_sniff(const uint8_t *b, size_t n) {
    if (n > 200) n = 200;
    for (size_t i = 0; i + 10 <= n; i++) if (b[i] == '-' && !memcmp(b + i, "-----BEGIN", 10)) {
        for (size_t j = i + 10; j + 11 <= n; j++) if (b[j] == 'P' && !memcmp(b + j, "PRIVATE KEY", 11)) return 1;
        return 0; }
    return 0;
}

static void ign_hide_in_repo(const char *storedir);
/* ---- overrides ---- */
enum { IGO_NONE = 0, IGO_INCLUDE = 1, IGO_EXCLUDE = 2, IGO_PASS = 3 };      /* resultado de ign_ovr_state; PASS = ancestral estrito de um include */
typedef struct { int kind; char *p; size_t pl; int scoped; } IgnOvrEnt;
typedef struct { IgnOvrEnt *e; int n, cap; } IgnOvr;
static void ign_ovr_path(const char *storedir, char *out, size_t cap) {      /* ao lado do store: <pai do store>/overrides */
    char t[4400]; snprintf(t, sizeof t, "%s", storedir);
    size_t l = strlen(t); while (l > 1 && t[l - 1] == '/') t[--l] = 0;
    char *sl = strrchr(t, '/'); if (sl) *sl = 0; else snprintf(t, sizeof t, ".");
    snprintf(out, cap, "%s/overrides", t);
}
static void ign_ovr_free(IgnOvr *O) { for (int i = 0; i < O->n; i++) free(O->e[i].p); free(O->e); memset(O, 0, sizeof *O); }
static void ign_ovr_push(IgnOvr *O, int kind, int scoped, const char *p, size_t pl) {
    while (pl > 1 && p[pl - 1] == '/') pl--;
    while (pl > 2 && p[0] == '.' && p[1] == '/') { p += 2; pl -= 2; }
    if (!pl || (pl == 1 && p[0] == '.')) return;
    if (O->n == O->cap) { O->cap = O->cap ? O->cap * 2 : 8; O->e = (IgnOvrEnt *)realloc(O->e, sizeof *O->e * (size_t)O->cap); }
    IgnOvrEnt *x = &O->e[O->n++]; x->kind = kind; x->scoped = scoped; x->p = (char *)malloc(pl + 1); memcpy(x->p, p, pl); x->p[pl] = 0; x->pl = pl;
}
static void ign_ovr_load(IgnOvr *O, const char *storedir) {
    memset(O, 0, sizeof *O);
    char f[4500]; ign_ovr_path(storedir, f, sizeof f);
    FILE *F = fopen(f, "rb"); if (!F) return;
    char ln[4300];
    while (fgets(ln, sizeof ln, F)) {
        size_t l = strlen(ln); while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r' || ln[l - 1] == ' ')) ln[--l] = 0;
        if (!l || ln[0] == '#') continue;
        if (!strncmp(ln, "include ", 8)) ign_ovr_push(O, IGO_INCLUDE, 0, ln + 8, l - 8);
        else if (!strncmp(ln, "scoped ", 7)) ign_ovr_push(O, IGO_INCLUDE, 1, ln + 7, l - 7);
        else if (!strncmp(ln, "exclude ", 8)) ign_ovr_push(O, IGO_EXCLUDE, 0, ln + 8, l - 8);
    }
    fclose(F);
}
/* estado de `rel` (relativo a raiz): a ultima linha que cobre o caminho (igual ou acima dele) decide; sem cobertura, PASS se algum include esta debaixo dele */
static int ign_ovr_state(const IgnOvr *O, const char *rel, int isdir) {
    if (!O || !O->n) return IGO_NONE;
    const size_t rl = strlen(rel); int pass = 0;
    for (int i = O->n - 1; i >= 0; i--) {
        const IgnOvrEnt *x = &O->e[i];
        if (x->pl <= rl && !memcmp(rel, x->p, x->pl) && (x->pl == rl || rel[x->pl] == '/')) return x->kind;
        if (isdir && x->kind == IGO_INCLUDE && x->pl > rl && !memcmp(x->p, rel, rl) && x->p[rl] == '/') pass = 1;
    }
    return pass ? IGO_PASS : IGO_NONE;
}
/* grava (kind: "include", "exclude", "scoped") trocando as linhas anteriores do mesmo caminho; tmp e rename, 0600. Devolve 0 ou -1 */
static int ign_ovr_set(const char *storedir, const char *kind, const char *rel) {
    char f[4500], t[4600]; ign_ovr_path(storedir, f, sizeof f);
    { char d[4500]; snprintf(d, sizeof d, "%s", f); char *sl = strrchr(d, '/'); if (sl) { *sl = 0; st_mkdirs(d); } }
    char *buf = NULL; size_t bl = 0, bc = 0; const size_t rl = strlen(rel);
    FILE *F = fopen(f, "rb");
    if (F) {
        char ln[4300];
        while (fgets(ln, sizeof ln, F)) {
            const char *sp = strchr(ln, ' '); size_t l = strlen(ln); while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r' || ln[l - 1] == ' ')) ln[--l] = 0;
            if (sp && ln[0] != '#' && strlen(sp + 1) == rl && !strcmp(sp + 1, rel)) continue;
            if (bl + l + 2 > bc) { bc = (bl + l + 2) * 2; buf = (char *)realloc(buf, bc); }
            memcpy(buf + bl, ln, l); bl += l; buf[bl++] = '\n';
        }
        fclose(F);
    }
    snprintf(t, sizeof t, "%s.%ld.tmp", f, (long)getpid());
    FILE *O = fopen(t, "wb"); if (!O) { free(buf); return -1; }
    if (!buf || !bl || buf[0] != '#') fputs("# katana coder overrides: include <path>, exclude <path>, scoped <path>; paths relative to the root; later lines win\n", O);
    if (bl) fwrite(buf, 1, bl, O);
    fprintf(O, "%s %s\n", kind, rel);
    fclose(O); chmod(t, 0600); free(buf);
    if (rename(t, f) != 0) { unlink(t); return -1; }
    ign_hide_in_repo(storedir);
    return 0;
}
/* arquivos que o katana grava ao lado do store e que nao devem ir para o repositorio: acrescenta ao .gitignore da pasta, se ele ja existe (init) */
static void ign_hide_in_repo(const char *storedir) {
    char par[4400], f[4500]; snprintf(par, sizeof par, "%s", storedir);
    size_t l = strlen(par); while (l > 1 && par[l - 1] == '/') par[--l] = 0;
    char *sl = strrchr(par, '/'); if (!sl) return; *sl = 0;
    snprintf(f, sizeof f, "%s/.gitignore", par);
    FILE *F = fopen(f, "rb"); if (!F) return;
    char ln[300]; int have_o = 0, have_i = 0, have_s = 0;
    while (fgets(ln, sizeof ln, F)) { ln[strcspn(ln, "\r\n")] = 0; if (!strcmp(ln, "overrides")) have_o = 1; else if (!strcmp(ln, "ignored")) have_i = 1; else if (!strcmp(ln, "scrub.stamp")) have_s = 1; }
    fclose(F);
    if (have_o && have_i && have_s) return;
    F = fopen(f, "ab"); if (!F) return;
    if (!have_o) fputs("overrides\n", F);
    if (!have_i) fputs("ignored\n", F);
    if (!have_s) fputs("scrub.stamp\n", F);
    fclose(F);
}
