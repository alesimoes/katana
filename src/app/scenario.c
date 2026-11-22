/* ---------- katana curriculum, motor de cenarios compostos ----------
 * Cada conversa nasce de uma celula: pilha (projeto pequeno de verdade, em Python, Node, shell, C ou Perl) x tipo de
 * tarefa x obstaculo x convencoes presentes (KATANA.md, skills, cartoes MCP). O projeto e gravado numa pasta, os testes
 * dele rodam de verdade, e uma politica escrita em C decide cada passo olhando o resultado real da ferramenta anterior.
 * Toda chamada passa pelo despacho do motor (ct_dispatch / agent_tool_run); nada aqui escreve o texto de um resultado.
 * So fica a conversa que termina verificada (testes passando, ou a recusa/pergunta certa); o resto e descartado (cu_die).
 * A linha gravada leva "scn" (semente, lingua, modo do prompt): `curriculum check` refaz a conversa inteira e compara. */

#define XF_NOTRAIN 1            /* a fala fica so como contexto */
#define XF_ERR 2                /* o resultado pode ser um erro do motor (o passo seguinte reage a ele) */
#define XF_OLD 4                /* ferramenta do primeiro conjunto (git, terminal, recall...): agent_tool_run */

typedef struct { const char *name, *args; int id; } XCall;
typedef struct { char role; const char *text; XCall c[2]; int nc, train; } XMsg;      /* role: s u e(nota do motor) a t */
#define X_MAXM 240
static struct {
    XMsg m[X_MAXM]; int n, nid;
    const char *fam; char cell[240];
    int pdone;                   /* progresso do plano a mandar junto da proxima acao */
    char seq[1600];              /* ferramentas das falas treinadas, na ordem */
    Buf ask;                     /* provas das perguntas (JSON) */
    int mode, decl;              /* modo do prompt desta conversa */
    int web;                     /* a conversa usa o servidor local de paginas */
} g_x;
static uint64_t g_x_rng0;        /* estado do sorteio no comeco desta tentativa (vai na linha gravada) */
static int g_x_replay;           /* 1: refazendo uma linha no check (sem o filtro de situacao repetida) */
static int g_x_rmode = -1, g_x_rdecl;      /* refazendo: modo e bloco de ferramentas da linha original */
static char g_x_res[70000];
static int g_x_port;             /* servidor local de paginas (0 = nao subiu) */
static int g_x_realgit;          /* a conversa precisa de um repositorio git de verdade */
static pid_t g_x_srv;

static void agent_tool_run(const char *name, const char *js, const char *end, char *out, size_t cap);
/* a chamada vale contra o esquema completo da ferramenta ou contra a declaracao curta que o prompt mostra (Edit.edits em
   texto, AskUserQuestion sem header: o motor aceita as duas formas) */
static int cu_ct_ok(const char *name, const char *args, char *err, size_t cap) {
    const char *full = cu_ct_schema(name);
    if (full && cu_valid(full, args, name, err, cap)) return 1;
    for (size_t i = 0; i < sizeof g_ct_short / sizeof g_ct_short[0]; i++) if (!strcmp(g_ct_short[i][0], name)) {
        char e2[400];
        if (cu_valid(cu_fmt("{\"type\": \"object\", \"properties\": {%s}, \"required\": [%s]}", g_ct_short[i][2], g_ct_short[i][3]), args, name, e2, sizeof e2)) return 1;
    }
    return 0;
}
static void x_cell(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_x.cell, sizeof g_x.cell, fmt, ap); va_end(ap); }
static XMsg *x_push(char role, const char *text, int train) {
    if (g_x.n + 4 >= X_MAXM) cu_die("conversa longa demais");
    XMsg *m = &g_x.m[g_x.n++]; memset(m, 0, sizeof *m);
    m->role = role; m->text = text ? cu_fmt("%s", text) : ""; m->train = train;
    return m;
}
/* resposta que o usuario da a proxima pergunta do motor (AskUserQuestion, item ask:, aprovacao de plano): entra pela
   entrada padrao no formato da interface */
static void x_feed(const char *line) {
    int fd[2];
    if (pipe(fd) != 0) cu_die("sem pipe");
    const char *t = cu_fmt("\x01" "ANS %s\n", line);
    if (write(fd[1], t, strlen(t)) < 0) {}
    close(fd[1]); dup2(fd[0], 0); close(fd[0]); clearerr(stdin);
    setenv("KATANA_UI", "1", 1);
}
static void x_unfeed(void) { unsetenv("KATANA_UI"); if (!freopen("/dev/null", "r", stdin)) {} }
/* roda a ferramenta do motor, com a saida de console do motor escondida, e anexa as notas do laco */
static const char *x_run(const char *name, const char *args, int flags) {
    g_x_res[0] = 0;
    fflush(stdout); fflush(stderr);
    const int so = dup(1), se = dup(2), dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); if (!getenv("KATANA_CU_DEBUG")) dup2(dn, 2);
    const int known = (flags & XF_OLD) || ct_lookup(name) != NULL;
    if (known) agent_tool_run(name, args, args + strlen(args), g_x_res, 60000);      /* o despacho do laco: inclui a trava do modo plano */
    fflush(stdout); fflush(stderr);
    dup2(so, 1); dup2(se, 2); close(so); close(se); close(dn);
    if (!known) cu_die("ferramenta fora da tabela do motor: %s", name);
    g_cu_pl++;
    loop_plan_note(g_x_res, 60000, name, g_cu_pl, g_cu_plold);
    if (strstr(g_x_res, "[katana: plan first") || strstr(g_x_res, "[katana: still no plan")) cu_die("o motor cobraria o plano aqui (%d chamadas sem TodoWrite, %s)", g_cu_pl, g_x.cell);
    return g_x_res;
}
static void x_seq(const char *name) {
    const size_t l = strlen(g_x.seq);
    snprintf(g_x.seq + l, sizeof g_x.seq - l, "%s%s", l ? ">" : "", strstr(name, "__") ? "mcp" : name);
}
/* chamada do modelo: valida contra o esquema do motor, roda, grava a fala e o resultado; devolve o resultado */
static const char *x_call(const char *say, const char *name, const char *args, int flags) {
    const int train = !(flags & XF_NOTRAIN);
    if (say && (strlen(say) > 140 || strchr(say, '\n'))) cu_die("texto antes da chamada longo demais: %s", say);
    if (!(flags & XF_OLD)) {
        char err[400]; const char *schema = cu_ct_schema(name);
        if (!schema) cu_die("ferramenta desconhecida: %s", name);
        if (!cu_ct_ok(name, args, err, sizeof err)) cu_die("%s (%s): %s\nargumentos: %s", name, g_x.cell, err, args);
    }
    XMsg *a = x_push('a', say, train);
    if (g_x.pdone && strcmp(name, "TodoWrite")) { a->c[a->nc].name = "TodoWrite"; a->c[a->nc].args = cu_fmt("{\"done\": %d}", g_x.pdone); a->c[a->nc++].id = ++g_x.nid; }
    g_x.pdone = 0;
    a->c[a->nc].name = cu_fmt("%s", name); a->c[a->nc].args = cu_fmt("%s", args); a->c[a->nc++].id = ++g_x.nid;
    const XCall c0 = a->c[0], c1 = a->c[a->nc - 1]; const int nc = a->nc;
    if (nc == 2) {
        const char *r = x_run("TodoWrite", c0.args, 0);
        if (!strncmp(r, "error:", 6)) cu_die("progresso do plano recusado pelo motor: %s", r);
        XMsg *t = x_push('t', r, 1); t->c[0] = c0; t->nc = 1;
    }
    const char *r = x_run(name, args, flags);
    if (train && !(flags & XF_ERR) && !strncmp(r, "error:", 6)) cu_die("%s falhou no motor (%s): %.300s", name, g_x.cell, r);
    if (strstr(r, "[syntax check FAILED") && !(flags & XF_ERR)) cu_die("%s: a checagem de sintaxe do motor falhou (%s)", name, g_x.cell);
    XMsg *t = x_push('t', r, 1); t->c[0] = c1; t->nc = 1;
    if (train) x_seq(name);
    return t->text;
}
static int x_err(const char *r) { return !strncmp(r, "error:", 6); }
static const char *x_abs(const Scn *s, const char *rel) { return rel[0] == '/' ? rel : cu_fmt("%s/%s", s->cwd, rel); }
static const char *x_bash(const char *say, const char *cmd, const char *desc, int flags) {
    return x_call(say, "Bash", desc ? cu_fmt("{\"command\": %s, \"description\": %s}", cu_q(cmd), cu_q(desc)) : cu_fmt("{\"command\": %s}", cu_q(cmd)), flags);
}
static const char *x_read(const Scn *s, const char *rel, int flags) { return x_call(NULL, "Read", cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, rel))), flags); }
static const char *x_write(const Scn *s, const char *say, const char *rel, const char *content) {
    return x_call(say, "Write", cu_fmt("{\"file_path\": %s, \"content\": %s}", cu_q(x_abs(s, rel)), cu_q(content)), 0);
}
static const char *x_edit(const Scn *s, const char *rel, const char *old, const char *nw, int all, int flags) {
    return x_call(NULL, "Edit", cu_fmt("{\"file_path\": %s, \"old_string\": %s, \"new_string\": %s%s}", cu_q(x_abs(s, rel)), cu_q(old), cu_q(nw), all ? ", \"replace_all\": true" : ""), flags);
}
/* varias trocas no mesmo arquivo numa chamada so: blocos <<<<<<< / ======= / >>>>>>> */
static const char *x_edits(const Scn *s, const char *rel, const char *const *old, const char *const *nw, int n) {
    Buf b = {0}; buf_str(&b, "");
    for (int i = 0; i < n; i++) {
        buf_str(&b, "<<<<<<<\n"); buf_str(&b, old[i]); if (old[i][strlen(old[i]) - 1] != '\n') buf_str(&b, "\n");
        buf_str(&b, "=======\n"); buf_str(&b, nw[i]); if (*nw[i] && nw[i][strlen(nw[i]) - 1] != '\n') buf_str(&b, "\n");
        buf_str(&b, ">>>>>>>\n");
    }
    return x_call(NULL, "Edit", cu_fmt("{\"file_path\": %s, \"edits\": %s}", cu_q(x_abs(s, rel)), cu_q(cu_keep(b.p))), 0);
}
static const char *x_grep(const char *pat, const char *mode, const char *glob) {
    return x_call(NULL, "Grep", cu_fmt("{\"pattern\": %s%s%s}", cu_q(pat), mode ? cu_fmt(", \"output_mode\": %s", cu_q(mode)) : "", glob ? cu_fmt(", \"glob\": %s", cu_q(glob)) : ""), 0);
}
static const char *x_glob(const char *pat) { return x_call(NULL, "Glob", cu_fmt("{\"pattern\": %s}", cu_q(pat)), 0); }
/* plano: escrito uma vez, em linhas; depois so done=N junto da proxima acao */
static void x_plan(const char *const *items, int n) {
    Buf o = {0}; buf_str(&o, "");
    for (int i = 0; i < n; i++) buf_str(&o, cu_fmt("%s%s %s", i ? "\n" : "", i ? "[ ]" : "[>]", items[i]));
    x_call(NULL, "TodoWrite", cu_fmt("{\"todos\": %s}", cu_q(cu_keep(o.p))), 0);
}
static void x_done(int k) { g_x.pdone = k; }
static void x_user(const char *t) {
    if (g_x.n > 1) {
        int td = 0; const int tn = todo_count(&td);
        if (tn > td) cu_die("pedido novo com plano em aberto (%s)", g_x.cell);
        g_undo_req++; g_todo_touched = 0; g_cu_pl = 0; g_cu_plold = 0;
    }
    g_x.pdone = 0;
    x_push('u', t, 1);
    { const size_t l = strlen(g_x.seq); if (l) snprintf(g_x.seq + l, sizeof g_x.seq - l, "|"); }
}
/* resposta final do pedido: o que o laco do motor faria aqui (nota de verificacao, fecho do ultimo passo) */
static void x_final(const char *t) {
    if (strlen(t) > 700) cu_die("resposta final longa demais (%s)", g_x.cell);
    g_x.pdone = 0;
    x_push('a', t, 1);
    char note[1400];
    if (brief_end_note(note, sizeof note)) cu_die("o motor mandaria a nota de verificacao antes desta resposta final (%s)", g_x.cell);
    int td = 0; const int tn = todo_count(&td);
    if (tn - td == 1) { fflush(stdout); const int so = dup(1), dn = open("/dev/null", O_WRONLY); dup2(dn, 1); ct_todo_autofinish(); fflush(stdout); dup2(so, 1); close(so); close(dn); }
    else if (tn > td && g_todo_touched) cu_die("resposta final com %d passos do plano em aberto (%s)", tn - td, g_x.cell);
    { const size_t l = strlen(g_x.seq); snprintf(g_x.seq + l, sizeof g_x.seq - l, "%s.", l && g_x.seq[l - 1] != '|' ? ">" : ""); }
}
/* prova de uma pergunta (ou da falta dela): o check confere contra o texto anterior. kind: ambiguous, missing, delete, none */
static void x_askproof(const char *kind, int asked, const char *need, const char *absent) {
    buf_str(&g_x.ask, cu_fmt("%s{\"kind\": %s, \"asked\": %s, \"need\": %s, \"absent\": %s}", g_x.ask.n ? ", " : "", cu_q(kind), asked ? "true" : "false", cu_q(need ? need : ""), cu_q(absent ? absent : "")));
}

/* ---- o que muda de uma execucao para outra sai do texto gravado: pasta de trabalho, pid, pasta de resultados guardados, porta ---- */
static char *x_norm(const char *s) {
    Buf b = {0}; buf_str(&b, "");
    const size_t pl = strlen(g_cu_sb);
    char port[40]; snprintf(port, sizeof port, "http://127.0.0.1:%d", g_x_port); const size_t ol = strlen(port);
    for (const char *p = s; *p; ) {
        if (pl && !strncmp(p, g_cu_sb, pl)) { p += pl; continue; }
        if (!strncmp(p, "/.katana/jobs/bash-", 19)) { buf_str(&b, "/.katana/jobs/bash-1000"); p += 19; while (isdigit((unsigned char)*p)) p++; continue; }
        if (!strncmp(p, "/.katana/context/", 17)) { buf_str(&b, "/.katana/context/1000"); p += 17; while (isdigit((unsigned char)*p) || *p == '-') p++; continue; }
        if (g_x_port && !strncmp(p, port, ol)) { buf_str(&b, "https://docs.example"); p += ol; continue; }
        buf_add(&b, p, 1); p++;
    }
    return b.p;
}
static void x_end(FILE *f) {
    Buf o = {0}; buf_str(&o, "{\"messages\": [");
    for (int i = 0; i < g_x.n; i++) {
        const XMsg *m = &g_x.m[i];
        if (i) buf_str(&o, ", ");
        if (m->role == 's') buf_str(&o, cu_fmt("{\"role\": \"system\", \"content\": %s}", cu_q(m->text)));
        else if (m->role == 'u' || m->role == 'e') buf_str(&o, cu_fmt("{\"role\": \"user\", \"content\": %s}", cu_q(m->text)));
        else if (m->role == 't') buf_str(&o, cu_fmt("{\"role\": \"tool\", \"tool_call_id\": \"call_%d\", \"name\": %s, \"content\": %s}", m->c[0].id, cu_q(m->c[0].name), cu_q(m->text)));
        else {
            buf_str(&o, cu_fmt("{\"role\": \"assistant\", \"content\": %s", cu_q(m->text)));
            if (m->nc) {
                buf_str(&o, ", \"tool_calls\": [");
                for (int k = 0; k < m->nc; k++) buf_str(&o, cu_fmt("%s{\"id\": \"call_%d\", \"type\": \"function\", \"function\": {\"name\": %s, \"arguments\": %s}}", k ? ", " : "", m->c[k].id, cu_q(m->c[k].name), m->c[k].args));
                buf_str(&o, "]");
            }
            buf_str(&o, m->train ? "}" : ", \"train\": false}");
        }
    }
    if (!g_x_replay) {      /* mesma situacao (prompt e primeiro pedido) de outra conversa: descarta */
        uint64_t h = 1469598103934665603ull;
        const char *u = strstr(o.p, "{\"role\": \"user\""), *ue = u ? strstr(u, "}, {\"role\": \"assistant\"") : NULL;
        for (const char *p = o.p; u && ue && p < ue; p++) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
        for (long i = 0; i < g_cu_nsits; i++) if (g_cu_sits[i] == h) { free(o.p); cu_die("situacao repetida (mesmo prompt e mesmo pedido de outra conversa)"); }
        if (g_cu_nsits == g_cu_csits) { g_cu_csits = g_cu_csits ? g_cu_csits * 2 : 4096; g_cu_sits = realloc(g_cu_sits, sizeof *g_cu_sits * (size_t)g_cu_csits); }
        g_cu_sits[g_cu_nsits++] = h;
    }
    buf_str(&o, cu_fmt("], \"source\": \"curriculum\", \"family\": %s, \"cell\": %s, \"seq\": %s, \"scn\": {\"rng\": \"%llu\", \"lang\": %d, \"hold\": %d, \"mode\": %d, \"decl\": %d}",
                       cu_q(g_x.fam), cu_q(g_x.cell), cu_q(g_x.seq), (unsigned long long)g_x_rng0, g_cu_lang, g_cu_hold, g_x.mode, g_x.decl));
    if (g_x.ask.n) { buf_str(&o, ", \"ask\": ["); buf_str(&o, g_x.ask.p); buf_str(&o, "]"); }
    buf_str(&o, "}\n");
    char *nm = x_norm(o.p);
    fwrite(nm, 1, strlen(nm), f);
    free(nm); free(o.p);
}

/* ======================= o projeto ======================= */
enum { X_PY, X_JS, X_SH, X_C, X_PL, X_NST };
static const char *const g_x_stid[X_NST] = {"py", "js", "sh", "c", "pl"};
static const char *const g_x_ext[X_NST] = {"py", "js", "sh", "c", "pl"};
static const char *const g_x_lang[X_NST] = {"Python", "JavaScript", "shell", "C", "Perl"};
/* funcao do acervo: nome, argumentos, expressao inteira (+ - * % e parenteses) dividida por den; bad: a expressao com o defeito */
typedef struct { const char *name, *args, *num; int den; const char *bad, *en, *pt; } XFn;
static const XFn g_x_fns[] = {
    {"percent_of", "part,total", "part * 100", 1, "part * 10", "part as a percentage of 100 units", "part como porcentagem de 100 unidades"},
    {"add_tax", "cents,rate", "cents * (100 + rate)", 100, "cents * rate", "the amount plus rate percent of tax", "o valor mais rate por cento de imposto"},
    {"discount", "cents,pct", "cents * (100 - pct)", 100, "cents * (100 + pct)", "the amount after a pct percent discount", "o valor depois de pct por cento de desconto"},
    {"area", "width,height", "width * height", 1, "width + height", "the area of a width by height rectangle", "a area de um retangulo width por height"},
    {"perimeter", "width,height", "2 * (width + height)", 1, "2 * width + height", "the perimeter of a width by height rectangle", "o perimetro de um retangulo width por height"},
    {"to_seconds", "minutes,seconds", "minutes * 60 + seconds", 1, "minutes * 100 + seconds", "minutes and seconds as seconds", "minutos e segundos em segundos"},
    {"to_minutes", "hours,minutes", "hours * 60 + minutes", 1, "hours * 24 + minutes", "hours and minutes as minutes", "horas e minutos em minutos"},
    {"average", "first,second", "first + second", 2, "first + second + second", "the whole-number average of two values", "a media inteira de dois valores"},
    {"leftover", "total,size", "total % size", 1, "size % total", "what is left after filling groups of size", "o que sobra depois de encher grupos de size"},
    {"dozens", "items", "items", 12, "items * 12", "how many full dozens fit in items", "quantas duzias inteiras cabem em items"},
    {"to_cents", "units,cents", "units * 100 + cents", 1, "units * 10 + cents", "units and cents as cents", "unidades e centavos em centavos"},
    {"total_price", "qty,unit", "qty * unit", 1, "qty + unit", "the price of qty items at unit each", "o preco de qty itens a unit cada"},
    {"with_fee", "amount,fee", "amount + fee", 1, "amount + fee + fee", "the amount with the fee added", "o valor com a taxa somada"},
    {"net", "gross,tax", "gross - tax", 1, "gross + tax", "the gross value minus the tax", "o valor bruto menos o imposto"},
    {"triple", "value", "3 * value", 1, "2 * value", "three times the value", "tres vezes o valor"},
    {"square", "side", "side * side", 1, "side + side", "the side squared", "o lado ao quadrado"},
    {"cube", "side", "side * side * side", 1, "side * side", "the side cubed", "o lado ao cubo"},
    {"to_days", "weeks,days", "weeks * 7 + days", 1, "weeks * 5 + days", "weeks and days as days", "semanas e dias em dias"},
    {"to_meters", "km,meters", "km * 1000 + meters", 1, "km * 100 + meters", "kilometers and meters as meters", "quilometros e metros em metros"},
    {"half", "value", "value", 2, "value * 2", "half of the value, rounded down", "metade do valor, arredondada para baixo"},
    {"points", "wins,draws", "wins * 3 + draws", 1, "wins * 2 + draws", "three points per win and one per draw", "tres pontos por vitoria e um por empate"},
    {"last_digit", "number", "number % 10", 1, "number % 100", "the last decimal digit of the number", "o ultimo digito decimal do numero"},
    {"offset_of", "index,step,base", "index * step + base", 1, "index + step + base", "the position of item index with the given step and base", "a posicao do item index com o passo e a base dados"},
    {"volume", "width,height,depth", "width * height * depth", 1, "width * height + depth", "the volume of a box", "o volume de uma caixa"},
    {"interest", "principal,rate,years", "principal * rate * years", 100, "principal * rate + years", "simple interest at rate percent over the years", "juros simples a rate por cento pelos anos"},
    {"fahrenheit", "celsius", "celsius * 9 + 160", 5, "celsius * 9 + 32", "the temperature in Fahrenheit, rounded down", "a temperatura em Fahrenheit, arredondada para baixo"},
    {"midpoint", "low,high", "low + high", 2, "low + high + high", "the whole-number midpoint of low and high", "o ponto medio inteiro entre low e high"},
    {"span", "low,high", "high - low + 1", 1, "high - low", "how many integers are in low..high", "quantos inteiros ha em low..high"},
    {"next_slot", "index,count", "(index + 1) % count", 1, "index % count", "the slot after index, wrapping at count", "a posicao depois de index, voltando ao zero em count"},
    {"checksum", "first,second", "(first * 31 + second) % 97", 1, "(first * 13 + second) % 97", "a small checksum of two values", "uma soma de verificacao pequena de dois valores"},
    {"ticks", "seconds,rate", "seconds * rate", 1, "seconds + rate", "the number of ticks in seconds at rate per second", "o numero de ticks em seconds a rate por segundo"},
    {"stock_left", "stock,sold,returned", "stock - sold + returned", 1, "stock - sold - returned", "the stock after sales and returns", "o estoque depois das vendas e devolucoes"},
    {"rows_needed", "items", "items + 7", 8, "items + 8", "the rows of eight needed for items", "as fileiras de oito necessarias para items"},
    {"shift_end", "start,hours", "(start + hours) % 24", 1, "(start + hours) % 12", "the hour a shift ends on a 24-hour clock", "a hora em que um turno termina no relogio de 24 horas"},
    {"score", "hits,misses", "hits * 5 - misses", 1, "hits * 5 + misses", "five points per hit minus one per miss", "cinco pontos por acerto menos um por erro"},
    {"padding", "length", "(8 - length % 8) % 8", 1, "8 - length % 8", "the bytes needed to pad length to a multiple of eight", "os bytes para completar length ate um multiplo de oito"},
};
#define X_NFN CU_N(g_x_fns)
static const char *const g_x_modnames[] = {"core", "calc", "rules", "tally", "ledger", "metrics", "convert", "pricing", "layout", "timing", "stock", "limits", "shapes", "report", "rates", "counts"};
static const char *const g_x_whats[] = {"billing", "inventory", "scheduling", "geometry", "telemetry", "invoicing", "routing", "payroll", "booking", "grading", "shipping", "metering"};

typedef struct { const XFn *f; const char *name, *num; long a[2][3], want[2]; } XFi;
typedef struct { const char *name; XFi fn[6]; int nfn; } XMod;
typedef struct {
    const Scn *s; int st; const char *name, *what;
    XMod mod[3]; int nmod;
    int brief, readme, docs, rules, never, askrule, conf, failfast;      /* o que o projeto tem */
    const char *testcmd, *testrel;
    int knows;                   /* o comando de teste ja apareceu na conversa (prompt, README ou busca) */
    int nread; const char *readf[24];      /* arquivos ja lidos inteiros nesta conversa */
    const char *xrel[8], *xtxt[8]; int xmode[8], nx;      /* arquivos a mais da tarefa */
    const char *ck[8], *cv[8], *cr[8]; int nck;           /* settings.conf: chave, valor, faixa */
} XProj;

static int x_nargs(const XFn *f) { return cu_count(f->args, ','); }
typedef struct { const char *p; const XFn *f; const long *v; int err; } XEv;
static long x_ev_expr(XEv *e);
static void x_ev_ws(XEv *e) { while (*e->p == ' ') e->p++; }
static long x_ev_atom(XEv *e) {
    x_ev_ws(e);
    if (*e->p == '(') { e->p++; const long r = x_ev_expr(e); x_ev_ws(e); if (*e->p == ')') e->p++; else e->err = 1; return r; }
    if (isdigit((unsigned char)*e->p)) { char *q; const long r = strtol(e->p, &q, 10); e->p = q; return r; }
    char id[32]; int n = 0;
    while (isalnum((unsigned char)*e->p) || *e->p == '_') { if (n < 31) id[n++] = *e->p; e->p++; }
    id[n] = 0;
    for (int i = 0, k = x_nargs(e->f); n && i < k; i++) if (!strcmp(id, cu_part(e->f->args, ',', i))) return e->v[i];
    e->err = 1; return 0;
}
static long x_ev_term(XEv *e) {
    long r = x_ev_atom(e);
    for (;;) {
        x_ev_ws(e);
        if (*e->p == '*') { e->p++; r *= x_ev_atom(e); }
        else if (*e->p == '%') { e->p++; const long d = x_ev_atom(e); if (d <= 0 || r < 0) { e->err = 1; return 0; } r %= d; }
        else return r;
    }
}
static long x_ev_expr(XEv *e) {
    long r = x_ev_term(e);
    for (;;) { x_ev_ws(e); if (*e->p == '+') { e->p++; r += x_ev_term(e); } else if (*e->p == '-') { e->p++; r -= x_ev_term(e); if (r < 0) e->err = 1; } else return r; }
}
/* valor da funcao com a expressao num; 0 se a conta sai do que as cinco linguagens fazem igual (negativo, modulo por zero) */
static int x_eval(const XFn *f, const char *num, const long *v, long *out) {
    XEv e = {num, f, v, 0};
    const long r = x_ev_expr(&e); x_ev_ws(&e);
    if (e.err || *e.p || r < 0) return 0;
    *out = r / f->den;
    return 1;
}
/* a expressao na linguagem da pilha */
static const char *x_expr(int st, const XFn *f, const char *num) {
    Buf b = {0}; buf_str(&b, "");
    for (const char *p = num; *p; ) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            char id[32]; int n = 0;
            while (isalnum((unsigned char)*p) || *p == '_') { if (n < 31) id[n++] = *p; p++; }
            id[n] = 0;
            int ix = 0; for (int i = 0, k = x_nargs(f); i < k; i++) if (!strcmp(id, cu_part(f->args, ',', i))) ix = i;
            buf_str(&b, st == X_PL ? cu_fmt("$%s", id) : st == X_SH ? cu_fmt("$%d", ix + 1) : id);
        } else { buf_add(&b, p, 1); p++; }
    }
    const char *e = cu_keep(b.p);
    if (f->den == 1) return e;
    return st == X_PY ? cu_fmt("(%s) // %d", e, f->den) : st == X_JS ? cu_fmt("Math.floor((%s) / %d)", e, f->den) : st == X_PL ? cu_fmt("int((%s) / %d)", e, f->den) : cu_fmt("(%s) / %d", e, f->den);
}
static const char *x_arglist(const XFn *f, const char *pre, const char *sep) {
    Buf b = {0}; buf_str(&b, "");
    for (int i = 0, k = x_nargs(f); i < k; i++) buf_str(&b, cu_fmt("%s%s%s", i ? sep : "", pre, cu_part(f->args, ',', i)));
    return cu_keep(b.p);
}
/* o texto da funcao no arquivo */
static const char *x_block(int st, const char *name, const XFn *f, const char *num) {
    const char *e = x_expr(st, f, num);
    switch (st) {
    case X_PY: return cu_fmt("def %s(%s):\n    return %s\n", name, x_arglist(f, "", ", "), e);
    case X_JS: return cu_fmt("function %s(%s) {\n  return %s;\n}\n", name, x_arglist(f, "", ", "), e);
    case X_SH: return cu_fmt("%s() {\n    echo $(( %s ))\n}\n", name, e);
    case X_C:  return cu_fmt("int %s(%s) {\n    return %s;\n}\n", name, x_arglist(f, "int ", ", "), e);
    default:   return cu_fmt("sub %s {\n    my (%s) = @_;\n    return %s;\n}\n", name, x_arglist(f, "$", ", "), e);
    }
}
static const char *x_proto(const char *name, const XFn *f) { return cu_fmt("int %s(%s);\n", name, x_arglist(f, "int ", ", ")); }
static const char *x_srcdir(const XProj *p) { return p->st == X_PY ? cu_fmt("%s/", p->name) : p->st == X_JS || p->st == X_C ? "src/" : "lib/"; }
static const char *x_src(const XProj *p, int mi) { return cu_fmt("%s%s.%s", x_srcdir(p), p->mod[mi].name, g_x_ext[p->st]); }
static const char *x_hdr(const XProj *p, int mi) { return cu_fmt("src/%s.h", p->mod[mi].name); }
static const char *x_vals(const long *a, int n, const char *sep) {
    Buf b = {0}; buf_str(&b, "");
    for (int i = 0; i < n; i++) buf_str(&b, cu_fmt("%s%ld", i ? sep : "", a[i]));
    return cu_keep(b.p);
}
/* a chamada como os testes mostram: nome(1, 2) */
static const char *x_shown(const char *name, const XFn *f, const long *a) { return cu_fmt("%s(%s)", name, x_vals(a, x_nargs(f), ", ")); }
/* a linha de um caso no arquivo de testes */
static const char *x_case(const XProj *p, const char *mod, const char *name, const XFn *f, const long *a, long want) {
    const int n = x_nargs(f);
    switch (p->st) {
    case X_PY: return cu_fmt("    (%s.%s, (%s%s), %ld),\n", mod, name, x_vals(a, n, ", "), n == 1 ? "," : "", want);
    case X_JS: return cu_fmt("  [%s.%s, [%s], %ld],\n", mod, name, x_vals(a, n, ", "), want);
    case X_SH: return cu_fmt("check %ld %s %s\n", want, name, x_vals(a, n, " "));
    case X_C:  return cu_fmt("    CHECK(%s(%s), %ld);\n", name, x_vals(a, n, ", "), want);
    default:   return cu_fmt("check(\"%s\", [%s], %ld);\n", name, x_vals(a, n, ", "), want);
    }
}
static const char *x_mod_text(const XProj *p, int mi) {
    const XMod *m = &p->mod[mi];
    Buf b = {0};
    buf_str(&b, p->st == X_PY ? cu_fmt("\"\"\"%s helpers of %s.\"\"\"\n\n\n", m->name, p->name) : p->st == X_JS ? "'use strict';\n\n" : p->st == X_SH ? cu_fmt("# %s helpers of %s\n\n", m->name, p->name)
                : p->st == X_C ? cu_fmt("#include \"%s.h\"\n\n", m->name) : "use strict;\nuse warnings;\n\n");
    for (int i = 0; i < m->nfn; i++) { if (i) buf_str(&b, p->st == X_PY ? "\n\n" : "\n"); buf_str(&b, x_block(p->st, m->fn[i].name, m->fn[i].f, m->fn[i].num)); }
    if (p->st == X_JS) { buf_str(&b, "\nmodule.exports = { "); for (int i = 0; i < m->nfn; i++) buf_str(&b, cu_fmt("%s%s", i ? ", " : "", m->fn[i].name)); buf_str(&b, " };\n"); }
    if (p->st == X_PL) buf_str(&b, "\n1;\n");
    return cu_keep(b.p);
}
static const char *x_hdr_text(const XProj *p, int mi) {
    const XMod *m = &p->mod[mi];
    char up[40]; snprintf(up, sizeof up, "%s", m->name); for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    Buf b = {0}; buf_str(&b, cu_fmt("#ifndef %s_H\n#define %s_H\n\n", up, up));
    for (int i = 0; i < m->nfn; i++) buf_str(&b, x_proto(m->fn[i].name, m->fn[i].f));
    buf_str(&b, "\n#endif\n");
    return cu_keep(b.p);
}
static const char *x_test_text(const XProj *p) {
    Buf b = {0}, c = {0}; buf_str(&c, "");
    for (int mi = 0; mi < p->nmod; mi++) for (int i = 0; i < p->mod[mi].nfn; i++) for (int k = 0; k < 2; k++) {
        const XFi *fi = &p->mod[mi].fn[i];
        buf_str(&c, x_case(p, p->mod[mi].name, fi->name, fi->f, fi->a[k], fi->want[k]));
    }
    Buf im = {0}; buf_str(&im, "");
    for (int mi = 0; mi < p->nmod; mi++) {
        const char *n = p->mod[mi].name;
        buf_str(&im, p->st == X_PY ? cu_fmt("%s%s", mi ? ", " : "", n) : p->st == X_JS ? cu_fmt("const %s = require('../src/%s.js');\n", n, n) : p->st == X_SH ? cu_fmt(". lib/%s.sh\n", n)
                     : p->st == X_C ? cu_fmt("#include \"../src/%s.h\"\n", n) : cu_fmt("require \"./lib/%s.pl\";\n", n));
    }
    switch (p->st) {
    case X_PY: buf_str(&b, cu_fmt("import os\nimport sys\n\nsys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), \"..\"))\nfrom %s import %s\n\nCASES = [\n%s]\n\n"
        "failed = 0\nfor fn, args, want in CASES:\n    got = fn(*args)\n    shown = \"%%s(%%s)\" %% (fn.__name__, \", \".join(str(a) for a in args))\n    if got == want:\n        print(\"ok \" + shown)\n"
        "    else:\n        failed += 1\n        print(\"FAIL %%s: got %%s, want %%s\" %% (shown, got, want))\n%sprint(\"%%d passed, %%d failed\" %% (len(CASES) - failed, failed))\nsys.exit(1 if failed else 0)\n", p->name, im.p, c.p, p->failfast ? "        print(\"stopped at the first failure\")\n        sys.exit(1)\n" : "")); break;
    case X_JS: buf_str(&b, cu_fmt("'use strict';\n%s\nconst CASES = [\n%s];\n\nlet failed = 0;\nfor (const [fn, args, want] of CASES) {\n  const got = fn(...args);\n  const shown = `${fn.name}(${args.join(', ')})`;\n"
        "  if (got === want) console.log('ok ' + shown);\n  else { failed++; console.log(`FAIL ${shown}: got ${got}, want ${want}`);%s }\n}\nconsole.log(`${CASES.length - failed} passed, ${failed} failed`);\nprocess.exit(failed ? 1 : 0);\n", im.p, c.p, p->failfast ? " console.log('stopped at the first failure'); process.exit(1);" : "")); break;
    case X_SH: buf_str(&b, cu_fmt("#!/bin/sh\ncd \"$(dirname \"$0\")/..\" || exit 2\n%spassed=0; failed=0\ncheck() {\n    want=$1; shift\n    got=$(\"$@\")\n    name=$1; shift\n    shown=\"$name($(echo \"$*\" | sed 's/ /, /g'))\"\n"
        "    if [ \"$got\" = \"$want\" ]; then passed=$((passed + 1)); echo \"ok $shown\"\n    else failed=$((failed + 1)); echo \"FAIL $shown: got $got, want $want\"%s; fi\n}\n%secho \"$passed passed, $failed failed\"\n[ \"$failed\" -eq 0 ]\n", im.p, p->failfast ? "; echo \"stopped at the first failure\"; exit 1" : "", c.p)); break;
    case X_C: buf_str(&b, cu_fmt("#include <stdio.h>\n%s\nstatic int passed, failed;\n#define CHECK(call, want) do { int got = (call); if (got == (want)) { passed++; printf(\"ok %%s\\n\", #call); } \\\n"
        "    else { failed++; printf(\"FAIL %%s: got %%d, want %%d\\n\", #call, got, (want));%s } } while (0)\n\nint main(void) {\n%s    printf(\"%%d passed, %%d failed\\n\", passed, failed);\n    return failed ? 1 : 0;\n}\n", im.p, p->failfast ? " printf(\"stopped at the first failure\\n\"); return 1;" : "", c.p)); break;
    default: buf_str(&b, cu_fmt("use strict;\nuse warnings;\n%s\nmy ($passed, $failed) = (0, 0);\nsub check {\n    my ($name, $args, $want) = @_;\n    no strict 'refs';\n    my $got = &$name(@$args);\n"
        "    my $shown = \"$name(\" . join(\", \", @$args) . \")\";\n    if ($got == $want) { $passed++; print \"ok $shown\\n\"; }\n    else { $failed++; print \"FAIL $shown: got $got, want $want\\n\";%s }\n}\n%s"
        "print \"$passed passed, $failed failed\\n\";\nexit($failed ? 1 : 0);\n", im.p, p->failfast ? " print \"stopped at the first failure\\n\"; exit(1);" : "", c.p)); break;
    }
    free(c.p); free(im.p);
    return cu_keep(b.p);
}
/* sorteia os casos de uma funcao: valores em que a expressao certa e a com defeito diferem (no primeiro caso) e as contas ficam nao negativas */
static void x_cases(XFi *fi) {
    for (int k = 0; k < 2; k++) {
        int ok = 0;
        for (int t = 0; t < 60 && !ok; t++) {
            for (int i = 0; i < 3; i++) fi->a[k][i] = 2 + cu_n(28);
            long g, b;
            if (!x_eval(fi->f, fi->f->num, fi->a[k], &g) || !x_eval(fi->f, fi->f->bad, fi->a[k], &b) || g == b) continue;
            if (k && fi->a[0][0] == fi->a[1][0]) continue;
            fi->want[k] = g; ok = 1;
        }
        if (!ok) cu_die("sem casos para %s", fi->f->name);
    }
}
static XFi *x_fi(XProj *p, const char *name, int *mi) {
    for (int m = 0; m < p->nmod; m++) for (int i = 0; i < p->mod[m].nfn; i++) if (!strcmp(p->mod[m].fn[i].name, name)) { if (mi) *mi = m; return &p->mod[m].fn[i]; }
    return NULL;
}
static const XFn *x_fn_new(const XProj *p) {      /* funcao do acervo que o projeto ainda nao tem */
    for (int t = 0; t < 80; t++) {
        const XFn *f = &g_x_fns[cu_ix(X_NFN)]; int has = 0;
        for (int m = 0; m < p->nmod; m++) for (int i = 0; i < p->mod[m].nfn; i++) has |= p->mod[m].fn[i].f == f;
        if (!has) return f;
    }
    cu_die("acervo de funcoes esgotado"); return NULL;
}
static const char *x_docs_text(const XProj *p) {
    Buf b = {0};
    buf_str(&b, cu_fmt("# Functions of %s\n\nEvery function takes whole numbers and returns a whole number. Results are never negative in the supported range.\n"
                       "Keep one entry per function, grouped by module, in the order they appear in the source file.\n", p->name));
    for (int m = 0; m < p->nmod; m++) {
        buf_str(&b, cu_fmt("\n## %s (%s)\n\n", p->mod[m].name, x_src(p, m)));
        for (int i = 0; i < p->mod[m].nfn; i++) { const XFi *fi = &p->mod[m].fn[i]; buf_str(&b, cu_fmt("- `%s(%s)`: %s. Example: `%s` is %ld.\n", fi->name, x_arglist(fi->f, "", ", "), fi->f->en, x_shown(fi->name, fi->f, fi->a[0]), fi->want[0])); }
    }
    buf_str(&b, "\n## Conventions\n\nA new function gets its entry here in the same change that adds it, with one example taken from the test cases.\n"
                "A renamed function keeps its entry; only the name changes. A removed function loses its entry.\n"
                "\n## Units\n\nMoney is in cents, time in the unit the function name says, lengths in the unit of the arguments. Percentages are whole numbers:\n"
                "a rate of 15 means fifteen percent. Division always rounds down, so callers that need rounding to nearest must add half of the\ndivisor before calling. "
                "Arguments are expected to be zero or positive; the functions do not check them.\n");
    return cu_keep(b.p);
}
static const char *x_brief_text(const XProj *p) {
    Buf b = {0};
    buf_str(&b, cu_fmt("# %s: %s helpers in %s\ntest: %s\nlayout:\n  %s  source, one file per module\n  %s  the test runner with its cases\n", p->name, p->what, g_x_lang[p->st], p->testcmd, x_srcdir(p), p->st == X_JS ? "test/" : "tests/"));
    if (p->rules) buf_str(&b, "rules:\n  - Add a test case for every new function.\n");
    if (p->never) buf_str(&b, cu_fmt("never:\n  - delete the test runner `rm*test_%s*`\n", p->name));
    if (p->askrule) buf_str(&b, "ask:\n  - delete files `rm *`\n");
    if (p->rules) buf_str(&b, cu_fmt("done: `%s` passes\n", p->testcmd));
    if (p->docs) buf_str(&b, "docs:\n  docs/functions.md  the list of functions; update it when one is added, renamed or removed\n");
    return cu_keep(b.p);
}
static const char *const x_conf_keys[][3] = {{"port", "8080", "1-65535"}, {"timeout", "30", "1-600"}, {"retries", "3", "0-10"}, {"batch_size", "50", "1-1000"}, {"workers", "4", "1-64"}, {"cache_minutes", "15", "0-1440"}};
static void x_conf_add(XProj *p, const char *k, const char *v, const char *r) { p->ck[p->nck] = k; p->cv[p->nck] = v; p->cr[p->nck++] = r; }
static const char *x_conf_text(const XProj *p) {
    Buf b = {0}; buf_str(&b, "# settings: key = value, one per line\n");
    for (int i = 0; i < p->nck; i++) buf_str(&b, cu_fmt("%s = %s\n", p->ck[i], p->cv[i]));
    return cu_keep(b.p);
}
static const char *x_conf_check(const XProj *p) {
    Buf b = {0};
    buf_str(&b, "#!/bin/sh\n# checks settings.conf: every key present, every value inside its range\ncd \"$(dirname \"$0\")/..\" || exit 2\nbad=0\n"
                "range() {\n    v=$(sed -n \"s/^$1 = //p\" settings.conf)\n    if [ -z \"$v\" ]; then echo \"missing key: $1\"; bad=1; return; fi\n"
                "    if [ \"$v\" -lt \"$2\" ] 2>/dev/null || [ \"$v\" -gt \"$3\" ] 2>/dev/null; then echo \"bad value for $1: $v (allowed $2-$3)\"; bad=1; else echo \"$1 = $v\"; fi\n}\n");
    for (int i = 0; i < p->nck; i++) buf_str(&b, cu_fmt("range %s %s %s\n", p->ck[i], cu_part(p->cr[i], '-', 0), cu_part(p->cr[i], '-', 1)));
    buf_str(&b, "if [ \"$bad\" -eq 0 ]; then echo \"settings ok\"; else echo \"settings invalid\"; exit 1; fi\n");
    return cu_keep(b.p);
}
static int g_x_mt;      /* horario de modificacao dos arquivos: crescente e fixo, para a ordem do Glob nao depender da hora */
static void x_put(const Scn *s, const char *rel, const char *text, int mode) {
    const char *path = x_abs(s, rel);
    cu_put(path, text, mode);
    struct timespec ts[2] = {{1700000000 + g_x_mt, 0}, {1700000000 + g_x_mt, 0}}; g_x_mt += 60;
    utimensat(AT_FDCWD, path, ts, 0);
}
static const char *x_slurp(const Scn *s, const char *rel) {
    FILE *f = fopen(x_abs(s, rel), "rb"); if (!f) return NULL;
    Buf b = {0}; buf_str(&b, ""); char t[4096]; size_t n;
    while ((n = fread(t, 1, sizeof t, f)) > 0) buf_add(&b, t, n);
    fclose(f);
    return cu_keep(b.p);
}
/* sorteia o projeto: pilha, modulos e funcoes; nfn = funcoes por modulo */
static void x_proj(XProj *p, const Scn *s, int st, int nmod, int nfn) {
    memset(p, 0, sizeof *p);
    p->s = s; p->st = st; p->name = s->name; p->what = CU_PA(g_x_whats);
    int used[X_NFN] = {0}, mused[CU_N(g_x_modnames)] = {0};
    p->nmod = nmod;
    for (int m = 0; m < nmod; m++) {
        int mi; do mi = cu_n(CU_N(g_x_modnames)); while (mused[mi] || !strcmp(g_x_modnames[mi], s->name));
        mused[mi] = 1; p->mod[m].name = g_x_modnames[mi]; p->mod[m].nfn = nfn;
        for (int i = 0; i < nfn; i++) {
            int fi; int t = 0; do fi = cu_ix(X_NFN); while (used[fi] && ++t < 400);
            if (used[fi]) cu_die("acervo de funcoes esgotado");
            used[fi] = 1;
            XFi *x = &p->mod[m].fn[i]; x->f = &g_x_fns[fi]; x->name = x->f->name; x->num = x->f->num;
            x_cases(x);
        }
    }
    p->testrel = cu_fmt("%s/test_%s.%s", st == X_JS ? "test" : "tests", p->name, g_x_ext[st]);
    p->testcmd = st == X_PY ? cu_fmt("python3 %s", p->testrel) : st == X_JS ? cu_fmt("node %s", p->testrel) : st == X_SH ? cu_fmt("sh %s", p->testrel) : st == X_C ? "make test" : cu_fmt("perl %s", p->testrel);
}
static void x_proj_write(const XProj *p) {
    const Scn *s = p->s;
    g_x_mt = 0;
    if (p->st == X_PY) x_put(s, cu_fmt("%s/__init__.py", p->name), "", 0);
    for (int m = 0; m < p->nmod; m++) { x_put(s, x_src(p, m), x_mod_text(p, m), 0); if (p->st == X_C) x_put(s, x_hdr(p, m), x_hdr_text(p, m), 0); }
    x_put(s, p->testrel, x_test_text(p), 0);
    if (p->st == X_C) x_put(s, "Makefile", cu_fmt("CC = gcc\nCFLAGS = -Wall -O1\n\ntest: build/test_%s\n\t./build/test_%s\n\nbuild/test_%s: src/*.c src/*.h tests/test_%s.c\n\tmkdir -p build\n\t$(CC) $(CFLAGS) -o $@ src/*.c tests/test_%s.c\n\n.PHONY: test\n",
                                                  p->name, p->name, p->name, p->name, p->name), 0);
    if (p->readme) x_put(s, "README.md", cu_fmt("# %s\n\n%s helpers in %s, one file per module under `%s`.\n\n## Tests\n\n    %s\n\nThe runner prints one line per case and ends with the totals.\n", p->name, p->what, g_x_lang[p->st], x_srcdir(p), p->testcmd), 0);
    if (p->docs) x_put(s, "docs/functions.md", x_docs_text(p), 0);
    if (p->conf) { x_put(s, "settings.conf", x_conf_text(p), 0); x_put(s, "tools/check_settings.sh", x_conf_check(p), 0755); }
    for (int i = 0; i < p->nx; i++) x_put(s, p->xrel[i], p->xtxt[i], p->xmode[i]);
    if (p->brief) x_put(s, "KATANA.md", x_brief_text(p), 0);
}

/* ---- a pasta e o estado do motor para uma conversa nova ---- */
static void x_begin(const char *fam, Scn *s, XProj *p, int nskills, int nmcp) {
    free(g_x.ask.p);
    memset(&g_x, 0, sizeof g_x); g_x.fam = fam; buf_str(&g_x.ask, ""); g_x.ask.n = 0;
    s->nsk = 0; s->nms = 0; s->brief = 0;
    if (nskills) cu_skill_pick(s, NULL, nskills);
    if (nmcp) cu_ms_scn(s, NULL, nmcp);
    cu_mkdirs(s->cwd);
    if (chdir(s->cwd) != 0) cu_die("nao entrei em %s", s->cwd);
    if (!g_x_realgit) cu_put(cu_fmt("%s/.git/HEAD", s->cwd), "ref: refs/heads/main\n", 0);
    cu_mkdirs(cu_fmt("%s/bin", g_cu_sb));
    setenv("PATH", cu_fmt("%s/bin:%s", g_cu_sb, g_cu_path0), 1);
    { char self[1200]; const ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1); if (l > 0) { self[l] = 0; if (symlink(self, cu_fmt("%s/bin/katana", g_cu_sb)) != 0) cu_die("nao criei o atalho do katana"); } }
    setenv("HOME", s->home, 1); setenv("USER", s->user, 1); setenv("SHELL", "/bin/bash", 1);
    setenv("GIT_AUTHOR_DATE", "2026-01-05T10:00:00", 1); setenv("GIT_COMMITTER_DATE", "2026-01-05T10:00:00", 1);
    setenv("GIT_AUTHOR_NAME", s->user, 1); setenv("GIT_COMMITTER_NAME", s->user, 1); setenv("GIT_AUTHOR_EMAIL", cu_fmt("%s@example.org", s->user), 1); setenv("GIT_COMMITTER_EMAIL", cu_fmt("%s@example.org", s->user), 1);
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1); setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
    if (p) {
        x_proj_write(p);
        if (g_x_realgit && system("git init -q -b main . >/dev/null 2>&1 && git add -A >/dev/null 2>&1 && git commit -q -m 'initial version' >/dev/null 2>&1") != 0) cu_die("git init falhou");
        if (p->brief) {
            const char *bt = x_brief_text(p);
            Brief *br = (Brief *)calloc(1, sizeof *br); McpB fd = {0}; mcp_bput(&fd, "", 0);
            const int errs = br_parse(bt, br, &fd, "KATANA.md");
            char why[300]; snprintf(why, sizeof why, "%.280s", fd.p);
            const int badf = errs || (fd.n && !strstr(fd.p, "is guidance only"));
            free(br); free(fd.p);
            if (badf) cu_die("o lint do motor reclama do KATANA.md do cenario: %s", why);
        }
    }
    {   CuD d = {0}; cu_base_dict(s, &d); cu_set(&d, "tool", "x"); cu_set(&d, "file", "x"); cu_set(&d, "msg", "x"); cu_set(&d, "fn", "main");
        if (p) { cu_set(&d, "test", p->testcmd); cu_set(&d, "build", p->testcmd); }
        for (int i = 0; i < s->nsk; i++) { cu_put(cu_fmt("%s/.katana/skills/%s/SKILL.md", s->cwd, s->sk[i]->name), cu_skill_md(s->sk[i], &d), 0); cu_needs_stubs(s->sk[i]); } }
    for (int i = 0; i < g_nmcp; i++) { for (int j = 0; j < g_mcp[i].ntools; j++) { free(g_mcp[i].tools[j].name); free(g_mcp[i].tools[j].desc); free(g_mcp[i].tools[j].schema); } free(g_mcp[i].tools); }
    g_nmcp = 0; g_mcp_ready = 1;
    for (int i = 0; i < s->nms; i++) {
        const CuMs *m = s->ms[i]; McpSrv *v = &g_mcp[g_nmcp++]; memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "%s", m->name); v->project = 1; v->alive = 1; v->was_ok = 1;
        snprintf(v->source, sizeof v->source, "%s/.katana/mcp.json", s->cwd);
        int nt = 0; while (nt < 8 && m->t[nt].name) nt++;
        v->tools = calloc((size_t)nt, sizeof v->tools[0]); v->ntools = nt;
        for (int j = 0; j < nt; j++) { v->tools[j].name = strdup(m->t[j].name); v->tools[j].desc = strdup(m->t[j].desc); v->tools[j].schema = strdup(cu_mt_schema(&m->t[j], 1)); }
        cu_put(cu_fmt("%s/.katana/mcp/%s.md", s->cwd, m->name), cu_fmt("what: %s\nfirst: %s\norder: %s\navoid: %s\n", m->purpose,
               m->list >= 0 || m->first >= 0 ? cu_fmt("%s__%s to get the %s ids", m->name, m->t[m->first >= 0 ? m->first : m->list].name, m->noun_en) : "", m->order ? m->order : "", m->avoid ? m->avoid : ""), 0);
    }
    setenv("HOME", s->home, 1); setenv("USER", s->user, 1); setenv("SHELL", "/bin/bash", 1);
    setenv("GIT_AUTHOR_DATE", "2026-01-05T10:00:00", 1); setenv("GIT_COMMITTER_DATE", "2026-01-05T10:00:00", 1);
    setenv("GIT_AUTHOR_NAME", s->user, 1); setenv("GIT_COMMITTER_NAME", s->user, 1); setenv("GIT_AUTHOR_EMAIL", cu_fmt("%s@example.org", s->user), 1); setenv("GIT_COMMITTER_EMAIL", cu_fmt("%s@example.org", s->user), 1);
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1); setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
    setenv("KATANA_CACHE_DIR", cu_fmt("%s/cache", g_cu_sb), 1);
    g_trust = 1; g_con_rich = 1;
    g_todo[0] = 0; ct_task_sync(); g_todo_touched = 0;
    ct_session_reset(); g_rd_delta_on = 1; g_plan_mode = 0;
    g_cp_seq = 0; g_cp_dir[0] = 0;
    g_ctb_seq = 0; g_job_seq = 0;      /* numeracao das tarefas em segundo plano: recomeca a cada conversa */
    g_undo_req++; g_cu_pl = 0; g_cu_plold = 0; g_cu_note[0] = 0;
    /* o modo do prompt sai do estado inicial do sorteio, sem gastar sorteio (o check refaz com o mesmo) */
    const int wsum = g_cu_mixw[0] + g_cu_mixw[1] + g_cu_mixw[2];
    int mode = g_cu_sysmode;
    if (wsum > 0) { const int r = (int)((g_x_rng0 >> 24) % (uint64_t)wsum); mode = r < g_cu_mixw[0] ? 0 : r < g_cu_mixw[0] + g_cu_mixw[1] ? 1 : 2; }
    int decl = mode == 2 || (mode == 0 && (int)((g_x_rng0 >> 40) % 100) < g_cu_decl);
    if (g_x_rmode >= 0) { mode = g_x_rmode; decl = g_x_rdecl; }
    g_x.mode = mode; g_x.decl = decl;
    static char sys[60000];
    setenv("KATANA_TOOL_DECL", decl ? "1" : "0", 1);
    g_tool_xml = 1; g_tool_glm = 0; g_web = 1; g_code = 0;
    agent_build_sys(sys, sizeof sys, "");
    mcp_sys(sys, sizeof sys);
    char *dt = strstr(sys, "- Today's date: ");
    if (dt && strlen(dt) > 27) memcpy(dt + 16, s->date, 10);
    const char *txt = sys;
    if (mode == 1) {      /* blocks: do prompt do motor ficam os cartoes MCP, o ambiente (pasta, arquivos, skills) e o bloco do projeto */
        const char *m = strstr(sys, "\n\n# MCP servers ("), *env = strstr(sys, "\n\n# Environment");
        txt = m && env && m < env ? m + 2 : env ? env + 2 : sys;
    }
    x_push('s', txt, 1);
}
