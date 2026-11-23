/* ---------- coder/fresh.c: confere um arquivo citado numa resposta contra o catalogo (fase 4, C.4) ----------
 * STUB da fase 3: so o ponto de chamada. A fase 4 compara (tamanho, mtime) do arquivo vivo com o catalogo; se diferem, reanalisa o arquivo (simbolos) e o
 * manda ao rastreador (TOUCH), e conta para a linha `[index: N files changed since indexed; shown from the files]`. Chamado nos tres lugares que releem
 * um arquivo para a evidencia: cs_read_doc (search.c), fd_text (tools/find.c) e, quando a fase 4 chegar, sy_file_load (symquery.c).
 * Devolve 0 = o arquivo esta como o catalogo diz (hoje sempre). */
static int fresh_check(const char *root, const char *rel, uint64_t size, int64_t mtime) { (void)root; (void)rel; (void)size; (void)mtime; return 0; }
