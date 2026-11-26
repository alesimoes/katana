/* ---------- uioe/classify.c: Classificar (docs/uioe.md 3.4 e a tabela de regras de 3.3.1 passo 5, fase U1) ----------
 * Cinco respostas independentes, cada uma com o numero da regra para o traco: dominio, tipo de tarefa, complexidade, se recuperar
 * ajuda, risco. Por regras e listas; nenhum modelo. Uma regra errada custa orcamento, nunca uma conclusao falsa: nada aqui devolve resposta.
 * Em U1 os dominios que o classificador decide sao code, files, chat e system, mais research e memory quando a pista e forte
 * (a URL, "ultima versao", "o que combinamos"), porque o tipo de tarefa web e historico ja existe na tabela; data, legal e finance
 * ficam para o estagio 4 (precisam de um conector registrado para a regra de dominio existir).
 * Decisoes do implementador: a regra ERRO ganha de IMPACTO, que ganha de FLUXO, que ganha de USO, como no documento; "UI_FINDINGS"
 * nao e produzido em U1. HOWTO e o unico tipo cujo dominio principal e code mesmo sem entidade de codigo (os documentos do projeto). */
static const char *const g_ui_greet_vocab[] = {"bom", "dia", "boa", "tarde", "noite", "oi", "ola", "tudo", "bem", "e", "ai", "hello", "hi", "hey", "good", "morning", "afternoon", "evening",
    "obrigado", "obrigada", "valeu", "thanks", "thank", "you", "ok", "okay", "certo", "beleza", "sim", "pode", "continue", "continua", "seguir", "go", "on", "please", "por", "favor", "opa",
    "tchau", "bye", "ate", "logo", "mais", "yes", "yep", "sure", "claro", "otimo", "perfeito", "show", "nao", "no", "tranquilo", "de", "nada", "agora", "now", NULL};
static const char *const g_ui_shellcmd[] = {"ls", "cd", "pwd", "git", "make", "npm", "pip", "docker", "cat", "rm", "mkdir", "cp", "mv", "sudo", "apt", "curl", "wget", "python", "python3", "node", "gcc", "cargo", "go", NULL};

/* pistas por tipo (portugues e ingles, sem acento, minusculas; "*" prefixo, "..." gap) */
static const char *const g_ui_k_web[] = {"na internet", "na web", "on the web", "on the internet", "search the web", "pesquise na web", "busque na web", "ultima versao", "versao atual", "versao mais recente",
    "current version", "latest", "news", "noticia*", "ultimas noticias", "previsao do tempo", "weather", "preco de", "price of", NULL};
static const char *const g_ui_k_hist[] = {"o que...combin*", "o que...decid*", "o que a gente combinou", "da ultima vez", "na ultima sessao", "sessao anterior", "lembra*", "voce disse", "vc disse", "o que voce falou",
    "what did we", "you said", "remember", "last session", "last time", "we agreed", "we decided", "decidimos", "combinamos", "ontem...fal*", "ontem...fiz*", "ontem...conversamos", NULL};
/* U8: pistas de preferencia e decisao anterior (a memoria tambem entra como apoio); as de historico (g_ui_k_hist) ja marcam o tipo history */
static const char *const g_ui_k_pref[] = {"minha preferencia", "minhas preferencias", "prefiro", "prefere*", "preferimos", "nossa decisao", "decisao anterior", "decisoes anteriores", "como costumamos", "como a gente costuma",
    "my preference", "my preferences", "i prefer", "we prefer", "our decision", "previous decision", "earlier decision", "as we usually", NULL};
static const char *const g_ui_k_error[] = {"por que...nao", "porque...nao", "por que...falh*", "why...not", "why...fail*", "erro", "erros", "error", "errors", "falha", "falhas", "falhou", "crash*", "trava", "travou", "exception",
    "stack trace", "traceback", "nao aparece", "nao funciona", "nao funcionou", "parou de funcionar", "stopped working", "quebrou", "broken", "bug", "bugs", "doesnt work", "does not work", "not working", "failed", "failing",
    "segfault", "panic", "undefined reference", "nao compila", "nao carrega", NULL};
static const char *const g_ui_k_impact[] = {"o que quebra", "o que pode quebrar", "o que vai quebrar", "what breaks", "what would break", "what will break", "what could break", "impacto", "impact*",
    "se eu mudar", "se eu alterar", "se eu remover", "se eu apagar", "se eu trocar", "se eu renomear", "if i change", "if i remove", "if i delete", "if i rename", "if i modify", "is it safe to",
    "e seguro", "quem depende", "who depends", "posso remover", "posso apagar", "can i remove", "can i delete", "pode quebrar", "vai quebrar", "afeta*", "affect*", NULL};
static const char *const g_ui_k_config[] = {"config*", "setting*", "opcao", "opcoes", "option", "options", "variavel de ambiente", "variaveis de ambiente", "environment variable", "env var", "timeout",
    "flag", "flags", "parametro", "parametros", "parameter", "parameters", "porta padrao", "default port", NULL};
static const char *const g_ui_k_flow[] = {"como...envi*", "como...mand*", "como...chega*", "como...receb*", "como...funciona*", "como...trata*", "como...comunic*", "como...liga*", "how...send*", "how...sent",
    "how...reach*", "how...work*", "how...handle*", "how...communicat*", "how...connect*", "fluxo", "flow", "o que acontece quando", "what happens when", "quem envia", "quem manda", "quem recebe", "quem trata",
    "who sends", "who receives", "who handles", "onde...trat*", "onde...enviad*", "where...handled", "where...sent", NULL};
static const char *const g_ui_k_usage[] = {"quem chama", "who calls", "quem chamou", "quem usa", "who uses", "onde...usad*", "where...used", "onde...chamad*", "onde...definid*", "onde...defin*", "where...defined",
    "definicao", "definition", "referencia*", "references", "callers", "chamadores", "usages", "quem define", "who defines", "quem referencia", "o que...chama", "what...calls", NULL};
static const char *const g_ui_k_howto[] = {"como instal*", "como us*", "como faco", "como posso", "como eu", "como rod*", "como execut*", "como configur*", "como compil*", "how do i", "how can i", "how to",
    "how should i", "install*", "instal*", "setup", "usage", "tutorial", "guia", "guide", "readme", "getting started", "passo a passo", "step by step", NULL};
static const char *const g_ui_k_overview[] = {"o que e este projeto", "o que e esse projeto", "o que e o projeto", "o que faz este projeto", "o que faz esse projeto", "what does this project do", "what is this project",
    "what is this repo", "estrutura", "structure", "arquitetura", "architecture", "visao geral", "overview", "resumo do projeto", "explique o projeto", "explain the project", "como esta organizado", "how is...organized", NULL};
static const char *const g_ui_k_locate[] = {"onde", "where", "qual arquivo", "which file", "find the", "ache", "procure", NULL};
/* dominio e risco */
static const char *const g_ui_d_code[] = {"funcao", "funcoes", "classe", "classes", "bug", "compilar", "compila", "endpoint", "function", "functions", "class", "build", "commit", "stack trace", "codigo", "code",
    "metodo", "method", "modulo", "module", "biblioteca", "library", "api", "compiler", "compilador", NULL};
static const char *const g_ui_d_files[] = {"arquivo", "arquivos", "pasta", "pastas", "planilha", "pdf", "documento", "documentos", "file", "files", "folder", "folders", "spreadsheet", "document", "documents", "diretorio", NULL};
static const char *const g_ui_d_system[] = {"instale", "instalar", "rode", "rodar", "execute", "executar", "reinicie", "reiniciar", "install", "run", "restart", "kill", "mate o processo", "porta", "processo", "process", "servico", "service", NULL};
static const char *const g_ui_r_run[] = {"rode", "rodar", "execute", "executar", "run", "compile", "compilar", "instale", "instalar", "install", "reinicie", "restart", "inicie", "start", NULL};
static const char *const g_ui_r_destroy[] = {"apagar", "apague", "remover", "remova", "delete", "drop", "reset", "limpar", "limpe", "destroy", "excluir", "exclua", "wipe", "rm", NULL};
static const char *const g_ui_r_secret[] = {"senha", "senhas", "password", "passwords", "secret", "segredo", "token", "tokens", "credencial", "credenciais", "credential", "credentials", "api key", "private key", "chave privada", "cpf", "cartao", NULL};
static const char *const g_ui_x_rewrite[] = {"traduza", "translate", "reescreva", "rewrite", "resuma", "summarize", "summarise", "corrija o texto", "revise o texto", "melhore o texto", "improve this text", NULL};
static const char *const g_ui_x_create[] = {"escreva um", "escreva uma", "write a", "write an", "crie um", "crie uma", "create a", "create an", "gere um", "gere uma", "generate a", "implemente um", "implemente uma", NULL};

static int ui_has_ent(const UioeAsk *a, int kind) { for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == kind) return 1; return 0; }
static int ui_nent_code(const UioeAsk *a) {         /* entidades que apontam para algo existente no codigo */
    int n = 0; for (int i = 0; i < a->nent; i++) { const int k = a->ent[i].kind; if (k == UK_SYMBOL || k == UK_KEY || k == UK_ENV || k == UK_LITERAL || k == UK_ERRTEXT || k == UK_PATH) n++; }
    return n;
}
/* ha entidade de caminho de arquivo de codigo? 1 codigo, 2 so documento/dado, 0 nenhuma */
static int ui_path_class(const UioeAsk *a) {
    int r = 0; char ext[12];
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_PATH) { const int c = ui_path_ext(a->ent[i].text, ext, sizeof ext); if (c == 1) return 1; if (c == 2) r = 2; }
    return r;
}
static int ui_folder_project(const UioeAsk *a) {
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_FOLDER && a->ent[i].resolved[0] && ui_is_project(a->ent[i].resolved)) return 1;
    return 0;
}
static void ui_note(char *dst, size_t cap, const char *fmt, const char *a) { snprintf(dst, cap, fmt, a ? a : ""); }

static void uioe_classify(UioeCtx *cx, const UioeAsk *a, UioeClass *k) {
    (void)cx; memset(k, 0, sizeof *k);
    UioeWords *W = (UioeWords *)calloc(1, sizeof *W); char *lf = (char *)malloc(strlen(a->text) + 2);
    if (!W || !lf) { free(W); free(lf); return; }
    /* palavras do pedido sem o texto de erro e as aspas (a entidade ja os capturou) */
    { char *t = (char *)malloc(strlen(a->text) + 2); snprintf(t, strlen(a->text) + 2, "%s", a->text); ui_lower(t, lf, strlen(t));
      for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_ERRTEXT || a->ent[i].kind == UK_LITERAL) { char *p = strstr(lf, a->ent[i].text); if (p) memset(p, ' ', strlen(a->ent[i].text)); }
      free(t); }
    uw_build(W, lf);
    const char *which = NULL;
    const int nent = a->nent, nterm = a->nterms, ncode = ui_nent_code(a), nurl = ui_has_ent(a, UK_URL);
    const int firstp = uw_any(W, g_ui_c_firstp, NULL) >= 0;
    /* ---- R10: nada a recuperar ---- */
    int greet = W->n > 0 && W->n <= 7 && !nent;
    for (int i = 0; greet && i < W->n; i++) { int ok = 0; for (int g = 0; g_ui_greet_vocab[g]; g++) if (!strcmp(W->w[i], g_ui_greet_vocab[g])) ok = 1; if (!ok) greet = 0; }
    int shell = 0;
    { const char *p = a->text; while (*p == ' ') p++;
      if (!strncmp(p, "$ ", 2) || !strncmp(p, "! ", 2)) shell = 1;
      for (int i = 0; !shell && g_ui_shellcmd[i]; i++) { const size_t l = strlen(g_ui_shellcmd[i]); if (!strncmp(p, g_ui_shellcmd[i], l) && (p[l] == ' ' || p[l] == 0) && W->n >= 1 && strchr(p, '?') == NULL && W->n <= 8) shell = 1; } }
    int kind = -1, sure = 0, rule = 0;
    if (greet) { kind = UI_NONE; sure = 1; rule = 10; snprintf(k->why_kind, sizeof k->why_kind, "greeting or acknowledgement only"); }
    else if (shell) { kind = UI_NONE; sure = 1; rule = 10; snprintf(k->why_kind, sizeof k->why_kind, "a shell command to run, no question"); }
    /* ---- R20 .. R99 ---- */
    if (kind < 0 && (uw_any(W, g_ui_k_web, &which) >= 0 || (nurl && nent == nurl))) { kind = UI_WEB; sure = 1; rule = 20; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which ? which : "URL as the only entity"); }
    if (kind < 0 && uw_any(W, g_ui_k_hist, &which) >= 0) { kind = UI_HISTORY; sure = 1; rule = 30; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); }
    if (kind < 0 && (ui_has_ent(a, UK_ERRTEXT) || uw_any(W, g_ui_k_error, &which) >= 0)) { kind = UI_ERROR; sure = 1; rule = 40;
        if (ui_has_ent(a, UK_ERRTEXT)) snprintf(k->why_kind, sizeof k->why_kind, "error-shaped text in the request"); else ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); }
    if (kind < 0 && uw_any(W, g_ui_k_impact, &which) >= 0) { kind = UI_IMPACT; sure = 1; rule = 50; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); }
    if (kind < 0 && (uw_any(W, g_ui_k_config, &which) >= 0)) { kind = UI_CONFIG; sure = 1; rule = 60; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); }
    if (kind < 0 && ui_has_ent(a, UK_ENV)) { kind = UI_CONFIG; sure = 0; rule = 60; snprintf(k->why_kind, sizeof k->why_kind, "an environment-variable entity"); }
    if (kind < 0 && !firstp) { const int pf = uw_any(W, g_ui_k_flow, &which); if (pf >= 0) { kind = UI_FLOW; sure = 1; rule = 70; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); } }
    if (kind < 0 && !firstp && (a->rel & (UREL_SEND | UREL_RECV)) && nterm >= 1) { kind = UI_FLOW; sure = 0; rule = 70; snprintf(k->why_kind, sizeof k->why_kind, "send/receive relation bit with informative terms"); }
    if (kind < 0 && uw_any(W, g_ui_k_usage, &which) >= 0) { kind = UI_USAGE; sure = 1; rule = 80; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\"", which); }
    if (kind < 0 && uw_any(W, g_ui_k_howto, &which) >= 0 && ncode == 0) { kind = UI_HOWTO; sure = 1; rule = 90; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\", no code entity", which); }
    if (kind < 0 && ncode == 0 && uw_any(W, g_ui_k_overview, &which) >= 0) { kind = UI_OVERVIEW; sure = 1; rule = 95; ui_note(k->why_kind, sizeof k->why_kind, "cue \"%s\", no entity", which); }
    if (kind < 0 && (nent >= 1 || nterm >= 2)) { kind = UI_LOCATE; sure = 0; rule = 99;
        if (uw_any(W, g_ui_k_locate, &which) >= 0) ui_note(k->why_kind, sizeof k->why_kind, "default; locate cue \"%s\"", which); else snprintf(k->why_kind, sizeof k->why_kind, "default (an entity or two informative terms)"); }
    if (kind < 0) { kind = UI_NONE; sure = 0; rule = 10; snprintf(k->why_kind, sizeof k->why_kind, "one informative term at most and no entity"); }
    k->kind = (UioeKind)kind; k->sure = (uint8_t)sure; k->rule_kind = (uint16_t)rule;
    /* U8: a memoria so e consultada com pista de historico (o tipo history) ou de preferencia/decisao anterior; pergunta so de codigo nunca a consulta */
    k->memcue = 0; k->why_memory[0] = 0;
    if (kind == UI_HISTORY) { k->memcue = 1; snprintf(k->why_memory, sizeof k->why_memory, "history cue: %.140s", k->why_kind); }
    else if (kind != UI_NONE) { const char *pw = NULL; if (uw_any(W, g_ui_k_pref, &pw) >= 0) { k->memcue = 1; ui_note(k->why_memory, sizeof k->why_memory, "preference or earlier-decision cue \"%s\"", pw); } }
    /* mudanca: so se a pergunta nao e de explicacao */
    k->change = a->change && kind != UI_ERROR && kind != UI_HISTORY && kind != UI_HOWTO && kind != UI_NONE && kind != UI_IMPACT && kind != UI_FLOW && kind != UI_USAGE;
    /* ---- dominio ---- */
    int nd = 0; UioeDomain dm[3]; int drule = 0;
    const int pc = ui_path_class(a), proj = ui_folder_project(a);
    const int codecue = uw_any(W, g_ui_d_code, NULL) >= 0, filecue = uw_any(W, g_ui_d_files, NULL) >= 0, syscue = uw_any(W, g_ui_d_system, NULL) >= 0;
    if (kind == UI_NONE) { dm[nd++] = shell ? UD_SYSTEM : UD_CHAT; drule = shell ? 8 : 9; snprintf(k->why_domain, sizeof k->why_domain, shell ? "a command to run" : "nothing to retrieve"); }
    else if (kind == UI_WEB) { dm[nd++] = UD_RESEARCH; drule = 6; snprintf(k->why_domain, sizeof k->why_domain, "web cue or URL"); }
    else if (kind == UI_HISTORY) { dm[nd++] = UD_MEMORY; drule = 7; snprintf(k->why_domain, sizeof k->why_domain, "past-tense cue about earlier work"); }
    else {
        if (ncode - (pc == 2 ? 1 : 0) > 0 && (ncode > 0) && !(pc == 2 && ncode == 1)) { dm[nd++] = UD_CODE; drule = 1; snprintf(k->why_domain, sizeof k->why_domain, "a symbol, key, literal or code path was recognized"); }
        else if (proj) { dm[nd++] = UD_CODE; drule = 1; snprintf(k->why_domain, sizeof k->why_domain, "the cited folder is a project"); }
        else if (kind == UI_FLOW || kind == UI_USAGE || kind == UI_IMPACT || kind == UI_CONFIG || kind == UI_ERROR || kind == UI_OVERVIEW || kind == UI_HOWTO) { dm[nd++] = UD_CODE; drule = 2; snprintf(k->why_domain, sizeof k->why_domain, "the task kind is about software"); }
        else if (pc == 2 || filecue || ui_has_ent(a, UK_FOLDER)) { dm[nd++] = UD_FILES; drule = 3; snprintf(k->why_domain, sizeof k->why_domain, "a document path, a folder or a file cue"); }
        else if (codecue) { dm[nd++] = UD_CODE; drule = 4; snprintf(k->why_domain, sizeof k->why_domain, "code cue words"); }
        else if (syscue) { dm[nd++] = UD_SYSTEM; drule = 5; snprintf(k->why_domain, sizeof k->why_domain, "system cue words"); }
        else { dm[nd++] = UD_CODE; drule = 4; snprintf(k->why_domain, sizeof k->why_domain, "default for a retrievable request"); }
        if (nd < 3 && dm[0] != UD_FILES && (pc == 2 || (filecue && !ncode && !proj))) dm[nd++] = UD_FILES;
        if (nd < 3 && dm[0] != UD_SYSTEM && syscue && (kind == UI_HOWTO || kind == UI_LOCATE)) dm[nd++] = UD_SYSTEM;
    }
    for (int i = 0; i < nd; i++) k->domain[i] = dm[i];
    k->ndomain = nd; k->rule_domain = (uint16_t)drule;
    k->complexity = (uint8_t)(nd >= 2 ? UX_MULTISOURCE : (kind == UI_FLOW || kind == UI_IMPACT || kind == UI_ERROR) ? UX_MULTIHOP : UX_LOOKUP);
    /* ---- recuperar ajuda? (C1 a C7) ---- */
    int ret = 0, cr = 7;
    const char *rcol = strchr(a->text, ':');
    while (rcol && rcol[1] == '/' && rcol[2] == '/') rcol = strchr(rcol + 3, ':');      /* o "http:" de um endereco nao e o dois-pontos de "traduza: texto" (U9) */
    const int rewrite = uw_any(W, g_ui_x_rewrite, NULL) >= 0 && (strlen(a->text) > 200 || strchr(a->text, '\n') || (rcol && strlen(rcol + 1) >= 15));
    const int create = uw_any(W, g_ui_x_create, &which) >= 0 && ncode == 0 && !ui_has_ent(a, UK_FOLDER) && kind != UI_HOWTO;
    if (kind == UI_NONE && !shell) { ret = 0; cr = 1; snprintf(k->why_retrieve, sizeof k->why_retrieve, "nothing to look up (greeting, acknowledgement or too short)"); }
    else if (shell) { ret = 0; cr = 4; snprintf(k->why_retrieve, sizeof k->why_retrieve, "a command to run and no question"); }
    else if (dm[0] == UD_CHAT || rewrite) { ret = 0; cr = 2; snprintf(k->why_retrieve, sizeof k->why_retrieve, "the material is in the request itself"); }
    else if (kind == UI_OVERVIEW) { ret = 1; cr = 6; snprintf(k->why_retrieve, sizeof k->why_retrieve, "an overview needs no entity (folder listing and title lines)"); }
    else if (create) { ret = a->place_note ? 2 : 0; cr = 3; snprintf(k->why_retrieve, sizeof k->why_retrieve, "creation request that names nothing existing"); }
    else if (ncode > 0 || nent > 0 || nterm >= 2) {
        ret = 1; cr = 6;
        if (ncode == 0 && a->place_note && nent <= a->nplaces + 1 && nterm < 2) { ret = 2; cr = 5; snprintf(k->why_retrieve, sizeof k->why_retrieve, "only a cited place that had to be located"); }
        else snprintf(k->why_retrieve, sizeof k->why_retrieve, "%d entit%s, %d informative term(s)", nent, nent == 1 ? "y" : "ies", nterm);
    } else { ret = 0; cr = 7; snprintf(k->why_retrieve, sizeof k->why_retrieve, "no entity and fewer than two informative terms"); }
    if (a->place_note && ret == 0 && !shell && kind != UI_NONE) { ret = 2; cr = 5; snprintf(k->why_retrieve, sizeof k->why_retrieve, "a cited place had to be located; saying where it is saves a step"); }
    k->retrieve = (uint8_t)ret; k->rule_retrieve = (uint16_t)cr;
    if (rewrite) { k->domain[0] = UD_CHAT; k->ndomain = 1; k->rule_domain = 9; k->complexity = UX_LOOKUP; snprintf(k->why_domain, sizeof k->why_domain, "a rewrite or translation of text that is in the request"); }
    /* ---- risco ---- */
    uint8_t risk = 0;
    if (k->change) risk |= UR_CHANGES_FILES;
    if (uw_any(W, g_ui_r_run, NULL) >= 0 && !firstp) risk |= UR_RUNS_COMMANDS;
    if (uw_any(W, g_ui_r_destroy, NULL) >= 0) risk |= UR_DESTRUCTIVE | UR_CHANGES_FILES;
    if (dm[0] == UD_RESEARCH || nurl || uw_any(W, g_ui_k_web, NULL) >= 0) risk |= UR_LEAVES_MACHINE;
    if (uw_any(W, g_ui_r_secret, NULL) >= 0) risk |= UR_SENSITIVE;
    for (int i = 0; i < a->nent; i++) if (a->ent[i].kind == UK_PATH && (strstr(a->ent[i].text, ".env") || strstr(a->ent[i].text, "id_rsa") || strstr(a->ent[i].text, ".pem"))) risk |= UR_SENSITIVE;
    k->risk = risk;
    free(W); free(lf);
}
