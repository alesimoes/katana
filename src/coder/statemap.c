/* ---------- coder/statemap.c: mapa de estado do indice (fase 6, J) ----------
 * STUB da fase 3: so o ponto de chamada. A fase 6 agrega, uma vez por geracao e junto com a visao, quantos arquivos de cada prefixo estao em cada nivel
 * (N0..N3), que e o que a ferramenta pode afirmar ou nao sobre aquela parte da arvore (linhas de status, `NOT FOUND ... so far`, Symbols action=tree).
 * Chamado depois de montada uma visao de leitura (cs_view_open_raw em search.c). `view` aponta para a visao; `prefix` e o prefixo da consulta. */
static int statemap_get(const void *view, const char *prefix) { (void)view; (void)prefix; return 0; }
