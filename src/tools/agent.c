/* ---------- Agent: subagente com contexto limpo (ver docs/tools.md) ----------
 * Por baixo e o delegate do katana (src/agent/delegate.c): o subagente so le, usa o mesmo modelo, e so a resposta
 * final dele volta. description e so um rotulo; subagent_type e aceito e ignorado (aqui ha um tipo so). */
static const char CT_AGENT_DESC[] = "Hand a large search or reading job to a sub-agent with a clean context; only its final report comes back. "
    "The sub-agent sees nothing of this conversation: put the whole task in prompt (what to find or read, and what to report). It can read and search, not change files";
static const char CT_AGENT_PROPS[] = "\"description\": {\"type\": \"string\", \"description\": \"The task in 3 to 5 words\"}, "
    "\"prompt\": {\"type\": \"string\", \"description\": \"The complete task for the sub-agent\"}, "
    "\"subagent_type\": {\"type\": \"string\"}";
static const char CT_AGENT_REQ[] = "\"description\", \"prompt\"";
static int ct_agents_note(const char *task, const char *report);      /* src/tools/messages.c: registro para ListAgents e SendMessage */
static void (*g_ct_sub_run)(const char *prompt, char *out, size_t cap);      /* src/tools/messages.c: gancho do subagente */
static void ct_sub_fake(const char *prompt, char *out, size_t cap);
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
    /* teste sem modelo: com KATANA_AGENT_FAKE=1 o subagente e o falso (ou o gancho de teste ja instalado), como em SendMessage e Workflow */
    { const char *fk = getenv("KATANA_AGENT_FAKE");
      if (fk && atoi(fk) == 1) { if (!g_ct_sub_run) g_ct_sub_run = ct_sub_fake; g_ct_sub_run(p, out, cap); }
      else tool_delegate(a, a + o, out, cap); }
    if (strncmp(out, "error:", 6) != 0) {       /* o modelo fica sabendo o nome para continuar este subagente com SendMessage */
        const int id = ct_agents_note(p, out);
        const size_t l = strlen(out);
        char tag[48]; const int tl = snprintf(tag, sizeof tag, "\n[agent-%d]", id);
        if (id > 0 && l + (size_t)tl + 1 < cap) memcpy(out + l, tag, (size_t)tl + 1);
    }
    free(a); free(p);
}
