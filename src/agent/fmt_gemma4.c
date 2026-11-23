/* ---------- Gemma 4: ferramentas no formato nativo ----------
 * Declaracao: <|tool>declaration:nome{description:<|"|>..<|"|>,parameters:{properties:{k:{type:<|"|>STRING<|"|>}},
 * required:[<|"|>k<|"|>],type:<|"|>OBJECT<|"|>}}<tool|> (chaves em ordem alfabetica, como no template do modelo).
 * Chamada: <|tool_call>call:nome{k:<|"|>valor<|"|>,n:3}<tool_call|>; resposta:
 * <|tool_response>response:nome{value:<|"|>texto<|"|>}<tool_response|>, no mesmo turno do modelo. */
static const char *js_ws(const char *p);
static int js_string(const char **pp, Buf *out);
static int js_skip(const char **pp, int depth);
#define G4Q "<|\"|>"

/* valor da chave key no objeto JSON em obj (aponta para o valor) ou NULL */
static const char *g4_js_get(const char *obj, const char *key) {
    const char *p = js_ws(obj);
    if (*p != '{') return NULL;
    p = js_ws(p + 1);
    while (*p == '"') {
        Buf k = {0};
        if (!js_string(&p, &k)) { free(k.p); return NULL; }
        p = js_ws(p);
        if (*p++ != ':') { free(k.p); return NULL; }
        p = js_ws(p);
        const int hit = k.p && strcmp(k.p, key) == 0;
        free(k.p);
        if (hit) return p;
        if (!js_skip(&p, 0)) return NULL;
        p = js_ws(p);
        if (*p == ',') p = js_ws(p + 1);
    }
    return NULL;
}
/* texto de uma string JSON em v (vazio se nao for string) */
static void g4_js_text(const char *v, Buf *out) {
    buf_clear(out);
    if (v && *v == '"' && !js_string(&v, out)) buf_clear(out);
    if (!out->p) buf_add(out, "", 0);
}
static void g4_upper(Buf *b) { for (size_t i = 0; i < b->n; i++) if (b->p[i] >= 'a' && b->p[i] <= 'z') b->p[i] -= 32; }
/* lista de strings JSON -> [<|"|>a<|"|>,...] */
static void g4_str_list(Buf *o, const char *v) {
    buf_str(o, "[");
    const char *p = js_ws(v);
    if (*p == '[') {
        p = js_ws(p + 1);
        int first = 1;
        while (*p == '"') {
            Buf t = {0};
            if (!js_string(&p, &t)) { free(t.p); break; }
            if (!first) buf_str(o, ",");
            buf_str(o, G4Q); if (t.p) buf_str(o, t.p); buf_str(o, G4Q);
            free(t.p); first = 0;
            p = js_ws(p);
            if (*p == ',') p = js_ws(p + 1);
        }
    }
    buf_str(o, "]");
}
static void g4_props(Buf *o, const char *props, int depth);
/* uma propriedade: k:{description:..,enum/items/properties/required..,type:<|"|>T<|"|>} */
static void g4_prop(Buf *o, const char *key, const char *v, int depth) {
    Buf t = {0}, ty = {0};
    int comma = 0;
    buf_str(o, key); buf_str(o, ":{");
    const char *dv = g4_js_get(v, "description");
    g4_js_text(dv, &t);
    if (dv && t.n) { buf_str(o, "description:" G4Q); buf_str(o, t.p); buf_str(o, G4Q); comma = 1; }
    g4_js_text(g4_js_get(v, "type"), &ty); g4_upper(&ty);
    if (!ty.p) buf_str(&ty, "");            /* sem tipo no esquema: o template escreve type:<|"|><|"|> */
    const char *ev = g4_js_get(v, "enum"), *iv = g4_js_get(v, "items"), *pv = g4_js_get(v, "properties"), *rv = g4_js_get(v, "required");
    if (strcmp(ty.p, "STRING") == 0 && ev) { if (comma) buf_str(o, ","); buf_str(o, "enum:"); g4_str_list(o, ev); comma = 1; }
    if (strcmp(ty.p, "ARRAY") == 0 && iv && *iv == '{') {
        /* como o template: as chaves de items em ordem alfabetica (description, enum, properties, required, type) */
        Buf it = {0}, idd = {0};
        g4_js_text(g4_js_get(iv, "type"), &it); g4_upper(&it);
        const char *ip = g4_js_get(iv, "properties"), *ir = g4_js_get(iv, "required"), *ie = g4_js_get(iv, "enum"), *id = g4_js_get(iv, "description");
        if (comma) buf_str(o, ",");
        buf_str(o, "items:{"); comma = 1;
        int c2 = 0;
        if (id && *id == '"') { g4_js_text(id, &idd); buf_str(o, "description:" G4Q); buf_str(o, idd.p ? idd.p : ""); buf_str(o, G4Q); c2 = 1; }
        if (ie && *ie == '[') { if (c2) buf_str(o, ","); buf_str(o, "enum:"); g4_str_list(o, ie); c2 = 1; }
        if (ip && *ip == '{' && depth < 6) { if (c2) buf_str(o, ","); buf_str(o, "properties:{"); g4_props(o, ip, depth + 1); buf_str(o, "}"); c2 = 1; }
        if (ir && *ir == '[') { if (c2) buf_str(o, ","); buf_str(o, "required:"); g4_str_list(o, ir); c2 = 1; }
        if (it.n) { if (c2) buf_str(o, ","); buf_str(o, "type:" G4Q); buf_str(o, it.p); buf_str(o, G4Q); }
        buf_str(o, "}");
        free(it.p); free(idd.p);
    }
    if (strcmp(ty.p, "OBJECT") == 0) {
        if (pv && *pv == '{' && depth < 6) { if (comma) buf_str(o, ","); buf_str(o, "properties:{"); g4_props(o, pv, depth + 1); buf_str(o, "}"); comma = 1; }
        if (rv && *rv == '[') { if (comma) buf_str(o, ","); buf_str(o, "required:"); g4_str_list(o, rv); comma = 1; }
    }
    if (comma) buf_str(o, ",");
    buf_str(o, "type:" G4Q); buf_str(o, ty.p); buf_str(o, G4Q "}");
    free(t.p); free(ty.p);
}
/* propriedades de um objeto JSON Schema, em ordem alfabetica */
static void g4_props(Buf *o, const char *props, int depth) {
    char *keys[64]; const char *vals[64]; int n = 0;
    const char *p = js_ws(props);
    if (*p != '{') return;
    p = js_ws(p + 1);
    while (*p == '"' && n < 64) {
        Buf k = {0};
        if (!js_string(&p, &k)) { free(k.p); break; }
        p = js_ws(p);
        if (*p++ != ':') { free(k.p); break; }
        p = js_ws(p);
        keys[n] = k.p ? k.p : strdup(""); vals[n] = p; n++;
        if (!js_skip(&p, 0)) break;
        p = js_ws(p);
        if (*p == ',') p = js_ws(p + 1);
    }
    /* ordena as chaves levando o valor junto */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(keys[j - 1], keys[j]) > 0; j--) {
            char *tk = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = tk;
            const char *tv = vals[j]; vals[j] = vals[j - 1]; vals[j - 1] = tv;
        }
    for (int i = 0; i < n; i++) { if (i) buf_str(o, ","); g4_prop(o, keys[i], vals[i], depth); free(keys[i]); }
}
/* uma linha {"type":"function","function":{...}} do bloco <tools> -> <|tool>declaration:...<tool|> */
static int g4_tool_decl(Buf *o, const char *line) {
    const char *f = g4_js_get(line, "function");
    if (!f) return 0;
    Buf nm = {0}, ds = {0}, ty = {0};
    g4_js_text(g4_js_get(f, "name"), &nm);
    if (!nm.n) { free(nm.p); return 0; }
    g4_js_text(g4_js_get(f, "description"), &ds);
    buf_str(o, "<|tool>declaration:"); buf_str(o, nm.p); buf_str(o, "{description:" G4Q); buf_str(o, ds.p); buf_str(o, G4Q);
    const char *pa = g4_js_get(f, "parameters");
    if (pa && *pa == '{') {
        const char *pr = g4_js_get(pa, "properties"), *rq = g4_js_get(pa, "required");
        buf_str(o, ",parameters:{");
        if (pr && *pr == '{' && *js_ws(pr + 1) != '}') { buf_str(o, "properties:{"); g4_props(o, pr, 0); buf_str(o, "},"); }
        if (rq && *rq == '[' && *js_ws(rq + 1) != ']') { buf_str(o, "required:"); g4_str_list(o, rq); buf_str(o, ","); }
        g4_js_text(g4_js_get(pa, "type"), &ty); g4_upper(&ty);
        if (ty.n) { buf_str(o, "type:" G4Q); buf_str(o, ty.p); buf_str(o, G4Q "}"); }
    }
    buf_str(o, "}<tool|>");
    free(nm.p); free(ds.p); free(ty.p);
    return 1;
}
/* prompt de sistema do agente (formato com <tools> JSON) -> texto do sistema do gemma4: as instrucoes de formato
   de chamada saem e as declaracoes vao no fim, no formato nativo */
static char g_g4_tools_json[24000];      /* o bloco <tools> em JSON antes da conversao: a restricao de chamada le daqui */
static void g4_agent_sys(char *sys, size_t cap) {
    char *a = strstr(sys, "# Tools\n\nYou may call"), *t0 = strstr(sys, "<tools>\n");
    char *t1 = t0 ? strstr(t0 + 8, "</tools>") : NULL;      /* o fecho da lista, nao o "<tools></tools>" da frase */
    if (!a || !t0 || !t1 || t1 < t0) { g_g4_tools_json[0] = 0; return; }
    snprintf(g_g4_tools_json, sizeof g_g4_tools_json, "%.*s", (int)(t1 + 8 - t0), t0);
    /* fim do paragrafo de instrucoes de formato: a ultima marca de fecho antes da memoria/extra
       (o texto Qwen traz "<tool_call></tool_call>" no meio e o exemplo "</tool_call>" no fim) */
    char *tail = t1 + 8;
    for (char *q = strstr(t1, "</tool_call>"); q; q = strstr(q + 1, "</tool_call>")) tail = q + 12;
    if (strstr(tail, "</IMPORTANT>")) tail = strstr(tail, "</IMPORTANT>") + 12;
    Buf decl = {0};
    for (char *p = t0 + 8; p < t1; ) {
        char *e = strchr(p, '\n');
        if (!e || e > t1) e = t1;
        const char save = *e; *e = 0;
        g4_tool_decl(&decl, p);
        *e = save;
        p = e + 1;
    }
    Buf out = {0};
    buf_add(&out, sys, (size_t)(a - sys));
    while (out.n && (out.p[out.n - 1] == '\n' || out.p[out.n - 1] == ' ')) out.n--;
    buf_add(&out, "", 0);
    /* declaracoes no fim, como no template do modelo, que escreve o texto do sistema e logo depois as declaracoes, e nada depois delas
       no turno do sistema (docs/new-models.md, Gemma 4) */
    if (*tail) { const char *q = tail; while (*q == '\n') q++; if (*q) { buf_str(&out, "\n\n"); buf_str(&out, q); } }
    while (out.n && (out.p[out.n - 1] == '\n' || out.p[out.n - 1] == ' ')) out.n--;
    buf_add(&out, "", 0);
    if (decl.p) buf_str(&out, decl.p);
    snprintf(sys, cap, "%s", out.p ? out.p : "");
    free(decl.p); free(out.p);
}
/* chamada nativa: call:nome{k:<|"|>v<|"|>,n:1} -> nome e "arguments": {"k": "v", "n": 1}. 0 = nao e chamada */
static int g4_call_parse(const char *tc, const char *end, char *name, size_t ncap, char *js, size_t jcap) {
    const char *f = strstr(tc, "call:");
    if (!f || f >= end) return 0;
    f += 5;
    const size_t nl = strcspn(f, "{\n");
    if (f + nl >= end || f[nl] != '{' || nl == 0 || nl >= ncap) return 0;
    memcpy(name, f, nl); name[nl] = 0;
    size_t o = (size_t)snprintf(js, jcap, "\"arguments\": {");
    const char *p = f + nl + 1;
    const size_t ql = strlen(G4Q);
    int first = 1;
    while (p < end && *p != '}' && o + 16 < jcap) {
        const size_t kl = strcspn(p, ":}");
        if (p[kl] != ':') break;
        o += (size_t)snprintf(js + o, jcap - o, "%s\"%.*s\": ", first ? "" : ", ", (int)kl, p);
        first = 0;
        p += kl + 1;
        if (strncmp(p, G4Q, ql) == 0) {         /* texto: ate o proximo <|"|>, escapado para JSON */
            const char *v = p + ql, *ve = strstr(v, G4Q);
            if (!ve || ve > end) ve = end;
            js[o++] = '"';
            for (const char *q = v; q < ve && o + 8 < jcap; q++) {
                const unsigned char ch = (unsigned char)*q;
                if (ch == '"' || ch == '\\') { js[o++] = '\\'; js[o++] = (char)ch; }
                else if (ch == '\n') { js[o++] = '\\'; js[o++] = 'n'; }
                else if (ch == '\r') { js[o++] = '\\'; js[o++] = 'r'; }
                else if (ch == '\t') { js[o++] = '\\'; js[o++] = 't'; }
                else if (ch < 0x20) js[o++] = ' ';
                else js[o++] = (char)ch;
            }
            js[o++] = '"';
            p = ve < end ? ve + ql : end;
        } else {                                /* numero, booleano, null, lista ou objeto: copiado, <|"|> vira aspas */
            int depth = 0;
            while (p < end && o + 8 < jcap) {
                if (strncmp(p, G4Q, ql) == 0) { js[o++] = '"'; p += ql; continue; }
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') { if (!depth) break; depth--; }
                else if (*p == ',' && !depth) break;
                js[o++] = *p++;
            }
        }
        if (p < end && *p == ',') p++;
    }
    snprintf(js + o, jcap - o, "}");
    return 1;
}

