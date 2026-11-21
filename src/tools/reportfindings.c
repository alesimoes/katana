/* ---------- ferramenta ReportFindings ----------
 * Achados de revisao de codigo como lista tipada: valida, mostra como lista numerada e, com KATANA_UI, emite um
 * marcador \x01FINDING arquivo\tlinha\tcategoria\tresumo por achado (mesmo esquema de todo_ui em agent/todo.c). */

static const char CT_REPORTFINDINGS_DESC[] =
    "Report the findings of a code review as a typed list, most severe first. Each finding needs file, summary and failure_scenario "
    "(the concrete input or situation in which the code fails). An empty list means no findings.";
static const char CT_REPORTFINDINGS_PROPS[] =
    "\"findings\": {\"type\": \"array\", \"description\": \"The findings, most severe first (may be empty)\", \"items\": {\"type\": \"object\", \"properties\": {"
    "\"file\": {\"type\": \"string\", \"description\": \"Path of the file\"}, "
    "\"line\": {\"type\": \"integer\", \"description\": \"Line number\"}, "
    "\"summary\": {\"type\": \"string\", \"description\": \"What is wrong\"}, "
    "\"failure_scenario\": {\"type\": \"string\", \"description\": \"Concrete scenario in which it fails\"}, "
    "\"category\": {\"type\": \"string\", \"description\": \"Kind of problem, e.g. bug, security, performance\"}, "
    "\"short_summary\": {\"type\": \"string\", \"description\": \"A few words\"}, "
    "\"verdict\": {\"type\": \"string\", \"description\": \"Your conclusion about the finding\"}, "
    "\"outcome\": {\"type\": \"string\", \"description\": \"What happened to it, e.g. fixed or confirmed\"}}, "
    "\"required\": [\"file\", \"summary\", \"failure_scenario\"]}}, "
    "\"level\": {\"type\": \"string\", \"description\": \"Review effort level, shown in the header\"}";
static const char CT_REPORTFINDINGS_REQ[] = "\"findings\"";

/* marcador da interface: campos sem tab, quebra de linha e \x01 */
static void rf_marker_field(const char *s) {
    for (; *s; s++) putchar((*s == '\t' || *s == '\n' || *s == '\r' || *s == 1) ? ' ' : *s);
}

static void ct_reportfindings(const char *js, const char *end, char *out, size_t cap) {
    const char *a, *z;
    if (!ct_find(js, end, "findings", &a, &z) || *a != '[') {
        snprintf(out, cap, "error: findings must be an array of objects (use [] when there are no findings)");
        return;
    }
    /* 1) valida tudo antes de mostrar qualquer coisa */
    const char *el[100]; const char *ee[100]; int n = 0;
    for (const char *p = a + 1; p < z; ) {
        while (p < z && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= z || *p == ']') break;
        const char *e = ct_skip(p, z);
        if (!e) break;
        if (n == 100) { snprintf(out, cap, "error: too many findings (max 100); report the most severe ones"); return; }
        if (*p != '{') { snprintf(out, cap, "error: findings[%d] must be an object with file, summary and failure_scenario", n); return; }
        el[n] = p; ee[n] = e; n++;
        p = e;
    }
    static const char *const need[] = { "file", "summary", "failure_scenario" };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            char *s = ct_str(el[i], ee[i], need[k]);
            const int bad = !s || !*s;
            free(s);
            if (bad) { snprintf(out, cap, "error: findings[%d] is missing %s (file, summary and failure_scenario are required)", i, need[k]); return; }
        }
    char *level = ct_str(js, end, "level");
    if (n == 0) { snprintf(out, cap, "No findings%s%s", level && *level ? " (level: " : "", level && *level ? level : ""); if (level && *level && strlen(out) + 2 < cap) strcat(out, ")"); free(level); return; }
    /* 2) mostra */
    size_t o = (size_t)snprintf(out, cap, "%d finding%s", n, n == 1 ? "" : "s");
    if (level && *level && o < cap) o += (size_t)snprintf(out + o, cap - o, " (level: %.40s)", level);
    free(level);
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, ", most severe first:\n");
    const int ui = getenv("KATANA_UI") != NULL;
    for (int i = 0; i < n; i++) {
        char *file = ct_str(el[i], ee[i], "file"), *sum = ct_str(el[i], ee[i], "summary"), *fs = ct_str(el[i], ee[i], "failure_scenario");
        char *cat = ct_str(el[i], ee[i], "category"), *ss = ct_str(el[i], ee[i], "short_summary");
        char *vd = ct_str(el[i], ee[i], "verdict"), *oc = ct_str(el[i], ee[i], "outcome");
        const long long line = ct_int(el[i], ee[i], "line", 0);
        char loc[1200];
        if (line > 0) snprintf(loc, sizeof loc, "%s:%lld", file, line); else snprintf(loc, sizeof loc, "%s", file);
        if (o < cap) o += (size_t)snprintf(out + o, cap - o, "\n%d. %s%s%s%s\n   %s\n   Failure scenario: %s\n", i + 1, loc,
                                           cat && *cat ? " [" : "", cat && *cat ? cat : "", cat && *cat ? "]" : "", sum, fs);
        if (ss && *ss && o < cap) o += (size_t)snprintf(out + o, cap - o, "   Short summary: %s\n", ss);
        if (vd && *vd && o < cap) o += (size_t)snprintf(out + o, cap - o, "   Verdict: %s\n", vd);
        if (oc && *oc && o < cap) o += (size_t)snprintf(out + o, cap - o, "   Outcome: %s\n", oc);
        if (ui) {
            fputs("\x01" "FINDING ", stdout); rf_marker_field(file); putchar('\t');
            if (line > 0) printf("%lld", line);
            putchar('\t'); rf_marker_field(cat ? cat : ""); putchar('\t'); rf_marker_field(sum); putchar('\n');
            fflush(stdout);
        }
        free(file); free(sum); free(fs); free(cat); free(ss); free(vd); free(oc);
    }
    if (o >= cap) { o = cap - 1; if (cap > 16) memcpy(out + cap - 14, "\n[truncated]", 13); }
    while (o > 0 && out[o - 1] == '\n') out[--o] = 0;
}
