/* ---------- modulo coder (inteligencia de codigo) (intel): tipos compartilhados ----------
 * O modulo roda em cima do motor: nao inclui nada de src/agent nem de src/engine alem do que o proprio submodulo declarar.
 * Cada submodulo vive em src/coder/<nome>.c, e registra seu subcomando `katana coder <nome>` em coder.c. */
typedef enum { CL_UNKNOWN, CL_C, CL_CPP, CL_JS, CL_TS, CL_PY, CL_KT, CL_JAVA, CL_GO, CL_RS, CL_SH, CL_QML, CL_PS1, CL_MD, CL_CONFIG,
               /* linguagens e formatos acrescentados depois (novos valores sempre antes de CL_N) */
               CL_CS, CL_SWIFT, CL_PHP, CL_RB, CL_LUA, CL_DART, CL_SCALA, CL_SQL, CL_JSON, CL_YAML, CL_TOML, CL_DOCKER, CL_MAKE, CL_CMAKE, CL_HTML, CL_CSS,
               CL_CLJ, CL_LISP, CL_EX, CL_ERL, CL_HS, CL_ML, CL_R, CL_JL, CL_PERL, CL_GROOVY, CL_OBJC, CL_ZIG, CL_NIM, CL_VLANG, CL_FORTRAN, CL_COBOL, CL_ASM,
               CL_PROTO, CL_GQL, CL_TF, CL_VUE, CL_TEX, CL_DOC, CL_XML, CL_PROPS, CL_N } CoderLang;
/* tipos de simbolo: os novos entram sempre antes de CK_N */
typedef enum { CK_FILE, CK_FUNC, CK_METHOD, CK_CLASS, CK_STRUCT, CK_ENUM, CK_IFACE, CK_VAR, CK_CONST, CK_MACRO, CK_TYPEDEF, CK_NAMESPACE, CK_HEADING,
               CK_CTOR, CK_UNION, CK_ENUMERATOR, CK_FIELD, CK_PROPERTY, CK_SIGNAL, CK_SECTION, CK_N } CoderKind;
/* tipos de referencia: CR_TYPE uso como tipo (inclui new T), CR_WRITE atribuicao a um global ou campo, CR_LITERAL texto entre aspas usado como chave */
typedef enum { CR_CALL, CR_USE, CR_IMPORT, CR_EXTENDS, CR_IMPLEMENTS, CR_TYPE, CR_WRITE, CR_LITERAL, CR_N } CoderRefKind;
typedef enum { CV_UNKNOWN, CV_PUBLIC, CV_EXPORTED, CV_FILE, CV_PRIVATE, CV_PROTECTED } CoderVis;       /* CV_FILE: static, nao exportado, _nome por convencao */
typedef enum { CC_EXACT, CC_HEUR, CC_LINE } CoderConf;       /* confianca do intervalo: exato, heuristico (macro, recuperacao), so a linha inicial */
typedef enum { CGK_RETURN, CGK_CONTINUE, CGK_BREAK, CGK_THROW, CGK_EXIT, CGK_FILTER, CGK_N } CoderGuardKind;
enum { CSF_DECL = 1, CSF_ASYNC = 2, CSF_TEST = 4, CSF_ENTRY = 8, CSF_GENERATED = 16, CSF_STATIC = 32 };       /* CoderSym.flags; CSF_DECL = so declaracao (prototipo) */
enum { CRF_MACRO = 1, CRF_GUARD = 2, CRF_DOC = 4, CRF_PRODUCE = 8, CRF_CONSUME = 16, CRF_DEFINE = 32 };       /* CoderRef.flags; os tres ultimos: lado de uma chave (CR_LITERAL) */
/* simbolo definido em um arquivo; parent = indice do simbolo que o contem (ou -1): e o escopo; linhas e bytes inclusivos/exclusivos: [line0, line1], [byte0, byte1).
   Campos novos (zero = desconhecido): body0 = primeiro byte do corpo (0 = sem corpo); vis = CoderVis; flags = CSF_*; conf = CoderConf; shape = impressao da forma do corpo */
typedef struct { char *name, *sig; int kind, parent, line0, line1; size_t byte0, byte1; size_t body0; int vis, flags, conf; uint64_t shape; } CoderSym;
/* referencia feita no corpo de um simbolo (from = indice do simbolo, ou -1 no nivel do arquivo). Campos novos: col = coluna em bytes, a partir de 1 (0 = nao sabe); flags = CRF_* */
typedef struct { int from, kind, line; char *name; int col, flags; } CoderRef;
/* saida antecipada dentro de um corpo: sym = indice do simbolo, kind = CoderGuardKind */
typedef struct { int sym, line, kind; } CoderGuard;
/* um lugar no codigo, para as consultas e para a confirmacao por servidor de linguagem */
typedef struct { const char *path; int line, col; } CoderLoc;
/* campos novos: guard/nguard; errors = quantas vezes o analisador perdeu o nivel e se recuperou (0 = arquivo analisado inteiro) */
/* chave de texto (keys.c): literal que liga arquivos por TEXTO IGUAL. role: PRODUCE envia/emite/escreve/define/chama; CONSUME recebe/trata/le/registra.
   cls: tipo da chave; meth: metodo HTTP (so para rotas; 0 = qualquer ou nao se aplica) */
/* CKR_DEFINE: o CONTRATO define a chave (OpenAPI, AsyncAPI, .proto, GraphQL, JSON Schema, enum/union tipado); CKC_CONTRACT: classe das chaves de contrato (o papel DEFINE e o consumo de
   nome de operacao/arquivo de contrato por cliente gerado); sub = CKS_* (subtipo da chave de contrato, so para CKC_CONTRACT; zero nas demais classes) */
enum { CKR_PRODUCE = 1, CKR_CONSUME = 2, CKR_DEFINE = 3 };
enum { CKC_OTHER = 0, CKC_MSG, CKC_EVENT, CKC_ROUTE, CKC_ENV, CKC_CONFIG, CKC_CMD, CKC_CONTRACT, CKC_N };
enum { CKS_NONE = 0, CKS_ROUTE, CKS_RPC, CKS_OP, CKS_MSG, CKS_SCHEMA, CKS_ENUM, CKS_FILE, CKS_GQL, CKS_N };
enum { CKM_NONE = 0, CKM_GET, CKM_POST, CKM_PUT, CKM_DELETE, CKM_PATCH, CKM_OTHER, CKM_N };
typedef struct { char *key; int line, role, cls, meth, col, sub; } CoderKey;
/* campos novos: guard/nguard; errors = quantas vezes o analisador perdeu o nivel e se recuperou (0 = arquivo analisado inteiro); key/nkey = chaves de texto (keys.c) */
typedef struct { int lang; CoderSym *sym; int nsym; CoderRef *ref; int nref; CoderGuard *guard; int nguard; int errors; CoderKey *key; int nkey; } CoderFile;
