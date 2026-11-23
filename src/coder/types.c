/* ---------- modulo coder (inteligencia de codigo) (intel): tipos compartilhados ----------
 * O modulo roda em cima do motor: nao inclui nada de src/agent nem de src/engine alem do que o proprio submodulo declarar.
 * Cada submodulo vive em src/coder/<nome>.c, e registra seu subcomando `katana coder <nome>` em coder.c. */
typedef enum { CL_UNKNOWN, CL_C, CL_CPP, CL_JS, CL_TS, CL_PY, CL_KT, CL_JAVA, CL_GO, CL_RS, CL_SH, CL_QML, CL_PS1, CL_MD, CL_CONFIG, CL_N } CoderLang;
typedef enum { CK_FILE, CK_FUNC, CK_METHOD, CK_CLASS, CK_STRUCT, CK_ENUM, CK_IFACE, CK_VAR, CK_CONST, CK_MACRO, CK_TYPEDEF, CK_NAMESPACE, CK_HEADING, CK_N } CoderKind;
typedef enum { CR_CALL, CR_USE, CR_IMPORT, CR_EXTENDS, CR_IMPLEMENTS, CR_N } CoderRefKind;
/* simbolo definido em um arquivo; parent = indice do simbolo que o contem (ou -1); linhas e bytes inclusivos/exclusivos: [line0, line1], [byte0, byte1) */
typedef struct { char *name, *sig; int kind, parent, line0, line1; size_t byte0, byte1; } CoderSym;
/* referencia feita no corpo de um simbolo (from = indice do simbolo, ou -1 no nivel do arquivo) */
typedef struct { int from, kind, line; char *name; } CoderRef;
typedef struct { int lang; CoderSym *sym; int nsym; CoderRef *ref; int nref; } CoderFile;
