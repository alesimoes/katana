/* ---------- katana intel <sub>: despacho dos subcomandos do modulo de inteligencia de codigo ---------- */
typedef int (*IntelCmd)(int argc, char **argv);
static const struct { const char *name, *help; IntelCmd fn; } g_intel_cmds[] = {
    /* cada submodulo acrescenta UMA linha aqui: {"nome", "uso em uma linha", funcao}, e o seu #include em katana.c antes deste arquivo */
    {NULL, NULL, NULL}
};
static int intel_main(int argc, char **argv) {
    if (argc >= 1) for (int i = 0; g_intel_cmds[i].name; i++) if (!strcmp(argv[0], g_intel_cmds[i].name)) return g_intel_cmds[i].fn(argc - 1, argv + 1);
    fprintf(stderr, "uso: katana intel <subcomando> ...\n");
    for (int i = 0; g_intel_cmds[i].name; i++) fprintf(stderr, "  %-10s %s\n", g_intel_cmds[i].name, g_intel_cmds[i].help);
    return argc >= 1 ? 2 : 0;
}
