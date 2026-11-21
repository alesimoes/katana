/* ---------- Agent: subagente com contexto limpo (nome e argumentos do Claude Code) ----------
 * Por baixo e o delegate do katana (src/agent/delegate.c): o subagente so le, usa o mesmo modelo, e so a resposta
 * final dele volta. description e so um rotulo; subagent_type e aceito e ignorado (aqui ha um tipo so). */
static const char CT_AGENT_DESC[] = "Hand a large search or reading job to a sub-agent with a clean context; only its final report comes back. "
    "The sub-agent sees nothing of this conversation: put the whole task in prompt (what to find or read, and what to report). It can read and search, not change files";
static const char CT_AGENT_PROPS[] = "\"description\": {\"type\": \"string\", \"description\": \"The task in 3 to 5 words\"}, "
    "\"prompt\": {\"type\": \"string\", \"description\": \"The complete task for the sub-agent\"}, "
    "\"subagent_type\": {\"type\": \"string\"}";
static const char CT_AGENT_REQ[] = "\"description\", \"prompt\"";
static void ct_agent(const char *js, const char *end, char *out, size_t cap) {
    char *p = ct_str(js, end, "prompt");
    if (!p || strlen(p) < 8) { free(p); snprintf(out, cap, "error: missing prompt (the complete task for the sub-agent: what to find or read, and what to report back)"); return; }
    /* o delegate le a chave "task" de um objeto JSON: monta um com o prompt escapado */
    const size_t n = strlen(p);
    char *a = (char *)malloc(n * 6 + 32); size_t o = 0;
    o += (size_t)sprintf(a + o, "{\"task\": \"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)p[i];
        if (ch == '"' || ch == '\\') { a[o++] = '\\'; a[o++] = (char)ch; }
        else if (ch == '\n') { a[o++] = '\\'; a[o++] = 'n'; }
        else if (ch == '\t') { a[o++] = '\\'; a[o++] = 't'; }
        else if (ch < 0x20) a[o++] = ' ';
        else a[o++] = (char)ch;
    }
    o += (size_t)sprintf(a + o, "\"}");
    tool_delegate(a, a + o, out, cap);
    free(a); free(p);
}
