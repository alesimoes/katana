/* ---------- ferramenta WebSearch: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_WEBSEARCH_DESC[] = "WebSearch (not implemented yet)";
static const char CT_WEBSEARCH_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_WEBSEARCH_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_websearch(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: WebSearch is not implemented yet");
}
