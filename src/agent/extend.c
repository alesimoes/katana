/* ---------- extensao do agente: katana init, katana skill, katana mcp e os comandos /init /skill /mcp ----------
 * Tudo sem modelo. Formatos (brief, ability, card) em docs/extending.md. */

/* ======== utilitarios ======== */
static int ex_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int ex_write(const char *path, const char *txt, int priv) {
    mkdir_parents(path);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(txt, f);
    fclose(f);
#ifndef _WIN32
    if (priv) chmod(path, 0600);
#endif
    return 1;
}
static int ex_rmtree(const char *p) {      /* remove arquivo ou pasta */
    struct stat st;
    if (lstat(p, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[2400]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
                ex_rmtree(c);
            }
            closedir(d);
        }
        return rmdir(p) == 0;
    }
    return unlink(p) == 0;
}

/* ======== katana init: KATANA.md no formato brief, sem modelo ======== */
typedef struct { const char *ext, *lang; int n; } ExLang;
static void ex_scan(const char *dir, int depth, ExLang *lv, int nl, int *total) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *total < 20000) {
        if (e->d_name[0] == '.' || env_noise(e->d_name)) continue;
        char p[2400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) ex_scan(p, depth + 1, lv, nl, total); continue; }
        const char *x = strrchr(e->d_name, '.');
        if (!x) continue;
        for (int i = 0; i < nl; i++) if (strcmp(lv[i].ext, x) == 0) { lv[i].n++; (*total)++; break; }
    }
    closedir(d);
}
static int ex_has_line(const char *txt, const char *prefix) {        /* alguma linha comeca com prefix */
    const size_t pl = strlen(prefix);
    for (const char *l = txt; l && *l;) { if (strncmp(l, prefix, pl) == 0) return 1; l = strchr(l, '\n'); if (l) l++; }
    return 0;
}
static const char *ex_guess_dir(const char *n) {
    static const struct { const char *k, *v; } G[] = {
        {"src", "source code"}, {"lib", "library code"}, {"include", "public headers"}, {"app", "application code"}, {"cmd", "program entry points"},
        {"pkg", "packages"}, {"internal", "internal packages"}, {"tests", "tests"}, {"test", "tests"}, {"spec", "tests"}, {"docs", "documentation"},
        {"doc", "documentation"}, {"scripts", "helper scripts"}, {"tools", "helper tools"}, {"bin", "executables and scripts"}, {"examples", "examples"},
        {"assets", "static assets"}, {"public", "static files served as is"}, {"static", "static files"}, {"config", "configuration"}, {"configs", "configuration"},
        {"data", "data files"}, {"models", "model files"}, {"migrations", "database migrations"}, {"deploy", "deployment files"}, {"docker", "container files"},
        {"web", "web front end"}, {"server", "server code"}, {"client", "client code"}, {"vendor", "third-party code"}, {"third_party", "third-party code"},
        {"benchmarks", "benchmarks"}, {"bench", "benchmarks"}, {"site", "web site"}, {"ui", "user interface"}, {NULL, NULL}};
    for (int i = 0; G[i].k; i++) if (strcasecmp(G[i].k, n) == 0) return G[i].v;
    return NULL;
}
static void ex_trimcpy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s); br_trim(dst); }
/* monta o KATANA.md de dir em out (texto novo) */
static void ex_init_build(const char *dir, McpB *out) {
    char p[2400], *txt;
    char build[300] = "", test[300] = "", run[300] = "", lint[300] = "";
    /* Makefile */
    snprintf(p, sizeof p, "%s/Makefile", dir);
    if ((txt = mcp_slurp(p, NULL))) {
        if (ex_has_line(txt, "build:") || ex_has_line(txt, "all:")) snprintf(build, sizeof build, "make%s", ex_has_line(txt, "build:") ? " build" : "");
        else snprintf(build, sizeof build, "make");
        if (ex_has_line(txt, "test:")) snprintf(test, sizeof test, "make test"); else if (ex_has_line(txt, "check:")) snprintf(test, sizeof test, "make check");
        if (ex_has_line(txt, "run:")) snprintf(run, sizeof run, "make run");
        if (ex_has_line(txt, "lint:")) snprintf(lint, sizeof lint, "make lint");
        free(txt);
    }
    snprintf(p, sizeof p, "%s/CMakeLists.txt", dir);
    if (!build[0] && access(p, F_OK) == 0) { snprintf(build, sizeof build, "cmake -S . -B build && cmake --build build"); if (!test[0]) snprintf(test, sizeof test, "ctest --test-dir build"); }
    snprintf(p, sizeof p, "%s/package.json", dir);
    if ((txt = mcp_slurp(p, NULL))) {
        if (!build[0] && strstr(txt, "\"build\"")) snprintf(build, sizeof build, "npm run build");
        if (!test[0] && strstr(txt, "\"test\"")) snprintf(test, sizeof test, "npm test");
        if (!run[0] && strstr(txt, "\"start\"")) snprintf(run, sizeof run, "npm start");
        else if (!run[0] && strstr(txt, "\"dev\"")) snprintf(run, sizeof run, "npm run dev");
        if (!lint[0] && strstr(txt, "\"lint\"")) snprintf(lint, sizeof lint, "npm run lint");
        free(txt);
    }
    snprintf(p, sizeof p, "%s/pyproject.toml", dir);
    char p2[2400]; snprintf(p2, sizeof p2, "%s/setup.py", dir);
    if (access(p, F_OK) == 0 || access(p2, F_OK) == 0) {
        char td[2400]; snprintf(td, sizeof td, "%s/tests", dir);
        char *py = mcp_slurp(p, NULL);
        if (!test[0] && (ex_isdir(td) || (py && strstr(py, "pytest")))) snprintf(test, sizeof test, "python3 -m pytest");
        if (!lint[0] && py && strstr(py, "ruff")) snprintf(lint, sizeof lint, "ruff check .");
        if (!build[0] && py && strstr(py, "[build-system]")) snprintf(build, sizeof build, "python3 -m build");
        free(py);
    }
    snprintf(p, sizeof p, "%s/Cargo.toml", dir);
    if (access(p, F_OK) == 0) { if (!build[0]) snprintf(build, sizeof build, "cargo build"); if (!test[0]) snprintf(test, sizeof test, "cargo test"); if (!run[0]) snprintf(run, sizeof run, "cargo run"); if (!lint[0]) snprintf(lint, sizeof lint, "cargo clippy"); }
    snprintf(p, sizeof p, "%s/go.mod", dir);
    if (access(p, F_OK) == 0) { if (!build[0]) snprintf(build, sizeof build, "go build ./..."); if (!test[0]) snprintf(test, sizeof test, "go test ./..."); if (!run[0]) snprintf(run, sizeof run, "go run ."); if (!lint[0]) snprintf(lint, sizeof lint, "go vet ./..."); }
    static const char *const sh[][2] = {{"build.sh", "build"}, {"test.sh", "test"}, {"run.sh", "run"}, {"lint.sh", "lint"}, {NULL, NULL}};
    for (int i = 0; sh[i][0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, sh[i][0]);
        if (access(p, F_OK) != 0) { snprintf(p, sizeof p, "%s/scripts/%s", dir, sh[i][0]); if (access(p, F_OK) != 0) continue; }
        char cmd[300]; snprintf(cmd, sizeof cmd, "sh %s", p + strlen(dir) + 1);
        char *dst = i == 0 ? build : i == 1 ? test : i == 2 ? run : lint;
        if (!dst[0]) snprintf(dst, 300, "%s", cmd);
    }
    /* linguagens por quantidade de arquivos */
    ExLang lv[] = {{".c", "C", 0}, {".h", "C", 0}, {".cc", "C++", 0}, {".cpp", "C++", 0}, {".hpp", "C++", 0}, {".py", "Python", 0}, {".js", "JavaScript", 0}, {".mjs", "JavaScript", 0},
        {".ts", "TypeScript", 0}, {".tsx", "TypeScript", 0}, {".jsx", "JavaScript", 0}, {".go", "Go", 0}, {".rs", "Rust", 0}, {".java", "Java", 0}, {".kt", "Kotlin", 0},
        {".rb", "Ruby", 0}, {".php", "PHP", 0}, {".cs", "C#", 0}, {".swift", "Swift", 0}, {".sh", "Shell", 0}, {".lua", "Lua", 0}, {".qml", "QML", 0}, {".html", "HTML", 0}, {".css", "CSS", 0}};
    const int nl = (int)(sizeof lv / sizeof lv[0]);
    int total = 0;
    ex_scan(dir, 0, lv, nl, &total);
    char top[3][40] = {"", "", ""}; int topn[3] = {0, 0, 0};
    for (int i = 0; i < nl; i++) {      /* soma por linguagem e pega as 3 maiores */
        int sum = 0; int first = 1;
        for (int j = 0; j < nl; j++) if (strcmp(lv[j].lang, lv[i].lang) == 0) { if (j < i) first = 0; sum += lv[j].n; }
        if (!first || !sum) continue;
        for (int k = 0; k < 3; k++) if (sum > topn[k]) { for (int m = 2; m > k; m--) { memcpy(top[m], top[m - 1], 40); topn[m] = topn[m - 1]; } snprintf(top[k], 40, "%s", lv[i].lang); topn[k] = sum; break; }
    }
    /* nome e o que e: primeiro paragrafo do README, ate 12 palavras */
    char nm[200]; { char rd[4400]; snprintf(rd, sizeof rd, "%s", dir); const char *b = strrchr(rd, '/'); snprintf(nm, sizeof nm, "%s", b && b[1] ? b + 1 : "project"); }
    char what[300] = "";
    static const char *const rn[] = {"README.md", "README", "readme.md", "README.txt", NULL};
    for (int i = 0; rn[i] && !what[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, rn[i]);
        if (!(txt = mcp_slurp(p, NULL))) continue;
        for (const char *l = txt; *l && !what[0];) {          /* primeira linha de texto que nao seja titulo, emblema, html ou lista */
            const size_t n = strcspn(l, "\n");
            char ln[600]; snprintf(ln, sizeof ln, "%.*s", (int)(n < 599 ? n : 599), l);
            l += n; if (*l) l++;
            br_trim(ln);
            if (!ln[0] || ln[0] == '#' || ln[0] == '[' || ln[0] == '!' || ln[0] == '<' || ln[0] == '-' || ln[0] == '*' || ln[0] == '=' || ln[0] == '`' || ln[0] == '|') continue;
            int w = 0; char *o = what; size_t used = 0;
            for (const char *c = ln; *c && w < 12; ) {
                while (*c == ' ') c++;
                const size_t wl = strcspn(c, " ");
                if (!wl) break;
                if (used + wl + 2 >= sizeof what) break;
                used += (size_t)snprintf(o + used, sizeof what - used, "%s%.*s", w ? " " : "", (int)wl, c);
                w++; c += wl;
            }
            while (used && (what[used - 1] == '.' || what[used - 1] == ',' || what[used - 1] == ':' || what[used - 1] == ';')) what[--used] = 0;
        }
        free(txt);
    }
    if (!what[0]) {
        if (top[0][0]) snprintf(what, sizeof what, "%s project%s%s%s%s", top[0], top[1][0] ? " with " : "", top[1], top[2][0] ? " and " : "", top[2]);
        else snprintf(what, sizeof what, "software project");
    }
    char ln[1200];
    mcp_bput(out, "", 0);
    snprintf(ln, sizeof ln, "# %s: %s\n", nm, what); mcp_bputs(out, ln);
    if (build[0]) { snprintf(ln, sizeof ln, "build: %s\n", build); mcp_bputs(out, ln); }
    if (test[0]) { snprintf(ln, sizeof ln, "test: %s\n", test); mcp_bputs(out, ln); }
    if (run[0]) { snprintf(ln, sizeof ln, "run: %s\n", run); mcp_bputs(out, ln); }
    if (lint[0]) { snprintf(ln, sizeof ln, "lint: %s\n", lint); mcp_bputs(out, ln); }
    /* layout: pastas do topo com um palpite do que guardam */
    {
        char *dn[40]; const int n = env_names(dir, dn, 40, 1, 0);
        int shown = 0;
        for (int i = 0; i < n && i < 40; i++) {
            size_t l = strlen(dn[i]); if (l) dn[i][l - 1] = 0;      /* tira a barra final */
            if (dn[i][0] != '.' && shown < 12) {
                char sub[2400]; snprintf(sub, sizeof sub, "%s/%s", dir, dn[i]);
                const char *g = ex_guess_dir(dn[i]);
                char guess[120] = "";
                if (g) snprintf(guess, sizeof guess, "%s", g);
                else {
                    ExLang lv2[] = {{".c", "C", 0}, {".h", "C", 0}, {".cpp", "C++", 0}, {".py", "Python", 0}, {".js", "JavaScript", 0}, {".ts", "TypeScript", 0}, {".go", "Go", 0}, {".rs", "Rust", 0}, {".java", "Java", 0}, {".md", "Markdown", 0}, {".sh", "Shell", 0}, {".qml", "QML", 0}, {".json", "JSON", 0}, {".html", "HTML", 0}};
                    int t2 = 0, best = -1; const int n2 = (int)(sizeof lv2 / sizeof lv2[0]);
                    ex_scan(sub, 2, lv2, n2, &t2);
                    for (int k = 0; k < n2; k++) if (lv2[k].n > 0 && (best < 0 || lv2[k].n > lv2[best].n)) best = k;
                    snprintf(guess, sizeof guess, best >= 0 ? "%s files" : "(fill in)", best >= 0 ? lv2[best].lang : "");
                }
                if (!shown) mcp_bputs(out, "layout:\n");
                snprintf(ln, sizeof ln, "  %s/  %s\n", dn[i], guess); mcp_bputs(out, ln); shown++;
            }
            free(dn[i]);
        }
    }
    if (test[0]) { snprintf(ln, sizeof ln, "done: `%s` passes\n", test); mcp_bputs(out, ln); }
    /* docs: README e arquivos de docs/ */
    {
        int shown = 0;
        for (int i = 0; rn[i]; i++) { snprintf(p, sizeof p, "%s/%s", dir, rn[i]); if (access(p, F_OK) == 0) { snprintf(ln, sizeof ln, "docs:\n  %s  what the project is and how to use it\n", rn[i]); mcp_bputs(out, ln); shown = 1; break; } }
        snprintf(p, sizeof p, "%s/docs", dir);
        DIR *dd = opendir(p);
        if (dd) {
            struct dirent *e; int c = 0;
            while ((e = readdir(dd)) != NULL && c < 4) {
                const size_t l = strlen(e->d_name);
                if (l < 4 || strcmp(e->d_name + l - 3, ".md") != 0 || e->d_name[0] == '.') continue;
                if (!shown) { mcp_bputs(out, "docs:\n"); shown = 1; }
                snprintf(ln, sizeof ln, "  docs/%s  read when working on this topic\n", e->d_name); mcp_bputs(out, ln); c++;
            }
            closedir(dd);
        }
    }
    mcp_bputs(out, "# add rules:, never:, ask:, style: (see docs/extending.md in the katana repository)\n");
}
/* katana init [pasta] [--force] */
static int init_cli(int argc, char **argv) {
    const char *dir = "."; int force = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--force")) force = 1; else dir = argv[i]; }
    char rd[4400];
    if (!realpath(dir, rd) || !ex_isdir(rd)) { fprintf(stderr, "init: %s is not a folder\n", dir); return 2; }
    char f[4500]; snprintf(f, sizeof f, "%s/KATANA.md", rd);
    if (access(f, F_OK) == 0 && !force) { fprintf(stderr, "init: %s already exists (use --force to overwrite it)\n", f); return 1; }
    McpB b = {0};
    ex_init_build(rd, &b);
    if (!ex_write(f, b.p, 0)) { fprintf(stderr, "init: cannot write %s\n", f); free(b.p); return 1; }
    printf("wrote %s (%zu characters):\n%s", f, b.n, b.p);
    free(b.p);
    return 0;
}

/* ======== skills ======== */
static const char *ex_skill_root(int global, char *out, size_t cap) {
    char cwd[3000], home[1000];
    agent_home(home, sizeof home);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (global || !cwd[0]) snprintf(out, cap, "%s/.katana/skills", home); else snprintf(out, cap, "%s/.katana/skills", cwd);
    return out;
}
/* cria a skill; devolve o caminho do SKILL.md em path (cap pcap); 0 = ok, senao a mensagem em err */
static int ex_skill_new(const char *name, int global, const char *desc, char *path, size_t pcap, char *err, size_t ecap) {
    for (const char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_')) { snprintf(err, ecap, "invalid skill name '%s': use letters, digits, - and _", name); return 1; }
    if (!*name || strlen(name) > 60) { snprintf(err, ecap, "the skill name must have 1 to 60 characters"); return 1; }
    char root[1200], dir[1400];
    ex_skill_root(global, root, sizeof root);
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    snprintf(path, pcap, "%s/SKILL.md", dir);
    if (access(path, F_OK) == 0) { snprintf(err, ecap, "%s already exists", path); return 1; }
    McpB b = {0};
    mcp_bput(&b, "", 0);
    char ln[900];
    snprintf(ln, sizeof ln, "---\nname: %s\nwhen: %s\nneeds: \nargs: \n---\n", name, desc && *desc ? desc : "describe in one line the situation in which to use this skill");
    mcp_bputs(&b, ln);
    mcp_bputs(&b,
        "# A skill defines an ability and what has to be done to run it. Lines starting with # are notes for you and are not shown to the model.\n"
        "# needs: programs, tools or MCP servers it requires, comma separated; the engine checks them when the skill loads.\n"
        "# args: named inputs; the model passes them as name=value and $name inside a step is replaced.\n"
        "# Steps: imperative, 120 characters at most, 9 at most; after => say what shows that the step worked.\n"
        "steps:\n"
        "1. Read the files the task touches => you can name the function or section to change\n"
        "2. Make the change => the edit applied without error\n"
        "3. Run the checks of the project => they pass\n"
        "done: the change is in place and the checks pass\n"
        "report: what changed, in which files, and the result of the checks\n"
        "fail: stop at the failing step, say which check failed and what you saw\n");
    const int ok = ex_write(path, b.p, 0);
    free(b.p);
    if (!ok) { snprintf(err, ecap, "cannot write %s", path); return 1; }
    return 0;
}
static const char *const EX_FILLER[] = {"the", "a", "an", "you", "we", "i", "then", "it", "this", "is", "are", NULL};
/* lint de uma skill; devolve erros; escreve achados em f */
static int ex_skill_check(const SkInfo *k, McpB *f) {
    char path[1500]; snprintf(path, sizeof path, "%s/SKILL.md", k->base);
    char *txt = sk_slurp(path);
    if (!txt) return 0;
    char nm[100], ds[300];
    const char *body = sk_front(txt, nm, sizeof nm, ds, sizeof ds);
    SkAbility *a = (SkAbility *)malloc(sizeof *a);
    sk_ability(txt, body, a);
    int errs = 0;
    char m[700];
#define SF(sev, ...) do { snprintf(m, sizeof m, __VA_ARGS__); mcp_bputs(f, k->name); mcp_bputs(f, ": " sev ": "); mcp_bputs(f, m); mcp_bputs(f, "\n"); if (sev[0] == 'e') errs++; } while (0)
    if (!ds[0]) SF("error", "missing 'when:' in the header (one line: the situation in which to use it)");
    else if (strncmp(ds, "describe in one line", 20) == 0) SF("error", "'when:' still has the template text");
    if (!a->ability) SF("warning", "no 'steps:' section: it loads as free text, no plan is written from it");
    else {
        if (a->nsteps > 9) SF("error", "%d steps; 9 at most (split the skill)", a->nsteps);
        for (int i = 0; i < a->nsteps; i++) {
            if (strlen(a->step[i]) > 120) SF("error", "step %d is over 120 characters", i + 1);
            char w[40]; size_t wl = strcspn(a->step[i], " "); snprintf(w, sizeof w, "%.*s", (int)(wl < 39 ? wl : 39), a->step[i]);
            int bad = wl > 4 && wl < 39 && strcmp(w + wl - 3, "ing") == 0;
            for (int q = 0; EX_FILLER[q]; q++) if (strcasecmp(w, EX_FILLER[q]) == 0) bad = 1;
            if (bad) SF("error", "step %d does not start with an imperative verb: %.40s", i + 1, a->step[i]);
        }
        if (!a->done[0]) SF("warning", "no 'done:' line (the observable end state)");
        if (!a->nsteps) SF("error", "'steps:' has no numbered step");
    }
    if (a->needs[0]) {
        char buf[300]; snprintf(buf, sizeof buf, "%s", a->needs);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            size_t l = strlen(t); while (l && t[l - 1] == ' ') t[--l] = 0;
            if (!*t || strstr(a->argdecl, t)) continue;
            int ok = sk_tool_exists(t) || mcp_which(t);
            if (!ok) { mcp_init(0); for (int i = 0; i < g_nmcp; i++) if (strcmp(g_mcp[i].name, t) == 0) ok = 1; }
            if (!ok) SF("error", "'needs:' names '%s', which is not a program on PATH, a tool or a connected MCP server", t);
        }
    }
#undef SF
    free(a); free(txt);
    return errs;
}
static int skill_cli(int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "list";
    static SkInfo v[200];
    if (!strcmp(sub, "new")) {
        if (argc < 2) { fprintf(stderr, "uso: katana skill new <name> [--global] [--description TEXT]\n"); return 2; }
        int global = 0; const char *desc = "";
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--global")) global = 1; else if (!strcmp(argv[i], "--description") && i + 1 < argc) desc = argv[++i]; }
        char path[1500], err[400];
        if (ex_skill_new(argv[1], global, desc, path, sizeof path, err, sizeof err)) { fprintf(stderr, "skill new: %s\n", err); return 1; }
        printf("created %s\nedit it: fill in when, steps, done, report and fail; check it with: katana skill check %s\n", path, argv[1]);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        const int n = sk_collect(v, 200);
        if (!n) { printf("no skills (create one: katana skill new <name>)\n"); return 0; }
        for (int i = 0; i < n; i++) printf("%-24s %-8s %s\n", v[i].name, v[i].origin ? "global" : "project", v[i].desc);
        return 0;
    }
    if (!strcmp(sub, "path")) {
        char r[4][1100]; const int n = sk_roots(r);
        for (int i = 0; i < n; i++) printf("%s%s\n", r[i], ex_isdir(r[i]) ? "" : "  (does not exist)");
        return 0;
    }
    if (!strcmp(sub, "show") || !strcmp(sub, "remove")) {
        if (argc < 2) { fprintf(stderr, "uso: katana skill %s <name>\n", sub); return 2; }
        const int n = sk_collect(v, 200);
        for (int i = 0; i < n; i++) if (!strcmp(v[i].name, argv[1])) {
            if (!strcmp(sub, "remove")) { if (!ex_rmtree(v[i].base)) { fprintf(stderr, "skill remove: could not remove %s\n", v[i].base); return 1; } printf("removed %s\n", v[i].base); return 0; }
            char p[1500]; snprintf(p, sizeof p, "%s/SKILL.md", v[i].base);
            char *t = sk_slurp(p);
            printf("%s\n%s", p, t ? t : "");
            free(t);
            return 0;
        }
        fprintf(stderr, "skill %s: no skill named '%s'\n", sub, argv[1]);
        return 1;
    }
    if (!strcmp(sub, "check")) {
        const int n = sk_collect(v, 200);
        McpB f = {0}; mcp_bput(&f, "", 0);
        int errs = 0, seen = 0;
        for (int i = 0; i < n; i++) if (argc < 2 || !strcmp(v[i].name, argv[1])) { errs += ex_skill_check(&v[i], &f); seen++; }
        if (argc >= 2 && !seen) { fprintf(stderr, "skill check: no skill named '%s'\n", argv[1]); free(f.p); return 1; }
        printf("%s%d skill(s) checked, %d error(s)\n", f.p, seen, errs);
        free(f.p);
        return errs ? 1 : 0;
    }
    fprintf(stderr, "uso: katana skill new|list|show|remove|path|check\n");
    return 2;
}

/* ======== MCP: configuracao, cartao, teste, comandos ======== */
/* reescreve o arquivo de configuracao: acrescenta, troca ou remove (entry NULL) o servidor name; 1 se mudou algo */
static int mcpcfg_edit(const char *path, const char *name, const char *entry, int *existed) {
    char *txt = mcp_slurp(path, NULL);
    char keys[64][130]; char *raws[64]; int n = 0, found = 0;
    McpB other = {0};      /* outras chaves do nivel de cima, compactas */
    mcp_bput(&other, "", 0);
    const char *root = txt ? strchr(txt, '{') : NULL;
    if (root) {
        char k[128]; const char *v, *np;
        for (const char *p = root + 1; (np = mcp_member(p, k, sizeof k, &v)) != NULL; p = np) {
            if (!strcmp(k, "mcpServers") && *v == '{') {
                char k2[128]; const char *v2, *np2;
                for (const char *q = v + 1; (np2 = mcp_member(q, k2, sizeof k2, &v2)) != NULL && n < 64; q = np2) {
                    const char *e = mcp_skip(v2);
                    snprintf(keys[n], sizeof keys[n], "%s", k2);
                    raws[n] = (char *)malloc((size_t)(e - v2) + 1); memcpy(raws[n], v2, (size_t)(e - v2)); raws[n][e - v2] = 0;
                    n++;
                }
            } else { const char *e = mcp_skip(v); if (e) { mcp_bputs(&other, ",\n  "); mcp_besc(&other, k); mcp_bputs(&other, ": "); mcp_bput(&other, v, (size_t)(e - v)); } }
        }
    }
    free(txt);
    int idx = -1;
    for (int i = 0; i < n; i++) if (!strcmp(keys[i], name)) idx = i;
    found = idx >= 0;
    if (existed) *existed = found;
    if (!entry && !found) { for (int i = 0; i < n; i++) free(raws[i]); free(other.p); return 0; }
    if (entry) { if (found) { free(raws[idx]); raws[idx] = strdup(entry); } else if (n < 64) { snprintf(keys[n], sizeof keys[n], "%s", name); raws[n++] = strdup(entry); } }
    else { free(raws[idx]); for (int i = idx; i + 1 < n; i++) { memcpy(keys[i], keys[i + 1], sizeof keys[0]); raws[i] = raws[i + 1]; } n--; }
    McpB o = {0};
    mcp_bputs(&o, "{\n  \"mcpServers\": {");
    for (int i = 0; i < n; i++) { mcp_bputs(&o, i ? ",\n    " : "\n    "); mcp_besc(&o, keys[i]); mcp_bputs(&o, ": "); mcp_bputs(&o, raws[i]); free(raws[i]); }
    mcp_bputs(&o, n ? "\n  }" : "}");
    mcp_bputs(&o, other.p); mcp_bputs(&o, "\n}\n");
    const int ok = ex_write(path, o.p, 1);
    free(o.p); free(other.p);
    return ok;
}
static void ex_user_cfg(char *out, size_t cap) { mcp_user_path(out, cap); }
static void ex_proj_cfg_new(char *out, size_t cap) { char cwd[3000]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "."); snprintf(out, cap, "%s/.katana/mcp.json", cwd); }
static int mcp_add_cli(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana mcp add <name> [--project] [--env K=V]... [--prefix P] (--url URL [--header 'K: V']... | -- command args...)\n"); return 2; }
    const char *name = argv[0], *url = NULL, *prefix = NULL; int project = 0, cmdi = -1;
    const char *envs[32], *hdrs[32]; int ne = 0, nh = 0;
    for (const char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_')) { fprintf(stderr, "mcp add: invalid name '%s' (letters, digits, - and _)\n", name); return 2; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { cmdi = i + 1; break; }
        else if (!strcmp(argv[i], "--project")) project = 1;
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc && ne < 32) envs[ne++] = argv[++i];
        else if (!strcmp(argv[i], "--header") && i + 1 < argc && nh < 32) hdrs[nh++] = argv[++i];
        else { fprintf(stderr, "mcp add: unknown option %s\n", argv[i]); return 2; }
    }
    if ((cmdi < 0 || cmdi >= argc) && !url) { fprintf(stderr, "mcp add: give a command after -- or --url URL\n"); return 2; }
    McpB e = {0};
    mcp_bputs(&e, "{");
    if (url) { mcp_bputs(&e, "\"url\": "); mcp_besc(&e, url); }
    else {
        mcp_bputs(&e, "\"command\": "); mcp_besc(&e, argv[cmdi]);
        mcp_bputs(&e, ", \"args\": [");
        for (int i = cmdi + 1; i < argc; i++) { if (i > cmdi + 1) mcp_bputs(&e, ", "); mcp_besc(&e, argv[i]); }
        mcp_bputs(&e, "]");
    }
    if (ne) {
        mcp_bputs(&e, ", \"env\": {");
        for (int i = 0; i < ne; i++) { const char *eq = strchr(envs[i], '='); if (!eq) { fprintf(stderr, "mcp add: --env wants K=V\n"); free(e.p); return 2; } char k[100]; snprintf(k, sizeof k, "%.*s", (int)(eq - envs[i]), envs[i]); if (i) mcp_bputs(&e, ", "); mcp_besc(&e, k); mcp_bputs(&e, ": "); mcp_besc(&e, eq + 1); }
        mcp_bputs(&e, "}");
    }
    if (nh) {
        mcp_bputs(&e, ", \"headers\": {");
        for (int i = 0; i < nh; i++) { const char *co = strchr(hdrs[i], ':'); if (!co) { fprintf(stderr, "mcp add: --header wants 'K: V'\n"); free(e.p); return 2; } char k[100]; snprintf(k, sizeof k, "%.*s", (int)(co - hdrs[i]), hdrs[i]); const char *v = co + 1; while (*v == ' ') v++; if (i) mcp_bputs(&e, ", "); mcp_besc(&e, k); mcp_bputs(&e, ": "); mcp_besc(&e, v); }
        mcp_bputs(&e, "}");
    }
    if (prefix) { mcp_bputs(&e, ", \"prefix\": "); mcp_besc(&e, prefix); }
    mcp_bputs(&e, "}");
    char path[4300];
    if (project) ex_proj_cfg_new(path, sizeof path); else ex_user_cfg(path, sizeof path);
    int existed = 0;
    if (!mcpcfg_edit(path, name, e.p, &existed)) { fprintf(stderr, "mcp add: cannot write %s\n", path); free(e.p); return 1; }
    free(e.p);
    printf("%s server '%s' in %s\n", existed ? "replaced" : "added", name, path);
    if (project) {      /* quem escreveu o arquivo ja confia nele */
        size_t tn; char *t = mcp_slurp(path, &tn); char hex[72];
        if (t) { sha_hex(t, tn, hex); if (!mcp_trusted(path, hex)) mcp_trust_add(path, hex); free(t); }
        if (ne || nh) printf("warning: this file is inside the project and may be committed; keep secrets (env values, headers) in your own file instead\n");
    }
    /* arquivo de uso: o que o modelo ve em first:/avoid: */
    char up[4400]; { char d[4300]; snprintf(d, sizeof d, "%s", path); char *sl = strrchr(d, '/'); if (sl) *sl = 0; snprintf(up, sizeof up, "%s/mcp/%s.md", d, name); }
    if (access(up, F_OK) != 0 && ex_write(up, "what: \nfirst: \norder: \navoid: \n", 0)) printf("created the usage file %s with empty keys (what, first, order, avoid: one line each); the model sees them in the server's card\n", up);
    printf("check it: katana mcp test %s\n", name);
    return 0;
}
static int mcp_remove_cli(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana mcp remove <name> [--project]\n"); return 2; }
    int project = argc > 1 && !strcmp(argv[1], "--project");
    char path[4300];
    if (project) ex_proj_cfg_new(path, sizeof path); else ex_user_cfg(path, sizeof path);
    int existed = 0;
    if (!mcpcfg_edit(path, argv[0], NULL, &existed) || !existed) {
        char other[4300];
        if (!project) ex_proj_cfg_new(other, sizeof other); else ex_user_cfg(other, sizeof other);
        int e2 = 0; char *t = mcp_slurp(other, NULL);
        if (t) { e2 = strstr(t, argv[0]) != NULL; free(t); }
        fprintf(stderr, "mcp remove: no server '%s' in %s%s\n", argv[0], path, e2 ? (project ? " (it may be in your own file: run without --project)" : " (it may be in the project file: add --project)") : "");
        return 1;
    }
    printf("removed server '%s' from %s\n", argv[0], path);
    return 0;
}
static void mcp_list_print(int tools) {
    mcp_init(0);
    if (!g_nmcp) { printf("No MCP servers configured (katana mcp add <name> -- <command> | --url <url>).\n"); return; }
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        printf("%s [%s] %s  (from %s%s)\n", s->name, mcpx_kind(s), s->alive ? "ok" : "unavailable", s->project ? "project file " : "your file ", s->source);
        if (!s->alive) { char pe[400]; mcp_plain_err(s, pe, sizeof pe); printf("  error: %s\n", pe); continue; }
        printf("  server: %s, protocol %s, %d tools%s\n", s->info, s->proto, s->ntools, s->nprompts ? ", prompts: /name:prompt" : "");
        if (tools) for (int j = 0; j < s->ntools; j++) {
            char d[110], fn[260]; snprintf(d, sizeof d, "%.100s", s->tools[j].desc);
            char *nl = strchr(d, '\n'); if (nl) *nl = 0;
            mcp_fname(s, s->tools[j].name, fn, sizeof fn);
            char an[200]; mcpx_ann_text(s, j, an, sizeof an);
            printf("  %s%s%s%s: %s\n", fn, an[0] ? " [" : "", an, an[0] ? "]" : "", d);
            if (s->tools[j].title && s->tools[j].title[0]) printf("      title: %s\n", s->tools[j].title);
        }
    }
}
static int mcp_test_cli(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana mcp test <name>\n"); return 2; }
    mcp_load_config();
    const int i = mcp_find(argv[0]);
    if (i < 0) { fprintf(stderr, "mcp test: no server '%s' (see: katana mcp list)\n", argv[0]); return 1; }
    McpSrv *s = &g_mcp[i];
    atexit(mcp_shutdown);
    printf("%s [%s] from %s\n", s->name, mcpx_kind(s), s->source);
    if (!s->cmd && !s->url) { printf("  FAILED: %s\n", s->err); return 1; }
    mcp_start(s);
    if (!s->alive) { char pe[400]; mcp_plain_err(s, pe, sizeof pe); printf("  FAILED: %s\n", pe); return 1; }
    printf("  connected in %.2f s: %s, protocol %s\n", s->t_start, s->info, s->proto);
    printf("  tools: %d", s->ntools);
    for (int j = 0; j < s->ntools && j < 12; j++) printf("%s%s", j ? ", " : " (", s->tools[j].name);
    printf("%s\n", s->ntools ? ")" : "");
    MrOut ro = {(char *)malloc(65536), 65536, 0, 0}; ro.o[0] = 0;
    const int nres = mr_list_one(s, &ro);
    printf("  resources: %s\n", nres < 0 ? "none offered" : "listed");
    if (nres > 0) printf("    %d resource(s)\n", nres);
    free(ro.o);
    printf("  prompts: %d%s\n", s->nprompts, s->nprompts ? " (use them in the agent as /name:prompt)" : "");
    printf("  OK\n");
    return 0;
}
static int mcp_card_cli(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana mcp card <name>\n"); return 2; }
    mcp_load_config();
    const int i = mcp_find(argv[0]);
    if (i < 0) { fprintf(stderr, "mcp card: no server '%s'\n", argv[0]); return 1; }
    atexit(mcp_shutdown);
    mcp_start(&g_mcp[i]);
    if (!g_mcp[i].alive) { char pe[400]; mcp_plain_err(&g_mcp[i], pe, sizeof pe); fprintf(stderr, "mcp card: %s is unavailable: %s\n", argv[0], pe); return 1; }
    McpB b = {0}; mcp_bput(&b, "", 0);
    mcp_card(&g_mcp[i], 1, &b);
    printf("%s[%zu characters]\n", b.p, b.n);
    free(b.p);
    return 0;
}
static int mcp_cli(int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "list";
    if (!strcmp(sub, "add")) return mcp_add_cli(argc - 1, argv + 1);
    if (!strcmp(sub, "remove")) return mcp_remove_cli(argc - 1, argv + 1);
    if (!strcmp(sub, "test")) return mcp_test_cli(argc - 1, argv + 1);
    if (!strcmp(sub, "card")) return mcp_card_cli(argc - 1, argv + 1);
    if (!strcmp(sub, "list")) { mcp_list_print(1); return 0; }
    fprintf(stderr, "uso: katana mcp list | add | remove | test | card\n");
    return 2;
}
/* /mcp: estado de cada servidor */
static void mcp_status_print(void) {
    mcp_init(0);
    if (!g_nmcp) { fprintf(stderr, "[no MCP servers configured: katana mcp add <name> -- <command>]\n"); return; }
    for (int i = 0; i < g_nmcp; i++) {
        McpSrv *s = &g_mcp[i];
        char pe[400] = ""; if (!s->alive || s->err[0]) mcp_plain_err(s, pe, sizeof pe);
        fprintf(stderr, "  %s [%s] %s, %d tools, %d prompts%s%s  (%s)\n", s->name, mcpx_kind(s), s->alive ? "connected" : "NOT connected", s->ntools, s->nprompts, pe[0] ? ", last error: " : "", pe, s->source);
    }
}

/* ======== comandos de barra do agente ======== */
static int ex_builtin(const char *n) {
    static const char *const B[] = {"new", "system", "save", "help", "exit", "compact", "session", "resume", "plan", "go", "undo", "cd", "teacher", "init", "skill", "mcp", NULL};
    for (int i = 0; B[i]; i++) if (!strcmp(B[i], n)) return 1;
    return 0;
}
static int ex_prompt_cmd(const char *line, int *handled);      /* abaixo */
/* 0 = nao e meu; 1 = tratado (nada vai ao modelo); 3 = g_ext_msg e a mensagem que o modelo recebe */
static int ext_slash(const char *line) {
    if (line[0] != '/' || !g_agent_sys) return 0;
    char w[80]; const size_t wl = strcspn(line + 1, " \t");
    snprintf(w, sizeof w, "%.*s", (int)(wl < 79 ? wl : 79), line + 1);
    const char *rest = line + 1 + wl; while (*rest == ' ') rest++;
    if (!strcmp(w, "init")) {
        char cwd[4200]; if (!getcwd(cwd, sizeof cwd)) return 1;
        char f[4300]; snprintf(f, sizeof f, "%s/KATANA.md", cwd);
        const int force = strstr(rest, "--force") != NULL;
        if (access(f, F_OK) == 0 && !force) { fprintf(stderr, "[/init: %s already exists; /init --force overwrites it]\n", f); return 1; }
        McpB b = {0}; ex_init_build(cwd, &b);
        if (!ex_write(f, b.p, 0)) { fprintf(stderr, "[/init: cannot write %s]\n", f); free(b.p); return 1; }
        fprintf(stderr, "%s[/init: wrote %s (%zu characters); the model will now improve it]%s\n", CE(C_DIM), f, b.n, CE(C_RESET));
        snprintf(g_ext_msg, sizeof g_ext_msg,
            "I just generated a first KATANA.md in this folder from what could be detected. Improve it by reading the project (README, build files, a few source files). "
            "Keep the brief format exactly: a first line '# name: what it is, 12 words at most', then keys at the start of a line: build:, test:, run:, lint:, done: (one value each), "
            "and blocks layout: (12 lines at most, '  path  what it holds'), rules: and style: (lines '  - imperative sentence', 100 characters at most), never: and ask: (lines '  - action `glob pattern`'), docs: ('  path  when to read it'). "
            "No prose, no hedging, only facts you verified; correct any wrong command. Only suggest never:/ask: items for dangerous actions you saw in the project. "
            "Write the result with Write to KATANA.md, then run `katana brief check` with Bash and fix what it reports. Current file:\n%s", b.p);
        free(b.p);
        return 3;
    }
    if (!strcmp(w, "skill")) {
        char sub[40] = ""; const size_t sl = strcspn(rest, " \t"); snprintf(sub, sizeof sub, "%.*s", (int)(sl < 39 ? sl : 39), rest);
        const char *a = rest + sl; while (*a == ' ') a++;
        if (!strcmp(sub, "new")) {
            char nm[100]; const size_t nl = strcspn(a, " \t"); snprintf(nm, sizeof nm, "%.*s", (int)(nl < 99 ? nl : 99), a);
            const char *what = a + nl; while (*what == ' ') what++;
            char path[1500], err[400];
            if (!nm[0]) { fprintf(stderr, "[usage: /skill new <name> <what it should do>]\n"); return 1; }
            if (ex_skill_new(nm, 0, what, path, sizeof path, err, sizeof err)) { fprintf(stderr, "[/skill new: %s]\n", err); return 1; }
            fprintf(stderr, "%s[/skill new: created %s]%s\n", CE(C_DIM), path, CE(C_RESET));
            snprintf(g_ext_msg, sizeof g_ext_msg,
                "I created the skill '%s' at %s from a template. Write it properly with Write, in this format: a header between --- lines with name, when (one line: the situation in which to use it), needs (programs, tools or MCP servers it requires) and args (named inputs); "
                "then 'steps:' with at most 9 numbered imperative steps of 120 characters at most, each 'action => check that shows it worked', then 'done:', 'report:' and 'fail:' lines. "
                "Base it on what I say it should do, and look at the project to make it concrete. When written, run `katana skill check %s` with Bash and fix what it reports. What the skill should do: %s",
                nm, path, nm, what[0] ? what : "(I did not say: ask me one short question first)");
            return 3;
        }
        char *argv2[4]; int n2 = 0; char buf[1200]; snprintf(buf, sizeof buf, "%s", a);
        argv2[n2++] = sub[0] ? sub : (char *)"list";
        for (char *t = strtok(buf, " "); t && n2 < 4; t = strtok(NULL, " ")) argv2[n2++] = t;
        fflush(stdout);
        const int sv = dup(1); dup2(2, 1);       /* a saida do comando vai para o terminal do usuario, nao para o canal da interface */
        skill_cli(n2, argv2);
        fflush(stdout); dup2(sv, 1); close(sv);
        return 1;
    }
    if (!strcmp(w, "mcp")) {
        char sub[40] = ""; const size_t sl = strcspn(rest, " \t"); snprintf(sub, sizeof sub, "%.*s", (int)(sl < 39 ? sl : 39), rest);
        const char *a = rest + sl; while (*a == ' ') a++;
        if (!strcmp(sub, "reconnect")) {
            mcp_init(0);
            char nm[100]; snprintf(nm, sizeof nm, "%s", a); br_trim(nm);
            const int i = mcp_find(nm);
            if (i < 0) { fprintf(stderr, "[/mcp reconnect: no server '%s']\n", nm); return 1; }
            mcp_restart(&g_mcp[i]);
            if (g_mcp[i].alive) fprintf(stderr, "[mcp %s: reconnected, %d tools (the system prompt keeps the old card until /new)]\n", nm, g_mcp[i].ntools);
            else { char pe[400]; mcp_plain_err(&g_mcp[i], pe, sizeof pe); fprintf(stderr, "[mcp %s: still unavailable: %s]\n", nm, pe); }
            return 1;
        }
        if (!strcmp(sub, "card")) {
            mcp_init(0);
            const int i = mcp_find(a);
            if (i < 0) { fprintf(stderr, "[/mcp card: no server '%s']\n", a); return 1; }
            McpB b = {0}; mcp_bput(&b, "", 0); mcp_card(&g_mcp[i], 1, &b);
            fprintf(stderr, "%s[%zu characters]\n", b.p, b.n); free(b.p);
            return 1;
        }
        mcp_status_print();
        return 1;
    }
    if (strchr(w, ':')) { int handled = 0; const int r = ex_prompt_cmd(line, &handled); if (handled) return r; }
    if (!ex_builtin(w)) {       /* /nome argumentos: uma skill com esse nome */
        static SkInfo v[200];
        const int n = sk_collect(v, 200);
        for (int i = 0; i < n; i++) if (!strcmp(v[i].name, w)) {
            McpB js = {0};
            mcp_bputs(&js, "{\"skill\": "); mcp_besc(&js, w); mcp_bputs(&js, ", \"args\": "); mcp_besc(&js, rest); mcp_bputs(&js, "}");
            static char res[14000];
            ct_skill(js.p, js.p + js.n, res, sizeof res);
            free(js.p);
            snprintf(g_ext_msg, sizeof g_ext_msg, "[The user ran the skill '%s'. Do it now, following its instructions.]\n%s%s%s", w, res, rest[0] ? "\n\nThe user's words: " : "", rest);
            fprintf(stderr, "%s[skill %s loaded]%s\n", CE(C_DIM), w, CE(C_RESET));
            return 3;
        }
    }
    return 0;
}
/* /servidor:prompt argumentos -> mensagem do usuario ao modelo (so o usuario chama; o modelo nao ve os prompts) */
static int ex_prompt_cmd(const char *line, int *handled) {
    char sv[60], pn[100];
    const char *c = line + 1; const size_t sl = strcspn(c, ":");
    snprintf(sv, sizeof sv, "%.*s", (int)(sl < 59 ? sl : 59), c);
    c += sl + 1;
    const size_t pl = strcspn(c, " \t"); snprintf(pn, sizeof pn, "%.*s", (int)(pl < 99 ? pl : 99), c);
    const char *rest = c + pl; while (*rest == ' ') rest++;
    mcp_init(0);
    const int si = mcp_find(sv);
    if (si < 0) return 0;
    *handled = 1;
    McpSrv *s = &g_mcp[si];
    mcpx_refresh(s, NULL);        /* prompts/list_changed recebido: lista atual antes de procurar o prompt */
    McpPrompt *pp = NULL;
    for (int i = 0; i < s->nprompts; i++) if (!strcmp(s->prompts[i].name, pn)) pp = &s->prompts[i];
    if (!pp) {
        fprintf(stderr, "[%s has %d prompt(s)%s", s->name, s->nprompts, s->nprompts ? ": " : "");
        for (int i = 0; i < s->nprompts; i++) fprintf(stderr, "%s%s", i ? ", " : "", s->prompts[i].name);
        fprintf(stderr, "; use /%s:<prompt> arguments]\n", s->name);
        return 1;
    }
    McpB ar = {0};
    mcp_bputs(&ar, "{\"name\": "); mcp_besc(&ar, pp->name); mcp_bputs(&ar, ", \"arguments\": {");
    char vals[8][400]; int have[8] = {0};
    /* k=v nomeado; o resto, em ordem, nos argumentos que faltam; um unico argumento recebe o texto todo */
    if (pp->nargs == 1 && !strchr(rest, '=')) { snprintf(vals[0], sizeof vals[0], "%s", rest); have[0] = rest[0] != 0; }
    else {
        char buf[1200]; snprintf(buf, sizeof buf, "%s", rest);
        int pos = 0;
        for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
            char *eq = strchr(t, '='); int k = -1;
            if (eq) { for (int i = 0; i < pp->nargs; i++) if ((size_t)(eq - t) == strlen(pp->argn[i]) && !strncmp(t, pp->argn[i], (size_t)(eq - t))) k = i; }
            if (k >= 0) { snprintf(vals[k], sizeof vals[k], "%s", eq + 1); have[k] = 1; pos = k + 1; continue; }
            while (pos < pp->nargs && have[pos]) pos++;
            if (pos < pp->nargs) { snprintf(vals[pos], sizeof vals[pos], "%s", t); have[pos++] = 1; }
            else if (pp->nargs) { size_t l = strlen(vals[pp->nargs - 1]); snprintf(vals[pp->nargs - 1] + l, sizeof vals[0] - l, " %s", t); }
        }
    }
    int first = 1;
    for (int i = 0; i < pp->nargs; i++) {
        if (!have[i]) { if (pp->argreq[i]) { fprintf(stderr, "[/%s:%s needs the argument '%s']\n", s->name, pp->name, pp->argn[i]); free(ar.p); return 1; } continue; }
        if (!first) mcp_bputs(&ar, ", ");
        first = 0; mcp_besc(&ar, pp->argn[i]); mcp_bputs(&ar, ": "); mcp_besc(&ar, vals[i]);
    }
    mcp_bputs(&ar, "}}");
    if (!s->alive) { fprintf(stderr, "[mcp %s is not connected: /mcp reconnect %s]\n", s->name, s->name); free(ar.p); return 1; }
    char *r = mcp_request(s, "prompts/get", ar.p, mcp_call_ms());
    free(ar.p);
    if (!r) { fprintf(stderr, "[/%s:%s failed: %s]\n", s->name, pp->name, s->err); return 1; }
    char em[300];
    if (mcp_errmsg(r, em, sizeof em)) { fprintf(stderr, "[/%s:%s: the server said: %s]\n", s->name, pp->name, em); free(r); return 1; }
    const char *res = mcp_get(strchr(r, '{'), "result"), *ms = res ? mcp_get(res, "messages") : NULL;
    McpB t = {0};
    mcp_bput(&t, "", 0);
    if (ms && *ms == '[') {
        const char *v, *np;
        for (const char *p = ms + 1; (np = mcp_elem(p, &v)) != NULL; p = np) {
            const char *ro = mcp_get(v, "role"), *ct = mcp_get(v, "content");
            if (t.n) mcp_bputs(&t, "\n\n");
            if (ro && !strncmp(ro, "\"assistant\"", 11)) mcp_bputs(&t, "[earlier assistant turn] ");
            if (ct && *ct == '{') mcp_block(ct, &t);
            else if (ct && *ct == '[') { const char *v2, *n2; for (const char *q = ct + 1; (n2 = mcp_elem(q, &v2)) != NULL; q = n2) { mcp_block(v2, &t); mcp_bputs(&t, "\n"); } }
        }
    }
    free(r);
    if (!t.n) { fprintf(stderr, "[/%s:%s returned no messages]\n", s->name, pp->name); free(t.p); return 1; }
    snprintf(g_ext_msg, sizeof g_ext_msg, "[The user ran the prompt '%s' of the MCP server '%s'; its text follows. Do what it asks.]\n%.15000s", pp->name, s->name, t.p);
    free(t.p);
    fprintf(stderr, "%s[prompt %s:%s loaded]%s\n", CE(C_DIM), s->name, pp->name, CE(C_RESET));
    return 3;
}
