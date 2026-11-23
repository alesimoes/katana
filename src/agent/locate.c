/* ---------- navegacao no computador: list_dir com detalhes e find_file ----------
 * list_dir mostra pastas primeiro, em ordem, com quantos itens cada uma tem, e os arquivos com tamanho e data: o modelo
 * escolhe o que abrir sem listar de novo. find_file acha arquivos e pastas pelo nome em qualquer lugar (por padrao a
 * partir da home), do mais recente para o mais antigo: e o "onde esta aquele projeto?" sem adivinhar caminhos. */
#ifndef _WIN32
typedef struct { char *name; int dir; long long size; time_t mt; int items; } LdEnt;
static int ld_cmp(const void *a, const void *b) {
    const LdEnt *x = (const LdEnt *)a, *y = (const LdEnt *)b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcmp(x->name, y->name);
}
static void loc_size(long long b, char *o, size_t cap) {
    if (b < 1024) snprintf(o, cap, "%lld B", b);
    else if (b < 1024 * 1024) snprintf(o, cap, "%.1f KB", (double)b / 1024);
    else if (b < 1024LL * 1024 * 1024) snprintf(o, cap, "%.1f MB", (double)b / (1024 * 1024));
    else snprintf(o, cap, "%.1f GB", (double)b / (1024.0 * 1024 * 1024));
}
static void tool_list_dir(const char *path, char *out, size_t cap) {
    DIR *d = opendir(path);
    if (!d) { snprintf(out, cap, "error: cannot list %s (%s)", path, strerror(errno)); return; }
    LdEnt *v = NULL;
    int n = 0, vc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 5000) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n == vc) { vc = vc * 2 + 64; v = (LdEnt *)realloc(v, sizeof(LdEnt) * (size_t)vc); if (!v) die("sem memoria"); }
        char full[8600]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        memset(&v[n], 0, sizeof v[n]);
        v[n].name = strdup(e->d_name);
        if (stat(full, &st) == 0) { v[n].dir = S_ISDIR(st.st_mode); v[n].size = (long long)st.st_size; v[n].mt = st.st_mtime; }
        n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, ld_cmp);
    size_t o = 0;
    int shown = 0;
    const size_t lim = cap > TOOL_OUT + 200 ? TOOL_OUT : cap > 300 ? cap - 200 : 0;
    for (int i = 0; i < n; i++) {
        char line[700], sz[32], day[16];
        if (v[i].dir) {
            if (shown < 60) {            /* quantos itens ha dentro (so das primeiras pastas, para nao demorar) */
                char full[8600]; snprintf(full, sizeof full, "%s/%s", path, v[i].name);
                DIR *s = opendir(full);
                if (s) { struct dirent *x; while ((x = readdir(s)) && v[i].items < 1000) if (strcmp(x->d_name, ".") && strcmp(x->d_name, "..")) v[i].items++; closedir(s); }
                else v[i].items = -1;
            }
            if (v[i].items < 0) snprintf(line, sizeof line, "%s/  (no access)\n", v[i].name);
            else snprintf(line, sizeof line, "%s/  (%d%s item%s)\n", v[i].name, v[i].items, v[i].items >= 1000 ? "+" : "", v[i].items == 1 ? "" : "s");
        } else {
            loc_size(v[i].size, sz, sizeof sz);
            strftime(day, sizeof day, "%Y-%m-%d", localtime(&v[i].mt));
            snprintf(line, sizeof line, "%s  %s  %s\n", v[i].name, sz, day);
        }
        if (o + strlen(line) >= lim) break;
        o += (size_t)snprintf(out + o, cap - o, "%s", line);
        shown++;
    }
    if (n == 0) snprintf(out, cap, "(empty)");
    else if (shown < n) snprintf(out + o, cap - o, "[%d of %d entries shown; use glob with a pattern or find_file to narrow it]", shown, n);
    for (int i = 0; i < n; i++) free(v[i].name);
    free(v);
}

#define FF_MAX 400
typedef struct { char path[1024]; int dir, rank, depth; long long size; time_t mt; } FfHit;
typedef struct { const char *needle; int kind; FfHit *h; int n, total; long visited; double t0; int stop;
                 dev_t sdev[64]; ino_t sino[64]; int nseen;
                 char **q; int *qd; int qn, qcap; } FfSt;   /* fila da busca em largura (pastas rasas primeiro) */     /* pastas ja percorridas por um atalho (nao repetir pelo disco) */
static int ff_cmp(const void *a, const void *b) {
    const FfHit *x = (const FfHit *)a, *y = (const FfHit *)b;
    if (x->rank != y->rank) return x->rank - y->rank;            /* nome exato, depois comeco do nome, depois parte do nome */
    if (x->depth != y->depth) return x->depth - y->depth;        /* mais perto da raiz primeiro */
    return x->mt < y->mt ? 1 : x->mt > y->mt ? -1 : strcmp(x->path, y->path);
}
static void ff_push(FfSt *s, const char *dir, int depth) {
    if (s->qn == s->qcap) {
        s->qcap = s->qcap * 2 + 256;
        s->q = (char **)realloc(s->q, sizeof(char *) * (size_t)s->qcap); s->qd = (int *)realloc(s->qd, sizeof(int) * (size_t)s->qcap);
        if (!s->q || !s->qd) die("sem memoria");
    }
    s->q[s->qn] = strdup(dir); s->qd[s->qn] = depth; s->qn++;
}
static void ff_dir(FfSt *s, const char *dir, int depth) {
    if (s->stop || depth > 12) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while (!s->stop && (e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (++s->visited % 2000 == 0 && (s->visited > 600000 || now_sec() - s->t0 > 5.0)) { s->stop = 1; break; }
        const size_t nl = strlen(nm), kl = strlen(s->needle);
        int hit = 0, at = 0;
        for (size_t i = 0; !hit && i + kl <= nl; i++) if (ci_ncmp(nm + i, s->needle, kl) == 0) { hit = 1; at = (int)i; }
        if (!hit && e->d_type == DT_REG) continue;                      /* arquivo comum que nao bate: nem consulta o disco */
        char full[1024]; struct stat st;
        if ((size_t)snprintf(full, sizeof full, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", nm) >= sizeof full) continue;
        if (lstat(full, &st) != 0) continue;
        const int lnk = S_ISLNK(st.st_mode);
        if (lnk && (depth > 1 || stat(full, &st) != 0)) continue;       /* atalhos so perto da raiz da busca (sem ciclos) */
        const int isd = S_ISDIR(st.st_mode);
        if (!isd && !S_ISREG(st.st_mode)) continue;
        if (hit && (s->kind == 0 || (s->kind == 1) == !isd)) {
            s->total++;
            if (s->n < FF_MAX) { FfHit *h = &s->h[s->n++]; snprintf(h->path, sizeof h->path, "%s", full); h->dir = isd; h->rank = at == 0 && nl == kl ? 0 : at == 0 ? 1 : 2; h->depth = depth; h->size = (long long)st.st_size; h->mt = st.st_mtime; }
        }
        /* pastas escondidas e de lixo nao sao percorridas (mas o nome delas pode ser o resultado, acima) */
        if (!isd || nm[0] == '.' || sf_skip_dir(nm) || env_noise(nm) || !strcmp(nm, "sys") || !strcmp(nm, "dev") || !strcmp(nm, "snap") || !strcmp(nm, "CMakeFiles") || !strcmp(nm, "site-packages")) continue;
        int seen = 0;
        for (int k = 0; k < s->nseen; k++) if (s->sdev[k] == st.st_dev && s->sino[k] == st.st_ino) seen = 1;
        if (seen) continue;
        if (lnk && s->nseen < 64) { s->sdev[s->nseen] = st.st_dev; s->sino[s->nseen] = st.st_ino; s->nseen++; }
        ff_push(s, full, depth + 1);
    }
    closedir(d);
}
/* percorre a fila em largura ate acabar ou estourar o tempo */
static void ff_run(FfSt *s) {
    for (int i = 0; i < s->qn; i++) { if (!s->stop) ff_dir(s, s->q[i], s->qd[i]); free(s->q[i]); }
    free(s->q); free(s->qd); s->q = NULL; s->qd = NULL; s->qn = s->qcap = 0;
}
static void tool_find_file(const char *js, const char *end, char *out, size_t cap) {
    static char name[300], under[4200], kind[16];
    char res[4200], err[1300];
    if (!json_str(js, end, "name", name, sizeof name) || !name[0]) { snprintf(out, cap, "error: missing name (part of the file or folder name)"); return; }
    const int everywhere = !json_str(js, end, "under", under, sizeof under) || !under[0];
    if (everywhere) agent_home(under, sizeof under);
    if (!json_str(js, end, "kind", kind, sizeof kind)) kind[0] = 0;
    if (!agent_read_access(under, 1, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    FfSt s; memset(&s, 0, sizeof s);
    s.needle = name; s.kind = strcmp(kind, "file") == 0 ? 1 : strcmp(kind, "dir") == 0 || strcmp(kind, "folder") == 0 ? 2 : 0;
    s.h = (FfHit *)malloc(sizeof(FfHit) * FF_MAX);
    if (!s.h) { snprintf(out, cap, "error: out of memory"); return; }
    s.t0 = now_sec();
    ff_push(&s, res, 0);
    char where[600]; snprintf(where, sizeof where, "%.250s", res);
    if (everywhere) {            /* sem `under`: tambem os outros discos montados (dados fora da home) */
        FILE *mf = fopen("/proc/mounts", "r");
        char ln[2048], dev[512], mp[1024], fs[64];
        while (mf && fgets(ln, sizeof ln, mf)) {
            if (sscanf(ln, "%511s %1023s %63s", dev, mp, fs) != 3) continue;
            if (!strstr("ext4 ext3 ext2 btrfs xfs f2fs ntfs ntfs3 vfat exfat fuseblk zfs", fs)) continue;
            if (strcmp(mp, "/") == 0 || strncmp(mp, "/boot", 5) == 0 || strncmp(mp, "/snap", 5) == 0 || strncmp(mp, "/var", 4) == 0) continue;
            if (strncmp(mp, res, strlen(res)) == 0 || strncmp(res, mp, strlen(mp)) == 0) continue;      /* ja coberto pela home */
            char r2[4200], e2[1300];
            if (!agent_read_access(mp, 1, r2, sizeof r2, e2, sizeof e2)) continue;
            ff_push(&s, r2, 0);
            const size_t wl = strlen(where);
            if (wl < 300) snprintf(where + wl, sizeof where - wl, ", %.250s", r2);
        }
        if (mf) fclose(mf);
    }
    ff_run(&s);
    qsort(s.h, (size_t)s.n, sizeof *s.h, ff_cmp);
    size_t o = 0;
    int shown = 0;
    const size_t lim = cap > TOOL_OUT + 300 ? TOOL_OUT : cap > 400 ? cap - 300 : 0;
    for (int i = 0; i < s.n && shown < 40; i++) {
        char line[1200], sz[32], day[16];
        strftime(day, sizeof day, "%Y-%m-%d", localtime(&s.h[i].mt));
        if (s.h[i].dir) snprintf(line, sizeof line, "%s/  (folder, %s)\n", s.h[i].path, day);
        else { loc_size(s.h[i].size, sz, sizeof sz); snprintf(line, sizeof line, "%s  %s  %s\n", s.h[i].path, sz, day); }
        if (o + strlen(line) >= lim) break;
        o += (size_t)snprintf(out + o, cap - o, "%s", line);
        shown++;
    }
    if (!s.total) snprintf(out, cap, "nothing named like \"%s\" under %s%s. Try a shorter part of the name, another folder in `under` (for example / or another disk under /mnt or /media), or search_files for text inside files",
                           name, where, s.stop ? " (search stopped at the time limit)" : "");
    else snprintf(out + o, cap - o, "[%d of %d match%s shown, best name match and shortest path first%s]", shown, s.total, s.total == 1 ? "" : "es", s.stop ? "; the search stopped at the time limit, narrow it with `under`" : "");
    free(s.h);
}

/* ---- caminho que nao existe: acha onde ele estaria de verdade. O modelo costuma escrever o caminho como o usuario falou ("mnt/dados/greymod")
 * e o katana o resolve contra a pasta de trabalho. Aqui procura, pelo nome de cada pedaco que falta (da direita para a esquerda, sem
 * os nomes comuns), uma pasta ou arquivo com esse nome exato no disco, e refaz o caminho com o resto. Devolve 1 e a sugestao em out ---- */
static int ff_resolve_missing(const char *abs, char *out, size_t cap) {
    static const char *const generic[] = {"src", "lib", "bin", "docs", "doc", "home", "mnt", "usr", "var", "opt", "tmp", "etc", "data", "dados", "source", "test", "tests", "build", "include", NULL};
    char buf[4200], *comp[64], *sv = NULL; int n = 0;
    snprintf(buf, sizeof buf, "%s", abs);
    for (char *p = strtok_r(buf, "/", &sv); p && n < 64; p = strtok_r(NULL, "/", &sv)) comp[n++] = p;
    char pre[4200] = ""; int k = 0;
    for (; k < n; k++) { char t[4300]; snprintf(t, sizeof t, "%s/%s", pre, comp[k]); if (access(t, F_OK) != 0) break; snprintf(pre, sizeof pre, "%s", t); }
    if (k >= n) return 0;
    char found[4][1100]; int nf = 0, mj = k, dirhint = 0, hj = 0; const double tstart = now_sec();
    int order[66], no = 0;                            /* pastas primeiro (da direita para a esquerda), o arquivo por ultimo */
    for (int j = n - 2; j >= k; j--) order[no++] = j;
    order[no++] = n - 1;                              /* por ultimo o proprio ultimo pedaco (um nome de pasta citado); so aceita pastas */
    for (int oi = 0; oi < no && nf == 0; oi++) {
        const int j = order[oi];
        const char *nd = comp[j];
        if (strlen(nd) < 3) continue;
        int gen = 0; for (int g = 0; generic[g]; g++) if (!strcasecmp(nd, generic[g])) gen = 1;
        if (gen) continue;
        char roots[10][1100]; int nr = 0;               /* discos de dados primeiro, a home por ultimo; cada raiz com o seu tempo (1,5 s) */
        char home[1100]; agent_home(home, sizeof home);
        FILE *mf = fopen("/proc/mounts", "r"); char ln[2048], dev[512], mp[1024], fs[64];
        while (mf && fgets(ln, sizeof ln, mf) && nr < 8) {
            if (sscanf(ln, "%511s %1023s %63s", dev, mp, fs) != 3) continue;
            if (!strstr("ext4 ext3 ext2 btrfs xfs f2fs ntfs ntfs3 vfat exfat fuseblk zfs", fs)) continue;
            if (strcmp(mp, "/") == 0 || strncmp(mp, "/boot", 5) == 0 || strncmp(mp, "/snap", 5) == 0 || strncmp(mp, "/var", 4) == 0) continue;
            if (strncmp(mp, home, strlen(home)) == 0 || strncmp(home, mp, strlen(mp)) == 0) continue;
            if (strncmp(mp, "/run/media", 10) == 0 || strncmp(mp, "/media", 6) == 0) continue;      /* removiveis: podem travar a leitura */
            snprintf(roots[nr++], sizeof roots[0], "%s", mp);
        }
        if (mf) fclose(mf);
        snprintf(roots[nr++], sizeof roots[0], "%s", home);
        for (int ri = 0; ri < nr && nf == 0 && now_sec() - tstart < 4.0; ri++) {
            FfSt s; memset(&s, 0, sizeof s);
            s.needle = nd; s.kind = 0; s.h = (FfHit *)malloc(sizeof(FfHit) * FF_MAX);
            if (!s.h) return 0;
            s.t0 = now_sec() - 3.5;
            ff_push(&s, roots[ri], 0);
            ff_run(&s);
            qsort(s.h, (size_t)s.n, sizeof *s.h, ff_cmp);
            int ok = 0; char tmp[4][1100];
            for (int i = 0; i < s.n && ok < 4; i++) {
                if (s.h[i].rank != 0) continue;
                char cand[2400]; size_t o = (size_t)snprintf(cand, sizeof cand, "%s", s.h[i].path);
                for (int q = j + 1; q < n && o < sizeof cand - 300; q++) o += (size_t)snprintf(cand + o, sizeof cand - o, "/%s", comp[q]);
                if (access(cand, F_OK) == 0 && strlen(cand) < sizeof tmp[0]) { if (j < n - 1 || s.h[i].dir) snprintf(tmp[ok++], sizeof tmp[0], "%s", cand); }
                else if (s.h[i].dir && !dirhint && j < n - 1) { dirhint = 1; hj = j; snprintf(found[3], sizeof found[0], "%s", s.h[i].path); }      /* a pasta existe, so falta o que vem depois */
            }
            free(s.h);
            for (int i = 0; i < ok && nf < 4; i++) snprintf(found[nf++], sizeof found[0], "%s", tmp[i]);
            if (nf) mj = j;
        }
    }
    if (!nf && dirhint) {
        char rest[600] = ""; size_t ro = 0;
        for (int q = hj + 1; q < n && ro < 500; q++) ro += (size_t)snprintf(rest + ro, sizeof rest - ro, "%s%s", q > hj + 1 ? "/" : "", comp[q]);
        snprintf(out, cap, "The folder \"%s\" exists at %s, but %s is not in it. List it with Bash (ls) or Glob (path=that folder) to see what it has.", comp[hj], found[3], rest);
        return 1;
    }
    if (!nf) return 0;
    size_t o = (size_t)snprintf(out, cap, "Did you mean: ");
    for (int i = 0; i < nf && o + 1200 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? ", " : "", found[i]);
    snprintf(out + o, cap - o, "? (katana looked for a folder or file with the name \"%s\" on this machine and rebuilt the path)", comp[mj]);
    return 1;
}

/* ---- o usuario cita uma pasta ou projeto ("o projeto greymod em mnt/dados/greymod") que nao existe a partir da pasta de trabalho:
 * o katana acha onde ela esta e avisa o modelo no inicio do pedido. Candidatos: pedacos com 2 ou mais barras (ou comecando por / ~/ ./)
 * e a palavra depois de pasta/projeto/folder/project/repo. No maximo 2 buscas por pedido. Devolve 1 e a nota em out ---- */
static int place_note(const char *msg, char *out, size_t cap) {
    char cwd[1500]; if (!getcwd(cwd, sizeof cwd)) return 0;
    char cands[4][700]; int nc = 0;
    static const char *const trig[] = {"pasta", "folder", "diretorio", "projeto", "project", "repo", "repositorio", "repository", NULL};
    static const char *const filler[] = {"chamado", "chamada", "named", "called", "o", "a", "de", "do", "da", "the", "um", "uma", "nome", NULL};
    char w[700]; int wl = 0; const char *p = msg; int armed = 0, skipped = 0;
    for (;; p++) {
        const int c = (unsigned char)*p;
        const int wc = c && !isspace(c) && c != ',' && c != ';' && c != ':' && c != '"' && c != '\'' && c != '(' && c != ')' && c != '`' && c != '?' && c != '!';
        if (wc) { if (wl < (int)sizeof w - 1) w[wl++] = (char)c; if (c) continue; }
        if (wl) {
            w[wl] = 0; while (wl && (w[wl - 1] == '.')) w[--wl] = 0;
            int slashes = 0; for (int i = 0; i < wl; i++) if (w[i] == '/') slashes++;
            char lw[700]; for (int i = 0; i <= wl; i++) lw[i] = (char)tolower((unsigned char)w[i]);
            const int pathlike = slashes >= 2 || (slashes >= 1 && (w[0] == '/' || w[0] == '~' || w[0] == '.')) ;
            const char *lastc = strrchr(w, '/'); const int filelike = lastc && strchr(lastc + 1, '.');      /* arquivo citado, nao um lugar */
            if (pathlike && !filelike && wl >= 6 && !strstr(w, "://") && strncmp(w, "http", 4) != 0 && nc < 4) {
                char t[1500];
                if (w[0] == '/') snprintf(t, sizeof t, "%s", w);
                else if (w[0] == '~') { char h[1100]; agent_home(h, sizeof h); snprintf(t, sizeof t, "%s%s", h, w + 1); }
                else snprintf(t, sizeof t, "%s/%s", cwd, w);
                if (access(t, F_OK) != 0) snprintf(cands[nc++], sizeof cands[0], "%s", t);
            } else if (armed) {
                int isf = 0; for (int i = 0; filler[i]; i++) if (!strcmp(lw, filler[i])) isf = 1;
                if (isf && skipped < 2) skipped++;
                else {
                    armed = 0; skipped = 0;
                    int ok = wl >= 4 && wl < 60 && !slashes; for (int i = 0; ok && i < wl; i++) if (!isalnum((unsigned char)w[i]) && w[i] != '_' && w[i] != '-' && w[i] != '.') ok = 0;
                    static const char *const stopw[] = {"que", "esta", "para", "this", "that", "com", "uma", "onde", "como", "your", "seu", "nosso", "atual", NULL};
                    for (int i = 0; ok && stopw[i]; i++) if (!strcmp(lw, stopw[i])) ok = 0;
                    if (ok && nc < 4) { char t[1500]; snprintf(t, sizeof t, "%s/%s", cwd, w); if (access(t, F_OK) != 0) snprintf(cands[nc++], sizeof cands[0], "%s", t); }
                }
            }
            for (int i = 0; trig[i]; i++) if (!strcmp(lw, trig[i])) { armed = 1; skipped = 0; }
            wl = 0;
        }
        if (!c) break;
    }
    size_t o = 0; int searched = 0; char seen[4][1100]; int ns = 0;
    for (int i = 0; i < nc && searched < 2; i++) {
        char sug[1500]; searched++;
        if (!ff_resolve_missing(cands[i], sug, sizeof sug) || strncmp(sug, "Did you mean: ", 14) != 0) continue;
        char *q = strchr(sug, '?'); if (q) *q = 0;
        int dup = 0; for (int k = 0; k < ns; k++) if (!strcmp(seen[k], sug + 14)) dup = 1;
        if (dup) continue;
        if (ns < 4) snprintf(seen[ns++], sizeof seen[0], "%s", sug + 14);
        const char *shown = cands[i] + (strncmp(cands[i], cwd, strlen(cwd)) == 0 && cands[i][strlen(cwd)] == '/' ? strlen(cwd) + 1 : 0);
        if (o + 700 < cap) o += (size_t)snprintf(out + o, cap - o, "%s[katana: you mention \"%s\", which does not exist from the working folder (%s). It is at: %s. Work there: pass that folder as `path` to Find, Glob and Grep, or use absolute paths.]", o ? "\n" : "", shown, cwd, sug + 14);
    }
    return o > 0;
}
#endif
