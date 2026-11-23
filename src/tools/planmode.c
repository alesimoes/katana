/* ---------- EnterPlanMode, ExitPlanMode ----------
 * Mesmo estado do /plan e do /go (g_plan_mode, planmode.c): a trava de somente leitura fica no motor. As duas rodam no
 * modo plano (marcadas como so leitura na tabela); ExitPlanMode pede a aprovacao pelo mesmo caminho do AskUserQuestion. */
static const char CT_ENTERPLANMODE_DESC[] =
    "Enter plan mode for a non-trivial task: you may only read and investigate (the engine refuses anything that changes files or the system). "
    "Investigate, then call ExitPlanMode with your plan so the user can approve it.";
static const char CT_ENTERPLANMODE_PROPS[] = "";
static const char CT_ENTERPLANMODE_REQ[] = "";
static void ct_enterplanmode(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    if (g_in_sub) { snprintf(out, cap, "error: a sub-agent cannot change the plan mode"); return; }
    if (g_plan_mode) { snprintf(out, cap, "Plan mode was already on. Investigate with read-only tools, then call ExitPlanMode with your plan."); return; }
    g_plan_mode = 1;
    plan_ui();
    snprintf(out, cap, "Plan mode is ON: only read-only tools work now. Investigate what you need, write a short numbered plan, then call ExitPlanMode with it (plan) "
                       "and wait for the user's approval. Do not change anything before it is approved.");
}

static const char CT_EXITPLANMODE_DESC[] =
    "Present your plan to the user for approval and leave plan mode when it is approved. Use it when the plan is ready, not before. "
    "If the user does not approve, plan mode stays on: read the feedback and revise the plan.";
static const char CT_EXITPLANMODE_PROPS[] = "\"plan\": {\"type\": \"string\", \"description\": \"The plan to present, as a short numbered list\"}";
static const char CT_EXITPLANMODE_REQ[] = "\"plan\"";
static void ct_exitplanmode(const char *js, const char *end, char *out, size_t cap) {
    char *plan = ct_str(js, end, "plan");
    if (g_in_sub) { snprintf(out, cap, "error: a sub-agent cannot change the plan mode"); goto done; }
    if (!g_plan_mode) { snprintf(out, cap, "error: plan mode is not on, so there is nothing to exit. Just do the work."); goto done; }
    if (!plan || !plan[0]) { snprintf(out, cap, "error: plan is required: the plan text to show the user (a short numbered list)"); goto done; }
    if (!getenv("KATANA_UI") && K_ISATTY(0)) fprintf(stderr, "\n[katana plan]\n%s\n", plan);       /* no terminal o plano aparece inteiro */
    static char q[1500], ans[1500];
    char lab[2][80] = { "Approve", "Keep planning" };
    snprintf(q, sizeof q, "Plan for approval: %.1100s%s Approve it, or type what to change.", plan, strlen(plan) > 1100 ? " [...]" : "");
    if (!ct_ask(q, lab, 2, ans, sizeof ans)) { snprintf(out, cap, "%s", ans); goto done; }
    ct_trim(ans);
    static const char *const yes[] = { "approve", "approved", "yes", "y", "ok", "sim", "go", "/go", NULL };
    int approved = 0;
    for (int i = 0; yes[i]; i++) if (ct_ieq(ans, yes[i])) approved = 1;
    if (approved) {
        g_plan_mode = 0;
        plan_ui();
        snprintf(out, cap, "The user approved the plan. Plan mode is OFF: you may now make changes. Execute the plan.");
    } else if (ct_ieq(ans, "keep planning")) {
        snprintf(out, cap, "The user did not approve yet and gave no details. Plan mode stays ON. Ask what they want changed or investigate further, then call ExitPlanMode again with a revised plan.");
    } else {
        snprintf(out, cap, "The user did not approve the plan. Plan mode stays ON. Their feedback: %s\nRevise the plan and call ExitPlanMode again.", ans);
    }
done:
    free(plan);
}
