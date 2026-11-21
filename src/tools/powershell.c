/* ---------- ferramenta PowerShell: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_POWERSHELL_DESC[] = "PowerShell (not implemented yet)";
static const char CT_POWERSHELL_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_POWERSHELL_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_powershell(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: PowerShell is not implemented yet");
}
