/* ---------- ferramentas EnterWorktree, ExitWorktree: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_ENTERWORKTREE_DESC[] = "EnterWorktree (not implemented yet)";
static const char CT_ENTERWORKTREE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_ENTERWORKTREE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_enterworktree(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: EnterWorktree is not implemented yet");
}
static const char CT_EXITWORKTREE_DESC[] = "ExitWorktree (not implemented yet)";
static const char CT_EXITWORKTREE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_EXITWORKTREE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_exitworktree(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: ExitWorktree is not implemented yet");
}
