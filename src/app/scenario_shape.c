/* ---------- katana curriculum, cenarios compostos: as formas de um pedido ----------
 * O mesmo pedido chega de jeitos diferentes: curto como a familia escreveu, longo com contexto, educado, seco, com erros
 * de digitacao, com outra lingua misturada, formal. A forma sai do estado inicial do sorteio da conversa (g_x.srng), sem
 * gastar o sorteio do cenario, entao o check refaz igual. Nada aqui muda o que o pedido pede: nomes, caminhos, valores e
 * tudo entre crases ficam como estao (os erros de digitacao so caem em palavras comuns de uma lista).
 * Reservado para o arquivo de validacao (nunca no treino): o pedido repassado de outra pessoa (fwd), o PS com contexto (ps),
 * o ultimo quarto das frases de contexto e os comecos de segundo pedido da lista reservada. */

static uint32_t xs_rand(void) { g_x.srng ^= g_x.srng << 13; g_x.srng ^= g_x.srng >> 7; g_x.srng ^= g_x.srng << 17; return (uint32_t)(g_x.srng >> 32); }
static int xs_n(int n) { return n <= 1 ? 0 : (int)(xs_rand() % (uint32_t)n); }
static void x_shape_tag(const char *tag) {
    const size_t l = strlen(g_x.shape);
    for (const char *p = g_x.shape; (p = strstr(p, tag)); p++) if ((p == g_x.shape || p[-1] == '+') && (p[strlen(tag)] == 0 || p[strlen(tag)] == '+')) return;
    snprintf(g_x.shape + l, sizeof g_x.shape - l, "%s%s", l ? "+" : "", tag);
}
static const char *const g_xs_ctx[][2] = {
    {"I'm picking this project up again after a few weeks away.", "To voltando nesse projeto depois de umas semanas longe."},
    {"We ship on Friday and I'm going through my list.", "A entrega e sexta e to passando pela minha lista."},
    {"A colleague reported this in the morning standup.", "Um colega falou disso na reuniao de hoje cedo."},
    {"I have a call in ten minutes.", "Tenho uma chamada em dez minutos."},
    {"This came up while I was reviewing yesterday's changes.", "Isso apareceu enquanto eu revisava as mudancas de ontem."},
    {"I'm new to this codebase and still finding my way around.", "Sou novo nesse codigo e ainda to me achando."},
    {"The client asked about this twice already.", "O cliente ja perguntou disso duas vezes."},
    {"I was about to do it by hand and thought better of it.", "Eu ia fazer na mao e pensei melhor."},
    {"Before I merge my branch I want this sorted.", "Antes de juntar meu branch quero isso resolvido."},
    {"I'm cleaning up before the handover next week.", "To arrumando a casa antes de passar o projeto semana que vem."},
    {"Our CI has been noisy lately and I want to rule this out.", "Nosso CI anda barulhento e quero descartar isso."},
    {"I only have the laptop with me today, no second screen.", "Hoje so to com o notebook, sem segunda tela."},
    {"It is the last item of my morning.", "E o ultimo item da minha manha."},
    {"I promised the team an answer on this today.", "Prometi pro time uma resposta sobre isso hoje."},
    {"Nobody has touched this part in months.", "Ninguem mexe nessa parte ha meses."},
    {"I'm back from vacation and catching up.", "Voltei de ferias e to me atualizando."},
    /* daqui para baixo: so no arquivo de validacao */
    {"My manager wants a status on this by lunch.", "Meu gerente quer um status disso ate o almoco."},
    {"I'm pairing with an intern and want to show how it is done.", "To pareando com um estagiario e quero mostrar como se faz."},
    {"This has been on my list since the last retro.", "Isso ta na minha lista desde a ultima retro."},
    {"I'm writing the release notes and noticed this.", "To escrevendo as notas da versao e reparei nisso."},
    {"The audit is next month and I am going file by file.", "A auditoria e mes que vem e to indo arquivo por arquivo."},
};
#define XS_NCTX_TRAIN 16
static const char *xs_ctx(int lang) {
    const int n = (int)(sizeof g_xs_ctx / sizeof g_xs_ctx[0]);
    return g_xs_ctx[g_cu_hold ? XS_NCTX_TRAIN + xs_n(n - XS_NCTX_TRAIN) : xs_n(XS_NCTX_TRAIN)][lang];
}
/* palavras comuns em que um erro de digitacao pode cair (nunca um nome do projeto, um caminho ou um valor) */
static const char *const g_xs_words[] = {"please", "function", "tests", "failing", "project", "should", "something", "where", "which", "defined", "change", "rename", "settings", "value", "again",
    "check", "explain", "delete", "files", "create", "called", "before", "update", "everywhere", "returning", "wrong", "there", "think", "version", "server", "connect", "write", "script", "about", "passing",
    "corrige", "testes", "funcao", "projeto", "arquivo", "arquivos", "porque", "onde", "qual", "muda", "renomeia", "valor", "roda", "novo", "confere", "explica", "apaga", "cria", "antes", "consegue",
    "alguma", "coisa", "coloca", "configuracoes", "favor", "devia", "errado", "quais", "esse", "escreve", "servidor", "versao", "sobre", "passando", "falhando", "lugar"};
static const char *xs_typo_word(const char *w, size_t l) {
    char *o = cu_fmt("%.*s", (int)l, w);
    if (l < 4) return o;
    const int op = xs_n(3), i = 1 + xs_n((int)l - 2);
    if (op == 0 && i + 1 < (int)l && o[i] != o[i + 1]) { const char c = o[i]; o[i] = o[i + 1]; o[i + 1] = c; }      /* duas letras trocadas */
    else if (op == 1) memmove(o + i, o + i + 1, l - (size_t)i);                                                /* uma letra a menos */
    else { char *d = cu_fmt("%.*s%c%s", i, o, o[i], o + i); return d; }                                        /* uma letra dobrada */
    return o;
}
static const char *xs_typos(const char *t) {
    Buf b = {0}; buf_str(&b, ""); int done = 0, tick = 0;
    const int want = 1 + xs_n(2);
    for (const char *p = t; *p; ) {
        if (*p == '`') { tick = !tick; buf_add(&b, p, 1); p++; continue; }
        if (!isalpha((unsigned char)*p) || tick) { buf_add(&b, p, 1); p++; continue; }
        size_t l = 0; while (isalnum((unsigned char)p[l]) || p[l] == '_') l++;
        int hit = 0;
        const int bound = (p == t || !(isalnum((unsigned char)p[-1]) || strchr("_./-$", p[-1]))) && !(p[l] && strchr("_./-(=", p[l]));
        if (bound && done < want) for (size_t i = 0; i < sizeof g_xs_words / sizeof g_xs_words[0] && !hit; i++) if (strlen(g_xs_words[i]) == l && !strncmp(g_xs_words[i], p, l)) hit = 1;
        if (hit && xs_n(3) != 0) { const char *w = xs_typo_word(p, l); buf_str(&b, w); done += strncmp(w, p, l) != 0 || strlen(w) != l; }
        else buf_add(&b, p, l);
        p += l;
    }
    char *r = cu_keep(b.p);
    return done ? r : NULL;
}
static const char *xs_formal(const char *t) {
    char *o = cu_fmt("%s", t);
    /* a primeira letra so sobe quando a primeira palavra e comum (um nome de funcao ou de chave nunca muda) */
    static const char *const start[] = {"the", "run", "set", "rename", "where", "which", "what", "find", "in", "edit", "change", "please", "can", "there", "i", "something", "tests", "is", "are", "explain", "how", "give",
        "list", "add", "create", "write", "connect", "delete", "remove", "tidy", "clean", "why", "check", "cut", "record", "take", "keep", "use", "send", "show", "does", "who", "and", "now", "one", "actually", "on", "sorry",
        "roda", "os", "o", "a", "onde", "qual", "quais", "em", "acha", "me", "como", "corrige", "alguma", "tem", "acho", "renomeia", "coloca", "muda", "edita", "apaga", "da", "limpa", "por", "consegue", "preciso",
        "acrescenta", "cria", "escreve", "liga", "fecha", "registra", "guarda", "tira", "usa", "nas", "lista", "e", "agora", "mais", "na", "pensando", "desculpa", "ela", "quem", "confere", "manda", "mostra"};
    size_t wl = 0; while (isalpha((unsigned char)o[wl])) wl++;
    int safe = 0;
    if (o[wl] == ' ' || o[wl] == ',' || o[wl] == 0 || o[wl] == '\'') for (size_t i = 0; i < sizeof start / sizeof start[0] && !safe; i++) safe = strlen(start[i]) == wl && !strncmp(start[i], o, wl);
    if (safe && islower((unsigned char)o[0])) o[0] = (char)toupper((unsigned char)o[0]);
    const size_t l = strlen(o);
    if (l && !strchr(".?!:)`\"", o[l - 1])) return cu_fmt("%s.", o);
    return o;
}
#define XS_PK(...) ({ const char *const a_[] = {__VA_ARGS__}; a_[xs_n((int)(sizeof a_ / sizeof a_[0]))]; })
/* o pedido t na forma sorteada para esta conversa; nreq = quantos pedidos ja houve (os seguintes mudam pouco) */
static const char *x_shape(const char *t, int nreq) {
    if (g_x.noshape || !*t) return t;
    const int pt = g_cu_lang != 0;
    if (strchr(t, '\n')) {      /* pedido com trecho colado: so a primeira linha muda */
        const char *nl = strchr(t, '\n');
        if (nreq || xs_n(100) < 55) return t;
        x_shape_tag("ctx");
        return cu_fmt("%s %.*s%s", xs_ctx(pt), (int)(nl - t), t, nl);
    }
    if (nreq) {      /* segundo pedido em diante */
        if (g_cu_hold) {
            if (xs_n(100) < 12) return t;
            x_shape_tag("held-lead");
            return cu_fmt("%s%s", pt ? XS_PK("mais uma coisa: ", "ja que ta ai: ", "antes que eu esqueca: ", "aproveitando: ") : XS_PK("one more thing: ", "while you are at it: ", "before I forget: ", "since you are here: "), t);
        }
        const int r = xs_n(100);
        if (r < 52) return t;
        if (r < 66) { x_shape_tag("lead"); return cu_fmt("%s%s", pt ? XS_PK("ok. ", "boa. ", "certo, ", "valeu. agora ", "beleza. ") : XS_PK("ok. ", "nice. ", "right, ", "thanks. now ", "good. "), t); }
        if (r < 76) { x_shape_tag("polite"); return cu_fmt("%s%s", t, pt ? XS_PK(" por favor", " obrigado", " se puder") : XS_PK(" please", " thanks", " if you can")); }
        if (r < 88) { const char *y = xs_typos(t); if (y) { x_shape_tag("typo"); return y; } return t; }
        x_shape_tag("formal"); return xs_formal(t);
    }
    int r = xs_n(100);
    if (g_cu_hold) {      /* validacao: formas reservadas na maioria; o resto com as frases de contexto reservadas */
        if (r < 34) { x_shape_tag("fwd"); return pt ? cu_fmt(XS_PK("repassando do chat do time: \"%s\"", "um colega pediu isso aqui: \"%s\" voce pega?", "chegou por email: \"%s\""), t) : cu_fmt(XS_PK("forwarding from the team chat: \"%s\"", "a colleague asked for this: \"%s\" can you take it?", "this came by email: \"%s\""), t); }
        if (r < 62) { x_shape_tag("ps"); return cu_fmt("%s PS: %s", xs_formal(t), xs_ctx(pt)); }
        if (r < 90) { const char *y = xs_typos(t); x_shape_tag("ctx"); if (y) x_shape_tag("typo"); return cu_fmt("%s %s", xs_ctx(pt), y ? y : t); }
        return t;
    }
    if (r < 26) return t;
    if (r < 42) { x_shape_tag("ctx"); return xs_n(2) ? cu_fmt("%s %s", xs_ctx(pt), t) : cu_fmt("%s %s", xs_formal(t), xs_ctx(pt)); }
    if (r < 53) { x_shape_tag("polite"); return pt ? cu_fmt(XS_PK("Oi! %s Valeu demais.", "Bom dia. %s", "%s - obrigado desde ja", "Desculpa incomodar: %s", "Se nao for pedir muito: %s", "Por gentileza: %s"), xs_formal(t)) : cu_fmt(XS_PK("Hi! %s Thanks a lot.", "Good morning. %s", "%s - thanks in advance", "Sorry to bother you: %s", "If it is not too much to ask: %s", "Kindly: %s"), xs_formal(t)); }
    if (r < 63) { x_shape_tag("blunt"); return pt ? cu_fmt(XS_PK("%s pra ontem", "%s - rapido", "%s!!", "seguinte: %s", "olha: %s", "sem enrolar: %s"), t) : cu_fmt(XS_PK("%s asap", "%s - quick", "%s!!", "listen: %s", "look: %s", "no fuss: %s"), t); }
    if (r < 74) { const char *y = xs_typos(t); if (y) { x_shape_tag("typo"); return y; } return t; }
    if (r < 83) { x_shape_tag("mix"); return xs_n(3) == 0 ? cu_fmt("%s %s", xs_ctx(!pt), t) : pt ? cu_fmt(XS_PK("hey, %s thanks", "quick one: %s", "hi there. %s", "%s (sorry, long day)"), t) : cu_fmt(XS_PK("oi, %s valeu", "rapidinho: %s", "e ai. %s", "%s (desculpa, dia longo)"), t); }
    if (r < 91) { x_shape_tag("formal"); return xs_formal(t); }
    {   const char *y = xs_typos(t); x_shape_tag("ctx"); x_shape_tag("polite"); if (y && xs_n(2)) { x_shape_tag("typo"); t = y; }
        return pt ? cu_fmt("%s %s Obrigado.", xs_ctx(1), xs_formal(t)) : cu_fmt("%s %s Thank you.", xs_ctx(0), xs_formal(t)); }
}
