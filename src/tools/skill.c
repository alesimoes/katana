/* ---------- ferramenta Skill ----------
 * Uma skill e uma pasta com SKILL.md (cabecalho opcional entre linhas "---" com name e description, depois as instrucoes).
 * Busca, em ordem: <pasta de trabalho>/.katana/skills, <pasta>/.claude/skills, ~/.katana/skills, ~/.claude/skills;
 * depois as receitas embutidas (howto), com o nome do assunto. */

static const char CT_SKILL_DESC[] =
    "Load a named set of instructions (a skill) and follow it. Skills live in .katana/skills/<name>/SKILL.md or "
    ".claude/skills/<name>/SKILL.md (in the working folder or the home folder); built-in recipes are skills too. "
    "Call it without skill, or with an unknown name, to list the available skills.";
static const char CT_SKILL_PROPS[] =
    "\"skill\": {\"type\": \"string\", \"description\": \"Name of the skill to load (letters, digits, - _ :)\"}, "
    "\"args\": {\"type\": \"string\", \"description\": \"Optional arguments for the skill, appended to its instructions\"}";
static const char CT_SKILL_REQ[] = "\"skill\"";

static const char *const SK_BUILTIN[] = { "web", "terminal", "linux", "script", "sudo", "files", "code", "packages", "logs" };
#define SK_NBUILTIN ((int)(sizeof SK_BUILTIN / sizeof SK_BUILTIN[0]))

static int sk_name_ok(const char *n) {
    if (!*n || strlen(n) > 80) return 0;
    for (; *n; n++) if (!(isalnum((unsigned char)*n) || *n == '-' || *n == '_' || *n == ':')) return 0;
    return 1;
}
/* as 4 pastas de skills, na ordem de busca */
static int sk_roots(char roots[4][1100]) {
    char cwd[1000], home[1000];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    agent_home(home, sizeof home);
    int n = 0;
    if (cwd[0]) { snprintf(roots[n++], 1100, "%s/.katana/skills", cwd); snprintf(roots[n++], 1100, "%s/.claude/skills", cwd); }
    snprintf(roots[n++], 1100, "%s/.katana/skills", home);
    snprintf(roots[n++], 1100, "%s/.claude/skills", home);
    return n;
}
/* arquivo inteiro (ate 256 KB) em memoria nova; NULL se nao abrir */
static char *sk_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc(262145);
    const size_t n = fread(b, 1, 262144, f);
    fclose(f);
    b[n] = 0;
    return b;
}
/* separa o cabecalho: devolve o inicio do corpo; name e desc (se houver) recebem os valores */
static const char *sk_front(const char *txt, char *name, size_t ncap, char *desc, size_t dcap) {
    name[0] = desc[0] = 0;
    const char *p = txt;
    if (strncmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;
    if (strncmp(p, "---", 3) != 0 || (p[3] != '\n' && p[3] != '\r')) return p;
    const char *l = strchr(p, '\n');
    if (!l) return p;
    l++;
    while (*l) {
        const char *e = strchr(l, '\n');
        const size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len >= 3 && strncmp(l, "---", 3) == 0 && (len == 3 || (len == 4 && l[3] == '\r'))) return e ? e + 1 : l + len;
        const char *colon = memchr(l, ':', len);
        if (colon) {
            const size_t kl = (size_t)(colon - l);
            const char *v = colon + 1;
            size_t vl = len - kl - 1;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= 2 && (*v == '"' || *v == '\'') && v[vl - 1] == *v) { v++; vl -= 2; }
            char *dst = NULL; size_t dc = 0;
            if (kl == 4 && strncmp(l, "name", 4) == 0) { dst = name; dc = ncap; }
            else if (kl == 11 && strncmp(l, "description", 11) == 0) { dst = desc; dc = dcap; }
            if (dst) { if (vl >= dc) vl = dc - 1; memcpy(dst, v, vl); dst[vl] = 0; }
        }
        if (!e) break;
        l = e + 1;
    }
    return p;     /* cabecalho nunca fechado: trata tudo como corpo */
}

static void sk_list(char *out, size_t cap, const char *why) {
    char roots[4][1100];
    const int nr = sk_roots(roots);
    size_t o = (size_t)snprintf(out, cap, "%s\nAvailable skills:\n", why);
    if (o >= cap) { out[cap - 1] = 0; return; }
    char seen[200][81]; int ns = 0;
    for (int r = 0; r < nr; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || !sk_name_ok(e->d_name) || ns >= 200) continue;
            int dup = 0;
            for (int k = 0; k < ns; k++) if (strcmp(seen[k], e->d_name) == 0) dup = 1;
            if (dup) continue;
            char path[1300];
            snprintf(path, sizeof path, "%.1100s/%.100s/SKILL.md", roots[r], e->d_name);
            char *txt = sk_slurp(path);
            if (!txt) continue;
            char nm[100], ds[300];
            sk_front(txt, nm, sizeof nm, ds, sizeof ds);
            free(txt);
            snprintf(seen[ns++], 81, "%s", e->d_name);
            for (char *c = ds; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            const int w = snprintf(out + o, cap - o, "- %s%s%s\n", e->d_name, ds[0] ? ": " : "", ds);
            if (w < 0 || (size_t)w >= cap - o) { closedir(d); return; }
            o += (size_t)w;
        }
        closedir(d);
    }
    for (int i = 0; i < SK_NBUILTIN; i++) {
        const int w = snprintf(out + o, cap - o, "- %s: built-in recipe (how to use the %s tools)\n", SK_BUILTIN[i], SK_BUILTIN[i]);
        if (w < 0 || (size_t)w >= cap - o) return;
        o += (size_t)w;
    }
}

static void ct_skill(const char *js, const char *end, char *out, size_t cap) {
    char *name = ct_str(js, end, "skill"), *args = ct_str(js, end, "args");
    if (!name || !*name) { sk_list(out, cap, "No skill name given."); free(name); free(args); return; }
    if (!sk_name_ok(name)) { sk_list(out, cap, "error: invalid skill name (use only letters, digits, - _ :)."); free(name); free(args); return; }
    char roots[4][1100];
    const int nr = sk_roots(roots);
    char *txt = NULL, base[1300] = "";
    for (int r = 0; r < nr && !txt; r++) {
        char path[1400];
        snprintf(base, sizeof base, "%.1100s/%.100s", roots[r], name);
        snprintf(path, sizeof path, "%s/SKILL.md", base);
        txt = sk_slurp(path);
    }
    McpB b = {0};
    char hd[1500];
    if (txt) {
        char nm[100], ds[300];
        const char *body = sk_front(txt, nm, sizeof nm, ds, sizeof ds);
        snprintf(hd, sizeof hd, "Loaded skill: %s (base directory: %s)\n\n", name, base);
        mcp_bputs(&b, hd); mcp_bputs(&b, body);
        free(txt);
    } else {
        int bi = -1;
        for (int i = 0; i < SK_NBUILTIN; i++) if (strcmp(SK_BUILTIN[i], name) == 0) bi = i;
        if (bi < 0) {
            char why[200];
            snprintf(why, sizeof why, "error: unknown skill '%.80s'.", name);
            sk_list(out, cap, why); free(name); free(args); return;
        }
        static char rec[8192];
        tool_howto(name, rec, sizeof rec);
        snprintf(hd, sizeof hd, "Loaded skill: %s (base directory: built-in)\n\n", name);
        mcp_bputs(&b, hd); mcp_bputs(&b, rec);
    }
    if (args && *args) { mcp_bputs(&b, "\n\n## Arguments\n"); mcp_bputs(&b, args); }
    if (b.n + 1 > cap) { memcpy(out, b.p, cap - 1); out[cap - 1] = 0; if (cap > 16) memcpy(out + cap - 14, "\n[truncated]", 13); }
    else memcpy(out, b.p, b.n + 1);
    free(b.p); free(name); free(args);
}
