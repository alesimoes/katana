/* ---------- ferramenta Read: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_READ_DESC[] = "Read (not implemented yet)";
static const char CT_READ_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_READ_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_read(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Read is not implemented yet");
}
