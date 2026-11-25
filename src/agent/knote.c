/* knote: o unico lugar por onde as notas `[katana: ...]` que o motor acrescenta ao que o modelo le sao emitidas, e o unico lugar que
 * escapa um `[katana` vindo de fora (shell, web, MCP). O texto de cada nota mora so na tabela KN_TAB (macros KNF_*): quem emite chama
 * KNOTE(tipo, saida, capacidade, argumentos...). O curriculo (src/app/scenario*.c, curriculum.c) e o gerador conferem esses textos
 * literalmente: mudar uma palavra aqui muda os exemplos de treino quando o v8 for gerado; a lista de quem depende do texto esta em
 * docs/katana-notes.md (secao 2.4).
 *
 * O tipo diz a classe da nota (ORDEM manda o modelo fazer algo; INFO conta um fato; ESTADO conta o que o usuario fez), onde ela entra
 * (RESULT = fim de um resultado de ferramenta, REQUEST = fim do pedido do usuario, TURN = turno proprio) e se entra no orcamento.
 * Quem escreve em uma saida que ja tem texto passa `out + n, cap - n`; knote nunca apaga o que veio antes. */

typedef enum {
    KN_PLAN_FIRST, KN_PLAN_STILL,          /* R7, R8: pedir o plano (3a e 6a chamada que nao seja so leitura) */
    KN_REPEAT1, KN_REPEATN,                /* R1, R2: chamada identica que ja deu certo (2a vez; 3a em diante) */
    KN_SAME_RESULT,                        /* R3: mesma chamada, mesmo resultado */
    KN_FAIL_AROUND, KN_FAIL_ROW,           /* R4, R5: falhas em volta da mesma coisa; falhas seguidas */
    KN_ROUNDS,                             /* R6: ultima rodada de ferramentas do pedido */
    KN_TODO_OPEN,                          /* T1: o modelo parou com passos em aberto (turno proprio) */
    KN_TODO_PREV,                          /* Q3: pedido novo com lista antiga em aberto (loop.c, entrada do pedido); so sai SEM bloco do UIOE: com bloco vira clausula de `Not found / limits` (ctxb.c) */
    KN_LINKS,                              /* R10: arquivos ligados ainda nao lidos (grafo do Find) */
    KN_READ_CURSOR,                        /* R12: Read continua de onde o anterior parou */
    KN_READ_SEEN, KN_READ_SEEN_ALL,        /* R12b: faixa pedida quase toda ja entregue (offset deslocado): segue do primeiro trecho novo / nada novo */
    KN_READ_MAPPED, KN_READ_SEQ,                           /* R12d: fatias em ordem do mesmo arquivo: nota curta manda ir ao ponto exato */
    KN_READ_MANY,                          /* R12c: muitas leituras do mesmo arquivo neste pedido */
    KN_READ_MAP, KN_READ_MAP_HIT,          /* R11: mapa de arquivo grande (sem e com trechos que casam com o pedido) */
    KN_STORED_ALL, KN_STORED_SAME,         /* R13, R15: leitura ja feita, nota guardada no lugar */
    KN_CONDENSED_LINES, KN_CONDENSED_OUT,  /* R14: nota condensada de um trecho de arquivo / da saida de uma ferramenta */
    KN_STORED_PRE,                         /* R16: notas dos trechos lidos antes, na frente dos proximos */
    KN_REMOVED,                            /* R17: resultado tirado do contexto */
    KN_COMPACT,                            /* T4: pedido de resumo (nao e nota; fora do orcamento) */
    KN_SUB_ROUNDS,                         /* T5: subagente sem rodadas */
    KN_CUT,                                /* R18: mensagem de canal cortada */
    KN_PLACE,                              /* Q1: pasta citada que nao existe da pasta de trabalho; so sai SEM bloco do UIOE (com bloco: `Question understood as` e `Next` dizem o caminho) */
    KN_DOCS,                               /* Q2: cabecalho dos trechos de documentos escolhidos pelo motor; com bloco do UIOE o cabecalho nao vai (as linhas entram em `Evidence`) */
    KN_FINISH_EXAMPLE, KN_FINISH_TEST, KN_FINISH_DONE,   /* T2, T3: antes de encerrar */
    KN_COUNT
} KnKind;

enum { KN_ORDER = 1, KN_INFO = 2, KN_STATE = 3 };          /* classe */
enum { KN_RESULT = 1, KN_REQUEST = 2, KN_TURN = 3 };       /* onde entra */
enum { KN_BUDGET = 1 };                                    /* flag: conta no orcamento (so INFO e recusada; ORDEM nunca e) */

/* Textos. Cada macro e o formato printf da nota, byte a byte como o modelo a le. */
#define KNF_PLAN_FIRST "\n[katana: plan first. This request already took %d tool calls without a plan. Now call %s with 3 to 8 concrete steps in the order you will do them, " \
    "the first one in progress, then continue with step 1.]"
#define KNF_PLAN_STILL "\n[katana: still no plan after %d tool calls. Stop and write the plan now with %s (3 to 8 steps, the current one in progress); then work through it in order.]"
/* R1 e R2: o resultado antigo pode ter sido tirado do contexto (R17); o texto nao promete que ele esta la */
#define KNF_REPEAT1 "[katana: you already made this exact call at step %d of this request; its result is above in this conversation, or listed there as removed to save context. " \
    "Do not repeat it: pick a different place to look (the symbol that this code calls, who defines it, or another file), or answer with what you have, saying what is still unknown.]"
#define KNF_REPEATN "[katana: this is the %d%s time you make this exact call (first at step %d of this request) and it will not run again. " \
    "Its result is above in this conversation, or listed there as removed to save context. " \
    "You can still: Read another part of the file with a different offset and limit, Grep (output_mode content, -n -C 2) for a name you saw there, Find for the file that " \
    "defines something it uses, or stop looking and answer with what you have, saying what is still unknown.]"
#define KNF_SAME_RESULT "\n[katana: this exact call already gave this exact result (%d times now). Repeating it changes nothing. Do something different: " \
    "read the message above, change the arguments, check the command's help, use another tool, or tell the user what is blocking you.]"
#define KNF_FAIL_AROUND "\n[katana: %d calls in this request failed around the same thing: %s (last error: \"%s\"). " \
    "If it is something you cannot produce yourself (a program or SDK the user must download, a login, a key, a file only they have), " \
    "%sstop probing it. Tell the user in one sentence exactly what they must provide, and go on now with the work that does not " \
    "depend on it: write the code and files anyway.]"
#define KNF_FAIL_ROW "\n[katana: %d calls in a row failed. Stop guessing. What do the errors have in common? Check the help or the documentation " \
    "(WebSearch), or tell the user in words what is missing and ask how to proceed.]"
#define KNF_ROUNDS "\n[katana: this request has used its tool rounds. Do not call a tool now. Tell the user in a few sentences what you did, " \
    "what is left and what you need from them; they can ask you to continue.]"
#define KNF_TODO_OPEN "[katana: your todo list still has unfinished steps]\n%sIf they are really done, mark them done with %s and give the final answer. " \
    "The user's newest message comes first: if it changed the plan, rewrite the list or drop the old steps and say so. Otherwise continue with the next step now."
#define KNF_TODO_PREV "%s\n[katana: your todo list from before still has %d unfinished steps. This message is newer and comes first: " \
    "if it changes the plan, follow it and rewrite or drop the old steps.]"
#define KN_LINKS_HEAD "\n[katana: linked files you have not read yet: "
#define KNF_LINKS KN_LINKS_HEAD "%s]"
#define KNF_READ_CURSOR "[katana: lines 1-%lld of %s were already given to you in %s; here is the rest, from line %lld]\n"
#define KNF_READ_SEEN "[katana: lines %lld-%lld of %s were already given to you in %s; here are the next lines, from line %lld]\n"
#define KNF_READ_SEEN_ALL "[katana: already seen lines %lld-%lld of %s: they were given to you in %s and nothing after them is new, so nothing is repeated. " \
    "Do not read this file again: use Grep (-n -C 2) or Symbols for the exact spot, or answer with what you have, saying what is still unknown.]"
#define KNF_READ_MAPPED "[katana: you already have the map and the start of %s (%lld lines) from %s. Below are the parts that match what you look for now; for anything else read an exact range with offset and limit, or use Symbols and Grep (-n -C 2).]"
#define KNF_READ_SEQ "[katana: you are reading %s in slices, in order. Go to the exact spot instead: Symbols (outline, or def/at) lists where things are, and Grep (-n -C 2) finds the line; then Read only that part.]"
#define KNF_READ_MANY "\n[katana: this is read number %d of %s in this request. Reading it in slices is not getting you closer. " \
    "Use Grep (-n -C 2) or Symbols for the exact spot, or answer now with what you have, saying what is still unknown.]"
#define KNF_READ_MAP_HIT "[katana: file has %lld lines; this is its map, its start and the parts that match what you are looking for. " \
    "Read another part with offset and limit (40 to 80 lines), or search inside it with Grep (-n -C 2).]"
#define KNF_READ_MAP "[katana: file has %lld lines; this is its map and its start. " \
    "Read a part with offset and limit (40 to 80 lines), or search inside it with Grep (-n -C 2).]"
#define KNF_STORED_ALL "[katana: you already read this whole file (%d lines) and it has not changed. These are the notes kept from that reading. " \
    "For exact lines call Read with offset and limit; to find something in it use Grep.]\n"
#define KNF_STORED_SAME "[katana: you already read this and it has not changed; this is the note kept from that reading (%zu bytes of raw text left out). " \
    "For exact lines call Read with offset and limit, or Find.]\n%s"
#define KNF_CONDENSED_LINES "[katana: condensed note of lines %d-%d of this file (%zu bytes read; the raw text was left out of the context and the note is kept). " \
    "For exact lines call Read with offset and limit.]\n%s%s%s"
#define KNF_CONDENSED_OUT "[katana: condensed note of this %s output (%zu bytes read; the raw text was left out of the context). " \
    "For exact lines call Read with offset and limit, or Find.]\n%s%s%s"
#define KNF_STORED_PRE "[katana: notes kept from the lines you read before in this file]\n%s[now the next lines]\n%s"
#define KNF_REMOVED "<tool_response>\n[katana: the result of %s%s%s was removed to save context (%zu bytes); %s%s%s]"
#define KNF_COMPACT "[katana: context compaction] Pause the task for this one message and do NOT call any tool. Write a faithful summary of " \
    "the conversation so far, in the language of the conversation, in at most 25 lines: 1) what the user asked, keeping exact requirements, " \
    "names and numbers; 2) what is already done: every file created or changed, with its path and what it contains now, and the commands run " \
    "with their important results; 3) the last error or failing test, quoted exactly; 4) what is left to do, in order. Do not invent anything. " \
    "After this message the older messages are removed and you continue the task from your summary."
#define KNF_SUB_ROUNDS "%s[katana: you are out of rounds. Do NOT call more tools: write your answer now with what you found.]"
#define KNF_CUT "\n[katana: message cut at %d bytes]"
#define KNF_PLACE "%s[katana: you mention \"%s\", which does not exist from the working folder (%s). It is at: %s. Work there: pass that folder as `path` to Find, Symbols and Grep, or use absolute paths.]"
#define KNF_DOCS "[katana: the engine picked this from the project documents that KATANA.md points to, because it matches this request. " \
    "It is a selection: open the file if you need more.]"
/* T2 sem o eco do pedido (ele ja esta na conversa; cortado em 2026-11-23, ver docs/katana-notes.md) */
#define KNF_FINISH_EXAMPLE "[katana: before you finish, compare the result with the request itself, not with your own tests. Run it on the request's own examples and check every detail it states: " \
    "exact text, line breaks, spaces, names, upper and lower case, exit codes. Fix what differs; if all matches, answer.]"
#define KNF_FINISH_TEST "[katana: this request changed files, and the project file says to verify with `%.200s`, which has not run in this request. Run it before you finish, or say why you cannot.]"
#define KNF_FINISH_DONE "[katana: this request changed files, and the project file defines done as: %.250s. Check that before you finish, or say why you cannot.]"

typedef struct {
    const char *name;           /* rotulo estavel (log, relatorio) */
    unsigned char cls, site, flags;
    unsigned short maxb;        /* teto de uma nota com orcamento, em bytes (0 = sem teto) */
    const char *fmt;
} KnSpec;
static const KnSpec KN_TAB[KN_COUNT] = {
    [KN_PLAN_FIRST]      = {"plan_first",      KN_ORDER, KN_RESULT, 0, 0, KNF_PLAN_FIRST},
    [KN_PLAN_STILL]      = {"plan_still",      KN_ORDER, KN_RESULT, 0, 0, KNF_PLAN_STILL},
    [KN_REPEAT1]         = {"repeat",          KN_ORDER, KN_RESULT, 0, 0, KNF_REPEAT1},
    [KN_REPEATN]         = {"repeat_cut",      KN_ORDER, KN_RESULT, 0, 0, KNF_REPEATN},
    [KN_SAME_RESULT]     = {"same_result",     KN_ORDER, KN_RESULT, 0, 0, KNF_SAME_RESULT},
    [KN_FAIL_AROUND]     = {"fail_around",     KN_ORDER, KN_RESULT, 0, 0, KNF_FAIL_AROUND},
    [KN_FAIL_ROW]        = {"fail_row",        KN_ORDER, KN_RESULT, 0, 0, KNF_FAIL_ROW},
    [KN_ROUNDS]          = {"rounds",          KN_ORDER, KN_RESULT, 0, 0, KNF_ROUNDS},
    [KN_TODO_OPEN]       = {"todo_open",       KN_ORDER, KN_TURN,   0, 0, KNF_TODO_OPEN},
    [KN_TODO_PREV]       = {"todo_prev",       KN_ORDER, KN_REQUEST,0, 0, KNF_TODO_PREV},
    [KN_LINKS]           = {"links",           KN_INFO,  KN_RESULT, KN_BUDGET, 1100, KNF_LINKS},
    [KN_READ_CURSOR]     = {"read_cursor",     KN_INFO,  KN_RESULT, 0, 0, KNF_READ_CURSOR},
    [KN_READ_SEEN]       = {"read_seen",       KN_INFO,  KN_RESULT, 0, 0, KNF_READ_SEEN},
    [KN_READ_SEEN_ALL]   = {"read_seen_all",   KN_INFO,  KN_RESULT, 0, 0, KNF_READ_SEEN_ALL},
    [KN_READ_MAPPED]     = {"read_mapped",     KN_INFO,  KN_RESULT, 0, 0, KNF_READ_MAPPED},
    [KN_READ_SEQ]        = {"read_seq",        KN_INFO,  KN_RESULT, 0, 0, KNF_READ_SEQ},
    [KN_READ_MANY]       = {"read_many",       KN_ORDER, KN_RESULT, 0, 0, KNF_READ_MANY},
    [KN_READ_MAP]        = {"read_map",        KN_INFO,  KN_RESULT, 0, 0, KNF_READ_MAP},
    [KN_READ_MAP_HIT]    = {"read_map_hit",    KN_INFO,  KN_RESULT, 0, 0, KNF_READ_MAP_HIT},
    [KN_STORED_ALL]      = {"stored_all",      KN_INFO,  KN_RESULT, 0, 0, KNF_STORED_ALL},
    [KN_STORED_SAME]     = {"stored_same",     KN_INFO,  KN_RESULT, 0, 0, KNF_STORED_SAME},
    [KN_CONDENSED_LINES] = {"condensed_lines", KN_INFO,  KN_RESULT, 0, 0, KNF_CONDENSED_LINES},
    [KN_CONDENSED_OUT]   = {"condensed_out",   KN_INFO,  KN_RESULT, 0, 0, KNF_CONDENSED_OUT},
    [KN_STORED_PRE]      = {"stored_pre",      KN_INFO,  KN_RESULT, 0, 0, KNF_STORED_PRE},
    [KN_REMOVED]         = {"removed",         KN_INFO,  KN_RESULT, 0, 0, KNF_REMOVED},
    [KN_COMPACT]         = {"compact",         KN_ORDER, KN_TURN,   0, 0, KNF_COMPACT},
    [KN_SUB_ROUNDS]      = {"sub_rounds",      KN_ORDER, KN_TURN,   0, 0, KNF_SUB_ROUNDS},
    [KN_CUT]             = {"cut",             KN_INFO,  KN_RESULT, 0, 0, KNF_CUT},
    [KN_PLACE]           = {"place",           KN_ORDER, KN_REQUEST,0, 0, KNF_PLACE},
    [KN_DOCS]            = {"docs",            KN_INFO,  KN_REQUEST,0, 0, KNF_DOCS},
    [KN_FINISH_EXAMPLE]  = {"finish_example",  KN_ORDER, KN_TURN,   0, 0, KNF_FINISH_EXAMPLE},
    [KN_FINISH_TEST]     = {"finish_test",     KN_ORDER, KN_TURN,   0, 0, KNF_FINISH_TEST},
    [KN_FINISH_DONE]     = {"finish_done",     KN_ORDER, KN_TURN,   0, 0, KNF_FINISH_DONE},
};

/* Orcamento (proposta do docs/katana-notes.md, secao 7.1; os valores abaixo sao mais folgados do que a proposta de 1200 bytes por
 * pedido porque esta mudanca nao pode alterar nenhum texto que o motor emite hoje; nada foi medido para escolhe-los):
 *  - por resultado de ferramenta: no maximo KN_RES_MAX bytes de nota (soma de todas as notas do resultado);
 *  - por pedido: no maximo KN_REQ_INFO_MAX bytes de nota INFO com orcamento (hoje so a nota de arquivos ligados, que ja tem teto de
 *    8 por pedido em FG_NOTES_MAX);
 * ORDEM nunca e recusada. INFO sem a flag KN_BUDGET nao e recusada (e o proprio resultado ou parte dele). Uma nota recusada devolve 0 e
 * deixa a saida como estava. */
#define KN_RES_MAX 1500
#define KN_REQ_INFO_MAX 6000
typedef struct { size_t res, req_info, req_order; int fired[KN_COUNT]; } KnBudget;
static KnBudget g_kn;
static void kn_request_begin(void) { memset(&g_kn, 0, sizeof g_kn); }      /* pedido novo (rr_reset) */
static void kn_result_begin(void) { g_kn.res = 0; }                        /* chamada nova (rr_check roda antes de toda chamada) */

/* Formata a nota em out (no maximo cap bytes, como snprintf) e devolve o tamanho que o snprintf devolveria; 0 = recusada pelo orcamento.
 * Quem usa o retorno como deslocamento deve se proteger de truncamento como com o snprintf. */
static size_t knote(KnKind k, char *out, size_t cap, ...) {
    const KnSpec *s = &KN_TAB[k];
    va_list ap; va_start(ap, cap);
    const int n = vsnprintf(out, cap, s->fmt, ap);
    va_end(ap);
    if (n < 0) { if (cap) out[0] = 0; return 0; }
    if (s->flags & KN_BUDGET) {
        if ((s->maxb && (size_t)n > s->maxb) || g_kn.res + (size_t)n > KN_RES_MAX || g_kn.req_info + (size_t)n > KN_REQ_INFO_MAX) { if (cap) out[0] = 0; return 0; }
        g_kn.req_info += (size_t)n;
    }
    if (s->cls == KN_ORDER) g_kn.req_order += (size_t)n;
    g_kn.res += (size_t)n; g_kn.fired[k]++;
    return (size_t)n;
}
/* o mesmo, com os argumentos conferidos pelo compilador contra o formato do tipo (sizeof nao executa nada) */
#define KNOTE(K, out, cap, ...) ((void)sizeof(printf(KNF_##K, ##__VA_ARGS__)), knote(KN_##K, out, cap, ##__VA_ARGS__))

/* Escape unico: uma linha que COMECA com `[katana` num texto que vem de fora (Bash, PowerShell, web, MCP, terminal, jobs) vem de um
 * programa ou de uma pagina, nao do katana. O modelo e instruido a seguir as notas `[katana: ...]` do programa, entao a marca e
 * quebrada ("[ katana") antes de o laco acrescentar as notas verdadeiras. Read e Grep ja prefixam o texto com numero de linha. */
static void kn_escape(char *out, size_t cap) {
    for (size_t i = 0; out[i]; i++) {
        if ((i == 0 || out[i - 1] == '\n') && strncmp(out + i, "[katana", 7) == 0) {
            const size_t len = strlen(out);
            if (len + 2 >= cap) return;
            memmove(out + i + 2, out + i + 1, len - i);      /* abre uma casa depois do '[' */
            out[i + 1] = ' ';
        }
    }
}
/* ferramentas cujo resultado traz conteudo de fora e passa por kn_escape */
static int kn_external(const char *name) {
    return strcmp(name, "Bash") == 0 || strcmp(name, "PowerShell") == 0 || strcmp(name, "WebFetch") == 0 || strcmp(name, "WebSearch") == 0 ||
           strcmp(name, "Monitor") == 0 || strcmp(name, "TaskOutput") == 0 || strcmp(name, "SendInput") == 0 ||
           strstr(name, "__") != NULL;
}
