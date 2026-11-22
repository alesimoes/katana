/* ---------- katana curriculum, cenarios compostos: as convencoes (servidor MCP, skill, KATANA.md) ----------
 * As nove familias de molde (mcp-call, mcp-chain, mcp-schema, skill-match, skill-missing-need, skill-fail, brief-write,
 * skill-write, mcp-add) passam para o motor de cenarios em tres familias:
 *   x-mcp      um servidor MCP de verdade (processo python escrito pelo gerador, com estado) atendido pelo cliente do motor;
 *   x-ability  skills de verdade do projeto: escolher entre parecidas, needs que falta, passo que falha, so ler, perguntar;
 *   x-make     criar e mudar KATANA.md, SKILL.md e a ligacao de um servidor, conferidos pelos comandos do proprio katana.
 * Cada situacao comeca de um jeito (id ja no pedido, id pelo nome, numero solto, id num arquivo, esquema no cartao ou nao,
 * servidor ocupado uma vez, item travado, servidor fora do ar, arquivo que ja existe, arquivo com erro, mensagem do /init).
 * Nenhum resultado e escrito aqui: tudo sai da ferramenta do motor ou do servidor rodando. */

static const char *x_tsel(const char *names);

/* servidor MCP generico do cenario: itens com id e estado; listar, mudar um item (os argumentos ficam gravados nele), criar
   (devolve uma referencia nova); pode responder ocupado na primeira chamada, ter um item travado ou cair a cada chamada */
static const char g_x_srv_py[] =
    "import sys, json, os\n"
    "cfg = sys.argv[1] if len(sys.argv) > 1 else os.path.splitext(os.path.abspath(__file__))[0] + '.json'\n"
    "C = json.load(open(cfg))\n"
    "items = {}\n"
    "for it in C['items']:\n    items[it['id']] = dict(it)\n"
    "calls = 0\nmade = 0\n"
    "def call(name, a):\n"
    "    global calls, made\n"
    "    calls += 1\n"
    "    t = None\n"
    "    for x in C['tools']:\n        if x['name'] == name:\n            t = x\n"
    "    if t is None:\n        return True, 'unknown tool ' + name\n"
    "    if C.get('die'):\n        sys.exit(3)\n"
    "    if C.get('fail_once') and calls == 1:\n        return True, 'busy: another client holds the session, try again'\n"
    "    if t.get('list'):\n        out = [items[k] for k in items]\n"
    "        for v in a.values():\n            if isinstance(v, str):\n                hit = [x for x in out if v.lower() in x['name'].lower()]\n                if hit:\n                    out = hit\n"
    "        if isinstance(a.get('limit'), int):\n            out = out[:a['limit']]\n        return False, json.dumps(out)\n"
    "    ida = t.get('idarg')\n"
    "    if ida:\n"
    "        v = a.get(ida)\n"
    "        if v not in items:\n            return True, \"unknown %s '%s': ids look like %s-0042\" % (C['noun'], v, C['idp'])\n"
    "        if v == C.get('locked'):\n            return True, '%s %s is locked by another session' % (C['noun'], v)\n"
    "        rest = dict((k, a[k]) for k in a if k != ida)\n        items[v][name] = rest if rest else True\n"
    "        if t.get('reply'):\n            return False, t['reply'].replace('@ID@', v)\n"
    "        return False, json.dumps(items[v])\n"
    "    made += 1\n"
    "    r = {'ok': True, 'ref': '%s-%d' % (name.split('_')[-1], 100 + made)}\n"
    "    r.update(a)\n"
    "    return False, json.dumps(r)\n"
    "for line in sys.stdin:\n"
    "    line = line.strip()\n"
    "    if not line:\n        continue\n"
    "    m = json.loads(line)\n"
    "    if 'id' not in m:\n        continue\n"
    "    k = m.get('method')\n"
    "    if k == 'initialize':\n        r = {'protocolVersion': m.get('params', {}).get('protocolVersion', '2025-06-18'), 'capabilities': {'tools': {}}, 'serverInfo': {'name': C['name'], 'version': '1.0'}}\n"
    "    elif k == 'tools/list':\n        r = {'tools': [{'name': x['name'], 'description': x['description'], 'inputSchema': x['inputSchema']} for x in C['tools']]}\n"
    "    elif k == 'tools/call':\n        err, txt = call(m['params']['name'], m['params'].get('arguments') or {})\n        r = {'content': [{'type': 'text', 'text': txt}], 'isError': err}\n"
    "    else:\n        print(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'error': {'code': -32601, 'message': 'method not found'}}), flush=True)\n        continue\n"
    "    print(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'result': r}), flush=True)\n";
/* grava o programa e a configuracao dele (mesmo nome, .json) */
static const CuMt *g_x_srv_rt; static const char *g_x_srv_reply;      /* a ferramenta que responde com os dados da tabela, e esses dados (com @ID@ no lugar do id) */
static void x_srv_write(const char *py, const CuMs *m, const char *const *ids, const char *const *names, int ni, int fail_once, const char *locked, int die) {
    Buf b = {0};
    buf_str(&b, cu_fmt("{\"name\": %s, \"noun\": %s, \"idp\": %s, \"fail_once\": %d, \"die\": %d, \"locked\": %s, \"items\": [", cu_q(m->name), cu_q(m->noun_en), cu_q(m->idp), fail_once, die, cu_q(locked ? locked : "")));
    for (int i = 0; i < ni; i++) buf_str(&b, cu_fmt("%s{\"id\": %s, \"name\": %s}", i ? ", " : "", cu_q(ids[i]), cu_q(names[i])));
    buf_str(&b, "], \"tools\": [");
    for (int j = 0; j < 8 && m->t[j].name; j++) {
        char idn[32] = ""; cu_mt_idarg(&m->t[j], idn, sizeof idn);
        buf_str(&b, cu_fmt("%s{\"name\": %s, \"description\": %s, \"inputSchema\": %s, \"list\": %s, \"idarg\": %s, \"reply\": %s}", j ? ", " : "", cu_q(m->t[j].name), cu_q(m->t[j].desc), cu_mt_schema(&m->t[j], 1), j == m->list ? "true" : "false", cu_q(idn),
                           cu_q(g_x_srv_rt == &m->t[j] && g_x_srv_reply ? g_x_srv_reply : "")));
    }
    buf_str(&b, "]}\n");
    char *js = cu_fmt("%s", py); js[strlen(js) - 3] = 0;
    cu_put(py, g_x_srv_py, 0); cu_put(cu_fmt("%s.json", js), b.p, 0);
    free(b.p); g_x_srv_rt = NULL; g_x_srv_reply = NULL;
}
/* o servidor registrado no x_begin passa a ter um processo de verdade: o cliente MCP do motor fala com ele */
static McpSrv *x_mcp_live(const CuMs *m, const char *py) {
    McpSrv *v = NULL; for (int i = 0; i < g_nmcp; i++) if (!strcmp(g_mcp[i].name, m->name)) v = &g_mcp[i];
    if (!v) cu_die("servidor %s fora da tabela", m->name);
    mcp_free_tools(v);
    v->cmd = strdup("python3"); v->args = malloc(sizeof(char *)); v->args[0] = strdup(py); v->nargs = 1;
    v->alive = 0; v->was_ok = 0;
    mcp_start(v);
    if (!v->alive || !v->ntools) cu_die("o servidor MCP do cenario nao subiu: %s", v->err);
    return v;
}
static const char *x_mbody(const char *r) { const char *n = strchr(r, '\n'); return !strncmp(r, "[external content from MCP tool", 31) && n ? n + 1 : r; }
static int x_merr(const char *r) { return !strncmp(x_mbody(r), "error:", 6); }
static const char *x_mitem(const char *r, const char *id) {      /* o objeto do item na lista que o servidor devolveu */
    const char *q = strstr(r, cu_fmt("{\"id\": \"%s\"", id)); if (!q) return NULL;
    int depth = 0; const char *e = q;
    for (; *e; e++) { if (*e == '{') depth++; else if (*e == '}' && --depth == 0) break; }
    return *e ? cu_fmt("%.*s", (int)(e - q + 1), q) : NULL;
}
static const char *x_mline(const char *r) { char *o = cu_fmt("%s", x_mbody(r)); size_t l = strlen(o); while (l && o[l - 1] == '\n') o[--l] = 0; return o; }
static const char *x_mproof(const char *r) {
    const char *o = x_mline(r);
    return strlen(o) <= 170 && !strchr(o, '\n') && !strchr(o, '`') ? cu_fmt(L(" The server answered `%s`.", " O servidor respondeu `%s`."), o) : "";
}
static const char *x_swap(const char *t, const char *from, const char *to) {
    const char *q = strstr(t, from); if (!q) cu_die("trecho ausente: %s", from);
    return cu_fmt("%.*s%s%s", (int)(q - t), t, to, q + strlen(from));
}
/* chamada a uma ferramenta do servidor: os argumentos valem contra o esquema que o servidor declara */
static const char *x_mc(const CuMs *m, const CuMt *t, const char *args, int flags) {
    char err[400];
    if (!cu_valid(cu_mt_schema(t, 1), args, t->name, err, sizeof err)) cu_die("%s__%s: %s\nargumentos: %s", m->name, t->name, err, args);
    {   Buf seen = {0}; buf_str(&seen, ""); char bad[80];      /* o modelo so pode mandar o que o pedido ou um resultado anterior trouxe */
        for (int i = 0; i < g_x.n; i++) if (g_x.m[i].role == 'u' || g_x.m[i].role == 't') { buf_str(&seen, g_x.m[i].text); buf_str(&seen, "\n"); }
        const int ok = cu_args_grounded(args, seen.p, bad, sizeof bad); free(seen.p);
        if (!ok) cu_die("argumento sem origem no pedido nem em resultado anterior"); }
    return x_call(NULL, cu_mfull(m, t), args, flags | XF_OLD);
}
/* verificacao do projeto antes da resposta final, quando o motor a cobraria (arquivos mudaram e ha test: no KATANA.md) */
static void x_verify(XProj *p) {
    br_sync();
    if (!g_br_changed || g_br_ran || g_br_noted) return;
    const Brief *b = br_cur(); const char *tc = br_single(b, "test", ""), *dn = br_single(b, "done", "");
    if (!tc && !dn) return;
    const char *o = x_test_end(p); if (!x_totals(o)) cu_die("sem totais na verificacao");
}

/* ======================= um servidor MCP de verdade ======================= */
enum { MS_KNOWN, MS_BYNAME, MS_RAWNUM, MS_BIG, MS_BUSY, MS_LOCKED, MS_DOWN, MS_DOWNRO, MS_TWO, MS_NONE, MS_FILE, MS_WRONGTOOL, MS_CARD, MS_CARDARG, MS_N };
static const char *const g_x_msname[MS_N] = {"id-known", "by-name", "bare-number", "big-tool", "busy-once", "locked", "server-down", "server-down-export", "two-items", "nobody-offers", "id-in-file", "wrong-tool-name", "card-tools", "card-args"};
static void fam_x_mcp(FILE *f, Scn *s) {
    int sit, fu, conv;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de mcp");
        sit = cu_n(MS_N); conv = cu_n(4);
        fu = (sit == MS_KNOWN || sit == MS_BYNAME || sit == MS_RAWNUM || sit == MS_FILE || sit == MS_WRONGTOOL) && cu_pct(55) ? 1 + cu_n(2) : 0;
        if (x_held(80 + sit, fu, conv, 0, 0) == (g_cu_hold != 0)) break;
    }
    const CuMs *m = NULL; int ti = -1; char idn[32] = "";
    for (int tr = 0; tr < 600 && !m; tr++) {
        const CuMs *c = cu_ms_pick(); idn[0] = 0;
        if (c->list < 0) continue;
        if (sit == MS_BIG) { if (c->big < 0) continue; ti = c->big; cu_mt_idarg(&c->t[c->big], idn, sizeof idn); }
        else if (sit == MS_BYNAME || sit == MS_TWO) { if (c->chain < 0 || !cu_mt_idarg(&c->t[c->chain], idn, sizeof idn)) continue; ti = c->chain; }
        else if (sit == MS_NONE || sit == MS_DOWNRO || sit == MS_CARD) ti = c->list;
        else { const int j = cu_n(8); if (!cu_mt_ok(&c->t[j]) || j == c->big || j == c->list || !cu_mt_idarg(&c->t[j], idn, sizeof idn)) continue; ti = j; }
        m = c;
    }
    if (!m) cu_die("nenhum servidor serve para %s", g_x_msname[sit]);
    const CuMt *t = &m->t[ti], *lt = &m->t[m->list];
    static XProj P; x_proj(&P, s, CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})), 2, 2); XProj *p = &P;
    p->readme = conv == 1; p->brief = conv >= 2; p->knows = 1;
    const int nall = cu_count(m->items, '/'), ni = nall < 4 ? nall : 4; const char *ids[4], *names[4]; int used[24] = {0};
    if (ni < 2) cu_die("servidor com poucos itens");
    for (int i = 0; i < ni; ) { const int k = cu_n(nall); if (k >= 24 || used[k]) continue; used[k] = 1; names[i] = cu_part(m->items, '/', k); ids[i] = cu_mid(m, 1000 + i * 2000 + cu_n(1900)); i++; }
    const char *id = ids[0];
    CuD d = {0}; cu_base_dict(s, &d); cu_feat(&d);
    const char *largs = "{}", *largs2 = NULL;      /* a lista: os obrigatorios de texto levam o nome procurado (ai ela e uma busca) */
    {   CuArg a[16]; const int n = cu_margs(lt, a, 16); int req = 0, other = 0; for (int i = 0; i < n; i++) if (!a[i].opt) { req = 1; other |= a[i].type != 's' || a[i].vals[0] == '@'; }
        const int uses = sit == MS_BYNAME || sit == MS_RAWNUM || sit == MS_TWO || sit == MS_LOCKED || sit == MS_DOWN || sit == MS_DOWNRO || fu == 2;
        if (req && uses && (other || sit != MS_BYNAME)) cu_die("lista com argumento obrigatorio nao serve para %s", g_x_msname[sit]);
        if (req) { CuD dl = {0}; cu_base_dict(s, &dl); g_cu_force_text = names[0]; largs = cu_margs_fill(m, lt, &dl, 0, NULL); g_cu_force_text = names[1]; largs2 = cu_margs_fill(m, lt, &dl, 0, NULL); g_cu_force_text = NULL; } }
    const char *args = sit == MS_NONE || sit == MS_DOWNRO || sit == MS_CARD ? "{}" : cu_margs_fill(m, t, &d, sit == MS_BIG ? 60 : sit == MS_BYNAME || sit == MS_TWO ? 0 : 45, id);
    if (sit == MS_BIG) { CuArg a[16]; const int n = cu_margs(t, a, 16); for (int i = 0; i < n; i++) if (a[i].type == 'o' && !cu_get(&d, a[i].name) && cu_pct(50)) { args = cu_margs_fill(m, t, &d, 100, id); break; } }
    const char *req, *wrongname = NULL;
    if (sit == MS_NONE) req = g_cu_nocap[cu_ix(CU_N(g_cu_nocap))][g_cu_lang];
    else if (sit == MS_CARD) req = cu_fmt(XV(L("what can the %s server do for me?", "o que o servidor %s consegue fazer por mim?"), L("which tools does %s give you?", "quais ferramentas o %s te da?"), L("what is connected as %s and what is it for?", "o que e o %s que esta conectado e pra que serve?")), m->name);
    else if (sit == MS_CARDARG) req = cu_fmt(XV(L("what do I have to give the %s tool of %s?", "o que eu preciso passar pra ferramenta %s do %s?"), L("which arguments does %s on %s take?", "quais argumentos o %s do %s aceita?")), t->name, m->name);
    else if (sit == MS_DOWNRO) req = cu_fmt(XV(L("which %ss does %s have right now, with their ids?", "quais %s o %s tem agora, com os ids?"), L("list the %ss of %s for me", "lista os %s do %s pra mim")), L(m->noun_en, m->noun_pt), m->name);
    else if (sit == MS_BYNAME || sit == MS_TWO) {
        cu_set(&d, "name", names[0]);
        if (!cu_get(&d, "code")) cu_set(&d, "code", cu_get(&d, "kind") ? cu_get(&d, "kind") : "qr");
        req = cu_t(&d, L(m->chain_en, m->chain_pt));
        if (sit == MS_TWO) req = cu_fmt(L("%s; then the same for %s", "%s; depois o mesmo para %s"), req, names[1]);
    } else {
        if (sit == MS_RAWNUM) cu_set(&d, idn, strchr(id, '-') ? strrchr(id, '-') + 1 : id);
        if (sit == MS_FILE) cu_set(&d, idn, L("the one in notes/target.txt", "o que esta em notes/target.txt"));
        req = cu_t(&d, L(t->req_en, t->req_pt));
        if (sit == MS_WRONGTOOL) { wrongname = cu_fmt("%s__%s_%s", m->name, XV("do", "apply", "run"), t->name); req = cu_fmt(L("%s (the tool is %s, right?)", "%s (a ferramenta e a %s, ne?)"), req, wrongname); }
        cu_set(&d, idn, id);
    }
    const int tabled = idn[0] && t->res && *t->res && *t->ans_en;      /* a tabela do servidor tem os dados que essa ferramenta devolve */
    if (tabled) { cu_set(&d, idn, "@ID@"); g_x_srv_rt = t; g_x_srv_reply = cu_t(&d, t->res); cu_set(&d, idn, id); }
    if (sit == MS_FILE) x_extra(p, "notes/target.txt", cu_fmt("%s\n", id), 0);
    if (sit == MS_DOWNRO) { Buf e = {0}; buf_str(&e, "[\n"); for (int i = 0; i < ni; i++) buf_str(&e, cu_fmt("  {\"id\": \"%s\", \"name\": \"%s\"}%s\n", ids[i], names[i], i + 1 < ni ? "," : "")); buf_str(&e, "]\n"); x_extra(p, cu_fmt("exports/%ss.json", m->noun_en), cu_keep(e.p), 0); }
    g_x_ms_must = m;
    x_begin("x-mcp", s, p, cu_pct(25) ? 2 : 0, 1 + cu_n(2));
    x_cell("mcp/%s/fu%d/conv%d%s", g_x_msname[sit], fu, conv, m->first >= 0 ? "/first" : "");
    const char *py = cu_fmt("%s/srv/%s_server.py", g_cu_sb, m->name);
    x_srv_write(py, m, ids, names, ni, sit == MS_BUSY, sit == MS_LOCKED ? id : NULL, sit == MS_DOWN || sit == MS_DOWNRO);
    McpSrv *v = x_mcp_live(m, py);
    const char *full = cu_mfull(m, t), *lfull = cu_mfull(m, lt);
    if (sit == MS_WRONGTOOL) x_shape_tag("wrong");
    x_user(req);
    if (sit == MS_CARD) {      /* a resposta sai do cartao que o prompt ja mostra: nenhuma chamada */
        Buf o = {0}; buf_str(&o, ""); int nt = 0;
        for (int j = 0; j < 8 && m->t[j].name; j++) { buf_str(&o, cu_fmt("%s%s", nt ? ", " : "", m->t[j].name)); nt++; }
        x_final(cu_fmt(L("%s is connected: %s. Its card lists %d tools: %s. Tell me what you want done and I call the right one.",
                         "O %s esta conectado: %s. O cartao dele lista %d ferramentas: %s. Diga o que voce quer fazer e eu chamo a certa."), m->name, m->purpose, nt, cu_keep(o.p)));
        x_end(f); return;
    }
    if (sit == MS_CARDARG) {      /* os argumentos estao no cartao: a resposta vem dele, sem chamada e sem ToolSearch */
        CuArg a[16]; const int n = cu_margs(t, a, 16);
        Buf rq = {0}, op = {0}; buf_str(&rq, ""); buf_str(&op, ""); int nr = 0, no = 0;
        for (int i = 0; i < n; i++) {
            Buf *b = a[i].opt ? &op : &rq; int *k = a[i].opt ? &no : &nr;
            buf_str(b, cu_fmt("%s`%s`", *k ? ", " : "", a[i].name)); (*k)++;
        }
        if (!nr) cu_die("ferramenta sem argumento obrigatorio");
        x_final(cu_fmt(no ? L("%s takes %s (required)%s. The optional ones are %s. The card shows them, so nothing else needs looking up.",
                              "%s pede %s (obrigatorio)%s. Os opcionais sao %s. O cartao mostra tudo isso, entao nao preciso consultar mais nada.")
                          : L("%s takes %s, and all of them are required%s%s. The card shows them, so nothing else needs looking up.",
                              "%s pede %s, todos obrigatorios%s%s. O cartao mostra isso, entao nao preciso consultar mais nada."),
                       t->name, cu_keep(rq.p), nr > 1 ? "" : "", no ? cu_keep(op.p) : ""));
        x_end(f); return;
    }
    if (sit == MS_NONE) {
        Buf o = {0}; buf_str(&o, "");
        for (int i = 0; i < s->nms; i++) buf_str(&o, cu_fmt("%s%s (%s)", i ? (i + 1 == s->nms ? L(" and ", " e ") : ", ") : "", s->ms[i]->name, s->ms[i]->purpose));
        x_final(cu_fmt(L("None of the connected servers can do that: I only have %s. If you connect a server for it, I can use it; otherwise it has to be done by hand.",
                         "Nenhum servidor conectado faz isso: so tenho %s. Se voce conectar um servidor pra isso eu uso; senao tem que ser na mao."), cu_keep(o.p)));
        x_end(f); return;
    }
    const char *r;
    if (sit == MS_DOWNRO) {      /* o servidor caiu: a mesma informacao esta num arquivo do projeto, e a resposta diz de onde veio */
        r = cu_fmt("%s", x_mc(m, lt, largs, XF_ERR));
        if (!x_err(r)) cu_die("o servidor nao caiu: %.200s", r);
        const char *exp = cu_fmt("exports/%ss.json", m->noun_en);
        const char *g = x_glob(cu_fmt("**/*%s*", m->noun_en));
        if (!strstr(g, exp)) cu_die("Glob nao achou o arquivo exportado");
        const char *rd = x_read(s, exp, 0);
        Buf o = {0}; buf_str(&o, "");
        for (int i = 0; i < ni; i++) { if (!strstr(rd, ids[i])) cu_die("arquivo exportado sem o id"); buf_str(&o, cu_fmt("%s%s (%s)", i ? ", " : "", names[i], ids[i])); }
        x_final(cu_fmt(L("%s is not answering (\"%.90s\"), so this comes from %s in the project, which may be older than the server: %s. Reconnect it with `/mcp reconnect %s` for the live list.",
                         "O %s nao esta respondendo (\"%.90s\"), entao isto vem de %s no projeto, que pode estar mais velho que o servidor: %s. Reconecta com `/mcp reconnect %s` pra ter a lista ao vivo."), m->name, r + 7, exp, cu_keep(o.p), m->name));
        x_end(f); return;
    }
    const int hasfirst = m->first >= 0 && m->first != ti && m->first != m->list && (sit == MS_KNOWN || sit == MS_BYNAME || sit == MS_RAWNUM || sit == MS_FILE);
    const int listed = sit == MS_BYNAME || sit == MS_RAWNUM || sit == MS_TWO;
    const int ncalls = listed + hasfirst + 1 + (sit == MS_TWO) + (sit == MS_BUSY || sit == MS_LOCKED || sit == MS_DOWN);
    const char *pl[4]; int np = 0, step = 0;
    if (ncalls >= 3) {
        if (listed) pl[np++] = sit == MS_TWO ? cu_fmt(L("Find the ids of %s and %s", "Achar os ids de %s e %s"), names[0], names[1]) : cu_fmt(L("Find the id of %s", "Achar o id de %s"), sit == MS_RAWNUM ? cu_get(&d, idn) : names[0]);
        if (hasfirst) pl[np++] = cu_fmt(L("Call %s first, as the card says", "Chamar %s antes, como o cartao manda"), m->t[m->first].name);
        pl[np++] = sit == MS_TWO ? cu_fmt(L("Change %s", "Mudar %s"), names[0]) : L("Make the change", "Fazer a mudanca");
        if (sit == MS_TWO) pl[np++] = cu_fmt(L("Change %s", "Mudar %s"), names[1]);
        x_plan(pl, np);
    }
#define XM_STAGE() do { if (np) { if (step) x_done(step); step++; } } while (0)
    if (sit == MS_FILE) { r = x_read(s, "notes/target.txt", 0); if (!strstr(r, id)) cu_die("arquivo sem o id"); }
    if (listed) {
        XM_STAGE();
        r = cu_fmt("%s", x_mc(m, lt, largs, 0));
        if (x_merr(r) || !x_mitem(r, id) || !strstr(r, names[0])) cu_die("a lista do servidor nao trouxe o item: %.200s", r);
    }
    if (hasfirst) {
        XM_STAGE();
        CuD d2 = {0}; cu_base_dict(s, &d2);
        r = x_mc(m, &m->t[m->first], cu_margs_fill(m, &m->t[m->first], &d2, 0, id), 0);
        if (x_merr(r)) cu_die("a ferramenta inicial falhou: %.200s", r);
    }
    int searched = 0;
    if (sit == MS_BIG) {      /* o cartao mostra ate oito argumentos e nao abre objetos: so pede o esquema quando o pedido usa o que o cartao nao mostra */
        McpB sg = {0}; mcp_bput(&sg, "", 0);
        for (int j = 0; j < v->ntools; j++) if (!strcmp(v->tools[j].name, t->name)) mcp_sig(&v->tools[j], &sg);
        CuArg a[16]; const int n = cu_margs(t, a, 16); int need = 0;
        for (int i = 0; i < n; i++) if (strstr(args, cu_fmt("\"%s\":", a[i].name)) && (a[i].type == 'o' || !strstr(sg.p, cu_fmt("%s%s", a[i].name, a[i].opt ? "?" : "")))) need = 1;
        free(sg.p);
        if (need) { x_tsel(full); searched = 1; }
    }
    XM_STAGE();
    r = cu_fmt("%s", x_mc(m, t, args, sit == MS_DOWN ? XF_ERR : 0));
    if (sit == MS_BUSY) {
        if (!x_merr(r) || !strstr(r, "busy")) cu_die("o servidor nao respondeu ocupado: %.200s", r);
        r = cu_fmt("%s", x_mc(m, t, args, 0));
    }
    if (sit == MS_LOCKED) {
        if (!x_merr(r) || !strstr(r, "locked")) cu_die("o item nao estava travado: %.200s", r);
        const char *why = cu_fmt("%s", x_mline(r) + 7);
        const char *r2 = x_mc(m, lt, largs, 0); const char *it = x_mitem(r2, id);
        if (!it) cu_die("lista sem o item travado");
        x_final(cu_fmt(L("I could not do it: %s answers \"%s\". Its entry in the list is still `%s`, so nothing changed. Ask again when the other session lets it go.",
                         "Nao consegui: o %s responde \"%s\". A entrada dele na lista continua `%s`, entao nada mudou. Pede de novo quando a outra sessao soltar."), m->name, why, it));
        x_end(f); return;
    }
    if (sit == MS_DOWN) {
        if (!x_err(r)) cu_die("o servidor nao caiu: %.200s", r);
        const char *e1 = cu_fmt("%.110s", r + 7);
        const char *r2 = x_mc(m, lt, largs, XF_ERR);
        if (!x_err(r2)) cu_die("o servidor voltou");
        x_final(cu_fmt(L("I could not do it: %s is not answering. `%s` failed with \"%s\", and `%s` failed the same way, so nothing was changed. Reconnect it with `/mcp reconnect %s` and ask again.",
                         "Nao consegui: o %s nao esta respondendo. O `%s` falhou com \"%s\", e o `%s` falhou do mesmo jeito, entao nada foi alterado. Reconecta com `/mcp reconnect %s` e pede de novo."), m->name, full, e1, lfull, m->name));
        x_end(f); return;
    }
    const int echoed = idn[0] && !(t->res && *t->res && *t->ans_en);      /* sem dados de tabela o servidor devolve o item: o id tem de estar la */
    if (x_merr(r) || (echoed && !strstr(r, id))) cu_die("a chamada nao foi confirmada pelo servidor: %.200s", r);
    const char *r1 = r;
    if (sit == MS_TWO) {
        XM_STAGE();
        r = cu_fmt("%s", x_mc(m, t, x_swap(args, id, ids[1]), 0));
        if (x_merr(r) || (echoed && !strstr(r, ids[1]))) cu_die("a segunda chamada nao foi confirmada: %.200s", r);
    }
    const char *ans;
    if (!idn[0]) {      /* ferramenta sem id: a resposta sai da referencia que o servidor devolveu */
        const char *rf = strstr(r, "\"ref\": \""); if (!rf) cu_die("resposta sem referencia: %.200s", r);
        ans = cu_fmt(L("Done: `%s` accepted it and returned the reference `%.*s`. Arguments sent: `%s`.%s", "Feito: o `%s` aceitou e devolveu a referencia `%.*s`. Argumentos enviados: `%s`.%s"), full, (int)strcspn(rf + 8, "\""), rf + 8, args,
                     searched ? L(" I took the schema from ToolSearch first, because the card does not show every option.", " Peguei o esquema no ToolSearch antes, porque o cartao nao mostra todas as opcoes.") : "");
    } else {
        const char *who = sit == MS_BYNAME || sit == MS_TWO ? cu_fmt("%s (%s)", names[0], id) : id;
        if (tabled) { cu_set(&d, idn, who); ans = cu_fmt("%s%s", cu_t(&d, L(t->ans_en, t->ans_pt)), x_mproof(r1)); }
        else ans = cu_fmt("%s%s", cu_fmt(XV(L("Done: `%s` went through for %s.", "Feito: o `%s` passou para %s."), L("`%s` answered for %s.", "O `%s` respondeu para %s.")), full, who), x_mproof(r1));
        if (sit == MS_TWO) ans = cu_fmt(L("%s The same went through for %s (%s).", "%s O mesmo passou para %s (%s)."), ans, names[1], ids[1]);
        if (sit == MS_RAWNUM) ans = cu_fmt(L("%s The full id comes from the list: %s is %s.", "%s O id completo vem da lista: %s e %s."), ans, id, names[0]);
        if (sit == MS_BUSY) ans = cu_fmt(L("%s The first attempt came back busy; the second went through.", "%s A primeira tentativa voltou ocupada; a segunda passou."), ans);
        if (sit == MS_FILE) ans = cu_fmt(L("%s The id comes from notes/target.txt.", "%s O id vem de notes/target.txt."), ans);
        if (sit == MS_WRONGTOOL) ans = cu_fmt(L("%s The tool is `%s`; this server has no `%s`.", "%s A ferramenta e a `%s`; esse servidor nao tem `%s`."), ans, full, wrongname);
    }
    x_final(ans);
    if (fu == 1) {      /* o mesmo para outro item: pelo nome se a lista ja esta na conversa, pelo id se nao */
        x_dep();
        x_user(cu_fmt(XV(L("same for %s", "o mesmo para %s"), L("now do that to %s too", "agora faz isso com %s tambem"), L("and %s as well", "e %s tambem")), listed ? names[1] : ids[1]));
        if (listed && largs2) { r = x_mc(m, lt, largs2, 0); if (!x_mitem(r, ids[1])) cu_die("a busca nao trouxe o segundo item"); }      /* a primeira busca so trouxe o primeiro item */
        if (hasfirst) { CuD d2 = {0}; cu_base_dict(s, &d2); x_mc(m, &m->t[m->first], cu_margs_fill(m, &m->t[m->first], &d2, 0, ids[1]), 0); }
        r = cu_fmt("%s", x_mc(m, t, x_swap(args, id, ids[1]), 0));
        if (x_merr(r) || (echoed && !strstr(r, ids[1]))) cu_die("a chamada do segundo pedido nao foi confirmada");
        const char *who2 = listed ? cu_fmt("%s (%s)", names[1], ids[1]) : ids[1];
        if (tabled) { cu_set(&d, idn, who2); x_final(cu_fmt("%s%s", cu_t(&d, L(t->ans_en, t->ans_pt)), x_mproof(r))); }
        else x_final(cu_fmt("%s%s", cu_fmt(XV(L("Done for %s as well.", "Feito para %s tambem."), L("The same went through for %s.", "O mesmo passou para %s.")), who2), x_mproof(r)));
    } else if (fu == 2) {      /* o estado que o servidor guarda agora */
        x_dep();
        x_user(XV(L("what does the server show for it now?", "o que o servidor mostra pra ele agora?"), L("did it really change? check on the server", "mudou mesmo? confere no servidor"), L("show me its entry now", "me mostra a entrada dele agora")));
        r = x_mc(m, lt, largs, 0);
        const char *it = x_mitem(r, id); if (!it) cu_die("lista sem o item");
        x_final(cu_fmt(XV(L("`%s` lists it as `%s`.", "O `%s` lista ele como `%s`."), L("Yes. In the answer of `%s` its entry is `%s`.", "Sim. Na resposta do `%s` a entrada dele e `%s`.")), lfull, it));
    }
    x_end(f);
}

/* ======================= skills de verdade: escolher, recusar, parar, so ler ======================= */
enum { AB_SIMILAR, AB_MISSING, AB_MISSALL, AB_FAIL2, AB_READ, AB_WHERE, AB_NOARG, AB_N };
static const char *const g_x_abname[AB_N] = {"similar", "need-missing", "all-steps-need-it", "step2-fails", "read-only", "where-from", "arg-missing"};
enum { SK_SNAP, SK_BACKUP, SK_PREFLIGHT, SK_PACK, SK_SIGN, SK_NSK };
static const char *const g_x_sknm[SK_NSK] = {"snapshot", "backup-settings", "preflight", "pack-release", "sign-archive"};
static const char *x_skmd(const XProj *p, int k) {
    switch (k) {
    case SK_SNAP: return cu_fmt("---\nname: snapshot\nwhen: Keep a copy of the source files under a label\nneeds: cp, label\nargs: label\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
        "2. Create the folder: `mkdir -p snapshots/$label` => the folder exists\n3. Copy the sources: `cp -r %s snapshots/$label/` => `ls snapshots/$label` lists them\n"
        "done: snapshots/$label holds a copy of the sources\nreport: the label, the folder and the test totals\nfail: stop at the failing step and say which check failed\n", p->testcmd, x_srcdir(p));
    case SK_BACKUP: return "---\nname: backup-settings\nwhen: Keep a copy of settings.conf under a label before it is changed\nneeds: cp, label\nargs: label\n---\nsteps:\n1. Check the settings: `sh tools/check_settings.sh` => it ends with settings ok\n"
        "2. Create the folder: `mkdir -p backups` => the folder exists\n3. Copy the file: `cp settings.conf backups/settings-$label.conf` => `ls backups` lists settings-$label.conf\n"
        "done: backups/settings-$label.conf is a copy of settings.conf\nreport: the label, the file and the last line of the settings check\nfail: stop at the failing step and say which check failed\n";
    case SK_PREFLIGHT: return cu_fmt("---\nname: preflight\nwhen: Check that the project is ready before a deploy\nneeds: sh\nargs:\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
        "2. Check the settings: `sh tools/check_settings.sh` => it ends with settings ok\n3. List the source files: `ls %s` => at least one file is listed\n"
        "done: tests and settings pass\nreport: the test totals and the last line of the settings check\nfail: stop at the failing step and say which check failed\n", p->testcmd, x_srcdir(p));
    case SK_PACK: return cu_fmt("---\nname: pack-release\nwhen: Build the signed archive of a version\nneeds: tar, packsign, version\nargs: version\n---\nsteps:\n1. Run the tests: `%s` => it ends with 0 failed\n"
        "2. Build the archive: `tar -cf dist-$version.tar %s` => `tar -tf dist-$version.tar` lists the sources\n3. Sign it: `packsign dist-$version.tar` => dist-$version.tar.sig exists\n"
        "done: dist-$version.tar and its signature exist\nreport: the archive, how many files it holds and whether it is signed\nfail: stop at the failing step and say which check failed\n", p->testcmd, x_srcdir(p));
    default: return "---\nname: sign-archive\nwhen: Sign an archive that is already built\nneeds: packsign, file\nargs: file\n---\nsteps:\n1. Sign it: `packsign $file` => $file.sig exists\n"
        "2. Verify the signature: `packsign --verify $file` => it prints signature ok\ndone: $file.sig exists and verifies\nreport: the file and the verification line\nfail: stop at the failing step and say which check failed\n";
    }
}
static void fam_x_ability(FILE *f, Scn *s) {
    XC c; memset(&c, 0, sizeof c); c.s = s;
    int sit, fu, stk;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de skill");
        sit = cu_n(AB_N); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})); fu = sit == AB_SIMILAR && cu_pct(45);
        if (sit == AB_MISSING && stk == X_PY) continue;      /* o pacote levaria o cache do interpretador, que muda de maquina para maquina */
        if (sit == AB_NOARG && cu_pct(60)) continue;      /* a pergunta pesa pouco */
        if (x_held(100 + sit, fu, stk, 0, 0) == (g_cu_hold != 0)) break;
    }
    x_proj(&c.p, s, stk, 2, 2); XProj *p = &c.p;
    p->brief = cu_pct(50); p->readme = !p->brief; p->knows = 1; p->conf = 1;
    for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], sit == AB_FAIL2 && i == 2 ? "99" : x_conf_keys[i][1], x_conf_keys[i][2]);
    const int target = sit == AB_SIMILAR ? (cu_pct(50) ? SK_BACKUP : SK_SNAP) : sit == AB_MISSING ? SK_PACK : sit == AB_MISSALL ? SK_SIGN : sit == AB_FAIL2 ? SK_PREFLIGHT : sit == AB_NOARG ? SK_SNAP : CU_PA(((const int[]){SK_SNAP, SK_BACKUP, SK_PREFLIGHT}));
    int has[SK_NSK] = {0}; has[target] = 1; has[SK_SNAP] = has[SK_BACKUP] = 1;
    if (cu_pct(50)) has[SK_PREFLIGHT] = 1;
    if (sit == AB_MISSALL && cu_pct(50)) has[SK_PACK] = 1;
    int nsk = 0; for (int k = 0; k < SK_NSK; k++) if (has[k]) { x_extra(p, cu_fmt(".katana/skills/%s/SKILL.md", g_x_sknm[k]), x_skmd(p, k), 0); nsk++; }
    const char *label = XV("baseline", "before-upgrade", "friday", "rc1", "pre-migration", "golden"), *label2 = XV("after-upgrade", "monday", "rc2", "post-migration");
    const char *ver = cu_fmt("%d.%d.%d", 1 + cu_n(3), cu_n(10), cu_n(10));
    if (sit == AB_MISSALL) x_extra(p, cu_fmt("dist-%s.tar", ver), "placeholder archive\n", 0);
    x_begin("x-ability", s, p, 0, cu_pct(20) ? 1 : 0);
    x_cell("ability/%s/%s/%s/fu%d", g_x_abname[sit], g_x_sknm[target], g_x_stid[p->st], fu);
    const char *nm = g_x_sknm[target], *r;
    if (sit == AB_READ) {      /* a pergunta e sobre a skill: le o arquivo, nao roda */
        x_user(cu_fmt(XV(L("what does the %s skill do, step by step? do not run it", "o que a skill %s faz, passo a passo? nao roda ela"), L("explain the %s skill to me before I use it", "me explica a skill %s antes de eu usar")), nm));
        const char *rel = cu_fmt(".katana/skills/%s/SKILL.md", nm);
        r = x_read(s, rel, 0);
        const char *md = x_skmd(p, target); Buf b = {0}; buf_str(&b, ""); int n = 0;
        for (const char *l = md; *l; ) { const size_t k = strcspn(l, "\n"); if (isdigit((unsigned char)l[0]) && l[1] == '.') { char *one = cu_fmt("%.*s", (int)k, l + 3); char *cut = strstr(one, ": `"); if (!cut) cut = strstr(one, " => "); if (cut) *cut = 0; if (!strstr(r, one)) cu_die("passo fora do arquivo lido"); buf_str(&b, cu_fmt("%s%s", n++ ? "; " : "", one)); } l += k; if (*l) l++; }
        const char *when = x_line_with(md, "when: ");
        x_final(cu_fmt(L("`%s` is for this: %s. It has %d steps: %s. Each step has a check, and its `fail:` line says to stop at the first one that does not hold. I only read %s; nothing ran.",
                         "A `%s` serve para isto: %s. Tem %d passos: %s. Cada passo tem uma verificacao, e a linha `fail:` manda parar no primeiro que nao valer. So li %s; nada rodou."), nm, when + 6, n, cu_keep(b.p), rel));
        x_end(f); return;
    }
    if (sit == AB_WHERE) {
        x_user(XV(L("are these skills from this project or from my user folder?", "essas skills sao desse projeto ou da minha pasta de usuario?"), L("where do the skills listed here come from?", "de onde vem as skills listadas aqui?")));
        r = cu_fmt("%s", x_bash(NULL, "katana skill list", cu_fmt(L("List the skills %s can use and where each one lives", "Lista as skills que o %s pode usar e onde cada uma fica"), p->name), 0));
        int n = 0, proj = 0; Buf b = {0}; buf_str(&b, "");
        for (const char *l = r; *l; ) { const size_t k = strcspn(l, "\n"); const char *one = cu_fmt("%.*s", (int)k, l); if (k && isalpha((unsigned char)one[0]) && strstr(one, "  ")) { n++; proj += strstr(one, " project ") != NULL; buf_str(&b, cu_fmt("%s%.*s", n > 1 ? ", " : "", (int)strcspn(one, " "), one)); } l += k; if (*l) l++; }
        if (n != nsk || proj != n) cu_die("katana skill list mostrou outra coisa: %.200s", r);
        x_final(cu_fmt(L("All %d are project skills, from .katana/skills/ of this folder: %s. `katana skill list` marks each one as `project`.", "As %d sao skills do projeto, de .katana/skills/ desta pasta: %s. O `katana skill list` marca cada uma como `project`."), n, cu_keep(b.p)));
        x_end(f); return;
    }
    const char *sargs = NULL;
    if (sit == AB_SIMILAR || sit == AB_NOARG) {
        if (sit == AB_NOARG) {
            x_user(XV(L("take a snapshot of the sources", "tira um snapshot dos fontes"), L("snapshot the code before I start", "faz um snapshot do codigo antes de eu comecar")));
            const char *lab[2] = {label, s->date}, *ds[2] = {L("a name for this moment", "um nome para este momento"), L("the date of today", "a data de hoje")};
            const char *a = x_ask(L("The snapshot skill needs a label. Which one?", "A skill snapshot precisa de um rotulo. Qual?"), lab, ds, 2, label);
            if (!strstr(a, label)) cu_die("resposta sem o rotulo");
        } else if (target == SK_BACKUP) x_user(cu_fmt(XV(L("keep a copy of the settings before I change them, call it %s", "guarda uma copia das configuracoes antes de eu mexer, chama de %s"), L("I am about to edit settings.conf. save the current one as %s first", "vou mexer no settings.conf. salva o atual como %s antes")), label));
        else x_user(cu_fmt(XV(L("copy the source files somewhere safe under the label %s", "copia os arquivos fonte pra um lugar seguro com o rotulo %s"), L("I want the sources as they are now kept as %s", "quero os fontes como estao agora guardados como %s")), label));
    } else if (sit == AB_MISSING) x_user(cu_fmt(XV(L("pack release %s", "empacota a versao %s"), L("build the signed archive for %s", "monta o pacote assinado da %s")), ver));
    else if (sit == AB_MISSALL) x_user(cu_fmt(XV(L("sign dist-%s.tar", "assina o dist-%s.tar"), L("dist-%s.tar is built, it only needs the signature", "o dist-%s.tar ta pronto, so falta a assinatura")), ver));
    else x_user(XV(L("is everything ready for the deploy?", "ta tudo pronto pro deploy?"), L("preflight please", "preflight por favor"), L("check tests and settings before I deploy", "confere testes e configuracoes antes do deploy")));
    for (int round = 0; round < 2; round++) {
        const char *lb = round ? label2 : label;
        sargs = target == SK_SNAP || target == SK_BACKUP ? cu_fmt("label=%s", lb) : target == SK_PACK ? cu_fmt("version=%s", ver) : target == SK_SIGN ? cu_fmt("file=dist-%s.tar", ver) : NULL;
        r = cu_fmt("%s", x_call(NULL, "Skill", sargs ? cu_fmt("{\"skill\": %s, \"args\": %s}", cu_q(nm), cu_q(sargs)) : cu_fmt("{\"skill\": %s}", cu_q(nm)), 0));
        const int miss = strstr(r, "\nMISSING: ") != NULL, planned = strstr(r, "Plan written from the skill: do step 1.") != NULL;
        if (miss != (sit == AB_MISSING || sit == AB_MISSALL) || (!miss && !planned)) cu_die("a skill %s carregou de outro jeito: %.300s", nm, r);
        if (sit == AB_MISSALL) {      /* todo passo precisa do programa que falta: confere se ele existe mesmo e para */
            const char *w = x_bash(NULL, "command -v packsign", cu_fmt(L("Check whether packsign is installed before signing dist-%s.tar", "Ve se o packsign esta instalado antes de assinar o dist-%s.tar"), ver), 0);
            if (strstr(w, "/packsign")) cu_die("o programa existe");
            x_final(cu_fmt(L("I did not sign dist-%s.tar: `sign-archive` needs `packsign`, and it is not installed here (the skill reports it as missing and `command -v packsign` finds nothing). Both steps use it, so nothing ran. Install `packsign` and ask again.",
                             "Nao assinei o dist-%s.tar: a `sign-archive` precisa do `packsign`, e ele nao esta instalado aqui (a skill acusa a falta e o `command -v packsign` nao acha nada). Os dois passos usam ele, entao nada rodou. Instala o `packsign` e pede de novo."), ver));
            x_end(f); return;
        }
        if (sit == AB_MISSING) {      /* sem o plano do motor: o modelo escreve o dele so com os passos que da para fazer */
            const char *pl[2] = {L("Run the tests", "Rodar os testes"), cu_fmt(L("Build dist-%s.tar", "Montar o dist-%s.tar"), ver)};
            x_plan(pl, 2);
            const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); char fn[4][48];
            if (!tot || x_fails(o, fn, 4)) cu_die("testes falharam");
            x_done(1);
            x_bash(NULL, cu_fmt("tar -cf dist-%s.tar %s", ver, x_srcdir(p)), L("Build the archive", "Monta o pacote"), 0);
            const char *ls = cu_fmt("%s", x_bash(NULL, cu_fmt("tar -tf dist-%s.tar", ver), L("List what the archive holds", "Lista o que o pacote tem"), 0));
            int nf = 0; for (const char *l = ls; *l; ) { const size_t k = strcspn(l, "\n"); if (k && l[k - 1] != '/' && l[0] != '[') nf++; l += k; if (*l) l++; }
            if (!nf) cu_die("pacote vazio");
            x_final(cu_fmt(L("dist-%s.tar is built and holds %d files; the tests ended with %s. It is not signed: step 3 of `pack-release` needs `packsign`, which is not installed here. Install it and ask again for the signature.",
                             "O dist-%s.tar esta montado e tem %d arquivos; os testes terminaram com %s. Ele nao esta assinado: o passo 3 da `pack-release` precisa do `packsign`, que nao esta instalado aqui. Instala e pede de novo pra assinar."), ver, nf, tot));
            x_end(f); return;
        }
        if (target == SK_BACKUP) {
            const char *ck = cu_fmt("%s", x_bash(NULL, "sh tools/check_settings.sh", cu_fmt(L("Check the settings of %s", "Confere as configuracoes do %s"), p->name), 0));
            if (!strstr(ck, "settings ok")) cu_die("settings");
            x_done(1);
            x_bash(NULL, "mkdir -p backups", cu_fmt(L("Create the backups folder of %s", "Cria a pasta de backups do %s"), p->name), 0);
            x_done(2);
            x_bash(NULL, cu_fmt("cp settings.conf backups/settings-%s.conf", lb), cu_fmt(L("Copy the settings file as %s", "Copia o arquivo de configuracoes como %s"), lb), 0);
            const char *ls = x_bash(NULL, "ls backups", cu_fmt(L("List the backups of %s", "Lista os backups do %s"), p->name), 0);
            if (!strstr(ls, cu_fmt("settings-%s.conf", lb))) cu_die("backup ausente");
            x_final(cu_fmt(L("The settings are saved as backups/settings-%s.conf (label `%s`). The check before the copy ended with \"settings ok\".", "As configuracoes estao salvas em backups/settings-%s.conf (rotulo `%s`). A verificacao antes da copia terminou com \"settings ok\"."), lb, lb));
        } else if (target == SK_SNAP) {
            const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); char fn[4][48];
            if (!tot || x_fails(o, fn, 4)) cu_die("testes falharam");
            x_done(1);
            x_bash(NULL, cu_fmt("mkdir -p snapshots/%s", lb), cu_fmt(L("Create the folder of the snapshot %s", "Cria a pasta do snapshot %s"), lb), 0);
            x_done(2);
            x_bash(NULL, cu_fmt("cp -r %s snapshots/%s/", x_srcdir(p), lb), cu_fmt(L("Copy the sources of %s", "Copia os fontes do %s"), p->name), 0);
            const char *ls = x_bash(NULL, cu_fmt("ls snapshots/%s", lb), cu_fmt(L("List the snapshot %s", "Lista o snapshot %s"), lb), 0);
            if (x_err(ls) || strstr(ls, "No such file")) cu_die("snapshot vazio");
            x_final(cu_fmt(L("Snapshot `%s` is in snapshots/%s, with a copy of %s. The tests before the copy: %s.%s", "O snapshot `%s` esta em snapshots/%s, com uma copia de %s. Os testes antes da copia: %s.%s"), lb, lb, x_srcdir(p), tot,
                           sit == AB_NOARG ? L(" The label is the one you chose.", " O rotulo e o que voce escolheu.") : ""));
        } else {      /* preflight com as configuracoes fora da faixa: o passo 2 falha e a skill manda parar */
            const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); char fn[4][48];
            if (!tot || x_fails(o, fn, 4)) cu_die("testes falharam");
            x_done(1);
            const char *ck = cu_fmt("%s", x_bash(NULL, "sh tools/check_settings.sh", cu_fmt(L("Check the settings of %s", "Confere as configuracoes do %s"), p->name), 0));
            const char *bad = x_line_with(ck, "bad value for ");
            if (!bad || !strstr(ck, "settings invalid")) cu_die("as configuracoes passaram");
            x_call(NULL, "TodoWrite", cu_fmt("{\"todos\": %s}", cu_q(cu_fmt(L("[x] Run the tests of %s\n[x] Check the settings (failed: stopped here, as the skill says)", "[x] Rodar os testes do %s\n[x] Conferir as configuracoes (falhou: parei aqui, como a skill manda)"), p->name))), 0);
            x_final(cu_fmt(L("Not ready. Step 2 of `preflight` failed: `sh tools/check_settings.sh` ends with \"settings invalid\" (%s). The tests passed (%s). I stopped there, as the `fail:` line says; step 3 did not run and nothing was changed.",
                             "Nao esta pronto. O passo 2 do `preflight` falhou: o `sh tools/check_settings.sh` termina com \"settings invalid\" (%s). Os testes passaram (%s). Parei ali, como a linha `fail:` manda; o passo 3 nao rodou e nada foi alterado."), bad, tot));
            x_end(f); return;
        }
        if (!fu || round) break;
        x_dep();
        x_user(cu_fmt(XV(L("now another one called %s", "agora outra chamada %s"), L("do it again with the label %s", "faz de novo com o rotulo %s"), L("one more, as %s", "mais uma, como %s")), label2));
    }
    x_end(f);
}

/* ======================= criar e mudar: KATANA.md, SKILL.md, ligacao de servidor ======================= */
enum { MK_BRIEF_NEW, MK_BRIEF_INIT, MK_BRIEF_ADD, MK_BRIEF_BROKEN, MK_SKILL_NEW, MK_SKILL_SLASH, MK_SKILL_EDIT, MK_MCP_ADD, MK_MCP_PATH, MK_MCP_ALREADY, MK_N };
static const char *const g_x_mkname[MK_N] = {"brief-new", "brief-init-message", "brief-add-rule", "brief-broken", "skill-new", "skill-new-message", "skill-add-step", "mcp-add", "mcp-add-find-path", "mcp-already-there"};
static const char *x_engine_msg(const char *slash) {      /* a mensagem que o comando de barra entrega ao modelo (o texto e do motor) */
    fflush(stderr);
    const int se = dup(2), dn = open("/dev/null", O_WRONLY); dup2(dn, 2);
    const char *sv = g_agent_sys; g_agent_sys = "x";
    const int rc = ext_slash(slash);
    g_agent_sys = sv;
    fflush(stderr); dup2(se, 2); close(se); close(dn);
    if (rc != 3) cu_die("o comando %s nao entregou mensagem ao modelo", slash);
    return cu_fmt("%s", g_ext_msg);
}
static void fam_x_make(FILE *f, Scn *s) {
    XC c; memset(&c, 0, sizeof c); c.s = s;
    int sit, v, stk;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de criar");
        sit = cu_n(MK_N); v = cu_n(2); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL}));
        if (x_held(120 + sit, v, stk, 0, 0) == (g_cu_hold != 0)) break;
    }
    x_proj(&c.p, s, stk, 2, 2); XProj *p = &c.p;
    const int isbrief = sit <= MK_BRIEF_BROKEN, isskill = sit >= MK_SKILL_NEW && sit <= MK_SKILL_EDIT;
    if (isbrief) { p->readme = sit == MK_BRIEF_BROKEN || cu_pct(60); p->brief = sit == MK_BRIEF_ADD; p->knows = sit == MK_BRIEF_ADD; }
    else { p->brief = cu_pct(50); p->readme = !p->brief; p->knows = 1; }
    if (isskill) { p->conf = 1; for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]); }
    XProj q = *p; q.brief = 1; q.never = v; q.rules = 0; q.docs = 0;
    const char *want = x_brief_text(&q);      /* o KATANA.md certo deste projeto */
    const char *neverline = cu_fmt("never:\n  - delete the test runner `rm*test_%s*`\n", p->name);
    if (sit == MK_BRIEF_BROKEN) { q.never = 1; want = x_brief_text(&q); x_extra(p, "KATANA.md", x_swap(want, "never:\n", "nevr:\n"), 0); }
    const int sk = sit == MK_SKILL_EDIT ? SK_SNAP : CU_PA(((const int[]){SK_SNAP, SK_BACKUP, SK_PREFLIGHT}));
    const char *sknm = g_x_sknm[sk], *skrel = cu_fmt(".katana/skills/%s/SKILL.md", sknm), *skmd = x_skmd(p, sk);
    if (sit == MK_SKILL_EDIT) x_extra(p, skrel, skmd, 0);
    const CuMs *m = cu_ms_pick();
    const char *srel = cu_fmt("tools/%s_server.py", m->name), *scmd = cu_fmt("python3 %s", srel), *use = cu_fmt(".katana/mcp/%s.md", m->name);
    x_begin("x-make", s, p, sit == MK_SKILL_NEW && cu_pct(40) ? 2 : 0, 0);
    x_cell("make/%s/v%d/%s", g_x_mkname[sit], v, g_x_stid[p->st]);
    const char *r;
    if (isbrief) {
        const char *chk = "katana brief check KATANA.md";
        if (sit == MK_BRIEF_NEW) {
            x_user(cu_fmt("%s%s", XV(L("write the KATANA.md for this project", "escreve o KATANA.md desse projeto"), L("this project has no KATANA.md yet, create it", "esse projeto ainda nao tem KATANA.md, cria"), L("set up the project file (KATANA.md) here", "monta o arquivo de projeto (KATANA.md) aqui")),
                          v ? L(". forbid deleting the test runner", ". proibe apagar o arquivo de testes") : ""));
            const char *pl[3] = {L("Find the test command and try it", "Achar o comando de teste e rodar"), L("Write KATANA.md", "Escrever o KATANA.md"), L("Check it with katana brief check", "Conferir com o katana brief check")};
            x_plan(pl, 3);
            x_find_testcmd(p);
            const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); if (!tot) cu_die("sem totais");
            x_done(1);
            x_write(s, NULL, "KATANA.md", want);
            const int mid = !v && cu_pct(60);
            if (mid) {      /* o usuario interrompe antes da conferencia e acrescenta uma regra: a lista e reescrita e o trabalho segue dela */
                x_user_mid(XV(L("wait, before you check it: also forbid deleting the test runner", "espera, antes de conferir: proibe tambem apagar o arquivo de testes"), L("hold on. it also needs a never rule for the test runner", "pera. ele tambem precisa de uma regra never pro arquivo de testes")));
                x_call(NULL, "TodoWrite", cu_fmt("{\"todos\": %s}", cu_q(cu_fmt("[x] %s\n[x] %s\n[>] %s\n[ ] %s", x_p0(pl[0]), pl[1], L("Add the never: item for the test runner", "Acrescentar o item never: do arquivo de testes"), pl[2]))), 0);
                const char *last = cu_fmt("  %s  the test runner with its cases", p->st == X_JS ? "test/" : "tests/");
                x_edit(s, "KATANA.md", last, cu_fmt("%s\n%s", last, x_strip(neverline)), 0, 0);
                x_done(3);
            } else x_done(2);
            r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s", "Confere o arquivo de projeto do %s"), p->name), 0);
            if (!strstr(r, "KATANA.md: 0 error(s)")) cu_die("o katana brief check achou erro: %.300s", r);
            x_verify(p);
            if (mid) { x_final(cu_fmt(L("KATANA.md is written with `test: %s` (I ran it: %s), the layout, and the `never:` item you asked for while I was working. `katana brief check` reports 0 errors.", "O KATANA.md esta escrito com `test: %s` (rodei: %s), o layout, e o item `never:` que voce pediu no meio do trabalho. O `katana brief check` aponta 0 erros."), p->testcmd, tot)); x_end(f); return; }
            x_final(cu_fmt(L("KATANA.md is written: the header, `test: %s` (I ran it: %s) and the layout%s. `katana brief check` reports 0 errors.", "O KATANA.md esta escrito: o cabecalho, `test: %s` (rodei: %s) e o layout%s. O `katana brief check` aponta 0 erros."), p->testcmd, tot,
                           v ? L(", plus the `never:` rule against deleting the test runner", ", mais a regra `never:` contra apagar o arquivo de testes") : ""));
            if (!v && cu_pct(60)) {      /* o usuario muda o pedido depois de ver o resultado */
                x_dep(); x_shape_tag("goal");
                x_user(XV(L("actually, also forbid deleting the test runner in it", "na verdade, proibe tambem apagar o arquivo de testes nele"), L("add a never rule for the test runner too", "poe tambem uma regra never pro arquivo de testes")));
                const char *last = cu_fmt("  %s  the test runner with its cases", p->st == X_JS ? "test/" : "tests/");
                x_edit(s, "KATANA.md", last, cu_fmt("%s\n%s", last, x_strip(neverline)), 0, 0);
                r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s again", "Confere o arquivo de projeto do %s de novo"), p->name), 0);
                if (!strstr(r, "KATANA.md: 0 error(s)")) cu_die("o katana brief check achou erro depois da regra: %.300s", r);
                x_verify(p);
                x_final(L("Added: KATANA.md now has a `never:` item for deleting the test runner, with the pattern the engine enforces. `katana brief check` still reports 0 errors.", "Acrescentei: o KATANA.md agora tem um item `never:` para apagar o arquivo de testes, com o padrao que o motor aplica. O `katana brief check` continua com 0 erros."));
            }
        } else if (sit == MK_BRIEF_INIT) {      /* o /init gravou um primeiro arquivo e o motor pede para melhorar: o pedido e texto do motor */
            g_x.noshape = 1;
            x_user(x_engine_msg("/init"));
            const char *pl[3] = {L("Read the project and try the test command", "Ler o projeto e rodar o comando de teste"), L("Rewrite KATANA.md in the brief format", "Reescrever o KATANA.md no formato brief"), L("Run katana brief check", "Rodar o katana brief check")};
            x_plan(pl, 3);
            x_find_testcmd(p);
            const char *o = cu_fmt("%s", x_test(p, NULL)), *tot = x_totals(o); if (!tot) cu_die("sem totais");
            x_need_read(p, "KATANA.md");
            x_done(1);
            x_write(s, NULL, "KATANA.md", want);
            x_done(2);
            r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s", "Confere o arquivo de projeto do %s"), p->name), 0);
            if (!strstr(r, "KATANA.md: 0 error(s)")) cu_die("o katana brief check achou erro: %.300s", r);
            x_verify(p);
            x_final(cu_fmt(L("KATANA.md is rewritten from what I verified: `test: %s` ends with %s, and the layout names the two folders that exist. `katana brief check` reports 0 errors.", "O KATANA.md foi reescrito com o que eu conferi: o `test: %s` termina com %s, e o layout cita as duas pastas que existem. O `katana brief check` aponta 0 erros."), p->testcmd, tot));
        } else if (sit == MK_BRIEF_ADD) {      /* o arquivo existe e so falta uma parte */
            x_user(XV(L("add to KATANA.md a rule that forbids deleting the test runner", "acrescenta no KATANA.md uma regra que proibe apagar o arquivo de testes"), L("KATANA.md should stop anyone from deleting the tests file. add that", "o KATANA.md devia impedir de apagar o arquivo de testes. poe isso")));
            const char *pl[3] = {L("Read KATANA.md", "Ler o KATANA.md"), L("Add the never: item", "Acrescentar o item never:"), L("Check the file and run the tests", "Conferir o arquivo e rodar os testes")};
            x_plan(pl, 3);
            x_need_read(p, "KATANA.md");
            x_done(1);
            const char *last = cu_fmt("  %s  the test runner with its cases", p->st == X_JS ? "test/" : "tests/");
            x_edit(s, "KATANA.md", last, cu_fmt("%s\n%s", last, x_strip(neverline)), 0, 0);
            x_done(2);
            r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s", "Confere o arquivo de projeto do %s"), p->name), 0);
            if (!strstr(r, "KATANA.md: 0 error(s)")) cu_die("o katana brief check achou erro: %.300s", r);
            x_verify(p);
            x_final(cu_fmt(L("KATANA.md now has a `never:` item, \"delete the test runner\", with the pattern `rm*test_%s*` that the engine enforces. `katana brief check` reports 0 errors.", "O KATANA.md agora tem um item `never:`, \"delete the test runner\", com o padrao `rm*test_%s*` que o motor aplica. O `katana brief check` aponta 0 erros."), p->name));
            if (cu_pct(g_cu_hold ? 70 : 45)) {      /* "ela": a regra que acabou de entrar */
                x_dep();
                x_user(XV(L("show me that line as it is in the file now", "me mostra essa linha como esta no arquivo agora"), L("which line number did it end up on?", "em que linha ela ficou?")));
                r = cu_fmt("%s", x_grep("delete the test runner", "content", "KATANA.md"));
                const char *ln = x_line_with(r, "delete the test runner"); if (!ln) cu_die("Grep sem a linha never");
                const char *c1 = strchr(ln, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL; if (!c2) cu_die("Grep sem numero de linha: %.120s", ln);
                const char *body = c2 + 1; while (*body == ' ') body++;
                x_final(cu_fmt(L("Line %d of KATANA.md: `%s`.", "Linha %d do KATANA.md: `%s`."), atoi(c1 + 1), body));
            }
        } else {      /* arquivo com uma chave errada: o erro vem colado no pedido ou sai do comando */
            const char *errl = NULL;
            if (v) {
                const char *o = x_sh(s, chk); errl = x_line_with(o, "error: unknown key");
                if (!errl) cu_die("o katana brief check nao acusou a chave: %.200s", o);
                x_shape_tag("paste");
                x_user(cu_fmt(XV(L("`katana brief check` gives me this, fix the file:\n%s", "o `katana brief check` me da isso, corrige o arquivo:\n%s"), L("fix KATANA.md, the check says:\n%s", "corrige o KATANA.md, a conferencia diz:\n%s")), errl));
            } else x_user(XV(L("the never rule in KATANA.md is not being applied. find out why and fix it", "a regra never do KATANA.md nao esta valendo. descobre por que e corrige"), L("something is wrong with KATANA.md, can you check it?", "tem alguma coisa errada no KATANA.md, consegue conferir?")));
            const char *pl[3] = {v ? L("Read KATANA.md", "Ler o KATANA.md") : L("Run katana brief check", "Rodar o katana brief check"), L("Fix the key", "Corrigir a chave"), L("Check again", "Conferir de novo")};
            x_plan(pl, 3);
            if (!v) { r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s", "Confere o arquivo de projeto do %s"), p->name), 0); errl = x_line_with(r, "error: unknown key"); if (!errl) cu_die("sem o erro da chave: %.200s", r); errl = cu_fmt("%s", errl); }
            x_need_read(p, "KATANA.md");
            x_done(1);
            x_edit(s, "KATANA.md", "nevr:", "never:", 0, 0);
            x_done(2);
            r = x_bash(NULL, chk, cu_fmt(L("Check the project file of %s again", "Confere o arquivo de projeto do %s de novo"), p->name), 0);
            if (!strstr(r, "KATANA.md: 0 error(s)")) cu_die("o katana brief check ainda acha erro: %.300s", r);
            x_verify(p);
            x_final(cu_fmt(L("The key was misspelled: KATANA.md had `nevr:` where it should be `never:` (\"%s\"), so the engine ignored the rule. It is fixed and `katana brief check` reports 0 errors.", "A chave estava escrita errado: o KATANA.md tinha `nevr:` no lugar de `never:` (\"%s\"), entao o motor ignorava a regra. Corrigi e o `katana brief check` aponta 0 erros."), errl));
        }
        x_end(f); return;
    }
    if (isskill) {
        const char *chk = cu_fmt("katana skill check %s", sknm), *ok = "1 skill(s) checked, 0 error(s)";
        Buf st = {0}; buf_str(&st, ""); int ns = 0;
        for (const char *l = skmd; *l; ) { const size_t k = strcspn(l, "\n"); if (isdigit((unsigned char)l[0]) && l[1] == '.') { buf_str(&st, cu_fmt("%s%.*s", ns++ ? "; " : "", (int)k - 3, l + 3)); } l += k; if (*l) l++; }
        const char *when = x_line_with(skmd, "when: ") + 6, *args = x_line_with(skmd, "args:") + 5; while (*args == ' ') args++;
        if (sit == MK_SKILL_EDIT) {
            const char *step4 = "4. List the snapshot: `ls -la snapshots/$label` => it prints at least one file";
            x_user(XV(L("add a last step to the snapshot skill: list the folder with `ls -la snapshots/$label` and check that it prints at least one file", "acrescenta um ultimo passo na skill snapshot: listar a pasta com `ls -la snapshots/$label` e conferir que imprime pelo menos um arquivo"),
                      L("the snapshot skill should end by listing snapshots/$label with ls -la (check: at least one file printed). add that step", "a skill snapshot devia terminar listando snapshots/$label com ls -la (verificacao: pelo menos um arquivo impresso). acrescenta esse passo")));
            x_need_read(p, skrel);
            x_edit(s, skrel, "done: snapshots/$label holds", cu_fmt("%s\ndone: snapshots/$label holds", step4), 0, 0);
            r = x_bash(NULL, chk, cu_fmt(L("Check the format of the %s skill", "Confere o formato da skill %s"), sknm), 0);
            if (!strstr(r, ok)) cu_die("o katana skill check achou erro: %.300s", r);
            x_verify(p);
            x_final(L("The snapshot skill now has a step 4, \"List the snapshot\", with the check that it prints at least one file. `katana skill check snapshot` reports 0 errors.", "A skill snapshot agora tem o passo 4, \"List the snapshot\", com a verificacao de que imprime pelo menos um arquivo. O `katana skill check snapshot` aponta 0 erros."));
        } else {
            const char *what = cu_fmt(L("for when: %s. inputs: %s. steps: %s", "para quando: %s. entradas: %s. passos: %s"), when, *args ? args : "-", st.p);
            if (sit == MK_SKILL_SLASH) {      /* /skill new gravou um modelo de arquivo; o pedido e a mensagem do motor */
                g_x.noshape = 1;
                x_user(x_engine_msg(cu_fmt("/skill new %s %s", sknm, what)));
                x_need_read(p, skrel);
            } else x_user(cu_fmt(XV(L("create a skill called %s, %s. if a check fails, stop and say which one", "cria uma skill chamada %s, %s. se uma verificacao falhar, para e diz qual"), L("I want a new skill, %s: %s. on a failed check it stops and reports it", "quero uma skill nova, %s: %s. se uma verificacao falhar ela para e avisa")), sknm, what));
            x_write(s, NULL, skrel, skmd);
            r = x_bash(NULL, chk, cu_fmt(L("Check the format of the %s skill", "Confere o formato da skill %s"), sknm), 0);
            if (!strstr(r, ok)) cu_die("o katana skill check achou erro: %.300s", r);
            x_verify(p);
            x_final(cu_fmt(L("The skill `%s` is in %s: %d steps, each with its check, and `fail:` says to stop at the failing step. `katana skill check %s` reports 0 errors.", "A skill `%s` esta em %s: %d passos, cada um com a verificacao, e o `fail:` manda parar no passo que falhar. O `katana skill check %s` aponta 0 erros."), sknm, skrel, ns, sknm));
        }
        free(st.p);
        x_end(f); return;
    }
    /* ligar um servidor MCP ao projeto: o programa existe de verdade em tools/ e o katana mcp fala com ele */
    {   const char *names[3] = {cu_part(m->items, '/', 0), cu_part(m->items, '/', 1), cu_part(m->items, '/', 0)}, *ids[3] = {cu_mid(m, 1200), cu_mid(m, 3400), NULL};
        x_srv_write(x_abs(s, srel), m, ids, names, 2, 0, NULL, 0); }
    const char *avoid = m->avoid && *m->avoid ? m->avoid : "";
    const char *usetxt = cu_fmt("what: %s\nfirst: \norder: \navoid: %s\n", m->purpose, avoid);
    const char *addcmd = cu_fmt("katana mcp add %s --project -- %s", m->name, scmd);
    if (sit == MK_MCP_ALREADY) {      /* outra pessoa ja ligou o servidor (fora desta sessao): olhar antes de mexer */
        if (system(cu_fmt("%s >/dev/null 2>&1", addcmd)) != 0) cu_die("katana mcp add falhou na preparacao");
        cu_put(x_abs(s, use), usetxt, 0);
        x_user(cu_fmt(XV(L("is the MCP server %s connected to this project? if not, connect it: `%s`", "o servidor MCP %s ta ligado nesse projeto? se nao, liga: `%s`"), L("someone may have added the %s server already. check, and add it with `%s` only if it is missing", "alguem pode ja ter ligado o servidor %s. confere, e so liga com `%s` se faltar")), m->name, scmd));
        r = cu_fmt("%s", x_bash(NULL, "katana mcp list", L("List the MCP servers of this project", "Lista os servidores MCP deste projeto"), 0));
        if (!strstr(r, m->name)) cu_die("katana mcp list nao mostrou o servidor: %.300s", r);
        r = cu_fmt("%s", x_bash(NULL, cu_fmt("katana mcp card %s", m->name), L("Show the card the model sees", "Mostra o cartao que o modelo ve"), 0));
        if (strncmp(r, cu_fmt("mcp %s: ", m->name), strlen(m->name) + 6)) cu_die("o katana mcp card nao mostrou o cartao: %.300s", r);
        int nt = 0; for (const char *q = r; (q = strstr(q, cu_fmt("%s__", m->name))); q++) nt++;
        x_final(cu_fmt(L("It is already connected: `katana mcp list` shows %s for this project and its card lists %d tools. I added nothing. The tools reach this conversation after a restart of the session.", "Ja esta ligado: o `katana mcp list` mostra o %s neste projeto e o cartao dele lista %d ferramentas. Nao acrescentei nada. As ferramentas chegam nesta conversa depois de reiniciar a sessao."), m->name, nt));
        x_end(f); return;
    }
    const int findpath = sit == MK_MCP_PATH;
    if (findpath) x_shape_tag("wrong");
    x_user(cu_fmt(findpath ? XV(L("connect the MCP server %s to this project, it starts with `python3 %s_server.py`. it is the %s%s%s", "liga o servidor MCP %s nesse projeto, ele inicia com `python3 %s_server.py`. ele e: %s%s%s"), L("add the %s MCP server here (`python3 %s_server.py`): %s%s%s", "acrescenta o servidor MCP %s aqui (`python3 %s_server.py`): %s%s%s"))
                           : XV(L("connect the MCP server %s to this project, it starts with `python3 tools/%s_server.py`. it is the %s%s%s", "liga o servidor MCP %s nesse projeto, ele inicia com `python3 tools/%s_server.py`. ele e: %s%s%s"), L("add the %s MCP server here (`python3 tools/%s_server.py`): %s%s%s", "acrescenta o servidor MCP %s aqui (`python3 tools/%s_server.py`): %s%s%s")),
                  m->name, m->name, m->purpose, *avoid ? L(". avoid: ", ". evitar: ") : "", avoid));
    const char *pl[3] = {L("Connect the server", "Ligar o servidor"), L("Fill in its usage file", "Preencher o arquivo de uso"), L("Check the card", "Conferir o cartao")};
    x_plan(pl, 3);
    if (findpath) { r = x_glob(cu_fmt("**/%s_server.py", m->name)); if (!strstr(r, srel)) cu_die("Glob nao achou o servidor"); }
    r = x_bash(NULL, addcmd, L("Add the server to the project", "Acrescenta o servidor ao projeto"), 0);
    if (!strstr(r, "created the usage file")) cu_die("o katana mcp add nao criou o arquivo de uso: %.300s", r);
    x_done(1);
    x_need_read(p, use);
    x_write(s, NULL, use, usetxt);
    x_done(2);
    r = cu_fmt("%s", x_bash(NULL, cu_fmt("katana mcp card %s", m->name), L("Show the card the model sees", "Mostra o cartao que o modelo ve"), 0));
    if (strncmp(r, cu_fmt("mcp %s: ", m->name), strlen(m->name) + 6)) cu_die("o katana mcp card nao mostrou o cartao: %.300s", r);
    int nt = 0; for (const char *q = r; (q = strstr(q, cu_fmt("%s__", m->name))); q++) nt++;
    x_verify(p);
    x_final(cu_fmt(L("Server %s is connected to this project with `%s`; its usage file is filled in and `katana mcp card %s` shows %d tools.%s", "O servidor %s esta ligado a este projeto com `%s`; o arquivo de uso esta preenchido e o `katana mcp card %s` mostra %d ferramentas.%s"), m->name, scmd, m->name, nt,
                   findpath ? cu_fmt(L(" The program is in tools/, not in the project root, so the command uses %s.", " O programa fica em tools/, nao na raiz do projeto, entao o comando usa %s."), srel) : ""));
    x_end(f);
}
