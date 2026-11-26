/* ---------- chamadas de ferramenta validas por construcao ----------
 * Quando o modelo abre uma chamada de ferramenta, um automato sobre o texto gerado diz se cada token ainda pode
 * virar uma chamada valida no formato nativo do modelo:
 *   - ChatML com XML (Qwen3.6): <tool_call>\n<function=NOME>\n<parameter=CHAVE>\nvalor\n</parameter>\n</function>\n</tool_call>
 *   - ChatML com JSON:          <tool_call>\n{"name": "NOME", "arguments": {...}}\n</tool_call>
 *   - Gemma 4:                  <|tool_call>call:NOME{chave:<|"|>texto<|"|>,n:3}<tool_call|>
 * O nome so pode ser de uma ferramenta que o agente tem; a chave so pode ser uma propriedade do esquema dessa
 * ferramenta, cada uma uma vez, e a chamada so fecha depois das obrigatorias. Valor de tipo array, object, integer,
 * number ou boolean segue a gramatica JSON do motor (forward.c); valor de texto fica LIVRE (texto cru ate o marcador
 * de fim do formato): nenhum byte de texto e proibido. TodoWrite.todos aceita o array JSON ou linhas de texto.
 * Uso preguicoso: o token sorteado normalmente so e conferido; so quando ele e recusado a mascara do vocabulario
 * e calculada e o token e sorteado de novo entre os permitidos. KATANA_CALL_GRAMMAR=0 desliga; =2 mostra as correcoes. */
#define CG_MAXT 160
#define CG_MAXP 40
#define CG_NAME 64
#define CG_WS 2
enum { CGF_OFF, CGF_XML, CGF_JSON, CGF_G4 };
typedef struct {
    char name[CG_NAME];
    int np;
    char pk[CG_MAXP][CG_NAME];
    signed char pt[CG_MAXP];      /* 0 texto livre, 1 valor JSON tipado, 2 JSON ou linhas (TodoWrite.todos) */
    int pn[CG_MAXP];              /* no da gramatica JSON do valor tipado */
    uint64_t req;
} CgTool;

/* fases */
enum {
    CX_FUNC = 1, CX_LB, CX_NAME, CX_TAG, CX_PNAME, CX_VSTART, CX_STR, CX_JSON, CX_AFTERV, CX_END,
    CJ_JSON, CJ_END,
    CG_CALL, CG_GNAME, CG_KEY, CG_KEY2, CG_VSTART, CG_QOPEN, CG_GSTR, CG_GJSON, CG_SEP, CG_CLOSE,
    CG_OPEN, CG_DONE
};
typedef struct {
    int ph, pos, ws, n, tool, prop, qm, bare, opened;
    uint64_t used;
    const char *force; int fpos;  /* teste: texto forcado no inicio (abre a chamada) */
    char nb[CG_NAME];
    GState js;                    /* por ultimo: as copias param no topo da pilha */
} CgState;

static struct {
    int fmt, nt, on, ready, budget, verbose, err;
    CgTool t[CG_MAXT];
    int jroot;
    CgState st;
    long checked, fixed, calls, gaveup;
} g_cg;

static int g_cg_only_sys;          /* teste: 1 so as ferramentas do bloco <tools>; -1 sempre as da tabela */
static const char CG_TODO_TOOL[] = "TodoWrite", CG_TODO_PROP[] = "todos";
static const char CG_G4Q[] = "<|\"|>";

static int cg_typeclass(const char *schema) {
    char key[64]; const char *val, *p, *e;
    if (!schema || *schema != '{') return 0;
    for (p = schema + 1; (e = gs_member(p, key, sizeof key, &val)); p = e)
        if (!strcmp(key, "type")) {
            const int ty = *val == '"' ? gs_typename(val) : -1;
            return ty == GN_OBJ || ty == GN_ARR || ty == GN_NUM || ty == GN_INT || ty == GN_BOOL;
        }
    return 0;
}
static CgTool *cg_find(const char *name) {
    for (int i = 0; i < g_cg.nt; i++) if (!strcmp(g_cg.t[i].name, name)) return &g_cg.t[i];
    return NULL;
}
/* acrescenta uma ferramenta: props = objeto {"k": {schema}, ...}, req = array ["k", ...] (texto JSON) */
static void cg_add(const char *name, const char *props, const char *req) {
    if (g_cg.nt >= CG_MAXT || !name[0] || strlen(name) >= CG_NAME || cg_find(name)) return;
    CgTool *t = &g_cg.t[g_cg.nt++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    char k[CG_NAME]; const char *v, *e, *p;
    if (props && *props == '{')
        for (p = props + 1; t->np < CG_MAXP && (e = gs_member(p, k, sizeof k, &v)); p = e) {
            const int i = t->np++;
            snprintf(t->pk[i], CG_NAME, "%s", k);
            t->pt[i] = (signed char)cg_typeclass(v);
            if (!strcmp(name, CG_TODO_TOOL) && !strcmp(k, CG_TODO_PROP)) {
                /* linhas de texto ou array: o declarado, ou o do esquema completo se o prompt declara texto */
                Buf w = {0}; char k2[CG_NAME]; const char *v2 = NULL;
                buf_str(&w, "{"); buf_str(&w, CT_TODOWRITE_PROPS); buf_str(&w, "}");
                if (t->pt[i]) v2 = v; else if (!gs_member(w.p + 1, k2, sizeof k2, &v2)) v2 = NULL;
                t->pn[i] = v2 ? gn_build(v2, 0) : 0;
                free(w.p);
                t->pt[i] = 2;
                if (g_gr.n[t->pn[i]].type == GN_ARR) g_gr.n[t->pn[i]].ordered = 1;   /* aceita texto */
            } else if (t->pt[i]) t->pn[i] = gn_build(v, 0);
        }
    if (req && *req == '[')
        for (p = req + 1; (e = gs_elem(p, &v)); p = e) {
            if (*v != '"') continue;
            for (int i = 0; i < t->np; i++) {
                const size_t L = strlen(t->pk[i]);
                if (!strncmp(v + 1, t->pk[i], L) && v[1 + L] == '"') t->req |= 1ULL << i;
            }
        }
}
static void cg_add_parameters(const char *name, const char *params) {
    char k[64]; const char *v, *e, *p, *props = NULL, *req = NULL;
    if (params && *params == '{')
        for (p = params + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "properties")) props = v;
            else if (!strcmp(k, "required")) req = v;
        }
    cg_add(name, props, req);
}
int ct_tool_info(int i, const char **name, const char **desc, const char **props, const char **req);

/* lista das ferramentas: as declaradas no bloco <tools> do prompt e, no segundo conjunto, as da tabela de src/tools */
static void cg_setup(const char *sys) {
    g_cg.nt = 0; g_cg.err = 0;
    gr_reset();
    /* gemma4: o prompt ja tem as declaracoes nativas; as ferramentas declaradas vem do bloco JSON guardado na conversao */
    if (sys && g_cg.fmt == CGF_G4 && !strstr(sys, "<tools>") && g_g4_tools_json[0] && !g_cg_only_sys) sys = g_g4_tools_json;
    if (sys) {
        static const char *pv[GN_MAXE]; static char names[GN_MAXE][64];
        int nt = 0;
        gr_tools_scan(sys, pv, names, &nt);
        for (int i = 0; i < nt; i++) cg_add_parameters(names[i], pv[i]);
    }
    if (g_cg_only_sys <= 0) {
        const char *nm, *ds, *pr, *rq;
        for (int i = 0; ct_tool_info(i, &nm, &ds, &pr, &rq); i++) {
            if (strstr(ds, "(not implemented yet)")) continue;
            Buf a = {0}, b = {0};
            buf_str(&a, "{"); buf_str(&a, pr); buf_str(&a, "}");
            buf_str(&b, "["); buf_str(&b, rq); buf_str(&b, "]");
            cg_add(nm, a.p, b.p);
            free(a.p); free(b.p);
        }
    }
    if (g_cg_only_sys <= 0) {
        /* ferramentas dos servidores MCP ligados: o modelo as ve em cartoes, fora do bloco <tools>; sem isto a restricao
           nao deixaria o nome sair. Valem as duas grafias: servidor__ferramenta e mcp__servidor__ferramenta */
        for (int i = 0; i < g_nmcp; i++) for (int j = 0; j < g_mcp[i].ntools; j++) {
            char nm[200];
            snprintf(nm, sizeof nm, "%s__%s", g_mcp[i].name, g_mcp[i].tools[j].name);
            cg_add_parameters(nm, g_mcp[i].tools[j].schema);
            snprintf(nm, sizeof nm, "mcp__%s__%s", g_mcp[i].name, g_mcp[i].tools[j].name);
            cg_add_parameters(nm, g_mcp[i].tools[j].schema);
        }
    }
    g_cg.jroot = 0;
    if (g_cg.fmt == CGF_JSON && g_cg.nt) {      /* {"name": <enum>, "arguments": <objeto da ferramenta>} */
        const int root = gn_new(GN_OBJ), en = gn_new(GN_ENUM), dp = gn_new(GN_DISP);
        GNode *r = &g_gr.n[root];
        r->ordered = 1; r->np = 2; r->req = 3;
        r->pk = gr_alloc(sizeof(char *) * 2); r->pn = gr_alloc(sizeof(int) * 2);
        g_gr.n[en].ev = gr_alloc(sizeof(char *) * (size_t)g_cg.nt); g_gr.n[en].ne = g_cg.nt;
        g_gr.n[dp].dn = gr_alloc(sizeof(int) * (size_t)g_cg.nt); g_gr.n[dp].ne = g_cg.nt;
        if (!g_gr.err) {
            r->pk[0] = gr_dup("name", 4); r->pk[1] = gr_dup("arguments", 9);
            r->pn[0] = en; r->pn[1] = dp;
            for (int i = 0; i < g_cg.nt && !g_gr.err; i++) {
                const CgTool *t = &g_cg.t[i];
                char b[80]; const int n = snprintf(b, sizeof b, "\"%s\"", t->name);
                g_gr.n[en].ev[i] = gr_dup(b, (size_t)n);
                const int o = gn_new(GN_OBJ);
                GNode *on = &g_gr.n[o];
                on->np = t->np; on->req = t->req;
                on->pk = gr_alloc(sizeof(char *) * (size_t)(t->np + 1)); on->pn = gr_alloc(sizeof(int) * (size_t)(t->np + 1));
                if (g_gr.err) break;
                for (int k = 0; k < t->np; k++) {
                    on->pk[k] = gr_dup(t->pk[k], strlen(t->pk[k]));
                    on->pn[k] = t->pt[k] ? t->pn[k] : gn_new(GN_STR);
                }
                g_gr.n[dp].dn[i] = o;
            }
        }
        g_cg.jroot = root;
    }
    g_cg.err = g_gr.err || g_cg.nt == 0;
}

/* ---- passo do automato ---- */
static int cg_isws(unsigned c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static int cg_json_done(const GState *s) {
    if (s->sp == 0) return 1;
    char tmp[16];
    return gr_completion(s, tmp, sizeof tmp) == 0;
}
static void cg_json_begin(CgState *s, int node) {
    memset(&s->js, 0, offsetof(GState, f));
    gn_push(&s->js, GF_VAL, node);
}
/* um byte do valor JSON tipado; 1 = consumido, 0 = recusado, 2 = o valor ja terminou e o byte e do que vem depois */
static int cg_json_byte(CgState *s, unsigned char c) {
    if (g_cg.budget && s->n >= g_cg.budget) {     /* teste: orcamento do valor acabou, so o caminho mais curto ate o fim */
        char comp[256];
        const int cl = s->js.sp ? gr_completion(&s->js, comp, sizeof comp) : 0;
        if (cl == 0) return 2;
        if (cl < 0 || c != (unsigned char)comp[0]) return 0;
    }
    s->n++;
    if (s->js.sp == 0) return 2;
    GState tmp;
    memcpy(&tmp, &s->js, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->js.sp);
    if (gn_step(&tmp, c)) { memcpy(&s->js, &tmp, offsetof(GState, f) + sizeof(GFrame) * (size_t)tmp.sp); return 1; }
    return cg_json_done(&s->js) ? 2 : 0;
}
/* casa nb+c com o comeco de alguma das opcoes (lista terminada em NULL); devolve o indice da completa ou -1 */
static int cg_name_byte(CgState *s, unsigned char c, const char *const *opt, int nopt, uint64_t skip, char term) {
    if (c == (unsigned char)term) {
        for (int i = 0; i < nopt; i++) if (!((skip >> (i & 63)) & 1) && (int)strlen(opt[i]) == s->pos && !strncmp(opt[i], s->nb, (size_t)s->pos)) return i;
        return -2;
    }
    if (s->pos + 1 >= CG_NAME) return -2;
    s->nb[s->pos] = (char)c;
    for (int i = 0; i < nopt; i++)
        if (!((skip >> (i & 63)) & 1) && (int)strlen(opt[i]) > s->pos && !strncmp(opt[i], s->nb, (size_t)s->pos + 1)) { s->pos++; return -1; }
    return -2;
}
static const char *cg_tool_names[CG_MAXT];
static uint64_t g_cg_skip;         /* nomes de ferramenta DESFAVORECIDOS pelo fluxo neste momento (bit = indice em cg_tool_names); agent/flow.c. Nunca proibe: so da o vies em cg_check */
static int g_cg_probe;             /* 1 so dentro da sonda de cg_check: o automato trata g_cg_skip como "fora"; fora da sonda ele aceita qualquer nome */
static float g_cg_bias = 3.0f;     /* penalidade (unidades de logit) dos tokens que so levam a ferramentas desfavorecidas; KATANA_FLOW_BIAS */
static const char *cg_prop_names(const CgTool *t, const char **out) { for (int i = 0; i < t->np; i++) out[i] = t->pk[i]; return NULL; }
/* literal com espacos antes (ate CG_WS) */
static int cg_lit(CgState *s, unsigned char c, const char *lit, int next) {
    if (s->pos == 0 && cg_isws(c)) return ++s->ws <= CG_WS;
    if (c != (unsigned char)lit[s->pos]) return 0;
    if (!lit[++s->pos]) { s->ph = next; s->pos = 0; s->ws = 0; }
    return 1;
}
static void cg_enter(CgState *s, int ph) { s->ph = ph; s->pos = 0; s->ws = 0; s->n = 0; s->qm = 0; }

static int cg_step_xml(CgState *s, unsigned char c);
static int cg_step_g4(CgState *s, unsigned char c);

static int cg_step(CgState *s, unsigned char c) {
    if (s->ph == CG_DONE) return 1;
    if (s->force && s->force[s->fpos]) { if (c != (unsigned char)s->force[s->fpos]) return 0; s->fpos++; }
    if (s->ph == CG_OPEN) {
        const char *op = g_cg.fmt == CGF_G4 ? "<|tool_call>" : "<tool_call>";
        if (c != (unsigned char)op[s->pos]) return 0;
        if (!op[++s->pos]) { s->opened = 1; cg_enter(s, g_cg.fmt == CGF_XML ? CX_FUNC : g_cg.fmt == CGF_JSON ? CJ_JSON : CG_CALL); if (g_cg.fmt == CGF_JSON) cg_json_begin(s, g_cg.jroot); }
        return 1;
    }
    if (g_cg.fmt == CGF_JSON) {
        if (s->ph == CJ_JSON) {
            if (s->js.sp == 0) { cg_enter(s, CJ_END); return cg_lit(s, c, "</tool_call>", CG_DONE); }
            /* teste: orcamento por valor (texto ou numero aberto no topo) e total; o forcado nao conta */
            const int top = s->js.sp ? s->js.f[s->js.sp - 1].t : -1, inval = top == GF_STR || top == GF_NUM;
            const int forcing = s->force && s->force[s->fpos];
            if (g_cg.budget && !forcing && ((inval && s->n >= g_cg.budget) || s->ws >= g_cg.budget * 40)) {
                char comp[256]; const int cl = gr_completion(&s->js, comp, sizeof comp);
                if (cl > 0 && c != (unsigned char)comp[0]) return 0;
            }
            if (!forcing) { s->n = inval ? s->n + 1 : 0; s->ws++; }
            if (!gn_step(&s->js, c)) return 0;
            return 1;
        }
        return cg_lit(s, c, "</tool_call>", CG_DONE);
    }
    return g_cg.fmt == CGF_XML ? cg_step_xml(s, c) : cg_step_g4(s, c);
}

static int cg_step_xml(CgState *s, unsigned char c) {
    static const char PEND[] = "</parameter>";
    for (;;) {
        const CgTool *t = s->tool >= 0 ? &g_cg.t[s->tool] : NULL;
        switch (s->ph) {
        case CX_FUNC: return cg_lit(s, c, "<function=", CX_NAME);
        case CX_NAME: {
            const int r = cg_name_byte(s, c, cg_tool_names, g_cg.nt, g_cg_probe ? g_cg_skip : 0, '>');
            if (r == -2) return 0;
            if (r >= 0) { s->tool = r; s->used = 0; cg_enter(s, CX_TAG); }
            return 1;
        }
        case CX_TAG: {                  /* <parameter= ou </function> (este so com as obrigatorias) */
            if (s->pos == 0 && cg_isws(c)) return ++s->ws <= CG_WS;
            const uint64_t all = t->np >= 64 ? ~0ULL : (1ULL << t->np) - 1;
            const int canp = (s->used & all) != all, canf = !(t->req & ~s->used);
            static const char *const L[2] = { "<parameter=", "</function>" };
            int ok = 0;
            for (int i = 0; i < 2; i++) if ((i ? canf : canp) && L[i][s->pos] == (char)c && !strncmp(L[i], s->nb, (size_t)s->pos)) ok = 1;
            if (!ok) return 0;
            s->nb[s->pos++] = (char)c;
            if (s->pos == 11) {
                if (!strncmp(s->nb, L[0], 11)) cg_enter(s, CX_PNAME);
                else { cg_enter(s, s->opened ? CX_END : CG_DONE); }
            }
            return 1;
        }
        case CX_PNAME: {
            const char *pn[CG_MAXP]; cg_prop_names(t, pn);
            const int r = cg_name_byte(s, c, pn, t->np, s->used, '>');
            if (r == -2) return 0;
            if (r >= 0) {
                s->prop = r; s->used |= 1ULL << r;
                if (t->pt[r]) { cg_enter(s, CX_VSTART); cg_json_begin(s, t->pn[r]); }
                else cg_enter(s, CX_STR);
            }
            return 1;
        }
        case CX_VSTART:                 /* valor tipado: espacos, depois JSON (ou linhas de texto em todos) */
            if (cg_isws(c)) return ++s->ws <= CG_WS;
            if (t->pt[s->prop] == 2 && c != '[') { cg_enter(s, CX_STR); continue; }
            if (t->pt[s->prop] == 2) { cg_enter(s, CX_LB); return 1; }   /* "[x] passo" ou array JSON: decide em seguida */
            cg_enter(s, CX_JSON);
            continue;
        case CX_LB: {                   /* depois de '[': "[x]" "[>]" "[~]" "[ ]" sao linhas; o resto e JSON */
            if (s->pos == 0 && c && strchr("xX>~ ", c)) { s->nb[0] = (char)c; s->pos = 1; return 1; }
            const int lines = s->pos == 1 && c == ']';
            char pend[3] = { '[', s->nb[0], (char)c }; const int np = s->pos == 1 ? 3 : 2;
            if (np == 2) pend[1] = (char)c;
            if (lines) { cg_enter(s, CX_STR); s->n = 3; return 1; }
            cg_enter(s, CX_JSON);
            for (int i = 0; i < np; i++) { const int r = cg_json_byte(s, (unsigned char)pend[i]); if (r != 1) return 0; }
            return 1;
        }
        case CX_JSON: {
            const int r = cg_json_byte(s, c);
            if (r == 2) { cg_enter(s, CX_AFTERV); continue; }
            return r;
        }
        case CX_AFTERV: return cg_lit(s, c, PEND, CX_TAG);
        case CX_STR:                    /* texto livre ate </parameter> */
            if (g_cg.budget && s->n >= g_cg.budget && c != (unsigned char)PEND[s->pos]) return 0;
            s->n++;
            if (c == (unsigned char)PEND[s->pos]) { if (!PEND[++s->pos]) cg_enter(s, CX_TAG); }
            else s->pos = c == '<';
            return 1;
        case CX_END: return cg_lit(s, c, "</tool_call>", CG_DONE);
        default: return 0;
        }
    }
}

/* Gemma 4: valor tipado escrito com <|"|> no lugar das aspas e chaves sem aspas; traduz para JSON e passa na gramatica */
static int cg_g4_feed(CgState *s, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        const int r = cg_json_byte(s, (unsigned char)b[i]);
        if (r != 1) return r;
    }
    return 1;
}
static int cg_g4_content(CgState *s, unsigned char c) {
    const GFrame *f = s->js.sp ? &s->js.f[s->js.sp - 1] : NULL;
    if (f && (f->t == GF_KEY || f->t == GF_STR) && s->bare && c == ':') { s->bare = 0; return cg_g4_feed(s, "\":", 2); }
    if (f && f->t == GF_STR) {                      /* texto: escapa como JSON */
        char e[8]; int n = 0;
        if (c == '"' || c == '\\') { e[n++] = '\\'; e[n++] = (char)c; }
        else if (c == '\n') { e[n++] = '\\'; e[n++] = 'n'; }
        else if (c == '\t') { e[n++] = '\\'; e[n++] = 't'; }
        else if (c == '\r') { e[n++] = '\\'; e[n++] = 'r'; }
        else if (c < 0x20) n = snprintf(e, sizeof e, "\\u%04x", c);
        else e[n++] = (char)c;
        return cg_g4_feed(s, e, n);
    }
    if (f && f->t == GF_OBJ && (f->ph == 0 || f->ph == 3) && (isalnum(c) || c == '_')) {   /* chave sem aspas */
        s->bare = 1;
        const char e[2] = { '"', (char)c };
        return cg_g4_feed(s, e, 2);
    }
    return cg_json_byte(s, c);
}
/* o valor JSON aceitaria uma aspa agora? (so entao <|"|> pode comecar; depois de um numero, nao) */
static int cg_quote_ok(const CgState *s) {
    if (s->js.sp == 0) return 0;
    GState tmp;
    memcpy(&tmp, &s->js, offsetof(GState, f) + sizeof(GFrame) * (size_t)s->js.sp);
    return gn_step(&tmp, '"');
}
static int cg_step_g4(CgState *s, unsigned char c) {
    for (;;) {
        const CgTool *t = s->tool >= 0 ? &g_cg.t[s->tool] : NULL;
        switch (s->ph) {
        case CG_CALL: return cg_lit(s, c, "call:", CG_GNAME);
        case CG_GNAME: {
            const int r = cg_name_byte(s, c, cg_tool_names, g_cg.nt, g_cg_probe ? g_cg_skip : 0, '{');
            if (r == -2) return 0;
            if (r >= 0) { s->tool = r; s->used = 0; cg_enter(s, CG_KEY); }
            return 1;
        }
        case CG_KEY: case CG_KEY2: {
            if (s->ph == CG_KEY && s->pos == 0 && c == '}') { if (t->req & ~s->used) return 0; cg_enter(s, CG_CLOSE); return 1; }
            const char *pn[CG_MAXP]; cg_prop_names(t, pn);
            const int r = cg_name_byte(s, c, pn, t->np, s->used, ':');
            if (r == -2) return 0;
            if (r >= 0) {
                s->prop = r; s->used |= 1ULL << r;
                if (t->pt[r]) { cg_enter(s, CG_VSTART); cg_json_begin(s, t->pn[r]); s->bare = 0; }
                else cg_enter(s, CG_QOPEN);
            }
            return 1;
        }
        case CG_QOPEN: { const int r = cg_lit(s, c, CG_G4Q, CG_GSTR); if (s->ph == CG_GSTR) s->nb[0] = 0; return r; }
        case CG_VSTART:
            if (t->pt[s->prop] == 2 && c == '<') { cg_enter(s, CG_QOPEN); continue; }   /* todos como texto */
            cg_enter(s, CG_GJSON);
            continue;
        case CG_GSTR:                    /* texto livre ate <|"|> */
            if (g_cg.budget && s->n >= g_cg.budget && c != (unsigned char)CG_G4Q[s->pos]) return 0;
            s->n++;
            if (c == (unsigned char)CG_G4Q[s->pos]) { if (!CG_G4Q[++s->pos]) { cg_enter(s, CG_SEP); return 1; } }
            else s->pos = c == '<';
            {   /* os marcadores de chamada e de resposta nao podem aparecer no texto: o leitor da chamada cortaria ali
                   (no Gemma 4 sao um token so, o modelo pode sorteia-los dentro de um valor) */
                const size_t W = sizeof s->nb - 1, l = strlen(s->nb);
                if (l == W) memmove(s->nb, s->nb + 1, W - 1);
                s->nb[l == W ? W - 1 : l] = (char)c; s->nb[l == W ? W : l + 1] = 0;
                static const char *const bad[] = {"<tool_call|>", "<|tool_call>", "<|tool_response>", NULL};
                const size_t nl = strlen(s->nb);
                for (int i = 0; bad[i]; i++) { const size_t bl = strlen(bad[i]); if (nl >= bl && !memcmp(s->nb + nl - bl, bad[i], bl)) return 0; }
            }
            return 1;
        case CG_GJSON: {
            /* <|"|> vira aspas; um prefixo dele fica pendente ate decidir */
            if (c == (unsigned char)CG_G4Q[s->qm] && (s->qm || cg_quote_ok(s))) {
                if (!CG_G4Q[++s->qm]) { s->qm = 0; const int r = cg_json_byte(s, '"'); if (r == 2) { cg_enter(s, CG_SEP); continue; } return r; }
                return 1;
            }
            if (s->qm) {                 /* nao era o marcador: os bytes pendentes sao conteudo */
                const int q = s->qm; s->qm = 0;
                for (int i = 0; i < q; i++) { const int r = cg_g4_content(s, (unsigned char)CG_G4Q[i]); if (r != 1) return 0; }
                if (c == '<') { s->qm = 1; return 1; }
            }
            const int r = cg_g4_content(s, c);
            if (r == 2) { cg_enter(s, CG_SEP); continue; }
            return r;
        }
        case CG_SEP:
            if (c == ',') { cg_enter(s, CG_KEY2); return 1; }
            if (c == '}') { if (t->req & ~s->used) return 0; cg_enter(s, CG_CLOSE); return 1; }
            return 0;
        case CG_CLOSE: return cg_lit(s, c, "<tool_call|>", CG_DONE);
        default: return 0;
        }
    }
}

static int cg_bytes(CgState *s, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (s->ph == CG_DONE) return 1; if (!cg_step(s, (unsigned char)b[i])) return 0; }
    return 1;
}
static size_t cg_size(const CgState *s) { return offsetof(CgState, js) + offsetof(GState, f) + sizeof(GFrame) * (size_t)s->js.sp; }

/* estado inicial: logo depois do abridor (opened=1) ou antes dele (CG_OPEN, usado pelo teste que forca a abertura) */
static void cg_start(CgState *s, int before_opener) {
    memset(s, 0, offsetof(CgState, js) + offsetof(GState, f));
    s->tool = -1; s->prop = -1;
    if (before_opener) { s->ph = CG_OPEN; return; }
    s->opened = 1;
    cg_enter(s, g_cg.fmt == CGF_XML ? CX_FUNC : g_cg.fmt == CGF_JSON ? CJ_JSON : CG_CALL);
    if (g_cg.fmt == CGF_JSON) cg_json_begin(s, g_cg.jroot);
}
/* prepara as ferramentas para o formato; 0 se nao ha o que restringir */
static int cg_prepare(int fmt, const char *sys) {
    g_cg.fmt = fmt;
    cg_setup(sys);
    for (int i = 0; i < g_cg.nt; i++) cg_tool_names[i] = g_cg.t[i].name;
    return !g_cg.err;
}

/* ---- amostragem preguicosa ---- */
static int cg_tok_ok(const CgState *s, int t) {
    const int n = g_gr.tlen[t];
    if (n <= 0) return s->ph == CG_DONE;
    const char *b = g_gr.tt + g_gr.toff[t];
    if ((s->ph == CX_STR || s->ph == CG_GSTR) && s->pos == 0 && !g_cg.budget && !memchr(b, '<', (size_t)n) && !(s->force && s->force[s->fpos])) return 1;
    CgState tmp;
    memcpy(&tmp, s, cg_size(s));
    return cg_bytes(&tmp, b, n);
}
/* t0 foi sorteado normalmente; se a chamada nao aceita, sorteia de novo so entre os tokens que ela aceita */
static int cg_tok_ok_probe(const CgState *s, int t) { g_cg_probe = 1; const int r = cg_tok_ok(s, t); g_cg_probe = 0; return r; }
static int cg_check(const float *lg, int V, float temp, int t0) {
    if (!g_cg.on) return t0;
    g_cg.checked++;
    /* vies suave do fluxo: no nome da ferramenta, o token que so leva a ferramentas desfavorecidas perde g_cg_bias de logit; nunca e excluido */
    const int soft = g_cg_skip && (g_cg.st.ph == CX_FUNC || g_cg.st.ph == CX_NAME || g_cg.st.ph == CG_CALL || g_cg.st.ph == CG_GNAME);
    if (!soft && t0 >= 0 && t0 < V && cg_tok_ok(&g_cg.st, t0)) return t0;
    int idx[TOP_K]; float val[TOP_K]; int k = 0;
    const int want = temp <= 0.0f ? 1 : (g_top_k > 0 && g_top_k < TOP_K ? g_top_k : TOP_K);
    for (int t = 0; t < V; t++) {
        if (k == want && lg[t] <= val[k - 1]) continue;      /* o escore so desce (sc <= lg), entao este corte e seguro */
        if (!cg_tok_ok(&g_cg.st, t)) continue;
        const float sc = soft && !cg_tok_ok_probe(&g_cg.st, t) ? lg[t] - g_cg_bias : lg[t];
        if (k == want && sc <= val[k - 1]) continue;
        int j = k < want ? k++ : k - 1;
        while (j > 0 && val[j - 1] < sc) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = sc; idx[j] = t;
    }
    if (k == 0) { g_cg.gaveup++; g_cg.on = 0; return t0; }
    g_cg.fixed++;
    int r = idx[0];
    if (temp > 0.0f && k > 1) {
        for (int i = 0; i < k; i++) val[i] /= temp;
        softmax(val, k);
        const float u = (float)rand() / ((float)RAND_MAX + 1.0f); float acc = 0.0f;
        r = idx[k - 1];
        for (int i = 0; i < k; i++) { acc += val[i]; if (u < acc) { r = idx[i]; break; } }
    }
    if (g_cg.verbose) fprintf(stderr, "[call grammar: token %d -> %d]\n", t0, r);
    return r;
}

static int cg_fmt_of(const Config *c) {
    const char *e = getenv("KATANA_CALL_GRAMMAR");
    if (e && !strcmp(e, "0")) return CGF_OFF;
    if (c->chat_fmt == 8) return CGF_G4;
    if (c->chat_fmt == 10 || c->glm_args || c->chat_fmt == 9) return CGF_OFF;
    if (c->tool_xml) return CGF_XML;
    if (c->chat_fmt == 0) return CGF_JSON;
    return CGF_OFF;
}
/* inicio de uma geracao do agente; 1 se o formato e restringido aqui (senao a geracao segue livre em forward.c) */
static int cg_turn_begin(const Config *c, const Tokenizer *tk, int V) {
    g_cg.on = 0;
    if (!g_agent_sys) return g_cg.ready = 0;
    const int fmt = cg_fmt_of(c);
    g_cg.ready = fmt != CGF_OFF;
    if (!g_cg.ready) return 0;
    g_cg.fmt = fmt;
    const char *e = getenv("KATANA_CALL_GRAMMAR");
    g_cg.verbose = e && !strcmp(e, "2");
    const char *bd = getenv("KATANA_CALL_GRAMMAR_BUDGET");     /* teste: bytes no maximo por valor */
    g_cg.budget = bd ? atoi(bd) : 0;
    static int forced;
    const char *fo = getenv("KATANA_CALL_FORCE");               /* teste: forca a abertura (e o comeco) da primeira chamada */
    if (fo && *fo && !forced) {
        forced = 1;
        gr_prepare(tk, V);
        if (cg_prepare(fmt, g_agent_sys)) { cg_start(&g_cg.st, 1); g_cg.st.force = fo; g_cg.on = 1; g_cg.calls++; }
    }
    return 1;
}
/* token aceito: avanca o automato, ou liga quando o modelo abre uma chamada (fora do raciocinio) */
static void cg_after(const Tokenizer *tk, int V, const Buf *cap, int t, int thinking) {
    if (!g_cg.ready) return;
    if (g_cg.on) {
        const int n = t >= 0 && t < g_gr.V ? g_gr.tlen[t] : 0;
        if (n > 0 && !cg_bytes(&g_cg.st, g_gr.tt + g_gr.toff[t], n)) { g_cg.on = 0; g_cg.gaveup++; }
        if (g_cg.st.ph == CG_DONE) g_cg.on = 0;
        return;
    }
    if (thinking || !cap || !cap->p) return;
    const char *op = g_cg.fmt == CGF_G4 ? "<|tool_call>" : "<tool_call>";
    const size_t ol = strlen(op);
    gr_prepare(tk, V);
    const int tl = t >= 0 && t < g_gr.V ? g_gr.tlen[t] : 0;
    if (cap->n < ol) return;
    /* o abridor termina dentro do ultimo token */
    size_t from = cap->n >= ol + (size_t)tl ? cap->n - ol - (size_t)tl : 0;
    const char *hit = NULL;
    for (size_t i = from; i + ol <= cap->n; i++) if (!memcmp(cap->p + i, op, ol)) hit = cap->p + i;
    if (!hit) return;
    if (!cg_prepare(g_cg.fmt, g_agent_sys)) return;
    cg_start(&g_cg.st, 0);
    g_cg.on = 1; g_cg.calls++;
    const char *rest = hit + ol;
    if (rest < cap->p + cap->n && !cg_bytes(&g_cg.st, rest, (int)(cap->p + cap->n - rest))) { g_cg.on = 0; g_cg.gaveup++; }
    if (g_cg.st.ph == CG_DONE) g_cg.on = 0;
}

/* ---- teste sem modelo: katana calltest ---- */
static int cg_text(const char *txt, int before) {
    cg_start(&g_cg.st, before);
    return cg_bytes(&g_cg.st, txt, (int)strlen(txt)) ? (g_cg.st.ph == CG_DONE ? 2 : 1) : 0;
}
/* o mesmo texto quebrado em pedacos de tamanho variado (tokens que cruzam simbolos) */
static int cg_text_split(const char *txt, int step) {
    cg_start(&g_cg.st, 0);
    const int L = (int)strlen(txt);
    for (int i = 0; i < L; i += step) {
        const int n = i + step <= L ? step : L - i;
        CgState tmp; memcpy(&tmp, &g_cg.st, cg_size(&g_cg.st));
        if (!cg_bytes(&tmp, txt + i, n)) return 0;
        memcpy(&g_cg.st, &tmp, cg_size(&tmp));
    }
    return g_cg.st.ph == CG_DONE ? 2 : 1;
}
static int calltest(void) {
    int fail = 0, n = 0;
#define CT_EXPECT(cond, what) do { n++; if (!(cond)) { fail++; printf("FAILED: %s\n", what); } } while (0)
    const char *sys = "x\n<tools>\n"
        "{\"type\": \"function\", \"function\": {\"name\": \"Read\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"file_path\": {\"type\": \"string\"}, \"offset\": {\"type\": \"integer\"}, \"limit\": {\"type\": \"integer\"}}, \"required\": [\"file_path\"]}}}\n"
        "{\"type\": \"function\", \"function\": {\"name\": \"ReadAll\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"flag\": {\"type\": \"boolean\"}}, \"required\": []}}}\n"
        "{\"type\": \"function\", \"function\": {\"name\": \"Write\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"file_path\": {\"type\": \"string\"}, \"content\": {\"type\": \"string\"}}, \"required\": [\"file_path\", \"content\"]}}}\n"
        "{\"type\": \"function\", \"function\": {\"name\": \"TodoWrite\", \"description\": \"d\", \"parameters\": {\"type\": \"object\", \"properties\": {\"todos\": {\"type\": \"array\", \"items\": {\"type\": \"object\", \"properties\": {\"content\": {\"type\": \"string\"}, \"status\": {\"type\": \"string\"}}, \"required\": [\"content\", \"status\"]}}}, \"required\": [\"todos\"]}}}\n"
        "</tools>";
    g_cg_only_sys = 1;                      /* so as ferramentas do bloco acima */
    struct { int fmt; const char *txt; int want; const char *what; } C[] = {
        /* XML */
        {CGF_XML, "\n<function=Read>\n<parameter=file_path>\nsrc/a.c\n</parameter>\n</function>\n</tool_call>", 2, "xml: Read"},
        {CGF_XML, "\n<function=Read>\n<parameter=file_path>\na.c\n</parameter>\n<parameter=offset>\n320\n</parameter>\n<parameter=limit>\n80\n</parameter>\n</function>\n</tool_call>", 2, "xml: Read with integers"},
        {CGF_XML, "\n<function=Read>\n<parameter=offset>\n3.5\n</parameter>", 0, "xml: integer refuses 3.5"},
        {CGF_XML, "\n<function=Read>\n<parameter=offset>\nten\n</parameter>", 0, "xml: integer refuses text"},
        {CGF_XML, "\n<function=Raed>", 0, "xml: unknown tool"},
        {CGF_XML, "\n<function=ReadAll>\n</function>\n</tool_call>", 2, "xml: name that extends another name"},
        {CGF_XML, "\n<function=Read>\n</function>", 0, "xml: closes before required parameter"},
        {CGF_XML, "\n<function=Read>\n<parameter=path>", 0, "xml: parameter of no schema"},
        {CGF_XML, "\n<function=Read>\n<parameter=file_path>\na\n</parameter>\n<parameter=file_path>", 0, "xml: same parameter twice"},
        {CGF_XML, "\n<function=Write>\n<parameter=file_path>\nx.html\n</parameter>\n<parameter=content>\n<p>a</p>\n<parameter=x> </function> </tool_call> \"q\" \\n {[\n</parameter>\n</function>\n</tool_call>", 2, "xml: free string with markup inside"},
        {CGF_XML, "\n<function=TodoWrite>\n<parameter=todos>\n[{\"content\": \"a\", \"status\": \"pending\"}]\n</parameter>\n</function>\n</tool_call>", 2, "xml: todos as JSON array"},
        {CGF_XML, "\n<function=TodoWrite>\n<parameter=todos>\n[x] read\n[>] fix\n[ ] test\n</parameter>\n</function>\n</tool_call>", 2, "xml: todos as lines"},
        {CGF_XML, "\n<function=TodoWrite>\n<parameter=todos>\n[{\"content\": \"a\"}]", 0, "xml: todos array item missing status"},
        {CGF_XML, "\n<function=ReadAll>\n<parameter=flag>\nyes", 0, "xml: boolean refuses yes"},
        {CGF_XML, "\n<function=Read>\n<parameter=file_path>\na\n</parameter>\n<function=", 0, "xml: tag that is neither parameter nor closing"},
        /* JSON */
        {CGF_JSON, "\n{\"name\": \"Read\", \"arguments\": {\"file_path\": \"a.c\", \"limit\": 5}}\n</tool_call>", 2, "json: Read"},
        {CGF_JSON, "\n{\"name\": \"Read\", \"arguments\": {\"limit\": 5}}", 0, "json: missing required"},
        {CGF_JSON, "\n{\"name\": \"Reed\"", 0, "json: unknown tool"},
        {CGF_JSON, "\n{\"name\": \"TodoWrite\", \"arguments\": {\"todos\": \"[x] a\\n[ ] b\"}}\n</tool_call>", 2, "json: todos as lines"},
        {CGF_JSON, "\n{\"name\": \"TodoWrite\", \"arguments\": {\"todos\": [{\"content\": \"a\", \"status\": \"pending\"}]}}\n</tool_call>", 2, "json: todos as array"},
        {CGF_JSON, "\n{\"name\": \"Read\", \"arguments\": {\"file_path\": \"a\", \"offset\": \"3\"}}", 0, "json: integer as string"},
        /* Gemma 4 */
        {CGF_G4, "call:Read{file_path:<|\"|>a.c<|\"|>,limit:80}<tool_call|>", 2, "g4: Read"},
        {CGF_G4, "call:Read{limit:80}", 0, "g4: missing required"},
        {CGF_G4, "call:Write{content:<|\"|>a \"q\" <b>x</b> {k:1}\n<|\"|>,file_path:<|\"|>x<|\"|>}<tool_call|>", 2, "g4: free string with markup"},
        {CGF_G4, "call:TodoWrite{todos:[{content:<|\"|>a<|\"|>,status:<|\"|>pending<|\"|>}]}<tool_call|>", 2, "g4: todos as array"},
        {CGF_G4, "call:TodoWrite{todos:<|\"|>[x] a\n[ ] b<|\"|>}<tool_call|>", 2, "g4: todos as lines"},
        {CGF_G4, "call:TodoWrite{todos:[{content:<|\"|>a<|\"|>}]}", 0, "g4: todos item missing status"},
        {CGF_G4, "call:Read{file_path:<|\"|>a<|\"|>,offset:x", 0, "g4: integer refuses text"},
        {CGF_G4, "call:Nope{", 0, "g4: unknown tool"},
        {CGF_G4, "call:Read{file_path:<|\"|>a<|\"|>,limit:0<|\"|>", 0, "g4: quote right after a number"},
        {CGF_G4, "call:Read{limit:7,file_path:<|\"|>a<|\"|>}<tool_call|>", 2, "g4: number then next key"},
        {CGF_G4, "call:Read{file_path:<|\"|>a<tool_call|>", 0, "g4: call closer inside a string"},
        {CGF_G4, "call:Read{file_path:<|\"|>a<|tool_call>", 0, "g4: call opener inside a string"},
        {CGF_G4, "call:Write{content:<|\"|><b>x</b> <tool_call> {k:1}<|\"|>,file_path:<|\"|>x<|\"|>}<tool_call|>", 2, "g4: other markup inside a string"},
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        CT_EXPECT(cg_prepare(C[i].fmt, sys), "setup");
        const int r = cg_text(C[i].txt, 0);
        char w[200]; snprintf(w, sizeof w, "%s (got %d, want %d)", C[i].what, r, C[i].want);
        CT_EXPECT(C[i].want == 0 ? r == 0 : r == C[i].want, w);
        if (C[i].want == 2)
            for (int step = 2; step <= 13; step += 3) {   /* tokens que cruzam simbolos */
                char w2[240]; snprintf(w2, sizeof w2, "%s split %d", C[i].what, step);
                CT_EXPECT(cg_text_split(C[i].txt, step) == 2, w2);
            }
    }
    {   /* prefixos validos continuam possiveis; o abridor forcado */
        cg_prepare(CGF_XML, sys);
        CT_EXPECT(cg_text("\n<function=Re", 0) == 1, "xml: valid prefix");
        CT_EXPECT(cg_text("<tool_call>\n<function=Write>\n<parameter=content>\nx", 1) == 1, "xml: before opener");
        CT_EXPECT(cg_text("<tool_cal>", 1) == 0, "xml: wrong opener");
        cg_prepare(CGF_G4, sys);
        CT_EXPECT(cg_text("<|tool_call>call:Read{", 1) == 1, "g4: before opener");
    }
    g_cg_only_sys = -1;                     /* a tabela */
    {   /* segundo conjunto: as ferramentas da tabela de src/tools */
        cg_prepare(CGF_XML, "");
        CT_EXPECT(g_cg.nt > 30, "second set: tools of the table");
        CT_EXPECT(cg_text("\n<function=Bash>\n<parameter=command>\nls -la | wc -l\n</parameter>\n<parameter=timeout>\n5000\n</parameter>\n</function>\n</tool_call>", 0) == 2, "second set: Bash");
        CT_EXPECT(cg_text("\n<function=AskUserQuestion>\n<parameter=questions>\n[{\"question\": \"q?\", \"header\": \"h\", \"options\": [{\"label\": \"a\", \"description\": \"x\"}, {\"label\": \"b\", \"description\": \"y\"}], \"multiSelect\": false}]\n</parameter>\n</function>\n</tool_call>", 0) == 2, "second set: AskUserQuestion array");
        CT_EXPECT(cg_text("\n<function=AskUserQuestion>\n<parameter=questions>\nwhich one?\n", 0) == 0, "second set: AskUserQuestion refuses text");
        CT_EXPECT(cg_text("\n<function=TodoWrite>\n<parameter=todos>\n[x] a\n[~] b\n</parameter>\n</function>\n</tool_call>", 0) == 2, "second set: TodoWrite lines");
        cg_prepare(CGF_JSON, "");
        CT_EXPECT(!g_cg.err, "second set: json grammar fits");
        CT_EXPECT(cg_text("\n{\"name\": \"Grep\", \"arguments\": {\"pattern\": \"a(\", \"-C\": 2}}\n</tool_call>", 0) == 2, "second set json: Grep");
    }
    {   /* o prompt de sistema real do agente: as gramaticas cabem nos tres formatos */
        static char rs[32000];
        const int x0 = g_tool_xml;
        for (int f = CGF_XML; f <= CGF_G4; f++) {
            g_tool_xml = f != CGF_JSON;
            agent_build_sys(rs, sizeof rs, "");
            char w[96]; snprintf(w, sizeof w, "real system prompt, format %d: grammar fits, %d tools", f, g_cg.nt);
            CT_EXPECT(cg_prepare(f, rs) && g_cg.nt > 30, w);
        }
        g_tool_xml = x0;
    }
    printf("calltest: %d tests, %d failures\n", n, fail);
#undef CT_EXPECT
    return fail != 0;
}
