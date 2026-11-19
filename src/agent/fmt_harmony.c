/* ---------- gpt-oss: ferramentas no formato harmony ----------
 * Declaracao no developer: namespace functions { // desc\n type nome = (_: { k: string, }) => any; ... }
 * Chamada do modelo: <|channel|>commentary to=functions.nome <|constrain|>json<|message|>{args}<|call|>
 * Resultado: <|start|>functions.nome to=assistant<|channel|>commentary<|message|>{texto}<|end|> + <|start|>assistant */
static void go_tool_decl(Buf *o, const char *tj) {
    const char *fn = g4_js_get(tj, "function");
    if (!fn) fn = tj;
    Buf nb = {0}, db = {0};
    g4_js_text(g4_js_get(fn, "name"), &nb);
    g4_js_text(g4_js_get(fn, "description"), &db);
    if (!nb.p || !nb.n) { free(nb.p); free(db.p); return; }
    if (db.p && db.n) { buf_str(o, "// "); buf_str(o, db.p); buf_str(o, "\n"); }
    buf_str(o, "type "); buf_str(o, nb.p); buf_str(o, " = (_: {\n");
    const char *pm = g4_js_get(fn, "parameters");
    const char *pr = pm ? g4_js_get(pm, "properties") : NULL;
    if (pr && *pr == '{') {
        const char *q = js_ws(pr + 1);
        while (*q == '"') {
            Buf k = {0}, ty = {0};
            if (!js_string(&q, &k)) { free(k.p); break; }
            q = js_ws(q);
            if (*q++ != ':') { free(k.p); break; }
            q = js_ws(q);
            g4_js_text(g4_js_get(q, "type"), &ty);
            const char *ts = (ty.p && (strcmp(ty.p, "integer") == 0 || strcmp(ty.p, "number") == 0)) ? "number"
                           : (ty.p && strcmp(ty.p, "boolean") == 0) ? "boolean" : "string";
            buf_str(o, k.p ? k.p : ""); buf_str(o, ": "); buf_str(o, ts); buf_str(o, ",\n");
            free(k.p); free(ty.p);
            if (!js_skip(&q, 0)) break;
            q = js_ws(q);
            if (*q == ',') q = js_ws(q + 1);
        }
    }
    buf_str(o, "}) => any;\n\n");
    free(nb.p); free(db.p);
}
static void go_agent_sys(char *sys, size_t cap) {
    char *a = strstr(sys, "# Tools\n\nYou may call"), *t0 = strstr(sys, "<tools>\n");
    char *t1 = t0 ? strstr(t0 + 8, "</tools>") : NULL;
    if (!a || !t0 || !t1 || t1 < t0) return;
    char *tail = t1 + 8;
    for (char *q = strstr(t1, "</tool_call>"); q; q = strstr(q + 1, "</tool_call>")) tail = q + 12;
    if (strstr(tail, "</IMPORTANT>")) tail = strstr(tail, "</IMPORTANT>") + 12;
    Buf decl = {0};
    buf_str(&decl, "# Tools\n\n## functions\n\nnamespace functions {\n\n");
    for (char *q = t0 + 8; q < t1; ) {
        char *e = strchr(q, '\n');
        if (!e || e > t1) e = t1;
        const char save = *e; *e = 0;
        go_tool_decl(&decl, q);
        *e = save;
        q = e + 1;
    }
    buf_str(&decl, "} // namespace functions");
    Buf out = {0};
    buf_add(&out, sys, (size_t)(a - sys));
    while (out.n && (out.p[out.n - 1] == '\n' || out.p[out.n - 1] == ' ')) out.n--;
    buf_add(&out, "", 0);
    buf_str(&out, "\n\n"); buf_str(&out, decl.p ? decl.p : "");
    if (*tail) { const char *q = tail; while (*q == '\n') q++; if (*q) { buf_str(&out, "\n\n"); buf_str(&out, q); } }
    snprintf(sys, cap, "%s", out.p ? out.p : "");
    free(decl.p); free(out.p);
}

