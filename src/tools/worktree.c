/* ---------- ferramentas EnterWorktree e ExitWorktree (docs/tools.md) ----------
 * EnterWorktree cria (ou entra em) um worktree do git e muda a pasta de trabalho do agente para ele; ExitWorktree volta
 * para a pasta original e deixa o worktree (keep) ou o apaga (remove). Estado numa sessao de worktree so. */
#ifndef _WIN32
static const char CT_ENTERWORKTREE_DESC[] =
    "Creates an isolated git worktree of the current repository under .katana/worktrees/<name> on a new branch "
    "worktree-<name>, and switches the working folder into it, so changes do not touch the original checkout. "
    "Use it only when the user asks for a worktree or an isolated copy. 'name' (letters, digits, '-', '_', '.') is "
    "generated when absent. With 'path' it switches into an existing worktree of this repository (as listed by git "
    "worktree list) instead of creating one. Fails outside a git repository or while a worktree session is already "
    "active. Leave the session with ExitWorktree.";
static const char CT_ENTERWORKTREE_PROPS[] =
    "\"name\": {\"type\": \"string\", \"description\": \"Name of the new worktree (letters, digits, -, _, .). Generated when absent.\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"Path of an existing worktree of this repository to switch into instead of creating one.\"}";
static const char CT_ENTERWORKTREE_REQ[] = "";
static const char CT_EXITWORKTREE_DESC[] =
    "Leaves the worktree session started by EnterWorktree and returns the working folder to the original one. "
    "action=\"keep\" (default) leaves the worktree and its branch on disk; action=\"remove\" deletes both, and refuses "
    "when the worktree has uncommitted changes or commits that are not in the original branch unless discard_changes "
    "is true (the error lists them; confirm with the user before passing true). Fails when no worktree session is active.";
static const char CT_EXITWORKTREE_PROPS[] =
    "\"action\": {\"type\": \"string\", \"enum\": [\"keep\", \"remove\"], \"description\": \"keep (default) or remove the worktree and its branch.\"}, "
    "\"discard_changes\": {\"type\": \"boolean\", \"description\": \"With remove: discard uncommitted changes and unmerged commits.\"}";
static const char CT_EXITWORKTREE_REQ[] = "";

static int g_wt_active;                 /* ha uma sessao de worktree */
static int g_wt_created;                /* o worktree foi criado por EnterWorktree (nao foi o da forma path) */
static char g_wt_orig[4200];            /* pasta original, para onde ExitWorktree volta */
static char g_wt_dir[4400];             /* pasta do worktree */
static char g_wt_branch[300];           /* ramo do worktree ("" se destacado) */
static char g_wt_origbranch[300];       /* ramo da pasta original ("" se destacado) */
static char g_wt_base[80];              /* commit da pasta original quando a sessao comecou */

/* texto entre aspas simples para o shell */
static void ct_wt_q(char *dst, size_t cap, const char *s) {
    size_t o = 0;
    if (cap < 3) { if (cap) dst[0] = 0; return; }
    dst[o++] = '\'';
    for (; *s && o + 6 < cap; s++) { if (*s == '\'') { memcpy(dst + o, "'\\''", 4); o += 4; } else dst[o++] = *s; }
    dst[o++] = '\''; dst[o] = 0;
}
/* roda "git <args>" (args ja com aspas) e devolve a saida sem o fim de linha; malloc. *rc = codigo */
static char *ct_wt_git(const char *args, int *rc) {
    char cmd[10400];
    snprintf(cmd, sizeof cmd, "git %s 2>&1", args);
    char *o = run_collect(cmd, 60, 20000, rc);
    if (!o) { o = strdup(""); if (*rc == 0) *rc = -1; }
    size_t n = strlen(o);
    while (n && (o[n - 1] == '\n' || o[n - 1] == '\r')) o[--n] = 0;
    return o;
}
static void ct_wt_ui(void) { if (getenv("KATANA_UI")) { char cwd[4200]; if (getcwd(cwd, sizeof cwd)) { printf("\x01" "CWD %s\t%d\n", cwd, g_trust); fflush(stdout); } } }
static int ct_wt_name_ok(const char *n) {
    if (!*n || strlen(n) > 100 || n[0] == '.') return 0;
    for (; *n; n++) if (!(isalnum((unsigned char)*n) || *n == '-' || *n == '_' || *n == '.')) return 0;
    return 1;
}
/* ramo atual de uma pasta ("" se destacado) */
static void ct_wt_branch_of(const char *dir, char *dst, size_t cap) {
    char q[4400], a[4600]; int rc;
    ct_wt_q(q, sizeof q, dir);
    snprintf(a, sizeof a, "-C %s symbolic-ref --short -q HEAD", q);
    char *o = ct_wt_git(a, &rc);
    snprintf(dst, cap, "%s", rc == 0 ? o : "");
    free(o);
}

static void ct_enterworktree(const char *js, const char *end, char *out, size_t cap) {
    char *name = ct_str(js, end, "name"), *path = ct_str(js, end, "path");
    char a[10000], cwd[4200], root[4300], dir[4400], branch[300]; int rc;
    out[0] = 0;
    if (name && !name[0]) { free(name); name = NULL; }
    if (path && !path[0]) { free(path); path = NULL; }
    if (name && !ct_wt_name_ok(name)) { snprintf(out, cap, "error: invalid worktree name \"%.100s\": use only letters, digits, '-', '_' and '.'", name); goto done; }
    if (g_wt_active) { snprintf(out, cap, "error: already inside a worktree session (%s); call ExitWorktree first", g_wt_dir); goto done; }
    if (!getcwd(cwd, sizeof cwd)) { snprintf(out, cap, "error: cannot read the working folder"); goto done; }
    {
        char *top = ct_wt_git("rev-parse --show-toplevel", &rc);
        if (rc != 0 || !top[0]) { free(top); snprintf(out, cap, "error: not a git repository (%s); EnterWorktree needs the working folder to be inside one", cwd); goto done; }
        snprintf(root, sizeof root, "%s", top); free(top);
        char *head = ct_wt_git("rev-parse HEAD", &rc);
        if (rc != 0) { snprintf(out, cap, "error: the repository has no commits yet; make a commit before creating a worktree"); free(head); goto done; }
        snprintf(g_wt_base, sizeof g_wt_base, "%.70s", head); free(head);
    }
    if (path) {
        char res[4300];
        if (!realpath(path, res)) { snprintf(out, cap, "error: cannot open %s", path); goto done; }
        char *list = ct_wt_git("worktree list --porcelain", &rc);
        int found = 0; char brn[300] = "";
        char *save = NULL;
        for (char *ln = strtok_r(list, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            if (strncmp(ln, "worktree ", 9) == 0) { if (found) break; char rp[4300]; if (realpath(ln + 9, rp) && strcmp(rp, res) == 0) found = 1; }
            else if (found && strncmp(ln, "branch refs/heads/", 18) == 0) snprintf(brn, sizeof brn, "%s", ln + 18);
        }
        free(list);
        if (!found) { snprintf(out, cap, "error: %s is not a worktree of this repository (see git worktree list)", res); goto done; }
        snprintf(dir, sizeof dir, "%s", res); snprintf(branch, sizeof branch, "%s", brn);
        g_wt_created = 0;
    } else {
        char gen[64], qd[4400], qb[400], rd[4400], par[4290];
        if (!name) { snprintf(gen, sizeof gen, "wt-%llx", (unsigned long long)time(NULL)); name = strdup(gen); }
        snprintf(dir, sizeof dir, "%s/.katana/worktrees/%s", root, name);
        snprintf(branch, sizeof branch, "worktree-%s", name);
        struct stat st;
        if (stat(dir, &st) == 0) { snprintf(out, cap, "error: %s already exists; choose another name, or pass path to switch into it", dir); goto done; }
        if (!agent_act("criar um worktree do git", dir)) { snprintf(out, cap, "error: the user denied this action"); goto done; }
        snprintf(par, sizeof par, "%.4200s/.katana/worktrees/x", root); mkdir_parents(par);
        ct_wt_q(qd, sizeof qd, dir); ct_wt_q(qb, sizeof qb, branch); ct_wt_q(rd, sizeof rd, root);
        snprintf(a, sizeof a, "-C %s worktree add -b %s %s", rd, qb, qd);
        char *o = ct_wt_git(a, &rc);
        if (rc != 0) { snprintf(out, cap, "error: git worktree add failed: %.1500s", o); free(o); goto done; }
        free(o);
        g_wt_created = 1;
    }
    ct_wt_branch_of(root, g_wt_origbranch, sizeof g_wt_origbranch);
    if (chdir(dir) != 0) { snprintf(out, cap, "error: cannot enter %s", dir); goto done; }
    snprintf(g_wt_orig, sizeof g_wt_orig, "%s", cwd);
    snprintf(g_wt_dir, sizeof g_wt_dir, "%s", dir);
    snprintf(g_wt_branch, sizeof g_wt_branch, "%s", branch);
    g_wt_active = 1;
    ct_wt_ui();
    snprintf(out, cap, "%s worktree %s on branch %s; the working folder is now %s (it was %s). Call ExitWorktree to leave.",
             g_wt_created ? "created" : "switched into", dir, branch[0] ? branch : "(detached HEAD)", dir, cwd);
done:
    free(name); free(path);
}

static void ct_exitworktree(const char *js, const char *end, char *out, size_t cap) {
    char *act = ct_str(js, end, "action");
    const int discard = ct_bool(js, end, "discard_changes");
    int rc; char q[4500], a[10000], dirsave[4400], brsave[300], qd[4500];
    out[0] = 0;
    const int remove = act && strcmp(act, "remove") == 0;
    if (act && act[0] && strcmp(act, "keep") != 0 && !remove) { snprintf(out, cap, "error: invalid action \"%.40s\": use keep or remove", act); goto done; }
    if (!g_wt_active) { snprintf(out, cap, "error: no worktree session is active (EnterWorktree has not been called in this conversation)"); goto done; }
    ct_wt_q(q, sizeof q, g_wt_dir);
    if (remove && !discard) {
        snprintf(a, sizeof a, "-C %s status --porcelain", q);
        char *st = ct_wt_git(a, &rc);
        snprintf(a, sizeof a, "-C %s log --oneline %s..HEAD", q, g_wt_base);
        char *lg = ct_wt_git(a, &rc);
        if (st[0] || lg[0]) {
            size_t o = (size_t)snprintf(out, cap, "error: the worktree has work that remove would lose; nothing was removed. Pass discard_changes=true only if the user agrees, or use action=\"keep\".");
            if (st[0] && o < cap) o += (size_t)snprintf(out + o, cap - o, "\nuncommitted changes:\n%.1500s", st);
            if (lg[0] && o < cap) snprintf(out + o, cap - o, "\ncommits not in the original branch:\n%.1500s", lg);
            free(st); free(lg); goto done;
        }
        free(st); free(lg);
    }
    if (remove && !agent_act("remover o worktree do git", g_wt_dir)) { snprintf(out, cap, "error: the user denied this action"); goto done; }
    if (chdir(g_wt_orig) != 0) { snprintf(out, cap, "error: cannot return to %s", g_wt_orig); goto done; }
    snprintf(dirsave, sizeof dirsave, "%s", g_wt_dir); snprintf(brsave, sizeof brsave, "%s", g_wt_branch);
    g_wt_active = 0;
    ct_wt_ui();
    if (!remove) {
        snprintf(out, cap, "left the worktree; the working folder is back to %s. The worktree %s and branch %s were kept on disk.", g_wt_orig, dirsave, brsave[0] ? brsave : "(detached)");
        goto done;
    }
    ct_wt_q(qd, sizeof qd, dirsave);
    snprintf(a, sizeof a, "worktree remove --force %s", qd);
    char *o = ct_wt_git(a, &rc);
    if (rc != 0) { snprintf(out, cap, "error: git worktree remove failed: %.1500s (the working folder is back to %s)", o, g_wt_orig); free(o); goto done; }
    free(o);
    size_t n = (size_t)snprintf(out, cap, "removed the worktree %s; the working folder is back to %s.", dirsave, g_wt_orig);
    if (brsave[0] && strcmp(brsave, g_wt_origbranch) != 0) {
        char qb[400]; ct_wt_q(qb, sizeof qb, brsave);
        snprintf(a, sizeof a, "branch -D %s", qb);
        o = ct_wt_git(a, &rc);
        if (n < cap) snprintf(out + n, cap - n, rc == 0 ? " Branch %s deleted." : " Could not delete branch %s: %.300s", brsave, o);
        free(o);
    }
done:
    free(act);
}
#else
static const char CT_ENTERWORKTREE_DESC[] = "EnterWorktree (not available on Windows yet)";
static const char CT_ENTERWORKTREE_PROPS[] = "";
static const char CT_ENTERWORKTREE_REQ[] = "";
static void ct_enterworktree(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
static const char CT_EXITWORKTREE_DESC[] = "ExitWorktree (not available on Windows yet)";
static const char CT_EXITWORKTREE_PROPS[] = "";
static const char CT_EXITWORKTREE_REQ[] = "";
static void ct_exitworktree(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
#endif
