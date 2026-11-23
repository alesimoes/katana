/* ---------- coder/roots.c: raiz do indice por init, indice da home, absorcao (fase 2 de docs/code-index-lifecycle.md) ----------
 * Uma pasta e raiz de indice quando o usuario rodou `katana init` (ou /init) nela: o marcador <pasta>/.katana/coder/ROOT. O indice dela fica
 * dentro dela (<pasta>/.katana/coder/store) e cobre a subarvore, menos as subpastas que tem marcador proprio (init aninhado: indice
 * independente, o pai nao desce nelas) e as copias de trabalho do git (.git que e ARQUIVO apontando para worktrees/).
 * Sem init em nenhum ancestral, o indice vai para ~/.katana/coder/<chave>/store (um store por pasta visitada, chave = hash do caminho real)
 * e a pasta nunca ganha .katana dentro. Registro das raizes da home: ~/.katana/coder/roots/<chave>.root (primeira linha = caminho real).
 *
 * Resolucao (index_resolve): a partir do caminho real, sobe pelos ancestrais; o primeiro com marcador e a raiz (a consulta fica restrita ao
 * prefixo do caminho dentro dela). Sem marcador: a raiz da home registrada mais alta que seja o caminho ou ancestral dele absorve o caminho;
 * se nao houver, a propria pasta visitada vira raiz da home (registrada quando mk=1). A subida para num diretorio de worktree do git
 * (ele fica fora do indice do ancestral, entao a consulta nele nao pode ser absorvida).
 *
 * Decisoes do implementador (o documento deixava abertas):
 *   - o registro da home guarda so a raiz da home (kind home); raizes com init nao entram nele (o marcador e a autoridade e `coder list` e da fase 8).
 *   - a absorcao (raiz da home que passa a cobrir raizes da home descendentes, ou init que cobre as da home) apaga os stores descendentes assim que o
 *     store do ancestral foi gravado por inteiro (a varredura e sincrona; nao ha niveis ainda), nao "ao chegar em N1".
 *   - init move o store da home para dentro da pasta com rename; se o rename falha (outro disco), o store da home e apagado e o catalogo
 *     e refeito. Store local ANTIGO sem marcador (pasta de lancamento de versoes anteriores) nao e lido e nao e apagado: `coder status` o aponta;
 *     rodar init na pasta o adota (o catalogo revalida tudo por tamanho e mtime).
 *   - o marcador fica fora do git (.katana/coder/.gitignore com ROOT): quem clona nao ganha indice sem rodar init.
 *   - init na home ou em / e recusado sem --force; consultas na home e em / continuam sem criar indice (ja era a regra das ferramentas).
 *   - "mover o projeto" so e tratado para raizes com init (o store viaja com a pasta); a deteccao por (dispositivo, inode) das raizes da home e fase 8. */
#ifndef _WIN32
typedef struct { char root[4096]; char prefix[2048]; char store[4400]; int kind; int init; int registered; } IndexLoc;
enum { IL_LOCAL = 0, IL_ANCESTOR = 1, IL_HOME = 2 };

static uint64_t store_hash64(const void *data, size_t n, uint64_t seed);      /* store.c */
static int st_mkdirs(const char *path);                                       /* store.c */

static void rt_key(const char *path, char *out, size_t cap) { snprintf(out, cap, "%016llx", (unsigned long long)store_hash64(path, strlen(path), 0)); }
static void rt_home(char *out, size_t cap) { agent_home(out, cap); }
static int rt_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int rt_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void rt_marker_path(const char *dir, char *out, size_t cap) { snprintf(out, cap, "%s/.katana/coder/ROOT", strcmp(dir, "/") ? dir : ""); }
static int rt_has_marker(const char *dir) { char m[4300]; rt_marker_path(dir, m, sizeof m); return rt_exists(m); }
/* .git que e ARQUIVO "gitdir: ...worktrees/..." = copia de trabalho ligada: duplica o codigo do projeto, fica fora do indice do ancestral */
static int rt_is_worktree(const char *dir) {
    char g[4300]; struct stat st; snprintf(g, sizeof g, "%s/.git", dir);
    if (lstat(g, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    FILE *f = fopen(g, "r"); if (!f) return 0;
    char b[600]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return !strncmp(b, "gitdir:", 7) && strstr(b, "/worktrees/") != NULL;
}
/* a varredura do indice nao desce nesta subpasta (nunca a raiz): init aninhado ou copia de trabalho do git */
static int rt_skip_subdir(const char *dir) { return rt_has_marker(dir) || rt_is_worktree(dir); }

static void rt_real(const char *path, char *out, size_t cap) {      /* caminho real da pasta; arquivo resolve pela pasta dele */
    char t[4200];
    if (!path || !*path) path = ".";
    if (!realpath(path, t)) snprintf(t, sizeof t, "%s", path);
    struct stat st;
    if (stat(t, &st) == 0 && !S_ISDIR(st.st_mode)) { char *sl = strrchr(t, '/'); if (sl) { if (sl == t) t[1] = 0; else *sl = 0; } }
    snprintf(out, cap, "%s", t);
}
static int rt_parent(char *p) {                                     /* sobe um nivel; 0 se ja e a raiz do disco */
    if (!strcmp(p, "/")) return 0;
    char *sl = strrchr(p, '/'); if (!sl) return 0;
    if (sl == p) p[1] = 0; else *sl = 0;
    return 1;
}
static void rt_reg_dir(char *out, size_t cap) { char h[800]; rt_home(h, sizeof h); snprintf(out, cap, "%s/.katana/coder/roots", h); }
static void rt_reg_file(const char *root, char *out, size_t cap) { char d[900], k[40]; rt_reg_dir(d, sizeof d); rt_key(root, k, sizeof k); snprintf(out, cap, "%s/%s.root", d, k); }
static void rt_home_store(const char *root, char *out, size_t cap) { char h[800], k[40]; rt_home(h, sizeof h); rt_key(root, k, sizeof k); snprintf(out, cap, "%s/.katana/coder/%s/store", h, k); }
static void rt_rmtree(const char *p) {
    struct stat st; if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        chmod(p, 0700);
        DIR *d = opendir(p);
        if (d) { struct dirent *de; while ((de = readdir(d))) { if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char c[4700]; snprintf(c, sizeof c, "%s/%s", p, de->d_name); rt_rmtree(c); } closedir(d); }
        rmdir(p);
    } else unlink(p);
}
static void rt_register(const char *root) {
    char d[900], f[1000], t[1100]; rt_reg_dir(d, sizeof d); st_mkdirs(d);
    rt_reg_file(root, f, sizeof f); if (rt_exists(f)) return;
    snprintf(t, sizeof t, "%s.%ld.tmp", f, (long)getpid());
    FILE *o = fopen(t, "w"); if (!o) return;
    fprintf(o, "%s\nkind home\ncreated %lld\n", root, (long long)time(NULL)); fclose(o); chmod(t, 0600);
    if (rename(t, f) != 0) unlink(t);
}
static void rt_unregister(const char *root) { char f[1000]; rt_reg_file(root, f, sizeof f); unlink(f); }

/* sobe de `path` ate achar o marcador; devolve 1 e a raiz com init (ancestral mais proximo, o proprio caminho incluido) */
static int index_marker_root(const char *path, char *root, size_t cap) {
    char cur[4200]; rt_real(path, cur, sizeof cur);
    for (;;) {
        if (rt_has_marker(cur)) { snprintf(root, cap, "%s", cur); return 1; }
        if (rt_is_worktree(cur)) return 0;
        if (!rt_parent(cur)) return 0;
    }
}
static void index_resolve(const char *path, int mk, IndexLoc *L) {
    memset(L, 0, sizeof *L);
    char rp[4200], cur[4200]; rt_real(path, rp, sizeof rp);
    snprintf(cur, sizeof cur, "%s", rp);
    char bnd[4200]; snprintf(bnd, sizeof bnd, "/");          /* limite superior da absorcao pela home (diretorio de worktree) */
    for (;;) {
        if (rt_has_marker(cur)) {
            snprintf(L->root, sizeof L->root, "%s", cur); L->init = 1; L->kind = !strcmp(cur, rp) ? IL_LOCAL : IL_ANCESTOR;
            snprintf(L->store, sizeof L->store, "%s/.katana/coder/store", strcmp(cur, "/") ? cur : ""); break;
        }
        if (rt_is_worktree(cur)) { snprintf(bnd, sizeof bnd, "%s", cur); break; }
        if (!rt_parent(cur)) break;
    }
    if (!L->init) {
        char a[4200]; snprintf(a, sizeof a, "%s", rp); L->kind = IL_HOME;
        for (;;) {                                           /* a raiz registrada mais alta dentro do limite absorve */
            char f[1000]; rt_reg_file(a, f, sizeof f);
            if (rt_exists(f)) { snprintf(L->root, sizeof L->root, "%s", a); L->registered = 1; }
            if (!strcmp(a, bnd) || !rt_parent(a)) break;
        }
        if (!L->root[0]) snprintf(L->root, sizeof L->root, "%s", rp);
        rt_home_store(L->root, L->store, sizeof L->store);
        if (mk && !L->registered) { rt_register(L->root); L->registered = 1; }
    }
    const size_t rl = strlen(L->root);
    if (strcmp(rp, L->root) != 0 && rl && !strncmp(rp, L->root, rl)) snprintf(L->prefix, sizeof L->prefix, "%s", rp + rl + (rl > 1 ? 1 : 0));
}
/* apaga os stores da home de raizes da home DENTRO de `root` (absorvidas por um ancestral ou por um init); devolve quantas */
static int index_retire_below(const char *root) {
    char d[900]; rt_reg_dir(d, sizeof d); DIR *D = opendir(d); if (!D) return 0;
    char h[800]; rt_home(h, sizeof h); const size_t rl = strlen(root); int n = 0; struct dirent *de;
    while ((de = readdir(D))) {
        const size_t l = strlen(de->d_name); if (l != 21 || strcmp(de->d_name + 16, ".root")) continue;
        char f[1100], ln[4300] = ""; snprintf(f, sizeof f, "%s/%s", d, de->d_name);
        FILE *F = fopen(f, "r"); if (!F) continue; if (!fgets(ln, sizeof ln, F)) ln[0] = 0; fclose(F);
        ln[strcspn(ln, "\r\n")] = 0;
        if (strncmp(ln, root, rl) != 0 || ln[rl] != '/') continue;
        char sd[1100]; snprintf(sd, sizeof sd, "%s/.katana/coder/%.16s", h, de->d_name);
        rt_rmtree(sd); unlink(f); n++;
    }
    closedir(D); return n;
}
/* init: marcador, adocao do store da home (rename) e aposentadoria das raizes da home de dentro. flags: 1 = ja era raiz, 2 = store movido da home */
static int index_init_root(const char *dir, int *info, char *err, size_t ecap) {
    char cd[4500], mk[4500], gi[4500]; if (info) *info = 0;
    snprintf(cd, sizeof cd, "%s/.katana", dir); mkdir(cd, 0700);
    snprintf(cd, sizeof cd, "%s/.katana/coder", dir); mkdir(cd, 0700);
    if (!rt_isdir(cd)) { snprintf(err, ecap, "cannot create %s (is the folder writable?)", cd); return -1; }
    rt_marker_path(dir, mk, sizeof mk);
    if (rt_exists(mk)) { if (info) *info |= 1; return 0; }
    FILE *f = fopen(mk, "w"); if (!f) { snprintf(err, ecap, "cannot write %s", mk); return -1; }
    { time_t t = time(NULL); char ts[40]; strftime(ts, sizeof ts, "%Y-%m-%d", localtime(&t)); fprintf(f, "katana root 1\n%s\n", ts); }
    fclose(f); chmod(mk, 0600);
    snprintf(gi, sizeof gi, "%s/.gitignore", cd);
    if (!rt_exists(gi)) { FILE *g = fopen(gi, "w"); if (g) { fputs("ROOT\n", g); fclose(g); chmod(gi, 0600); } }
    char hs[4500], ls[4500]; rt_home_store(dir, hs, sizeof hs); snprintf(ls, sizeof ls, "%s/store", cd);
    if (rt_isdir(hs)) {
        if (!rt_exists(ls) && rename(hs, ls) == 0) { if (info) *info |= 2; }
        else if (rt_isdir(hs)) rt_rmtree(hs);                /* outro disco ou ja havia store local: o do home sai, o catalogo revalida */
        char k[40], hd[4500], h[800]; rt_key(dir, k, sizeof k); rt_home(h, sizeof h); snprintf(hd, sizeof hd, "%s/.katana/coder/%s", h, k); rt_rmtree(hd);
    }
    rt_unregister(dir);
    index_retire_below(dir);
    return 0;
}
/* katana init --remove: tira o marcador e o store local; a pasta volta a seguir as regras sem init */
static int index_remove_root(const char *dir) {
    char mk[4500], st[4500]; rt_marker_path(dir, mk, sizeof mk); snprintf(st, sizeof st, "%s/.katana/coder/store", dir);
    const int had = rt_exists(mk); unlink(mk); rt_rmtree(st);
    char gi[4500]; snprintf(gi, sizeof gi, "%s/.katana/coder/.gitignore", dir); unlink(gi);
    char cd[4500]; snprintf(cd, sizeof cd, "%s/.katana/coder", dir); rmdir(cd); snprintf(cd, sizeof cd, "%s/.katana", dir); rmdir(cd);
    return had;
}
/* pasta de lancamento de versoes anteriores: store local sem marcador (nao e lido; init o adota) */
static int index_old_local_store(const char *dir) { char m[4500]; snprintf(m, sizeof m, "%s/.katana/coder/store/MANIFEST", dir); return !rt_has_marker(dir) && rt_exists(m); }
#else
typedef struct { char root[4096]; char prefix[2048]; char store[4400]; int kind; int init; int registered; } IndexLoc;
enum { IL_LOCAL = 0, IL_ANCESTOR = 1, IL_HOME = 2 };
static int rt_skip_subdir(const char *dir) { (void)dir; return 0; }
static int index_marker_root(const char *path, char *root, size_t cap) { (void)path; (void)root; (void)cap; return 0; }
static void index_resolve(const char *path, int mk, IndexLoc *L) { (void)mk; memset(L, 0, sizeof *L); snprintf(L->root, sizeof L->root, "%s", path); L->kind = IL_LOCAL; L->init = 1; snprintf(L->store, sizeof L->store, "%s/.katana/coder/store", path); }
static int index_retire_below(const char *root) { (void)root; return 0; }
static int index_init_root(const char *dir, int *info, char *err, size_t ecap) { (void)dir; if (info) *info = 0; (void)err; (void)ecap; return 0; }
static int index_remove_root(const char *dir) { (void)dir; return 0; }
static int index_old_local_store(const char *dir) { (void)dir; return 0; }
#endif
