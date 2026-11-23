/* ---------- katana coder <sub>: despacho dos subcomandos do modulo coder (inteligencia de codigo) ---------- */
typedef int (*CoderCmd)(int argc, char **argv);
static const struct { const char *name, *help; CoderCmd fn; } g_coder_cmds[] = {
    /* cada submodulo acrescenta UMA linha aqui: {"nome", "uso em uma linha", funcao}, e o seu #include em katana.c antes deste arquivo */
    {"symbols", "<arquivo|pasta> [--refs] [--json]: simbolos com intervalo exato e referencias", coder_symbols_main},
    {"kb", "kb check|lookup <termo>|expand \"frase\"|stats  (base de conhecimento de padroes)", coder_kb_main},
    {"index", "index [pasta]: varre e atualiza o catalogo de arquivos (incremental)", coder_cmd_index},
    {"crawl", "crawl <pasta> [--prio <prefixo>] [--changed <caminho>...] [--sweep] [--stop] [--quiet]: o rastreador do indice (niveis N0..N3, um por raiz, sai quando termina)", coder_crawl_main},
    {"status", "status [pasta]: segmentos, arquivos, tamanho em disco, versao", coder_cmd_status},
    {"verify", "verify [pasta]: confere checksums de todas as secoes e o manifesto", coder_cmd_verify},
    {"compact", "compact [pasta]: funde todos os segmentos em um", coder_cmd_compact},
    {"search", "search \"pergunta\" [pasta] [--limit N] [--json] [--why]: busca por nome e texto (canais: exato, subpalavra, bm25, trigrama, literal, conceitual)", coder_search_main},
    {"find-name", "find-name <nome> [pasta] [--limit N] [--json]: arquivos com esse nome (exato, parcial, parecido) e quem cita o identificador", coder_find_name_main},
    {"def", "def <nome> [pasta] [--json] [--lines N|--body]: onde e definido (cascata de nomes, confianca por resultado)", coder_def_main},
    {"refs", "refs <nome> [pasta] [--json] [--limit N]: todas as referencias, por tipo", coder_refs_main},
    {"callers", "callers <nome> [pasta] [--json] [--limit N]: quem chama", coder_callers_main},
    {"callees", "callees <nome> [pasta] [--json] [--limit N]: o que o simbolo chama, em ordem de chamada", coder_callees_main},
    {"uses", "uses <nome> [pasta] [--json] [--limit N]: ocorrencias que nao sao chamadas", coder_uses_main},
    {"outline", "outline <arquivo> [pasta] [--json]: simbolos do arquivo, aninhados", coder_outline_main},
    {"at", "at <arquivo>:<linha> [pasta] [--json]: qual simbolo contem a linha e a hierarquia", coder_at_main},
    {"deps", "deps <arquivo> [pasta] [--json]: o que o arquivo importa e quem o importa", coder_deps_main},
    {"impact", "impact <nome> [pasta] [--depth N] [--nodes N] [--json]: fecho de chamadores", coder_impact_main},
    {"flow", "flow <chave|arquivo[:linha]> [pasta] [--json] | flow --links [pasta]: ligacoes por chave de texto (quem envia/emite/escreve e quem trata/le a mesma chave)", coder_flow_main},
    {"tokens", "tokens \"texto\" [--lang pt|en|auto] [--json] | --file ARQ", coder_tokens_main},
    {NULL, NULL, NULL}
};
static int coder_main(int argc, char **argv) {
    if (argc >= 1) for (int i = 0; g_coder_cmds[i].name; i++) if (!strcmp(argv[0], g_coder_cmds[i].name)) return g_coder_cmds[i].fn(argc - 1, argv + 1);
    fprintf(stderr, "uso: katana coder <subcomando> ...\n");
    for (int i = 0; g_coder_cmds[i].name; i++) fprintf(stderr, "  %-10s %s\n", g_coder_cmds[i].name, g_coder_cmds[i].help);
    return argc >= 1 ? 2 : 0;
}
