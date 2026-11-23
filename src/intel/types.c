/* ---------- modulo de inteligencia de codigo (intel): tipos compartilhados ----------
 * O modulo roda em cima do motor: nao inclui nada de src/agent nem de src/engine alem do que o proprio submodulo declarar.
 * Cada submodulo vive em src/intel/<nome>.c, e registra seu subcomando `katana intel <nome>` em intel.c. */
typedef enum { IL_UNKNOWN, IL_C, IL_CPP, IL_JS, IL_TS, IL_PY, IL_KT, IL_JAVA, IL_GO, IL_RS, IL_SH, IL_QML, IL_PS1, IL_MD, IL_CONFIG, IL_N } IntelLang;
typedef enum { IK_FILE, IK_FUNC, IK_METHOD, IK_CLASS, IK_STRUCT, IK_ENUM, IK_IFACE, IK_VAR, IK_CONST, IK_MACRO, IK_TYPEDEF, IK_NAMESPACE, IK_HEADING, IK_N } IntelKind;
typedef enum { IR_CALL, IR_USE, IR_IMPORT, IR_EXTENDS, IR_IMPLEMENTS, IR_N } IntelRefKind;
/* simbolo definido em um arquivo; parent = indice do simbolo que o contem (ou -1); linhas e bytes inclusivos/exclusivos: [line0, line1], [byte0, byte1) */
typedef struct { char *name, *sig; int kind, parent, line0, line1; size_t byte0, byte1; } IntelSym;
/* referencia feita no corpo de um simbolo (from = indice do simbolo, ou -1 no nivel do arquivo) */
typedef struct { int from, kind, line; char *name; } IntelRef;
typedef struct { int lang; IntelSym *sym; int nsym; IntelRef *ref; int nref; } IntelFile;
