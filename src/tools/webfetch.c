/* ---------- ferramenta WebFetch: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_WEBFETCH_DESC[] = "WebFetch (not implemented yet)";
static const char CT_WEBFETCH_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_WEBFETCH_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_webfetch(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: WebFetch is not implemented yet");
}
