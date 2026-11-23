/* ---------- coder/watch.c: vigia de arquivos da sessao (fase 4 de docs/code-index-lifecycle.md) ----------
 * STUB da fase 3: so o ponto de chamada. A fase 4 poe aqui o descritor inotify da sessao, lido sem bloquear nos tres momentos de C.1 (antes de uma
 * consulta Find ou Symbols, depois de cada ferramenta, com a sessao ociosa no prompt), o orcamento de vigias, o estouro da fila virando varredura, e
 * entrega as mudancas ao rastreador (TOUCH pelo socket ou `katana coder crawl --changed`). Chamado por index_ensure (indexctl.c) e, na fase 4, pelo laco
 * do agente (agent/loop.c). Hoje nao faz nada. */
static void watch_poll(void) {}
