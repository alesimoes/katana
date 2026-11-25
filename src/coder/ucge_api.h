/* ---------- coder/ucge_api.h: contrato estavel do UCGE entre as fases (docs/ucge.md, "API") ----------
 * NAO e incluido por ninguem: o katana e uma unidade de compilacao so (src/katana.c) e os arquivos ucge*.c sao `static`. Este arquivo e a
 * lista do que cada modulo da fase 2 pode usar e do que precisa fornecer. Mudar uma assinatura aqui exige mudar docs/ucge.md e avisar os modulos.
 * Ordem de inclusao (katana.c, depois de coder/symedit.c): ucge.c, ucgecheck.c, ucgetype.c, ucgedoc.c, ucgepy.c, [um arquivo por backend], ucgetool.c.
 * Dependencias: so a biblioteca padrao (<stdio.h> <stdlib.h> <string.h> <stdarg.h> <ctype.h> <math.h>); ucgetool.c e o unico que conhece o katana.
 *
 * ---- entrada e erros (ucge.c) ----
 *   int  cgx_parse(Cgx *g, const char *spec);            texto CSP ou JSON -> g->prog (AST); 0 se houve erro de sintaxe. g e zerado dentro.
 *   void cgx_free(Cgx *g);                                solta AST, simbolos, erros: tudo de uma vez (arena).
 *   g->err[i].line / .msg, g->nerr (ate 24), g->nerr_all  erros com linha (1-based, do texto normalizado se veio de JSON: g->norm) e o que se esperava.
 *   CgxNode: k (CGN_*), id (pre-ordem, estavel), sid (simbolo internado de s), line, ty (tipo apos cgx_check), a/b/c e b1/b2/b3 (filhos), raw[4], ref.
 *   int  cgx_intern(Cgx *g, const char *s);              id >= 1 do nome; cgx_symname(g, id) volta ao texto.
 * ---- validacao (ucgecheck.c) ----
 *   int  cgx_check(Cgx *g);                               valida e anota tipos (n->ty), n->bi (embutida), n->ref (declaracao); 0 com erros em g->err.
 *   void cgx_stats(Cgx *g, CgxStats *st);                 funcoes, classes, testes, raw, nivel L1-L3, lista de operacoes.
 * ---- tipos e inferencia (ucgetype.c; a fase 2 liga ao validador) ----
 *   CgxTypes T; cgx_ty_init/free; cgx_ty_prim/list/map/opt/tuple/fn/class/union/var/gen; cgx_ty_find; cgx_ty_unify; cgx_ty_assignable;
 *   cgx_ty_generalize / cgx_ty_instantiate (let-polimorfismo por niveis: T.level = profundidade do let); cgx_ty_str.
 * ---- geracao (ucgedoc.c + um arquivo por backend) ----
 *   CgxGenOpts {width, comments, indent};  CgxBackend {name, alias, ext, gen};  const CgxBackend *cgx_backend(const char *name);
 *   int gen(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests);   g ja validado; erros do backend em g->err; tests = corpo do harness (ou NULL).
 *   Documentos: cgx_dtext/dline/dsoft/dhard/dcat/dcatn/dnest/dgroup, cgx_render(doc, width, &out), cgx_docs_free.
 * ---- o que a fase 2 acrescenta (nomes reservados) ----
 *   ucgecfg.c    CgxIr *cgx_ir_build(Cgx *g);  void cgx_ir_free(CgxIr *);  void cgx_ir_dump(const CgxIr *, CgxB *);  (CFG, dominadores, SSA; feito, ver docs/ucge.md secao 15)
 *   ucgeflow.c   int cgx_flow_run(CgxIr *ir, CgxFlowReport *r);  (definicoes alcancaveis, vivacidade, constantes, codigo morto, variavel nao usada; feito)
 *   ucgeopt.c    int cgx_opt_run(Cgx *g, const CgxOptOpts *o, CgxOptReport *r); int cgx_opt_fold(Cgx *g);  (otimizador com passes; feito)
 *   ucgeinfer.c  int cgx_infer(Cgx *g, CgxTypes *T);  (substitui o cgx_check de tipos; mesmos erros, tipos mais precisos)
 *   ucgetool.c   void ucge_tool(const char *js, const char *end, char *out, size_t cap);  (a ferramenta CodeGen)
 */
