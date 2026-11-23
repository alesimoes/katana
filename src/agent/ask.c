/* ---------- interacao com o usuario: AskUserQuestion ----------
 * O modelo faz UMA pergunta e espera a resposta, com opcoes curtas quando houver. Na interface (KATANA_UI) sai o
 * marcador "CHOICE pergunta<TAB>opcao1<TAB>opcao2..." e a resposta chega na linha "\x01ANS texto" (o desktop mostra as
 * opcoes como botoes junto da caixa de mensagem; texto digitado tambem vale). No terminal: opcoes numeradas e uma linha
 * do teclado. Sem ninguem para responder (entrada por pipe): o modelo recebe a instrucao de decidir sozinho. */
static void ask_flat(char *s) { for (; *s; s++) if (*s == '\n' || *s == '\r' || *s == '\t' || *s == 1) *s = ' '; }
static void tool_ask_user(const char *js, const char *end, char *out, size_t cap) {
    static char q[1500], opts[800], line[4000];
    char *op[5];
    int no = 0;
    if (!json_str(js, end, "question", q, sizeof q) || !q[0]) { snprintf(out, cap, "error: missing question"); return; }
    if (!json_str(js, end, "options", opts, sizeof opts)) opts[0] = 0;
    ask_flat(q); ask_flat(opts);
    for (char *p = opts; *p && no < 5; ) {
        while (*p == ' ' || *p == '|') p++;
        if (!*p) break;
        char *e = strchr(p, '|');
        if (e) *e = 0;
        size_t l = strlen(p);
        while (l && p[l - 1] == ' ') p[--l] = 0;
        if (l) op[no++] = p;
        if (!e) break;
        p = e + 1;
    }
    line[0] = 0;
    if (getenv("KATANA_UI")) {
        printf("\x01" "CHOICE %s", q);
        for (int i = 0; i < no; i++) printf("\t%s", op[i]);
        printf("\n"); fflush(stdout);
        if (!read_line(line, sizeof line)) line[0] = 0;
        if (strncmp(line, "\x01" "ANS ", 5) == 0) memmove(line, line + 5, strlen(line + 5) + 1);
        else if (strncmp(line, "\x01" "ANS", 4) == 0) line[0] = 0;
    } else if (K_ISATTY(0)) {
        fprintf(stderr, "\n[katana asks] %s\n", q);
        for (int i = 0; i < no; i++) fprintf(stderr, "  %d) %s\n", i + 1, op[i]);
        fprintf(stderr, no ? "[number or your own answer] > " : "> ");
        fflush(stderr);
        if (!fgets(line, sizeof line, stdin)) line[0] = 0;
        line[strcspn(line, "\r\n")] = 0;
        const int k = atoi(line);
        if (no && k >= 1 && k <= no && strlen(line) <= 2) snprintf(line, sizeof line, "%s", op[k - 1]);
    } else {
        snprintf(out, cap, "error: nobody can answer right now (non-interactive run). Choose the most reasonable option yourself, say which one you chose and why, and continue");
        return;
    }
    char *a = line;
    while (*a == ' ') a++;
    if (!*a) snprintf(out, cap, "the user gave no answer (cancelled). Do not ask the same question again: choose the safest reasonable option, say which one, and continue; if every option is risky, stop and explain");
    else snprintf(out, cap, "the user answered: %s", a);
}
