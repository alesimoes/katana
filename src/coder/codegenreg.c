/* ---------- coder/codegenreg.c: Codegen, registro de operacoes (docs/codegen.md, "Registro de operacoes") ----------
 * UMA tabela, extensivel, para tudo que o motor sabe sobre as funcoes embutidas: nome, aridade, restricao de tipo de cada argumento e tipo do
 * resultado (com polimorfismo simples: "o mesmo tipo do argumento 0", "elemento da lista"), nivel L1/L2/L3, efeito (pura, le, muta, io, lanca),
 * semantica em uma linha e o mapeamento por linguagem de destino (modelo de texto + imports, ou "unsupported: mensagem"), alem das regras de
 * simplificacao algebrica. O validador (codegencheck.c), o backend Python (codegenpy.c) e as analises leem esta tabela; acrescentar uma operacao e
 * acrescentar um bloco de dados, sem recompilar: o texto embutido (codegenregdata abaixo) e o mesmo formato de arquivo que o dono tambem pode
 * carregar (cgx_reg_load_file, variavel KATANA_KATANA_CODEGEN_OPS, ou `codegencli --ops arquivo`).
 *
 * Formato (linhas; `#` comentario; as linhas de uma operacao comecam com espaco):
 *   op NOME MIN MAX level=1|2|3 effect=pure|reads|mutates|io|throws
 *     use    texto de uso, como aparece nas mensagens (`len(list|str|map)`)
 *     sem    semantica normativa, uma linha
 *     arg N  TIPOS : o que se esperava       TIPOS = num | all | int float str bool list map class void null any, separados por `|`
 *     ret    int | float | str | bool | void | any | list[str] | =0 (tipo do argumento 0) | elem0 | num0 | num01 | sum0
 *     py|js|ts|c [prec=N] [import=M] MODELO     ou  py[map] ... (variante pelo tipo do argumento 0)     ou   js unsupported: mensagem
 *   rule (PADRAO) => (RESULTADO) [if pure x] [if int x] [if str x]
 * Modelo de texto: `$N` argumento N como operando primario; `{N}` argumento N solto; `{N@P}` com precedencia minima P; `{N?}` vazio se faltar;
 * `{*N}` argumentos de N em diante entre parenteses (quebra um por linha se nao couber); `{[i,j]}` os listados, idem. Outro texto e literal.
 * Padrao de regra: `(CABECA arg...)`; cabeca = ADD SUBTRACT MULTIPLY DIVIDE IDIVIDE MOD AND OR NOT NEGATE ou o nome de uma embutida; atomos: uma
 * letra minuscula (variavel; a mesma letra duas vezes exige subarvores iguais), inteiro, float, true, false. */

/* ids das embutidas, na ordem do texto embutido (os ganchos do validador e as estatisticas usam estes) */
enum { CGB_NONE, CGB_LEN, CGB_PUSH, CGB_POP, CGB_STR, CGB_INT, CGB_FLOAT, CGB_UPPER, CGB_LOWER, CGB_TRIM, CGB_SPLIT, CGB_WORDS, CGB_JOIN, CGB_CONTAINS, CGB_INDEXOF, CGB_REPLACE,
       CGB_STARTS, CGB_ENDS, CGB_SLICE, CGB_KEYS, CGB_PRINT, CGB_ABS, CGB_MIN, CGB_MAX, CGB_SQRT, CGB_FLOOR, CGB_POW, CGB_SUM, CGB_SORT, CGB_REVERSE, CGB_MATCHES, CGB_ERRMSG, CGB_N };
#define CGX_OP_MAX 192
#define CGX_NTARGET 4                                     /* 0 python, 1 js, 2 ts, 3 c */
#define CGX_NVAR 4                                        /* variantes por alvo (padrao + por tipo do argumento 0) */
#define CGX_RULE_MAX 128
static const char *const CGX_TARGET_NAME[CGX_NTARGET] = {"py", "js", "ts", "c"};
enum { CGE_PURE, CGE_READS, CGE_MUTATES, CGE_IO, CGE_THROWS };
static const char *const CGE_NAME[] = {"pure", "reads", "mutates", "io", "throws"};
enum { CGR_FIXED, CGR_ARG0, CGR_ELEM0, CGR_NUM0, CGR_NUM01, CGR_SUM0 };
typedef struct { char *tmpl, *imp, *unsup; int prec, kind; } CgxOpMap;     /* kind: -1 padrao, ou CGT_* do argumento 0 */
typedef struct { unsigned mask; char *what; } CgxOpArg;
typedef struct {
    char *name, *use, *sem, *rets; int id, lo, hi, level, effect;
    CgxOpArg arg[4]; int narg;
    int rk, rkind, relem;                                 /* resultado */
    CgxOpMap map[CGX_NTARGET][CGX_NVAR]; int nmap[CGX_NTARGET];
    int builtin;                                          /* veio do texto embutido */
} CgxOp;
typedef struct CgxPat CgxPat;
struct CgxPat { int k; long long iv; double dv; int var; char head[24]; int op, nk; CgxPat *kid[3]; };    /* k: 0 var, 1 int, 2 float, 3 bool, 4 operador, 5 chamada */
typedef struct { char *src; int pure_x, int_x, str_x; CgxPat *lhs, *rhs; } CgxRule;
static CgxOp CGX_OPS[CGX_OP_MAX]; static int cgx_nops;
static CgxRule CGX_RULES[CGX_RULE_MAX]; static int cgx_nrules;
static char cgx_reg_names_buf[2048];
static int cgx_reg_ready;

#define CGM_ALL ((1u << (CGT_NULL + 1)) - 1)
static const char *const CGX_KIND_NAME[10] = {"any", "int", "float", "str", "bool", "void", "list", "map", "class", "null"};
static unsigned cgx_reg_mask(const char *s) {
    unsigned m = 0; char t[160]; snprintf(t, sizeof t, "%s", s);
    for (char *tok = strtok(t, "| \t"); tok; tok = strtok(NULL, "| \t")) {
        if (!strcmp(tok, "num")) { m |= 1u << CGT_INT | 1u << CGT_FLOAT; continue; }
        if (!strcmp(tok, "all")) { m |= CGM_ALL; continue; }
        int f = 0; for (int i = 0; i < 10; i++) if (!strcmp(tok, CGX_KIND_NAME[i])) { m |= 1u << i; f = 1; }
        if (!f) return 0xFFFFFFFFu;
    }
    return m;
}
static int cgx_reg_kind_of(const char *s) { for (int i = 0; i < 10; i++) if (!strcmp(s, CGX_KIND_NAME[i])) return i; return -2; }
static const CgxOp *cgx_op_find(const char *name) {
    for (int i = 0; i < cgx_nops; i++) if (!strcmp(CGX_OPS[i].name, name)) return &CGX_OPS[i];
    return NULL;
}
static const CgxOp *cgx_op_by_id(int id) { return id >= 1 && id <= cgx_nops ? &CGX_OPS[id - 1] : NULL; }
static void cgx_reg_rebuild_names(void) {
    size_t w = 0; cgx_reg_names_buf[0] = 0;
    for (int i = 0; i < cgx_nops && w + 40 < sizeof cgx_reg_names_buf; i++) w += (size_t)snprintf(cgx_reg_names_buf + w, sizeof cgx_reg_names_buf - w, "%s%s", i ? " " : "", CGX_OPS[i].name);
}
static char *cgx_rtrim_dup(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    char *r = (char *)malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r;
}

/* ---- padroes de regra ---- */
static void cgx_pat_free(CgxPat *q);
static CgxPat *cgx_pat_parse(const char **p, char *err, size_t ecap) {
    while (**p == ' ') (*p)++;
    CgxPat *q = (CgxPat *)calloc(1, sizeof *q);
    if (**p == '(') {
        (*p)++; q->k = 4; int n = 0; while (**p == ' ') (*p)++;
        while (**p && **p != ' ' && **p != ')' && n < 23) q->head[n++] = *(*p)++;
        static const char *const hn[CGO_N] = {"ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "IDIVIDE", "MOD", NULL, NULL, NULL, NULL, NULL, NULL, "AND", "OR", "NOT", "NEGATE", NULL, NULL};
        q->op = -1;
        for (int i = 0; i < CGO_N; i++) if (hn[i] && !strcmp(hn[i], q->head)) q->op = i;
        if (q->op < 0) q->k = 5;
        for (;;) {
            while (**p == ' ') (*p)++;
            if (**p == ')') { (*p)++; break; }
            if (!**p || q->nk >= 3) { snprintf(err, ecap, "malformed pattern"); cgx_pat_free(q); return NULL; }
            q->kid[q->nk] = cgx_pat_parse(p, err, ecap);
            if (!q->kid[q->nk]) { cgx_pat_free(q); return NULL; }
            q->nk++;
        }
        return q;
    }
    char tok[40]; int n = 0; while (**p && **p != ' ' && **p != ')' && n < 39) tok[n++] = *(*p)++; tok[n] = 0;
    if (!n) { snprintf(err, ecap, "empty pattern"); free(q); return NULL; }
    if (!strcmp(tok, "true") || !strcmp(tok, "false")) { q->k = 3; q->iv = tok[0] == 't'; }
    else if (isdigit((unsigned char)tok[0]) || tok[0] == '-') { if (strchr(tok, '.')) { q->k = 2; q->dv = atof(tok); } else { q->k = 1; q->iv = atoll(tok); } }
    else if (n == 1 && islower((unsigned char)tok[0])) { q->k = 0; q->var = tok[0]; }
    else { snprintf(err, ecap, "unknown atom `%s`", tok); free(q); return NULL; }
    return q;
}
static void cgx_pat_free(CgxPat *q) { if (!q) return; for (int i = 0; i < q->nk; i++) cgx_pat_free(q->kid[i]); free(q); }

/* ---- carga ---- */
static void cgx_reg_clear_op(CgxOp *o) {
    free(o->name); free(o->use); free(o->sem); free(o->rets);
    for (int i = 0; i < 4; i++) free(o->arg[i].what);
    for (int t = 0; t < CGX_NTARGET; t++) for (int v = 0; v < o->nmap[t]; v++) { free(o->map[t][v].tmpl); free(o->map[t][v].imp); free(o->map[t][v].unsup); }
    memset(o, 0, sizeof *o);
}
static void cgx_reg_reset(void) {
    for (int i = 0; i < cgx_nops; i++) cgx_reg_clear_op(&CGX_OPS[i]);
    cgx_nops = 0;
    for (int i = 0; i < cgx_nrules; i++) { free(CGX_RULES[i].src); cgx_pat_free(CGX_RULES[i].lhs); cgx_pat_free(CGX_RULES[i].rhs); }
    cgx_nrules = 0; cgx_reg_names_buf[0] = 0; cgx_reg_ready = 0;
}
static int cgx_reg_set_ret(CgxOp *o, const char *s) {
    free(o->rets); o->rets = cgx_rtrim_dup(s); o->rk = CGR_FIXED; o->rkind = CGT_ANY; o->relem = CGT_ANY;
    if (!strcmp(s, "=0")) o->rk = CGR_ARG0; else if (!strcmp(s, "elem0")) o->rk = CGR_ELEM0; else if (!strcmp(s, "num0")) o->rk = CGR_NUM0;
    else if (!strcmp(s, "num01")) o->rk = CGR_NUM01; else if (!strcmp(s, "sum0")) o->rk = CGR_SUM0;
    else if (!strncmp(s, "list[", 5)) { char t[20]; snprintf(t, sizeof t, "%s", s + 5); char *e = strchr(t, ']'); if (e) *e = 0; o->rkind = CGT_LIST; o->relem = cgx_reg_kind_of(t); if (o->relem < 0) return 0; }
    else { o->rkind = cgx_reg_kind_of(s); if (o->rkind < 0) return 0; }
    return 1;
}
/* texto -> registro. Uma operacao com nome ja existente (carga do usuario) substitui a anterior. 1 se ok; senao "line N: ..." em err. */
static int cgx_reg_load_text(const char *text, int builtin, char *err, size_t ecap) {
    CgxOp *cur = NULL; int ln = 0; const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        char *line = (char *)malloc(l + 1); memcpy(line, p, l); line[l] = 0; p += l + (e ? 1 : 0); ln++;
        char *s = line; const int indented = *s == ' ' || *s == '\t'; while (*s == ' ' || *s == '\t') s++;
        size_t sl = strlen(s); while (sl && (s[sl - 1] == '\r' || s[sl - 1] == ' ')) s[--sl] = 0;
        if (!*s || *s == '#') { free(line); continue; }
#define FAIL(...) do { snprintf(err, ecap, "line %d: ", ln); snprintf(err + strlen(err), ecap - strlen(err), __VA_ARGS__); free(line); cgx_reg_rebuild_names(); return 0; } while (0)
        char w[24]; int wn = 0; const char *r = s; while (*r && *r != ' ' && wn < 23) w[wn++] = *r++; w[wn] = 0; while (*r == ' ') r++;
        if (!indented && !strcmp(w, "op")) {
            char nm[64], lv[24] = "level=1", ef[24] = "effect=pure"; int lo, hi;
            if (sscanf(r, "%63s %d %d %23s %23s", nm, &lo, &hi, lv, ef) < 3) FAIL("expected `op NAME MIN MAX level=N effect=E`");
            if (lo < 0 || hi < lo || hi > 16) FAIL("bad arity %d..%d", lo, hi);
            const int lvn = strncmp(lv, "level=", 6) ? 1 : atoi(lv + 6); if (lvn < 1 || lvn > 3) FAIL("level must be 1, 2 or 3");
            int efn = -1; for (int i = 0; i < 5; i++) if (!strcmp(ef + (strncmp(ef, "effect=", 7) ? 0 : 7), CGE_NAME[i])) efn = i;
            if (efn < 0) FAIL("effect must be pure, reads, mutates, io or throws");
            int idx = -1; for (int i = 0; i < cgx_nops; i++) if (!strcmp(CGX_OPS[i].name, nm)) idx = i;
            if (idx >= 0 && builtin) FAIL("duplicate operation `%s`", nm);
            if (idx < 0) { if (cgx_nops >= CGX_OP_MAX) FAIL("too many operations (max %d)", CGX_OP_MAX); idx = cgx_nops++; }
            else cgx_reg_clear_op(&CGX_OPS[idx]);
            cur = &CGX_OPS[idx]; cur->name = strdup(nm); cur->id = idx + 1; cur->lo = lo; cur->hi = hi; cur->level = lvn; cur->effect = efn; cur->builtin = builtin;
            cur->narg = hi < 4 ? hi : 4; cur->rets = strdup("any"); cur->rk = CGR_FIXED; cur->rkind = CGT_ANY;
            { char u[160]; snprintf(u, sizeof u, "%s(...)", nm); cur->use = strdup(u); }
            cur->sem = strdup("");
            for (int i = 0; i < 4; i++) cur->arg[i].what = strdup("a value");
        } else if (!indented && !strcmp(w, "rule")) {
            if (cgx_nrules >= CGX_RULE_MAX) FAIL("too many rules");
            const char *arrow = strstr(r, "=>"); if (!arrow) FAIL("rule needs `=>`");
            char lhs[200], rest[200]; const size_t ll = (size_t)(arrow - r); if (ll >= sizeof lhs) FAIL("rule too long");
            memcpy(lhs, r, ll); lhs[ll] = 0; snprintf(rest, sizeof rest, "%s", arrow + 2);
            char *cond = strstr(rest, " if "); if (cond) { *cond = 0; cond += 4; }
            const char *a = lhs; char pe[120] = ""; CgxPat *L = cgx_pat_parse(&a, pe, sizeof pe);
            if (!L || L->k < 4) { cgx_pat_free(L); FAIL("bad rule pattern: %s", pe[0] ? pe : "the left side must be a (HEAD ...) form"); }
            const char *b = rest; CgxPat *R = cgx_pat_parse(&b, pe, sizeof pe);
            if (!R) { cgx_pat_free(L); FAIL("bad rule result: %s", pe); }
            { char *k = cgx_rtrim_dup(r); int dup = 0; for (int i = 0; i < cgx_nrules; i++) if (!strcmp(CGX_RULES[i].src, k)) dup = 1; free(k); if (dup) { cgx_pat_free(L); cgx_pat_free(R); free(line); continue; } }
            CgxRule *ru = &CGX_RULES[cgx_nrules++]; memset(ru, 0, sizeof *ru);
            ru->lhs = L; ru->rhs = R; ru->src = cgx_rtrim_dup(r);
            if (cond) { ru->pure_x = strstr(cond, "pure x") != NULL; ru->int_x = strstr(cond, "int x") != NULL; ru->str_x = strstr(cond, "str x") != NULL; }
        } else if (indented && cur) {
            if (!strcmp(w, "use")) { free(cur->use); cur->use = cgx_rtrim_dup(r); }
            else if (!strcmp(w, "sem")) { free(cur->sem); cur->sem = cgx_rtrim_dup(r); }
            else if (!strcmp(w, "arg")) {
                int i = -1; const char *q = r; if (isdigit((unsigned char)*q)) { i = atoi(q); while (isdigit((unsigned char)*q)) q++; } while (*q == ' ') q++;
                if (i < 0 || i > 3) FAIL("`arg N TYPES : what` with N from 0 to 3");
                const char *colon = strchr(q, ':'); char ty[160] = "";
                { size_t tl = colon ? (size_t)(colon - q) : strlen(q); if (tl >= sizeof ty) tl = sizeof ty - 1; memcpy(ty, q, tl); ty[tl] = 0; }
                const unsigned m = cgx_reg_mask(ty); if (m == 0xFFFFFFFFu) FAIL("unknown type in `%s`", ty);
                cur->arg[i].mask = m; free(cur->arg[i].what); cur->arg[i].what = cgx_rtrim_dup(colon ? colon + 1 : "a value");
                if (i + 1 > cur->narg) cur->narg = i + 1;
            }
            else if (!strcmp(w, "ret")) { if (!cgx_reg_set_ret(cur, r)) FAIL("unknown result type `%s`", r); }
            else {
                char base[24]; snprintf(base, sizeof base, "%s", w); int vk = -1, t = -1;
                char *br = strchr(base, '[');
                if (br) { *br = 0; char kk[16]; snprintf(kk, sizeof kk, "%s", strchr(w, '[') + 1); char *e2 = strchr(kk, ']'); if (e2) *e2 = 0; vk = cgx_reg_kind_of(kk); if (vk < 0) FAIL("unknown type variant `%s`", kk); }
                for (int i = 0; i < CGX_NTARGET; i++) if (!strcmp(base, CGX_TARGET_NAME[i])) t = i;
                if (t < 0) FAIL("unknown line `%s` (expected use, sem, arg, ret, py, js, ts or c)", w);
                if (cur->nmap[t] >= CGX_NVAR) FAIL("too many variants for %s", base);
                CgxOpMap *m = &cur->map[t][cur->nmap[t]++]; memset(m, 0, sizeof *m); m->kind = vk; m->prec = 10;
                for (;;) {
                    if (!strncmp(r, "prec=", 5)) { m->prec = atoi(r + 5); while (*r && *r != ' ') r++; while (*r == ' ') r++; }
                    else if (!strncmp(r, "import=", 7)) { const char *q = r + 7; size_t k = 0; while (q[k] && q[k] != ' ') k++; free(m->imp); m->imp = (char *)malloc(k + 1); memcpy(m->imp, q, k); m->imp[k] = 0; r = q + k; while (*r == ' ') r++; }
                    else break;
                }
                if (!strncmp(r, "unsupported", 11)) { const char *q = r + 11; if (*q == ':') q++; m->unsup = cgx_rtrim_dup(q); if (!m->unsup[0]) { free(m->unsup); m->unsup = strdup("not available in this target"); } }
                else { if (!*r) FAIL("empty template"); m->tmpl = cgx_rtrim_dup(r); }
            }
        } else FAIL("unexpected line `%s` (an operation starts with `op` at column 1, its lines are indented)", s);
#undef FAIL
        free(line);
    }
    cgx_reg_rebuild_names();
    return 1;
}
static int cgx_reg_load_file(const char *path, char *err, size_t ecap) {
    FILE *f = fopen(path, "rb"); if (!f) { snprintf(err, ecap, "cannot open %s", path); return 0; }
    size_t cap = 1 << 14, n = 0; char *b = (char *)malloc(cap);
    for (;;) { size_t r = fread(b + n, 1, cap - n - 1, f); n += r; if (n + 1 < cap) break; cap *= 2; b = (char *)realloc(b, cap); }
    b[n] = 0; fclose(f);
    const int ok = cgx_reg_load_text(b, 0, err, ecap); free(b); return ok;
}

/* ---- tabela embutida: o mesmo formato, lido na primeira consulta ---- */
static const char CGX_REG_DATA[] =
"# operacoes embutidas da UIR (ordem = id; a ordem e fixada pelo enum CGB_*)\n"
"op len 1 1 level=1 effect=pure\n"
"  use len(list|str|map)\n  sem number of elements, characters or keys\n  arg 0 list|str|map : a list, str or map\n  ret int\n"
"  py len{*0}\n  js $0.length\n  js[map] Object.keys($0).length\n  ts $0.length\n  ts[map] Object.keys($0).length\n  c unsupported: len needs the array length passed alongside\n"
"op push 2 2 level=1 effect=mutates\n"
"  use push(list, value)\n  sem append the value at the end of the list (the list is changed in place)\n  arg 0 list : a list\n  arg 1 all : a value\n  ret void\n"
"  py $0.append{*1}\n  js $0.push{*1}\n  ts $0.push{*1}\n"
"op pop 1 1 level=1 effect=mutates\n"
"  use pop(list)\n  sem remove and return the last element (the list is changed in place)\n  arg 0 list : a list\n  ret elem0\n"
"  py $0.pop()\n  js $0.pop()\n  ts $0.pop()\n"
"op str 1 1 level=1 effect=pure\n"
"  use str(x)\n  sem text of the value\n  arg 0 all : a value\n  ret str\n"
"  py str{*0}\n  js String{*0}\n  ts String{*0}\n"
"op int 1 1 level=1 effect=throws\n"
"  use int(x)\n  sem integer from a number (truncated) or a decimal string; throws on a bad string\n  arg 0 num|str|bool : a number or str\n  ret int\n"
"  py int{*0}\n  js unsupported: int(x) needs a helper that truncates and throws on a bad string\n  ts unsupported: int(x) needs a helper that truncates and throws on a bad string\n"
"op float 1 1 level=1 effect=throws\n"
"  use float(x)\n  sem float from a number or a decimal string; throws on a bad string\n  arg 0 num|str : a number or str\n  ret float\n"
"  py float{*0}\n  js Number{*0}\n  ts Number{*0}\n"
"op upper 1 1 level=2 effect=pure\n"
"  use upper(s)\n  sem upper-case copy\n  arg 0 str : a str\n  ret str\n  py $0.upper()\n  js $0.toUpperCase()\n  ts $0.toUpperCase()\n"
"op lower 1 1 level=2 effect=pure\n"
"  use lower(s)\n  sem lower-case copy\n  arg 0 str : a str\n  ret str\n  py $0.lower()\n  js $0.toLowerCase()\n  ts $0.toLowerCase()\n"
"op trim 1 1 level=2 effect=pure\n"
"  use trim(s)\n  sem copy without leading and trailing white space\n  arg 0 str : a str\n  ret str\n  py $0.strip()\n  js $0.trim()\n  ts $0.trim()\n"
"op split 2 2 level=2 effect=pure\n"
"  use split(s, sep)\n  sem pieces between the separator\n  arg 0 str : a str\n  arg 1 str : a str separator\n  ret list[str]\n  py $0.split{*1}\n  js $0.split{*1}\n  ts $0.split{*1}\n"
"op words 1 1 level=2 effect=pure\n"
"  use words(s)\n  sem pieces separated by runs of white space, no empty pieces\n  arg 0 str : a str\n  ret list[str]\n  py $0.split()\n  js $0.split(/\\s+/).filter(Boolean)\n  ts $0.split(/\\s+/).filter(Boolean)\n"
"op join 2 2 level=2 effect=pure\n"
"  use join(list, sep)\n  sem the strings of the list with the separator between them\n  arg 0 list : a list of str\n  arg 1 str : a str separator\n  ret str\n  py $1.join({0})\n  js $0.join({1})\n  ts $0.join({1})\n"
"op contains 2 2 level=2 effect=pure\n"
"  use contains(str|list|map, x)\n  sem substring, element or key membership\n  arg 0 str|list|map : a str, list or map\n  arg 1 all : a value\n  ret bool\n"
"  py prec=5 {1@6} in {0@6}\n  js $0.includes{*1}\n  js[map] Object.prototype.hasOwnProperty.call({0}, {1})\n  ts $0.includes{*1}\n  ts[map] Object.prototype.hasOwnProperty.call({0}, {1})\n"
"op indexof 2 2 level=2 effect=pure\n"
"  use indexof(s, sub)\n  sem index of the first occurrence, -1 when absent\n  arg 0 str : a str\n  arg 1 str : a str\n  ret int\n  py $0.find{*1}\n  js $0.indexOf{*1}\n  ts $0.indexOf{*1}\n"
"op replace 3 3 level=2 effect=pure\n"
"  use replace(s, old, new)\n  sem copy with every occurrence replaced\n  arg 0 str : a str\n  arg 1 str : a str\n  arg 2 str : a str\n  ret str\n  py $0.replace{*1}\n  js $0.replaceAll{*1}\n  ts $0.replaceAll{*1}\n"
"op startswith 2 2 level=2 effect=pure\n"
"  use startswith(s, prefix)\n  sem true when s begins with the prefix\n  arg 0 str : a str\n  arg 1 str : a str\n  ret bool\n  py $0.startswith{*1}\n  js $0.startsWith{*1}\n  ts $0.startsWith{*1}\n"
"op endswith 2 2 level=2 effect=pure\n"
"  use endswith(s, suffix)\n  sem true when s ends with the suffix\n  arg 0 str : a str\n  arg 1 str : a str\n  ret bool\n  py $0.endswith{*1}\n  js $0.endsWith{*1}\n  ts $0.endsWith{*1}\n"
"op slice 2 3 level=2 effect=pure\n"
"  use slice(str|list, from[, to])\n  sem copy of the part from `from` to `to` (end exclusive)\n  arg 0 str|list : a str or list\n  arg 1 int : an int\n  arg 2 int : an int\n  ret =0\n  py $0[{1}:{2?}]\n  js $0.slice{*1}\n  ts $0.slice{*1}\n"
"op keys 1 1 level=1 effect=pure\n"
"  use keys(map)\n  sem the keys, in insertion order\n  arg 0 map : a map\n  ret list[str]\n  py list($0.keys())\n  js Object.keys{*0}\n  ts Object.keys{*0}\n"
"op print 1 1 level=1 effect=io\n"
"  use print(x)\n  sem write the text of the value and a line break to standard output\n  arg 0 int|float|str|bool|list|map|class|null|any : a value\n  ret void\n  py print{*0}\n  js console.log{*0}\n  ts console.log{*0}\n"
"op abs 1 1 level=1 effect=pure\n"
"  use abs(n)\n  sem absolute value, same numeric type\n  arg 0 num : a number\n  ret num0\n  py abs{*0}\n  js Math.abs{*0}\n  ts Math.abs{*0}\n"
"op min 2 2 level=1 effect=pure\n"
"  use min(a, b)\n  sem the smaller number\n  arg 0 num : a number\n  arg 1 num : a number\n  ret num01\n  py min{*0}\n  js Math.min{*0}\n  ts Math.min{*0}\n"
"op max 2 2 level=1 effect=pure\n"
"  use max(a, b)\n  sem the larger number\n  arg 0 num : a number\n  arg 1 num : a number\n  ret num01\n  py max{*0}\n  js Math.max{*0}\n  ts Math.max{*0}\n"
"op sqrt 1 1 level=1 effect=pure\n"
"  use sqrt(n)\n  sem square root, float\n  arg 0 num : a number\n  ret float\n  py import=math math.sqrt{*0}\n  js Math.sqrt{*0}\n  ts Math.sqrt{*0}\n"
"op floor 1 1 level=1 effect=pure\n"
"  use floor(n)\n  sem largest integer not above n\n  arg 0 num : a number\n  ret int\n  py import=math math.floor{*0}\n  js Math.floor{*0}\n  ts Math.floor{*0}\n"
"op pow 2 2 level=1 effect=pure\n"
"  use pow(a, b)\n  sem a raised to b, float\n  arg 0 num : a number\n  arg 1 num : a number\n  ret float\n  py import=math math.pow{*0}\n  js Math.pow{*0}\n  ts Math.pow{*0}\n"
"op sum 1 1 level=1 effect=pure\n"
"  use sum(list)\n  sem sum of a list of numbers\n  arg 0 list : a list of numbers\n  ret sum0\n  py sum{*0}\n  js $0.reduce((a, b) => a + b, 0)\n  ts $0.reduce((a, b) => a + b, 0)\n"
"op sort 1 1 level=1 effect=pure\n"
"  use sort(list)\n  sem sorted copy in ascending order (the argument is not changed)\n  arg 0 list : a list\n  ret =0\n  py sorted{*0}\n  js [...$0].sort()\n  ts [...$0].sort()\n"
"op reverse 1 1 level=2 effect=pure\n"
"  use reverse(list|str)\n  sem reversed copy\n  arg 0 list|str : a list or str\n  ret =0\n  py $0[::-1]\n  js [...$0].reverse()\n  js[str] [...$0].reverse().join(\"\")\n  ts [...$0].reverse()\n  ts[str] [...$0].reverse().join(\"\")\n"
"op matches 2 2 level=2 effect=pure\n"
"  use matches(s, regex)\n  sem true when the regular expression matches somewhere in s\n  arg 0 str : a str\n  arg 1 str : a regex str\n  ret bool\n  py prec=5 import=re re.search({1}, {0}) is not None\n  js new RegExp({1}).test({0})\n  ts new RegExp({1}).test({0})\n"
"op errmsg 1 1 level=1 effect=pure\n"
"  use errmsg(e)\n  sem the text of the error caught by `catch e`\n  arg 0 class : the error caught by `catch e`\n  ret str\n  py str{*0}\n  js $0.message\n  ts $0.message\n"
"# simplificacao algebrica (so o que vale para todos os alvos; condicoes evitam mudar excecoes e tipos)\n"
"rule (ADD x 0) => x if int x\nrule (ADD 0 x) => x if int x\nrule (SUBTRACT x 0) => x\nrule (MULTIPLY x 1) => x\nrule (MULTIPLY 1 x) => x\n"
"rule (MULTIPLY x 0) => 0 if int x, pure x\nrule (MULTIPLY 0 x) => 0 if int x, pure x\nrule (SUBTRACT x x) => 0 if int x, pure x\n"
"rule (IDIVIDE x 1) => x if int x\nrule (MOD x 1) => 0 if int x, pure x\n"
"rule (NOT (NOT x)) => x\nrule (NEGATE (NEGATE x)) => x\n"
"rule (AND x true) => x\nrule (AND true x) => x\nrule (OR x false) => x\nrule (OR false x) => x\n"
"rule (AND x false) => false if pure x\nrule (OR x true) => true if pure x\n"
"rule (abs (abs x)) => (abs x)\nrule (upper (upper x)) => (upper x)\nrule (lower (lower x)) => (lower x)\nrule (trim (trim x)) => (trim x)\n"
"rule (sort (sort x)) => (sort x)\nrule (str (str x)) => (str x)\nrule (reverse (reverse x)) => x if str x\n";

static void cgx_reg_init(void) {
    if (cgx_reg_ready) return;
    char err[200] = "";
    cgx_reg_ready = 1;
    if (!cgx_reg_load_text(CGX_REG_DATA, 1, err, sizeof err)) { fprintf(stderr, "codegen: embedded operation table: %s\n", err); exit(1); }
    const char *env = getenv("KATANA_KATANA_CODEGEN_OPS");
    if (env && *env && !cgx_reg_load_file(env, err, sizeof err)) fprintf(stderr, "codegen: KATANA_KATANA_CODEGEN_OPS: %s\n", err);
}
static const char *cgx_reg_names_all(void) { cgx_reg_init(); return cgx_reg_names_buf; }

/* ---- consultas ---- */
/* mapeamento da operacao para o alvo t (0 py, 1 js, 2 ts, 3 c), escolhendo a variante pelo tipo do argumento 0; NULL se nao ha linha */
static const CgxOpMap *cgx_op_map(const CgxOp *o, int t, int arg0kind) {
    const CgxOpMap *def = NULL;
    for (int v = 0; v < o->nmap[t]; v++) { if (o->map[t][v].kind == arg0kind) return &o->map[t][v]; if (o->map[t][v].kind < 0) def = &o->map[t][v]; }
    return def;
}
static int cgx_op_pure(const CgxOp *o) { return o->effect == CGE_PURE; }

/* ---- dump (volta a ser lido pelo mesmo carregador) ---- */
static void cgx_reg_dump(CgxB *o) {
    cgx_reg_init();
    for (int i = 0; i < cgx_nops; i++) {
        const CgxOp *p = &CGX_OPS[i];
        cgx_bf(o, "op %s %d %d level=%d effect=%s\n  use %s\n  sem %s\n", p->name, p->lo, p->hi, p->level, CGE_NAME[p->effect], p->use, p->sem);
        for (int a = 0; a < p->narg; a++) {
            char tn[120] = ""; size_t w = 0; int all = p->arg[a].mask == CGM_ALL;
            if (all) snprintf(tn, sizeof tn, "all");
            else for (int k = 0; k < 10; k++) if (p->arg[a].mask & (1u << k)) w += (size_t)snprintf(tn + w, sizeof tn - w, "%s%s", w ? "|" : "", CGX_KIND_NAME[k]);
            if (!p->arg[a].mask) snprintf(tn, sizeof tn, "any");
            cgx_bf(o, "  arg %d %s : %s\n", a, tn, p->arg[a].what);
        }
        cgx_bf(o, "  ret %s\n", p->rets);
        for (int t = 0; t < CGX_NTARGET; t++) for (int v = 0; v < p->nmap[t]; v++) {
            const CgxOpMap *m = &p->map[t][v];
            cgx_bf(o, "  %s", CGX_TARGET_NAME[t]); if (m->kind >= 0) cgx_bf(o, "[%s]", CGX_KIND_NAME[m->kind]);
            if (m->prec != 10) cgx_bf(o, " prec=%d", m->prec);
            if (m->imp) cgx_bf(o, " import=%s", m->imp);
            if (m->unsup) cgx_bf(o, " unsupported: %s\n", m->unsup); else cgx_bf(o, " %s\n", m->tmpl);
        }
    }
    for (int i = 0; i < cgx_nrules; i++) cgx_bf(o, "rule %s\n", CGX_RULES[i].src);
}
/* tabela legivel: nome, aridade, tipos, nivel, efeito, alvos */
static void cgx_reg_list(CgxB *o) {
    cgx_reg_init();
    cgx_bf(o, "%-11s %-5s %-3s %-6s %-22s targets\n", "name", "arity", "lvl", "effect", "result");
    for (int i = 0; i < cgx_nops; i++) {
        const CgxOp *p = &CGX_OPS[i]; char ar[12], tg[40] = ""; size_t w = 0;
        if (p->lo == p->hi) snprintf(ar, sizeof ar, "%d", p->lo); else snprintf(ar, sizeof ar, "%d-%d", p->lo, p->hi);
        for (int t = 0; t < CGX_NTARGET; t++) {
            int ok = 0, un = 0; for (int v = 0; v < p->nmap[t]; v++) { if (p->map[t][v].tmpl) ok = 1; if (p->map[t][v].unsup) un = 1; }
            if (ok || un) w += (size_t)snprintf(tg + w, sizeof tg - w, "%s%s%s", w ? " " : "", CGX_TARGET_NAME[t], ok ? "" : "(no)");
        }
        cgx_bf(o, "%-11s %-5s L%-2d %-6s %-22s %s\n", p->name, ar, p->level, CGE_NAME[p->effect], p->rets, tg);
    }
}

/* ---- simplificacao algebrica sobre a AST ja validada ---- */
static int cgx_node_eq(const CgxNode *a, const CgxNode *b) {
    if (a == b) return 1;
    if (!a || !b || a->k != b->k || a->op != b->op) return 0;
    if (a->k == CGN_INT || a->k == CGN_BOOL) { if (a->iv != b->iv) return 0; }
    if (a->k == CGN_FLT && a->dv != b->dv) return 0;
    if ((a->s || b->s) && (!a->s || !b->s || strcmp(a->s, b->s))) return 0;
    if (!cgx_node_eq(a->a, b->a) || !cgx_node_eq(a->b, b->b) || !cgx_node_eq(a->c, b->c)) return 0;
    if (a->b1.n != b->b1.n) return 0;
    for (int i = 0; i < a->b1.n; i++) if (!cgx_node_eq(a->b1.p[i], b->b1.p[i])) return 0;
    return 1;
}
/* expressao sem efeito e que nao pode lancar: descartavel */
static int cgx_expr_pure(const CgxNode *n) {
    if (!n) return 1;
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: case CGN_VAR: return 1;
        case CGN_BIN:
            if ((n->op == CGO_DIV || n->op == CGO_IDIV || n->op == CGO_MOD) && !(n->b->k == CGN_INT && n->b->iv != 0) && !(n->b->k == CGN_FLT && n->b->dv != 0)) return 0;
            return cgx_expr_pure(n->a) && cgx_expr_pure(n->b);
        case CGN_UN: return cgx_expr_pure(n->a);
        case CGN_LIST: case CGN_MAP: for (int i = 0; i < n->b1.n; i++) if (!cgx_expr_pure(n->b1.p[i])) return 0; return 1;
        case CGN_TERN: return cgx_expr_pure(n->a) && cgx_expr_pure(n->b) && cgx_expr_pure(n->c);
        case CGN_CALL: { if (!n->bi) return 0; const CgxOp *o = cgx_op_by_id(n->bi); if (!o || !cgx_op_pure(o)) return 0; for (int i = 0; i < n->b1.n; i++) if (!cgx_expr_pure(n->b1.p[i])) return 0; return 1; }
        default: return 0;
    }
}
typedef struct { CgxNode *bound[128]; } CgxBind;
static int cgx_pat_match(const CgxPat *q, CgxNode *n, CgxBind *bd) {
    switch (q->k) {
        case 0: { CgxNode **slot = &bd->bound[(unsigned char)q->var & 127]; if (!*slot) { *slot = n; return 1; } return cgx_node_eq(*slot, n); }
        case 1: return n->k == CGN_INT && n->iv == q->iv;
        case 2: return n->k == CGN_FLT && n->dv == q->dv;
        case 3: return n->k == CGN_BOOL && n->iv == q->iv;
        case 4:
            if (q->op == CGO_NOT || q->op == CGO_NEG) { if (n->k != CGN_UN || n->op != q->op || q->nk != 1) return 0; return cgx_pat_match(q->kid[0], n->a, bd); }
            if (n->k != CGN_BIN || n->op != q->op || q->nk != 2) return 0;
            return cgx_pat_match(q->kid[0], n->a, bd) && cgx_pat_match(q->kid[1], n->b, bd);
        case 5:
            if (n->k != CGN_CALL || !n->bi || strcmp(n->s, q->head) || n->b1.n != q->nk) return 0;
            for (int i = 0; i < q->nk; i++) if (!cgx_pat_match(q->kid[i], n->b1.p[i], bd)) return 0;
            return 1;
    }
    return 0;
}
static CgxNode *cgx_pat_build(Cgx *g, const CgxPat *q, CgxBind *bd, const CgxNode *root) {
    CgxNode *n;
    switch (q->k) {
        case 0: return bd->bound[(unsigned char)q->var & 127];
        case 1: n = cgx_node(g, CGN_INT, root->line); n->iv = q->iv; n->ty = cgx_t(CGT_INT); return n;
        case 2: n = cgx_node(g, CGN_FLT, root->line); n->dv = q->dv; n->ty = cgx_t(CGT_FLOAT); return n;
        case 3: n = cgx_node(g, CGN_BOOL, root->line); n->iv = q->iv; n->ty = cgx_t(CGT_BOOL); return n;
        case 4:
            n = cgx_node(g, q->nk == 1 ? CGN_UN : CGN_BIN, root->line); n->op = q->op; n->ty = root->ty;
            n->a = cgx_pat_build(g, q->kid[0], bd, root); if (q->nk > 1) n->b = cgx_pat_build(g, q->kid[1], bd, root);
            return n;
        default: {
            const CgxOp *o = cgx_op_find(q->head); n = cgx_node(g, CGN_CALL, root->line); n->s = cgx_dup(g, q->head); n->bi = o ? o->id : 0; n->ty = root->ty;
            for (int i = 0; i < q->nk; i++) cgx_blk_add(g, &n->b1, cgx_pat_build(g, q->kid[i], bd, root));
            return n;
        }
    }
}
/* tenta as regras na raiz n; 1 se reescreveu (n passa a ter o conteudo do resultado) */
static int cgx_simp_root(Cgx *g, CgxNode *n) {
    if (n->k != CGN_BIN && n->k != CGN_UN && n->k != CGN_CALL) return 0;
    for (int r = 0; r < cgx_nrules; r++) {
        const CgxRule *ru = &CGX_RULES[r]; CgxBind bd; memset(&bd, 0, sizeof bd);
        if (!cgx_pat_match(ru->lhs, n, &bd)) continue;
        CgxNode *x = bd.bound['x'];
        if (ru->pure_x && x && !cgx_expr_pure(x)) continue;
        if (ru->int_x && x && x->ty.k != CGT_INT) continue;
        if (ru->str_x && x && x->ty.k != CGT_STR) continue;
        if (ru->pure_x && !x) { /* regra sem x: exige que tudo que sumir seja puro */ if (!cgx_expr_pure(n)) continue; }
        CgxNode *res = cgx_pat_build(g, ru->rhs, &bd, n);
        const int line = n->line; const CgxTy ty = n->ty;
        *n = *res; n->line = line; if (n->ty.k == CGT_ANY && ty.k != CGT_ANY) n->ty = ty;
        return 1;
    }
    return 0;
}
static int cgx_simp_node(Cgx *g, CgxNode *n) {
    if (!n) return 0;
    int c = 0;
    c += cgx_simp_node(g, n->a); c += cgx_simp_node(g, n->b); c += cgx_simp_node(g, n->c);
    CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) c += cgx_simp_node(g, bl[k]->p[i]);
    for (int i = 0; i < n->npar; i++) c += cgx_simp_node(g, n->par[i].def);
    for (int it = 0; it < 8 && cgx_simp_root(g, n); it++) c++;
    return c;
}
/* aplica as regras ate o ponto fixo; devolve quantas reescritas fez (a AST e renumerada) */
static int cgx_reg_simplify(Cgx *g) {
    cgx_reg_init();
    int total = 0;
    for (int pass = 0; pass < 6; pass++) { const int c = cgx_simp_node(g, g->prog); total += c; if (!c) break; }
    if (total) cgx_number(g);
    return total;
}
