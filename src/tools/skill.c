/* ---------- ferramenta Skill ----------
 * Uma skill e uma pasta com SKILL.md (cabecalho opcional entre linhas "---" com name e description, depois as instrucoes).
 * Busca, em ordem: <pasta de trabalho>/.katana/skills, <pasta>/.config/katana/skills, ~/.katana/skills, ~/.config/katana/skills;
 * depois as receitas embutidas (howto), com o nome do assunto. */

static const char CT_SKILL_DESC[] =
    "Load a named set of instructions (a skill) and follow it. Skills live in .katana/skills/<name>/SKILL.md or "
    ".config/katana/skills/<name>/SKILL.md (in the working folder or the home folder); built-in recipes are skills too. "
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
    if (cwd[0]) { snprintf(roots[n++], 1100, "%s/.katana/skills", cwd); snprintf(roots[n++], 1100, "%s/.config/katana/skills", cwd); }
    snprintf(roots[n++], 1100, "%s/.katana/skills", home);
    snprintf(roots[n++], 1100, "%s/.config/katana/skills", home);
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
            else if ((kl == 11 && strncmp(l, "description", 11) == 0) || (kl == 4 && strncmp(l, "when", 4) == 0)) { dst = desc; dc = dcap; }     /* when e o nome atual; description continua valendo */
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

/* ---- skills do usuario, sem as receitas embutidas: nome, descricao, pasta, origem (0 projeto, 1 pessoal) ---- */
typedef struct { char name[81], desc[300], base[1300]; int origin; } SkInfo;
static int sk_collect(SkInfo *v, int max) {
    char roots[4][1100];
    const int nr = sk_roots(roots);
    int n = 0;
    for (int r = 0; r < nr; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            if (e->d_name[0] == '.' || !sk_name_ok(e->d_name)) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) if (strcmp(v[k].name, e->d_name) == 0) dup = 1;
            if (dup) continue;
            char path[1500];
            snprintf(v[n].base, sizeof v[n].base, "%.1100s/%.100s", roots[r], e->d_name);
            snprintf(path, sizeof path, "%s/SKILL.md", v[n].base);
            char *txt = sk_slurp(path);
            if (!txt) continue;
            char nm[100];
            sk_front(txt, nm, sizeof nm, v[n].desc, sizeof v[n].desc);
            free(txt);
            for (char *c = v[n].desc; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
            v[n].origin = r >= nr - 2;       /* as duas ultimas pastas sao as pessoais */
            n++;
        }
        closedir(d);
    }
    /* projeto antes do pessoal, e dentro de cada grupo em ordem alfabetica */
    for (int i = 1; i < n; i++) for (int j = i; j > 0; j--) {
        const int a = v[j - 1].origin * 1000, b = v[j].origin * 1000;
        if (a + strcmp(v[j - 1].name, v[j].name) > b) { SkInfo t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; } else break;
    }
    return n;
}
/* uma linha para o bloco de ambiente do prompt: as skills que existem, para o modelo saber que pode carregar uma
   (no maximo 10 skills e 600 bytes; descricao cortada em 60; nada se nao houver skills) */
static void sk_env_line(char *out, size_t cap) {
    static SkInfo v[200];
    out[0] = 0;
    const int n = sk_collect(v, 200);
    if (!n) return;
    size_t o = (size_t)snprintf(out, cap, "- Skills (load one with the Skill tool): ");
    const size_t start = o;
    int shown = 0;
    for (int i = 0; i < n && shown < 10; i++) {
        char ds[80];
        snprintf(ds, sizeof ds, "%.60s", v[i].desc);
        if (strlen(v[i].desc) > 60) snprintf(ds + 57, sizeof ds - 57, "...");
        char one[200];
        const int w = snprintf(one, sizeof one, "%s%s%s%s", shown ? "; " : "", v[i].name, ds[0] ? ": " : "", ds);
        if (o - start + (size_t)w > 600 || o + (size_t)w + 80 >= cap) break;
        memcpy(out + o, one, (size_t)w + 1); o += (size_t)w; shown++;
    }
    if (shown < n) o += (size_t)snprintf(out + o, cap - o, " (+%d more: call Skill to list them)", n - shown);
    snprintf(out + o, cap - o, ".\n");
}
/* arquivos extras da pasta de uma skill (scripts, documentos de referencia), com o caminho completo, ate 2 niveis */
static void sk_extra_files(const char *dir, const char *rel, int depth, McpB *b, int *count) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < 40) {
        if (e->d_name[0] == '.') continue;
        char path[2400], r2[1300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 2) sk_extra_files(path, r2, depth + 1, b, count); continue; }
        if (!S_ISREG(st.st_mode) || (!rel[0] && strcmp(e->d_name, "SKILL.md") == 0)) continue;
        char ln[2600];
        snprintf(ln, sizeof ln, "- %s (%ld bytes)\n", path, (long)st.st_size);
        if (!*count) mcp_bputs(b, "\n\nFiles in this skill folder (read them with Read, or run a script with Bash, using these paths):\n");
        mcp_bputs(b, ln); (*count)++;
    }
    closedir(d);
}

/* ---- formato "ability" (docs/extending.md): cabecalho name/when/needs/args e corpo steps:/done:/report:/fail: ---- */
typedef struct {
    int ability, nsteps;
    char needs[300], argdecl[200], done[300], report[300], fail[300];
    char step[12][200], check[12][200];
} SkAbility;
static void sk_hdr_key(const char *txt, const char *key, char *out, size_t cap) {      /* valor de uma chave do cabecalho entre --- */
    out[0] = 0;
    const char *p = txt;
    if (strncmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;
    if (strncmp(p, "---", 3) != 0) return;
    const char *l = strchr(p, '\n');
    const size_t kl = strlen(key);
    while (l && *++l) {
        const size_t len = strcspn(l, "\n");
        if (len >= 3 && strncmp(l, "---", 3) == 0) return;
        if (len > kl && strncmp(l, key, kl) == 0 && l[kl] == ':') {
            const char *v = l + kl + 1; size_t vl = len - kl - 1;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return;
        }
        l += len;
    }
}
static void sk_ability(const char *txt, const char *body, SkAbility *a) {
    memset(a, 0, sizeof *a);
    sk_hdr_key(txt, "needs", a->needs, sizeof a->needs);
    sk_hdr_key(txt, "args", a->argdecl, sizeof a->argdecl);
    int in_steps = 0;
    for (const char *l = body; *l;) {
        const size_t n = strcspn(l, "\n");
        char ln[600]; snprintf(ln, sizeof ln, "%.*s", (int)(n < 599 ? n : 599), l);
        l += n; if (*l) l++;
        size_t e = strlen(ln); while (e && (ln[e - 1] == '\r' || ln[e - 1] == ' ')) ln[--e] = 0;
        if (strcmp(ln, "steps:") == 0) { a->ability = 1; in_steps = 1; continue; }
        const char *k = NULL; char *dst = NULL; size_t dc = 0;
        if (strncmp(ln, "done:", 5) == 0) { k = ln + 5; dst = a->done; dc = sizeof a->done; }
        else if (strncmp(ln, "report:", 7) == 0) { k = ln + 7; dst = a->report; dc = sizeof a->report; }
        else if (strncmp(ln, "fail:", 5) == 0) { k = ln + 5; dst = a->fail; dc = sizeof a->fail; }
        if (dst) { while (*k == ' ') k++; snprintf(dst, dc, "%s", k); in_steps = 0; continue; }
        if (in_steps && isdigit((unsigned char)ln[0])) {
            const char *d = ln; while (isdigit((unsigned char)*d)) d++;
            if (*d == '.' && a->nsteps < 12) {
                d++; while (*d == ' ') d++;
                const char *ar = strstr(d, "=>");
                if (ar) { snprintf(a->step[a->nsteps], sizeof a->step[0], "%.*s", (int)(ar - d), d); size_t sl = strlen(a->step[a->nsteps]); while (sl && a->step[a->nsteps][sl - 1] == ' ') a->step[a->nsteps][--sl] = 0; ar += 2; while (*ar == ' ') ar++; snprintf(a->check[a->nsteps], sizeof a->check[0], "%s", ar); }
                else snprintf(a->step[a->nsteps], sizeof a->step[0], "%s", d);
                a->nsteps++;
            }
        }
    }
}
/* "nome=valor outro=valor com espacos" -> o valor de name em out; 1 se achou */
static int sk_arg(const char *args, const char *name, char *out, size_t cap) {
    const size_t nl = strlen(name);
    for (const char *p = args; p && *p; p++) {
        if ((p == args || p[-1] == ' ') && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1, *e = v;
            while (*e) { if (*e == ' ') { const char *q = e + 1; size_t w = strcspn(q, " ="); if (q[w] == '=' && w > 0) break; } e++; }
            size_t l = (size_t)(e - v); if (l >= cap) l = cap - 1;
            memcpy(out, v, l); out[l] = 0;
            return 1;
        }
    }
    return 0;
}
static void sk_subst(const char *in, const char *args, char *out, size_t cap) {      /* $nome no passo vira o valor do argumento */
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap;) {
        if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            char nm[40]; size_t k = 0; const char *q = p + 1;
            while ((isalnum((unsigned char)*q) || *q == '_' || *q == '-') && k < 39) nm[k++] = *q++;
            nm[k] = 0;
            char v[300];
            if (args && sk_arg(args, nm, v, sizeof v)) { o += (size_t)snprintf(out + o, cap - o, "%s", v); p = q; continue; }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}
static int sk_tool_exists(const char *name) {
    if (!TSR_HAVE_TABLE) return 0;
    const char *nm, *ds, *pr, *rq;
    for (int i = 0; ct_tool_info(i, &nm, &ds, &pr, &rq); i++) if (strcmp(nm, name) == 0) return 1;
    return 0;
}
/* confere "needs": programa (no PATH), ferramenta, servidor MCP conectado ou entrada ($nome / nome dos args passados) */
static void sk_check_needs(const char *needs, const char *args, const SkAbility *a, McpB *missing) {
    char buf[300]; snprintf(buf, sizeof buf, "%s", needs);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        size_t l = strlen(t); while (l && t[l - 1] == ' ') t[--l] = 0;
        if (!*t) continue;
        int ok = 0;
        if (sk_tool_exists(t)) ok = 1;
        else if (mcp_which(t)) ok = 1;
        else {
            if (strstr(a->argdecl, t)) { char v[100]; ok = args && sk_arg(args, t[0] == '$' ? t + 1 : t, v, sizeof v); if (!ok) { mcp_bputs(missing, missing->n ? ", " : ""); mcp_bputs(missing, "input "); mcp_bputs(missing, t); continue; } }
            else { mcp_init(0); for (int i = 0; i < g_nmcp; i++) if (strcmp(g_mcp[i].name, t) == 0 && g_mcp[i].alive) ok = 1; }
        }
        if (!ok) { mcp_bputs(missing, missing->n ? ", " : ""); mcp_bputs(missing, t); }
    }
}
/* escreve a lista de tarefas a partir dos passos (mesmo caminho do TodoWrite); 1 se escreveu */
static int sk_write_plan(const SkAbility *a, const char *args) {
    int td = 0; const int tn = todo_count(&td);
    if (tn > td || !a->nsteps) return 0;      /* ja ha plano em aberto neste pedido */
    McpB j = {0};
    mcp_bputs(&j, "{\"todos\": [");
    for (int i = 0; i < a->nsteps; i++) {
        char st[420]; sk_subst(a->step[i], args, st, sizeof st);
        if (i) mcp_bputs(&j, ", ");
        mcp_bputs(&j, "{\"content\": "); mcp_besc(&j, st);
        mcp_bputs(&j, i == 0 ? ", \"status\": \"in_progress\", \"activeForm\": " : ", \"status\": \"pending\", \"activeForm\": "); mcp_besc(&j, st);
        mcp_bputs(&j, "}");
    }
    mcp_bputs(&j, "]}");
    static char res[2000];
    ct_todowrite(j.p, j.p + j.n, res, sizeof res);
    const int ok = strncmp(res, "error:", 6) != 0;
    free(j.p);
    return ok;
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
        SkAbility *ab = (SkAbility *)malloc(sizeof *ab);
        sk_ability(txt, body, ab);
        snprintf(hd, sizeof hd, "Loaded skill: %s (base directory: %s)\n", name, base);
        mcp_bputs(&b, hd);
        if (ab->ability) {
            McpB miss = {0};
            mcp_bput(&miss, "", 0);
            if (ab->needs[0]) sk_check_needs(ab->needs, args, ab, &miss);
            if (miss.n) { mcp_bputs(&b, "MISSING: "); mcp_bputs(&b, miss.p); mcp_bputs(&b, ". Tell the user what is missing and do only the steps that do not need it.\n"); }
            free(miss.p);
            if (ds[0]) { mcp_bputs(&b, "when: "); mcp_bputs(&b, ds); mcp_bputs(&b, "\n"); }
            const int planned = !miss.n && sk_write_plan(ab, args);
            mcp_bputs(&b, planned ? "Plan written from the skill: do step 1.\n" : "");
            mcp_bputs(&b, "steps:\n");
            for (int i = 0; i < ab->nsteps; i++) {
                char st[420], ln[700]; sk_subst(ab->step[i], args, st, sizeof st);
                snprintf(ln, sizeof ln, "%d. %s%s%s\n", i + 1, st, ab->check[i][0] ? " => " : "", ab->check[i]);
                mcp_bputs(&b, ln);
            }
            const char *const ek[3] = {"done: ", "report: ", "fail: "}, *const ev[3] = {ab->done, ab->report, ab->fail};
            for (int i = 0; i < 3; i++) if (ev[i][0]) { char t2[700]; sk_subst(ev[i], args, t2, sizeof t2); mcp_bputs(&b, ek[i]); mcp_bputs(&b, t2); mcp_bputs(&b, "\n"); }
        } else { mcp_bputs(&b, "\n"); mcp_bputs(&b, body); }
        free(ab);
        free(txt);
        { int nf = 0; sk_extra_files(base, "", 0, &b, &nf); }
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
    if (args && *args && !(strchr(args, '=') && txt == NULL && 0)) { mcp_bputs(&b, "\n\n## Arguments\n"); mcp_bputs(&b, args); }
    if (b.n + 1 > cap) { memcpy(out, b.p, cap - 1); out[cap - 1] = 0; if (cap > 16) memcpy(out + cap - 14, "\n[truncated]", 13); }
    else memcpy(out, b.p, b.n + 1);
    free(b.p); free(name); free(args);
}
