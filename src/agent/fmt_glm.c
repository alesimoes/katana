/* ---------- GLM (4.5 em diante): chamada de ferramenta do template do modelo ----------
 * <tool_call>nome<arg_key>k</arg_key><arg_value>v</arg_value>...</tool_call>; o resultado volta em
 * <|observation|><tool_response>...</tool_response>. O valor vai como texto (numero e booleano tambem: as
 * ferramentas aceitam os dois como texto, igual ao formato XML). 0 = nao e chamada neste formato. */
static int glm_call_parse(const char *tc, const char *end, char *name, size_t ncap, char *js, size_t jcap) {
    const char *f = tc + 11;                                /* depois de <tool_call> */
    while (f < end && (*f == ' ' || *f == '\n')) f++;
    if (f >= end || *f == '{' || *f == '<') return 0;       /* JSON ou XML: os outros leitores cuidam */
    const char *ak = strstr(f, "<arg_key>");
    const char *ne = ak && ak < end ? ak : end;
    size_t nl = (size_t)(ne - f);
    while (nl && (f[nl - 1] == ' ' || f[nl - 1] == '\n')) nl--;
    if (!nl || nl >= ncap) return 0;
    for (size_t i = 0; i < nl; i++) if (f[i] == ' ' || f[i] == '\n' || f[i] == '"') return 0;
    memcpy(name, f, nl); name[nl] = 0;
    size_t o = (size_t)snprintf(js, jcap, "\"arguments\": {");
    int first = 1;
    for (const char *p = ak; p && p < end && o + 16 < jcap; ) {
        p += 9;
        const char *ke = strstr(p, "</arg_key>");
        if (!ke || ke >= end) break;
        const char *v = strstr(ke, "<arg_value>");
        if (!v || v >= end) break;
        v += 11;
        /* fim do valor: o </arg_value> seguido de outro argumento ou do fim da chamada (o texto pode conter a marca) */
        const char *ve = NULL;
        for (const char *q = strstr(v, "</arg_value>"); q && q < end; q = strstr(q + 1, "</arg_value>")) {
            const char *r = q + 12;
            while (r < end && (*r == ' ' || *r == '\n')) r++;
            if (r >= end || strncmp(r, "<arg_key>", 9) == 0 || strncmp(r, "</tool_call>", 12) == 0) { ve = q; break; }
        }
        const char *next = ve ? ve + 12 : end;
        if (!ve) ve = end;
        o += (size_t)snprintf(js + o, jcap - o, "%s\"%.*s\": \"", first ? "" : ", ", (int)(ke - p), p);
        first = 0;
        for (const char *q = v; q < ve && o + 8 < jcap; q++) {      /* escapa para JSON */
            const unsigned char ch = (unsigned char)*q;
            if (ch == '"' || ch == '\\') { js[o++] = '\\'; js[o++] = (char)ch; }
            else if (ch == '\n') { js[o++] = '\\'; js[o++] = 'n'; }
            else if (ch == '\r') { js[o++] = '\\'; js[o++] = 'r'; }
            else if (ch == '\t') { js[o++] = '\\'; js[o++] = 't'; }
            else if (ch < 0x20) js[o++] = ' ';
            else js[o++] = (char)ch;
        }
        o += (size_t)snprintf(js + o, jcap - o, "\"");
        p = next < end ? strstr(next, "<arg_key>") : NULL;
    }
    snprintf(js + o, jcap - o, "}");
    return 1;
}
