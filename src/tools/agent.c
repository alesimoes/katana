/* ---------- ferramenta Agent: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_AGENT_DESC[] = "Agent (not implemented yet)";
static const char CT_AGENT_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_AGENT_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_agent(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Agent is not implemented yet");
}
