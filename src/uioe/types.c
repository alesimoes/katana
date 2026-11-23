/* ---------- uioe/types.c: tipos do UIOE (docs/uioe.md, fases U0 e U1) ----------
 * UIOE = porta de entrada antes do modelo: Interpretar, Classificar, Roteirizar (U1); Recuperar e Orquestrar vem em U3.
 * Nesta entrega nada roda no laco do agente: `katana uioe ask` interpreta e planeja SEM executar e SEM modelo.
 *
 * Decisoes do implementador (o documento deixava abertas):
 *   - sem estado mutavel de arquivo em src/uioe/: tudo pendura em UioeCtx (por chamada) ou UioeSession (do dono: a CLI cria uma
 *     na pilha/heap; o laco criara a sua quando o gancho de U2 existir). O registro de conectores mora na sessao.
 *   - os bits de tipo de entidade usam o prefixo UK_ (o documento usa UE_ para flags de conector e arestas; evita colisao).
 *   - U0 acessa as globais de estado do pedido (g_rs, g_rc, g_intent_*, g_place_dirs, g_fd_*, rr_*, rp_*) por UM instantaneo
 *     (uioe_req_sync, state.c) em vez de mover os dados: o comportamento do laco e das ferramentas nao muda um byte.
 *   - o grafo de evidencia, os disjuntores com contagem real e o cache entram em U3; aqui so os campos que U0/U1 usam.
 *   - a chamada ao modelo nunca existe aqui: nenhum arquivo de src/uioe/ inclui nada de engine/ alem do que ja esta antes. */
#define UIOE_MAXCONN 48
#define UIOE_NENT 32

typedef enum { UIOE_OFF = 0, UIOE_PACK = 1, UIOE_FULL = 2 } UioeMode;
typedef enum { UD_CODE, UD_FILES, UD_DATA, UD_LEGAL, UD_FINANCE, UD_RESEARCH, UD_MEMORY, UD_SYSTEM, UD_CHAT } UioeDomain;
typedef enum { UI_NONE, UI_WEB, UI_HISTORY, UI_ERROR, UI_IMPACT, UI_CONFIG, UI_FLOW, UI_USAGE, UI_HOWTO, UI_OVERVIEW, UI_LOCATE, UI_FINDINGS } UioeKind;
typedef enum { UX_LOOKUP, UX_MULTIHOP, UX_MULTISOURCE } UioeComplex;
typedef enum { UG_NONE, UG_LIMITS, UG_PACKET } UioeDecision;
typedef enum { UO_REQUEST, UO_TOOL, UO_CLI } UioeOrigin;
/* tipos de entidade (3.3) */
typedef enum { UK_SYMBOL, UK_PATH, UK_FOLDER, UK_LITERAL, UK_ERRTEXT, UK_KEY, UK_ENV, UK_URL, UK_DATE, UK_AMOUNT, UK_NAME, UK_CONCEPT } UioeEntKind;
/* bits de relacao (3.3.1 passo 4) */
enum { UREL_CALLERS = 1, UREL_DEFINED = 2, UREL_USES = 4, UREL_SEND = 8, UREL_RECV = 16, UREL_WORKFAIL = 32, UREL_NEG = 64, UREL_CALLEES = 128 };
/* risco (3.4) */
enum { UR_CHANGES_FILES = 1, UR_RUNS_COMMANDS = 2, UR_LEAVES_MACHINE = 4, UR_SENSITIVE = 8, UR_DESTRUCTIVE = 16 };
/* capacidades (3.8.3) */
enum { UQ_WHERE = 1, UQ_DEF = 2, UQ_USES = 4, UQ_CALLERS = 8, UQ_CALLEES = 16, UQ_LINKS = 32, UQ_IMPORTS = 64, UQ_IMPACT = 128, UQ_CONFIG = 256,
       UQ_LITERAL = 512, UQ_STRUCTURE = 1024, UQ_DOC = 2048, UQ_FACT = 4096, UQ_HISTORY = 8192, UQ_EXTERNAL = 16384, UQ_PLACE = 32768, UQ_VERIFY = 65536, UQ_RECIPE = 131072 };
/* custo (3.8.2) e flags do conector */
typedef enum { UC_INSTANT, UC_INDEX, UC_FILES, UC_PROCESS, UC_NETWORK, UC_MODEL } UioeCost;
enum { UCF_READONLY = 1, UCF_LEAVES_MACHINE = 2, UCF_SIDE_EFFECTS = 4, UCF_NEEDS_MODEL = 8, UCF_UNTRUSTED = 16 };

typedef struct {
    char kind;                          /* UioeEntKind */
    char text[200];                     /* como foi escrito */
    char how[120];                      /* como foi reconhecida/resolvida (regra) */
    char resolved[1100];                /* caminho absoluto, quando resolvida no disco */
    int conf;                           /* 0-100 */
    uint8_t guess;                      /* reconhecida so pela forma */
} UioeEntity;

typedef struct { char what[16]; char text[120]; } UioeConstraint;

typedef struct {
    char text[2000];                    /* o pedido original, sem as notas [katana: ...] */
    char lang[4];                       /* "pt", "en", "?" */
    UioeEntity ent[UIOE_NENT]; int nent;
    uint8_t rel;                        /* UREL_* */
    char dir[8];                        /* direcao do fluxo: "send", "recv" ou "" */
    UioeConstraint con[8]; int ncon;
    char root[1100], prefix[512];       /* onde procurar */
    char terms[24][48]; int nterms;     /* termos informativos (forma original) */
    char norm[24][48];                  /* e sua forma normalizada pelo tokenizador */
    char concept[24][80];               /* conceitos da base de conhecimento para cada termo ("" = nenhum) */
    char dropped[24][32]; int ndropped; /* palavras genericas descartadas (palavras de pedido, gatilhos de lugar) */
    char places[4][1100]; int nplaces;  /* pastas citadas e achadas (place_note) */
    char place_cited[4][200]; int nplace_cited; /* o que o usuario escreveu para elas */
    uint8_t place_note;                 /* a pasta citada precisou ser localizada (regra C5) */
    uint8_t change;                     /* o pedido quer alterar algo */
    uint8_t ambiguous; char amb[200];
    uint8_t origin;
    uint8_t inherits;
    uint8_t index_ready;                /* ha armazenamento do indice para a raiz (so olha o disco, nao abre) */
    char index_note[160];
} UioeAsk;

typedef struct {
    UioeDomain domain[3]; int ndomain;
    UioeKind kind; uint8_t sure;
    uint8_t change, complexity, retrieve, risk;
    uint16_t rule_domain, rule_kind, rule_retrieve;
    char why_kind[160], why_retrieve[160], why_domain[160];
} UioeClass;

typedef struct {
    int n;                              /* numero do passo */
    char label[16];                     /* where, links, callers... */
    uint32_t cap;                       /* capacidade (UQ_*) */
    char conn[24];                      /* conector escolhido ("" = nenhum disponivel) */
    char input[220];                    /* consulta */
    int dep;                            /* depende do passo N (0 = nao) */
    uint8_t status;                     /* 0 pronto, 1 indisponivel, 2 nao roteado (politica) */
    char note[140];
} UioeStep;

typedef struct {
    char pipeline[24];
    UioeStep step[24]; int nstep;
    int deadline_ms, token_ceiling;
    int confidence;                     /* antes da recuperacao */
    char signals[8][100]; int nsig;     /* o que somou/subtraiu */
    UioeDecision decision;              /* decisao possivel antes da recuperacao */
    uint16_t rule;                      /* G1..G6 (so G1..G3 sao decididas em U1) */
    char decision_note[200];
    char notrouted[300];                /* fontes que nao entram e por que */
    char sends_out[120];
} UioeRoute;

/* ---- estado por pedido (3.6.2): instantaneo das globais do laco e das ferramentas ---- */
typedef struct { char path[1024]; int a, b, how; } UioeRange;
typedef struct { char path[1024]; long long cov_end, last_off, total; } UioeCursor;
typedef struct { int rr_n, rr_total, rp_n, rp_fail, rp_refused, rp_stuck, bk_n; } UioeLoopState;
typedef struct {
    uint64_t id;
    UioeAsk ask; UioeClass cls; UioeRoute route;
    UioeRange shown[256]; int nshown;   /* apontadas pelo Find (how bit 1) */
    UioeRange read[256]; int nread;     /* entregues pelo Read (how bit 2) */
    UioeCursor cur[64]; int ncur;       /* cursor de leitura por arquivo */
    char intent_user[800], intent_find[400];
    char place_dirs[4][1100]; int nplace;
    char last_query[400]; int nlast;    /* paginacao do Find */
    UioeLoopState loop;
    int packet_tokens, model_steps;
} UioeRequest;
typedef struct { uint8_t open; int fails, skip_left; } UioeBreaker;
typedef struct UioeConnector {
    const char *id;
    uint32_t answers;
    UioeCost cost;
    int conf_typical, need_level;       /* need_level: -1 nenhum, 0 N0, 1 N1, 2 N2, 3 N3 */
    uint32_t flags;
    int default_ms;
    const char *version;
    const char *domain;                 /* "code", "files", "memory"... so para exibir */
    /* available: 1 pronto, 0 nao, 2 parcial; why recebe o motivo. root = pasta de busca. Nao imprime. */
    int (*available)(const struct UioeConnector *self, const char *root, char *why, size_t cap);
    /* run: so em U3 (nulo aqui) */
    int (*run)(void *ctx, const void *call, void *result);
} UioeConnector;
typedef struct {
    UioeRequest *cur;                   /* o pedido corrente (heap) */
    UioeConnector reg[UIOE_MAXCONN]; int nreg;
    UioeBreaker brk[UIOE_MAXCONN];
    char roots[4][1100]; int nroots;
    uint64_t seq;                       /* contador de pedidos */
} UioeSession;
typedef struct {
    UioeSession *s;
    char cwd[1100], root[1100];
    int deadline_ms, token_ceiling, json, trace;
    UioeMode mode;
    time_t now;
} UioeCtx;

static const char *const g_uioe_domname[] = {"code", "files", "data", "legal", "finance", "research", "memory", "system", "chat"};
static const char *const g_uioe_kindname[] = {"none", "web", "history", "error", "impact", "config", "flow", "usage", "howto", "overview", "locate", "findings"};
static const char *const g_uioe_entname[] = {"symbol", "path", "folder", "literal", "error-text", "key", "env", "url", "date", "amount", "name", "concept"};
static const char *const g_uioe_cxname[] = {"lookup", "multi-hop", "multi-source"};
static const char *const g_uioe_costname[] = {"instant", "index", "files", "process", "network", "model"};

/* KATANA_UIOE=off|pack|full; o padrao e off e o laco nao chama nada ainda (U2) */
static UioeMode uioe_mode(void) {
    const char *e = getenv("KATANA_UIOE");
    if (!e || !*e) return UIOE_OFF;
    if (!strcmp(e, "pack")) return UIOE_PACK;
    if (!strcmp(e, "full")) return UIOE_FULL;
    return UIOE_OFF;
}
static int uioe_env_int(const char *name, int dflt, int lo, int hi) {
    const char *e = getenv(name); if (!e || !*e) return dflt;
    int v = atoi(e); return v < lo ? lo : v > hi ? hi : v;
}
