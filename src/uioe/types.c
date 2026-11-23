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
    char xw[64][24]; int xw_of[64]; int nxw;   /* U3: palavras em ingles/codigo dos conceitos de cada termo (base de conhecimento), para casar com o codigo */
} UioeAsk;

typedef struct {
    UioeDomain domain[3]; int ndomain;
    UioeKind kind; uint8_t sure;
    uint8_t change, complexity, retrieve, risk;
    uint16_t rule_domain, rule_kind, rule_retrieve;
    char why_kind[160], why_retrieve[160], why_domain[160];
    uint8_t memcue;                     /* U8: o pedido pede historico, preferencia ou decisao anterior: a memoria entra como evidencia datada */
    char why_memory[160];
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
    uint8_t ran; int rstatus, rnodes, rcalls; double rms; char rnote[160], rconn[64];     /* U3: o que aconteceu quando o passo rodou (ran: 0 nao, 1 rodou, 2 nao rodou com motivo) */
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

/* ---- U3: grafo de evidencias, resultado de conector, recuperacao (docs/uioe.md 3.6, 3.7, 4.5) ---- */
#define UG_MAXNODE 400
#define UG_MAXEDGE 800
typedef struct UioeCtx UioeCtx;
typedef enum { UN_FILE, UN_RANGE, UN_SYMBOL, UN_KEY, UN_DOCLINE, UN_FACT, UN_FOLDER } UioeNodeKind;
typedef enum { UE_CALLS, UE_DEFINES, UE_USES, UE_IMPORTS, UE_SENDS, UE_HANDLES, UE_LINKS, UE_CONTAINS, UE_MENTIONS, UE_VIA } UioeEdgeKind;
enum { UV_NONE = 0, UV_OK, UV_MOVED, UV_STALE, UV_GONE, UV_UNREADABLE, UV_SKIPPED };     /* verificacao de um no (3.6.1) */
enum { UR_OK = 0, UR_PARTIAL, UR_EMPTY, UR_UNAVAILABLE, UR_TIMEOUT, UR_ERROR };           /* resultado de um conector (4.5) */
typedef struct {
    uint32_t id; uint8_t kind;
    char path[400]; int line0, line1;           /* onde (relativo a raiz); vazio para um fato sem lugar */
    char name[100], sig[160], cls[12];          /* simbolo/chave/titulo, assinatura ou "in simbolo (a-b)", classe da chave ou tipo do simbolo */
    char text[200];                              /* a linha citada (lida de volta na verificacao) */
    char vkey[100];                              /* o que deve ocorrer na linha para a verificacao */
    int conf, step, tier, relevance;             /* confianca 0-100, passo da cascata (1 exato, 2 derivado, 3 provavel), faixa 1-4, relevancia (so ordena) */
    uint8_t ver; int vline; char vnote[100];     /* UV_*; linha corrigida; nota ("file changed since indexed") */
    int8_t cver;                                 /* o proprio conector ja reconferiu a linha: 1 sim, -1 falhou, 0 nao */
    int nsrc, conn; double rrf;                  /* conectores independentes que trouxeram o no, o primeiro, soma RRF */
    char how[170];                               /* proveniencia */
    uint8_t shown, read, dropped, onpath, anchor, secret;
} UioeNode;
typedef struct { int32_t from, to; uint8_t kind; int line, conf; uint8_t ver; int conn; char how[170]; char key[70]; } UioeEdge;
typedef struct { UioeNode n[UG_MAXNODE]; int nn; UioeEdge e[UG_MAXEDGE]; int ne; int lost_n, lost_e; } UioeGraph;
/* o que se pede a um conector e o que ele devolve (4.5); nunca imprime */
typedef struct { uint32_t cap; char arg[420], arg2[220]; int limit, depth, line, tier, anchor, deadline_ms; } UioeCall;
typedef struct {
    int status;
    UioeNode *node; int nnode, capn; UioeEdge *edge; int nedge, cape;      /* arestas: from/to = indice local do no; -1 = o no `anchor` da chamada */
    char source[64]; char limits[400]; char notfound[300]; char sent[200]; char note[200];
    double ms; int total, step, from_cache;
} UioeResult;
typedef struct { int n; char state[4]; char label[16]; char conn[24]; char input[170]; int status; double ms; int nnode, nedge, retried; char note[200]; } UioeExec;
typedef struct {
    UioeGraph *g;
    UioeExec ex[80]; int nex;
    int path[8], pathedge[8], npath;             /* o caminho de resposta: nos em ordem; aresta que liga o passo i ao i+1 (-1 = sem) */
    int items[96], nitems, nitems_all;           /* nos para mostrar (ate 24, os que serao relidos), em ordem de fusao; nitems_all = quantos qualificavam */
    int branch[4], nbranch;                      /* caminhos nao seguidos (so fluxo) */
    char limits[20][230]; int nlim;              /* "Not found / limits": o que nao se achou ou nao se olhou, e por que */
    char hyp[8][200]; int nhyp;                  /* hipoteses abertas */
    char scope[260]; int idx_mode;               /* 0 sem indice, 1 parcial, 2 completo */
    char degrade[8][170]; int ndeg;              /* degraus da escada usados */
    char states[16][120]; int nstates;           /* o percurso da maquina de estados */
    int conf_after; char sig[10][130]; int nsig;
    int decision, rule; char decision_note[260];
    double ms, ms_index, ms_verify; int cut; char cut_note[160];
    int nverified, nstale, nmoved, nnotreread, nrange_total;
    char next[220];
    char score[24][230]; int nscore;               /* as ligacoes mais bem pontuadas para a pergunta, com a conta (traco: `score`) */
    int ran;                                     /* a recuperacao rodou */
} UioeRecall;

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
    UioeRecall rc;                      /* U3: o que a recuperacao achou (grafo, caminho, lacunas, hipoteses) */
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
    /* run: U3. Preenche o resultado, nunca imprime; NULL = o conector existe no registro mas nao foi construido nesta fase */
    int (*run)(UioeCtx *cx, const UioeCall *call, UioeResult *res);
} UioeConnector;
typedef struct {
    UioeRequest *cur;                   /* o pedido corrente (heap) */
    UioeConnector reg[UIOE_MAXCONN]; int nreg;
    UioeBreaker brk[UIOE_MAXCONN];
    char roots[4][1100]; int nroots;
    uint64_t seq;                       /* contador de pedidos */
    /* U4: a investigacao aberta (3.6.2): o ultimo pedido que recebeu bloco. Um pedido curto que so se refere a ele ("e onde isso chega?") herda o assunto.
       Zera com /new, /clear e ao trocar de assunto; some depois de UIOE_INHERIT_TURNS pedidos sem bloco novo. */
    struct { uint8_t open; char text[2000]; char root[1100]; uint64_t id; int turns; int nsh; struct { char path[400]; int a, b; } sh[48]; } inv;
    int todo_open;                      /* Q3: passos em aberto da lista de um pedido anterior (o laco poe antes de uioe_prepare; 0 = nenhum): vira uma clausula em "Not found / limits" */
    int npend; struct { char path[1100]; int a, b; } pend[64];     /* faixas do bloco, a registrar no estado do pedido DEPOIS do rr_reset do laco */
} UioeSession;
struct UioeCtx {
    UioeSession *s;
    char cwd[1100], root[1100];
    int deadline_ms, token_ceiling, json, trace;
    UioeMode mode;
    time_t now;
    /* U3 */
    double t0;                          /* inicio da recuperacao (depois do indice) */
    int det, verify, record, explain, build_index, run, refresh;      /* KATANA_UIOE_DET; --no-verify; marcar o que mostrou; --explain; --index; 0 = so planejar */
    SyView view; int view_state; char view_note[240];        /* UMA visao do indice por pedido: 0 fechada, 1 aberta, -1 falhou */
    int has_store, have_sym, have_keys, idx_mode;
    const Tokenizer *tk;                /* so o agente passa (docs.select precisa dele); a CLI nao tem modelo */
    char fault[160];                    /* KATANA_UIOE_FAULT=conector=timeout|error,...: gancho de teste da escada de degradacao */
    char vpath[420]; char *vbuf; size_t vn; size_t *vln; int vnl, vok; uint64_t vsize; int64_t vmtime;   /* arquivo lido pela verificacao */
};

static const char *const g_uioe_domname[] = {"code", "files", "data", "legal", "finance", "research", "memory", "system", "chat"};
static const char *const g_uioe_kindname[] = {"none", "web", "history", "error", "impact", "config", "flow", "usage", "howto", "overview", "locate", "findings"};
static const char *const g_uioe_entname[] = {"symbol", "path", "folder", "literal", "error-text", "key", "env", "url", "date", "amount", "name", "concept"};
static const char *const g_uioe_cxname[] = {"lookup", "multi-hop", "multi-source"};
static const char *const g_uioe_costname[] = {"instant", "index", "files", "process", "network", "model"};

/* KATANA_UIOE=off|pack|full. Decisao do dono (2026-11-23): o padrao e `pack` (o bloco antes do modelo, so para pedidos de codigo e arquivos, atras da porta
   conservadora de api.c); `off` desliga e e o escape. `full` (fachadas das ferramentas, U5) ainda nao existe: age como `pack`. Qualquer outro valor desliga. */
static UioeMode uioe_mode(void) {
    const char *e = getenv("KATANA_UIOE");
    if (!e || !*e) return UIOE_PACK;
    if (!strcmp(e, "pack")) return UIOE_PACK;
    if (!strcmp(e, "full")) return UIOE_FULL;
    return UIOE_OFF;
}
static int uioe_env_int(const char *name, int dflt, int lo, int hi) {
    const char *e = getenv(name); if (!e || !*e) return dflt;
    int v = atoi(e); return v < lo ? lo : v > hi ? hi : v;
}
