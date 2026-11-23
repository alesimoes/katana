/* ---------- katana coder <sub>: despacho dos subcomandos do modulo coder (inteligencia de codigo) ---------- */
typedef int (*CoderCmd)(int argc, char **argv);
static const struct { const char *name, *help; CoderCmd fn; } g_coder_cmds[] = {
    /* cada submodulo acrescenta UMA linha aqui: {"nome", "uso em uma linha", funcao}, e o seu #include em katana.c antes deste arquivo */
    {"kb", "kb check|lookup <termo>|expand \"frase\"|stats  (base de conhecimento de padroes)", coder_kb_main},
    {"index", "index [pasta]: varre e atualiza o catalogo de arquivos (incremental)", coder_cmd_index},
    {"status", "status [pasta]: segmentos, arquivos, tamanho em disco, versao", coder_cmd_status},
    {"verify", "verify [pasta]: confere checksums de todas as secoes e o manifesto", coder_cmd_verify},
    {"compact", "compact [pasta]: funde todos os segmentos em um", coder_cmd_compact},
    {NULL, NULL, NULL}
};
static int coder_main(int argc, char **argv) {
    if (argc >= 1) for (int i = 0; g_coder_cmds[i].name; i++) if (!strcmp(argv[0], g_coder_cmds[i].name)) return g_coder_cmds[i].fn(argc - 1, argv + 1);
    fprintf(stderr, "uso: katana coder <subcomando> ...\n");
    for (int i = 0; g_coder_cmds[i].name; i++) fprintf(stderr, "  %-10s %s\n", g_coder_cmds[i].name, g_coder_cmds[i].help);
    return argc >= 1 ? 2 : 0;
}
