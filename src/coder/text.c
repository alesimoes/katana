/* ---------- coder/text: tokenizador e normalizador morfologico proprio (pt + en), sem bibliotecas e sem alocar por token ----------
 * Usado para indexar codigo e para interpretar perguntas em portugues e ingles, inclusive misturadas.
 *   coder_split_ident  divide um identificador (CamelCase, snake_case, kebab-case, dotted.names, siglas e digitos)
 *   coder_norm_word    normaliza UMA palavra (minuscula, sem acento) por regras de sufixo + vocabulario controlado
 *   coder_tokens       texto -> tokens {normalizado, original, posicao de byte, campo, idioma, tipo, stop}
 * Idea: nao e um lematizador. A forma normalizada e uma RAIZ CANONICA (nem sempre uma palavra de dicionario) escolhida para que
 * as formas de uma familia convirjam: pt atualizando/atualizado/atualizacoes/atualizar/atualizacao -> atualizar (infinitivo);
 * en update/updated/updating/updates -> updat. A forma original continua em CoderTok.orig; o identificador inteiro e um termo a parte (kind 1).
 * Dados (listas simples no fim do arquivo): stopwords, palavras de pedido, verbos pt por classe, irregulares, excecoes, abreviacoes.
 * Abreviacoes: tabela interna minima + arquivo opcional (KATANA_CODER_KB ou data/coder/kb/abbreviations.kb), uma por linha
 * "abbr expansao[,expansao2]" (ou abbr=expansao), # comentario; o arquivo substitui entradas internas de mesma chave.
 * Convencoes: um identificador com varias partes produz o termo inteiro (norm = partes minusculas unidas por _) e uma
 * entrada por parte (letras isoladas nao viram termo; digitos sim); abreviacao conhecida gera tambem o termo expandido (kind 2).
 * Flag stop (so em palavras soltas, nao em partes de identificador): 1 palavra funcional, 2 palavra de pedido sem valor de busca.
 * O que as regras NAO cobrem (limites conhecidos):
 *   - pt: genero (menino/menina, atualizada so via particípio), -ao irregular (-sao/-ssao so por tabela), derivados em -agem/-ncia/-dade/-vel,
 *     verbos fora das listas de raizes ou sem sufixo caracteristico (atualiza converge, 'zera' nao), subjuntivo/futuro raros, pronomes clíticos,
 *     verbos -er/-ir desconhecidos no participio ('partido' -> 'partider'), plural -ses (meses), -inho/-inha de palavras fora da lista de excecoes.
 *   - en: o canonico e um radical ('updat', 'valid', 'impl'), irregulares so os da tabela, -ion/-ment/-er reduzem demais algumas palavras
 *     (version~vers, header~head via -er quando nao esta na lista, document~docu), dobra de l/s/z/f nao desfeita (controlled != control),
 *     prefixos (un-, re-, dis-) nao tratados, composicao em palavras coladas (getuser) nao e dividida.
 *   - idioma por palavra: so listas funcionais, acentos e terminacoes; palavras de conteudo sem evidencia ficam 'unknown'
 *     (a normalizacao usa o idioma dominante do texto, ou en para identificador).
 *   - Unicode alem de Latin-1 vira separador; textos > 255 bytes sem separador sao cortados em pedacos.
 * Nao e thread-safe na primeira chamada (ct_init); depois so leitura. */
typedef enum { CT_L_UNKNOWN, CT_L_PT, CT_L_EN, CT_L_CODE, CT_L_N } CoderTLang;
#define CT_AUTO (-1)
typedef struct { char norm[48], orig[48]; int pos, field, lang, kind, stop; } CoderTok;   /* kind: 0 palavra/parte, 1 identificador inteiro, 2 expansao de abreviacao */
static const char *const g_ct_langname[] = {"unknown", "pt", "en", "code"};

/* ---- mapa de palavras (tabela hash aberta; chaves apontam para texto estatico ou para o buffer do arquivo) ---- */
typedef struct { const char *k, *v; unsigned char kl, vl; unsigned short f; } CtEnt;
typedef struct { CtEnt *e; unsigned mask, n; } CtMap;
static unsigned ct_hash(const char *s, int l) { unsigned h = 2166136261u; for (; l > 0; l--, s++) { h ^= (unsigned char)*s; h *= 16777619u; } return h; }
static void ctm_init(CtMap *m, unsigned cap) { m->e = (CtEnt *)calloc(cap, sizeof(CtEnt)); m->mask = cap - 1; m->n = 0; }
static CtEnt *ctm_get(const CtMap *m, const char *k, int l) {
    unsigned h = ct_hash(k, l) & m->mask;
    while (m->e[h].k) { if (m->e[h].kl == l && !memcmp(m->e[h].k, k, (size_t)l)) return &m->e[h]; h = (h + 1) & m->mask; }
    return NULL;
}
static void ctm_put(CtMap *m, const char *k, int kl, const char *v, int vl, int f) {
    if (kl <= 0 || kl > 47 || vl > 200) return;
    CtEnt *e = ctm_get(m, k, kl);
    if (!e) {
        if (m->n * 2 >= m->mask + 1) return;
        unsigned h = ct_hash(k, kl) & m->mask;
        while (m->e[h].k) h = (h + 1) & m->mask;
        e = &m->e[h]; e->k = k; e->kl = (unsigned char)kl; m->n++;
    }
    e->f = (unsigned short)(e->f | f);
    if (v) { e->v = v; e->vl = (unsigned char)vl; }
}
static void ctm_words(CtMap *m, const char *s, int f) {
    while (*s) {
        while (*s == ' ') s++;
        const char *b = s; while (*s && *s != ' ') s++;
        if (s > b) ctm_put(m, b, (int)(s - b), NULL, 0, f);
    }
}
static void ctm_pairs(CtMap *m, const char *s) {       /* "chave=valor chave=valor" */
    while (*s) {
        while (*s == ' ') s++;
        const char *b = s; while (*s && *s != ' ') s++;
        const char *q = (const char *)memchr(b, '=', (size_t)(s - b));
        if (q) ctm_put(m, b, (int)(q - b), q + 1, (int)(s - q - 1), 0);
    }
}

static CtMap g_ct_words, g_ct_pv, g_ct_enk, g_ct_ptk, g_ct_enirr, g_ct_ptirr, g_ct_abbr;
enum { CW_PTSTOP = 1, CW_ENSTOP = 2, CW_PTREQ = 4, CW_ENREQ = 8 };
enum { CV_AR = 1, CV_ER = 2, CV_IR = 4 };
enum { CK_NOS = 1, CK_NOLY = 2, CK_NOER = 4, CK_NOAT = 8 };

/* ---- dados: listas simples ---- */
static const char g_ct_pt_stop[] = "as os um uma uns umas de do da dos das em no na nos nas num numa ao aos para pra por pelo pela pelos pelas com sem sob sobre entre ate desde "
    "ou mas se que como quando onde porque pois nem tambem ja ainda so apenas muito muita muitos muitas mais menos mesmo mesma outro outra outros outras todo toda todos todas cada "
    "qual quais quem cujo isso isto aquilo esse essa esses essas este esta estes estas aquele aquela eu tu ele ela vos eles elas lhe lhes meu minha seu sua nosso nossa dele dela "
    "ser sou eh foi sao era eram fui estar estao estava tem tinha ha havia aqui ali la";
static const char g_ct_en_stop[] = "an the of to in on at by for with from into onto about is are was were be been being am does did have has had will would can could should shall may might must "
    "not and or but if then than so that this these those it its he she they we you my your our their his her them us what which who whom when where why how there here all any some each every "
    "such only own same too very just also more most up down out over under again further once both few";
static const char g_ct_pt_req[] = "descubra descubro descobrir descobre investigue investigar entenda entender explique explicar mostre mostrar diga dizer quero queria gostaria preciso precisa "
    "favor projeto projetos codigo codigos repositorio altere alterar modifique modificar mude mudar nao nada nenhum nenhuma qualquer coisa coisas algo alguma algum ache achar encontre "
    "encontrar procure procurar responda poderia pq";
static const char g_ct_en_req[] = "find figure out please tell show explain investigate look discover understand want need like dont nothing anything something thing things change modify "
    "project projects codebase code repo repository answer question help";
/* verbos pt por classe da raiz (infinitivo = raiz + ar/er/ir); a raiz tambem serve de evidencia para formas fracas (-a, -e, -am, -em, -ia) */
static const char g_ct_pt_ar[] = "list cri delet remov busc carreg salv fech envi atualiz ger process valid execut compil instal config inicializ finaliz cham retorn grav pag ativ desativ cancel aprov "
    "reprov copi col renomei ordin orden filtr agrup mostr alter modific solicit tent test us utiliz arm armazen import export analis calcul comput conect desconect sincroniz notific verific "
    "identific autentic autoriz trat ajust adicion cont particion junt combin mape pesquis indic index pars format serializ deserializ public assin escut inici termin encerr bloque liber reserv "
    "aloc desaloc limp reinici rod lanc disponibiliz habilit desabilit relacion document coment gerenci organiz transform renderiz desenh compar execu opin nomei marc desmarc seleccion "
    "chec confirm rejeit negoci mont desmont empacot descompact compact zip registr cadastr acion dispar interromp pul mud cad trabalh tir post respir ped mand dev";
static const char g_ct_pt_er[] = "escrev receb corr vend perd mov respond defend entend cumpr aprend esquec conhec escolh pod faz diz traz convert resolv envolv desenvolv volv compreend submet promet "
    "dev bat beb cre met ofend prend sofr ten ven ler";
static const char g_ct_pt_ir[] = "constru inser corrig substitu distribu contribu atribu inclu exclu conclu defin exib admit permit prefer refer transfer abr cobr descobr part sub sent serv segu "
    "repet med ped imprim reduz produz introduz reproduz conduz traduz decid divid assist resist exist insist persist consist emit omit remet transmit dirig surg fug ag cumpr un ouv "
    "sai exig expand impl";
static const char g_ct_pt_irr[] = "feito=fazer feita=fazer fez=fazer dito=dizer escrito=escrever escrita=escrever aberto=abrir aberta=abrir coberto=cobrir execucao=executar selecao=selecionar "
    "protecao=proteger decisao=decidir permissao=permitir conversao=converter compressao=comprimir descricao=descrever inscricao=inscrever leitura=ler lendo=ler lido=ler lida=ler caracteres=caractere paises=pais "
    "sao=ser foi=ser era=ser eram=ser fui=ser estao=estar estava=estar tem=ter tinha=ter fazer=fazer fazendo=fazer faz=fazer";
static const char g_ct_pt_keep[] = "universidade universal universo caminho vizinho linha minha sozinho carinho espinho padrinho farinha rainha galinha cozinha "
    "marinha vinho ninho pais mais menos jamais atras apos agora sempre nunca entao assim ainda tambem apenas aquele aquela outro outra todo toda estado estados dado dados "
    "arquivo casa mesa pasta tarefa tela rota base classe funcao versao sessao conexao extensao expressao impressao "
    "pedido partido sentido cumprimento comprimento medida ruido tecido individuo atendimento sentimento";   /* substantivos em -ido/-imento/-ida: nao sao participio (pedido != peder) */
static const char g_ct_en_irr[] = "children=child indices=index vertices=vertex matrices=matrix axes=axis analyses=analysis criteria=criterion data=data datum=data men=man women=woman people=person "
    "feet=foot teeth=tooth mice=mouse geese=goose leaves=leaf knives=knife wolves=wolf halves=half shelves=shelf selves=self was=be were=be been=be being=be is=be are=be am=be ran=run "
    "built=build written=write wrote=write took=take taken=take gave=give given=give made=make found=find sent=send kept=keep held=hold got=get gotten=get went=go gone=go did=do done=do "
    "does=do had=have has=have said=say saw=see seen=see came=come became=become began=begin begun=begin chose=choose chosen=choose drew=draw drawn=draw threw=throw thrown=throw "
    "broke=break broken=break spoke=speak spoken=speak stood=stand understood=understand lost=lose meant=mean met=meet paid=pay led=lead sold=sell told=tell thought=think brought=bring "
    "bought=buy caught=catch taught=teach fell=fall fallen=fall hid=hide hidden=hide shown=show known=know grown=grow flew=fly flown=fly ate=eat eaten=eat dug=dig hung=hang";
static const char g_ct_en_nos[] = "series species news alias canvas atlas bias whereas always perhaps lens gas";                         /* nao tirar o -s */
static const char g_ct_en_noly[] = "apply supply reply multiply family assembly only early italy july fly rely ally anomaly butterfly";   /* nao tirar o -ly */
static const char g_ct_en_noer[] = "number member buffer header folder character register parameter integer cluster master center answer letter matter filter water paper power tower never "
    "under other after either super inter outer inner border corner upper lower over ever river order layer user server";                 /* nao tirar o -er */
static const char g_ct_en_noat[] = "format concat combat compat habitat stat wombat";                                                    /* nao tirar o -at final */
static const char g_ct_abbr_def[] = "cfg=config,configuration msg=message ctx=context idx=index tmp=temp,temporary req=request resp=response impl=implementation init=initialize,initialization "
    "buf=buffer ptr=pointer len=length num=number str=string src=source dst=destination dest=destination cmd=command arg=argument args=arguments param=parameter params=parameters "
    "val=value var=variable fn=function func=function proc=process,procedure dir=directory env=environment err=error evt=event db=database doc=document docs=documents lib=library "
    "mem=memory alloc=allocate,allocation dealloc=deallocate conn=connection sess=session auth=authentication,authorization addr=address attr=attribute calc=calculate cb=callback "
    "cnt=count col=column cur=current curr=current def=definition desc=description diff=difference elem=element exec=execute fmt=format gen=generate hdr=header info=information "
    "max=maximum min=minimum obj=object opt=option opts=options orig=original pkg=package prev=previous ref=reference repo=repository seq=sequence srv=server sys=system tok=token "
    "ts=timestamp usr=user util=utility utils=utilities vec=vector ver=version wnd=window ws=workspace mgr=manager svc=service sz=size idx=index fd=descriptor";
static char *g_ct_kbbuf;
static int g_ct_inited;       /* 0 = nao feito, 1 = em andamento (uma thread), 2 = pronto: depois disso as tabelas so sao lidas (varias threads do indexador) */

static int ct_loadkb(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (8 << 20)) { fclose(f); return -1; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return -1; }
    fclose(f); b[sz] = 0;
    int n = 0; char *p = b;
    while (*p) {
        char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        char *c = (char *)memchr(p, '#', (size_t)(e - p)); char *le = c ? c : e;
        while (p < le && (*p == ' ' || *p == '\t')) p++;
        char *k = p; while (p < le && *p != ' ' && *p != '\t' && *p != '=' && *p != '\r') p++;
        char *ke = p;
        while (p < le && (*p == ' ' || *p == '\t' || *p == '=')) p++;
        char *v = p; while (le > v && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        if (ke > k && le > v) {
            for (char *q = k; q < ke; q++) *q = (char)tolower((unsigned char)*q);
            CtEnt *old = ctm_get(&g_ct_abbr, k, (int)(ke - k));
            if (old) { old->v = v; old->vl = (unsigned char)(le - v > 200 ? 200 : le - v); } else ctm_put(&g_ct_abbr, k, (int)(ke - k), v, (int)(le - v), 0);
            n++;
        }
        p = *e ? e + 1 : e;
    }
    g_ct_kbbuf = b;   /* as chaves apontam para o buffer: nunca liberado */
    return n;
}
static int coder_abbr_load(const char *path) { if (!g_ct_inited) return -1; return ct_loadkb(path); }
static void ct_init(void) {
    if (__atomic_load_n(&g_ct_inited, __ATOMIC_ACQUIRE) == 2) return;
    { int zero = 0; if (!__atomic_compare_exchange_n(&g_ct_inited, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { while (__atomic_load_n(&g_ct_inited, __ATOMIC_ACQUIRE) != 2) usleep(100); return; } }
    ctm_init(&g_ct_words, 2048); ctm_init(&g_ct_pv, 1024); ctm_init(&g_ct_enk, 512); ctm_init(&g_ct_ptk, 512);
    ctm_init(&g_ct_enirr, 512); ctm_init(&g_ct_ptirr, 256); ctm_init(&g_ct_abbr, 4096);
    ctm_words(&g_ct_words, g_ct_pt_stop, CW_PTSTOP); ctm_words(&g_ct_words, g_ct_en_stop, CW_ENSTOP);
    ctm_words(&g_ct_words, g_ct_pt_req, CW_PTREQ); ctm_words(&g_ct_words, g_ct_en_req, CW_ENREQ);
    ctm_words(&g_ct_pv, g_ct_pt_ar, CV_AR); ctm_words(&g_ct_pv, g_ct_pt_er, CV_ER); ctm_words(&g_ct_pv, g_ct_pt_ir, CV_IR);
    ctm_words(&g_ct_ptk, g_ct_pt_keep, 1);
    ctm_words(&g_ct_enk, g_ct_en_nos, CK_NOS); ctm_words(&g_ct_enk, g_ct_en_noly, CK_NOLY);
    ctm_words(&g_ct_enk, g_ct_en_noer, CK_NOER); ctm_words(&g_ct_enk, g_ct_en_noat, CK_NOAT);
    ctm_pairs(&g_ct_enirr, g_ct_en_irr); ctm_pairs(&g_ct_ptirr, g_ct_pt_irr); ctm_pairs(&g_ct_abbr, g_ct_abbr_def);
    const char *kb = getenv("KATANA_CODER_KB");
    if (!(kb && *kb && ct_loadkb(kb) >= 0)) ct_loadkb("data/coder/kb/abbreviations.kb");
    __atomic_store_n(&g_ct_inited, 2, __ATOMIC_RELEASE);
}

/* ---- divisao de identificadores ---- */
static const char g_ct_c3[65] = "AAAAAA CEEEEIIII NOOOOO  UUUUY  aaaaaa ceeeeiiii nooooo  uuuuy y";
/* dobra acentos Latin-1 (mantem a caixa); src[i] = byte de origem do caractere i, src[k] = bytes consumidos */
static int ct_fold(const char *s, int n, char *o, int *src, int cap) {
    int k = 0, i = 0;
    while (i < n && k < cap) {
        const unsigned char c = (unsigned char)s[i]; src[k] = i;
        if (c < 0x80) { o[k++] = (char)c; i++; }
        else if (c == 0xC3 && i + 1 < n && ((unsigned char)s[i + 1] & 0xC0) == 0x80) { o[k++] = g_ct_c3[(unsigned char)s[i + 1] - 0x80]; i += 2; }
        else { o[k++] = ' '; i++; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++; }
    }
    src[k] = i;
    return k;
}
/* partes [ps,pe) de f (ja dobrado): separadores, minuscula->MAIUSCULA, SIGLAComPalavra (HTTPServer -> HTTP|Server), letra<->digito */
static int ct_spans(const char *f, int fl, int *ps, int *pe, int cap) {
    int np = 0, st = -1;
    for (int i = 0; i <= fl; i++) {
        const int c = i < fl ? (unsigned char)f[i] : 0, an = isalnum(c);
        int cut = !an;
        if (an && st >= 0) {
            const int a = (unsigned char)f[i - 1];
            if ((isdigit(c) != 0) != (isdigit(a) != 0)) cut = 1;
            else if (islower(a) && isupper(c)) cut = 1;
            else if (isupper(a) && isupper(c) && i + 1 < fl && islower((unsigned char)f[i + 1])) cut = 1;
        }
        if (cut && st >= 0) { if (np < cap) { ps[np] = st; pe[np] = i; np++; } st = -1; }
        if (an && st < 0) st = i;
    }
    return np;
}
static int coder_split_ident(const char *id, char parts[][48], int cap) {
    char f[260]; int src[262], ps[64], pe[64];
    int n = (int)strlen(id); if (n > 255) n = 255;
    const int fl = ct_fold(id, n, f, src, 256), np = ct_spans(f, fl, ps, pe, 64);
    int k = 0;
    for (int p = 0; p < np && k < cap; p++) {
        int l = pe[p] - ps[p]; if (l > 47) l = 47;
        for (int i = 0; i < l; i++) parts[k][i] = (char)tolower((unsigned char)f[ps[p] + i]);
        parts[k][l] = 0; k++;
    }
    return k;
}

/* ---- normalizacao ---- */
static int ct_ends(const char *w, int l, const char *suf) { const int s = (int)strlen(suf); return l >= s && memcmp(w + l - s, suf, (size_t)s) == 0; }
static int ct_isv(int c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
static int ct_hasv(const char *w, int l) { for (int i = 0; i < l; i++) if (ct_isv(w[i]) || w[i] == 'y') return 1; return 0; }
static int ct_vclass(const char *w, int l) {
    const CtEnt *e = ctm_get(&g_ct_pv, w, l);
    return !e ? 0 : (e->f & CV_AR) ? 'a' : (e->f & CV_ER) ? 'e' : (e->f & CV_IR) ? 'i' : 0;
}
static int ct_undouble(const char *w, int l) { return (l >= 4 && w[l - 1] == w[l - 2] && strchr("bdgmnprt", w[l - 1])) ? l - 1 : l; }

enum { SX_STRONG, SX_GE, SX_LISTED, SX_WEAK };
static const struct { const char *suf; int cut, kind, cls; } g_ct_ptsuf[] = {
    {"amento", 6, SX_STRONG, 'a'}, {"imento", 6, SX_LISTED, 0}, {"acao", 4, SX_STRONG, 'a'}, {"icao", 4, SX_LISTED, 0}, {"ucao", 3, SX_LISTED, 0},
    {"ando", 4, SX_STRONG, 'a'}, {"endo", 4, SX_GE, 'e'}, {"indo", 4, SX_GE, 'i'}, {"aram", 4, SX_STRONG, 'a'}, {"eram", 4, SX_LISTED, 0},
    {"iram", 4, SX_LISTED, 0}, {"avam", 4, SX_STRONG, 'a'}, {"ava", 3, SX_STRONG, 'a'}, {"amos", 4, SX_STRONG, 'a'}, {"emos", 4, SX_LISTED, 0},
    {"imos", 4, SX_LISTED, 0}, {"ado", 3, SX_STRONG, 'a'}, {"ada", 3, SX_STRONG, 'a'}, {"ido", 3, SX_GE, 'e'}, {"ida", 3, SX_GE, 'e'},
    {"eu", 2, SX_LISTED, 0}, {"iu", 2, SX_LISTED, 0}, {"ei", 2, SX_STRONG, 'a'}, {"ou", 2, SX_STRONG, 'a'}, {"iam", 3, SX_LISTED, 0},
    {"ia", 2, SX_LISTED, 0}, {"am", 2, SX_WEAK, 0}, {"em", 2, SX_WEAK, 0}, {"e", 1, SX_WEAK, 0}, {"a", 1, SX_WEAK, 0}, {NULL, 0, 0, 0}};

static int ct_pt_norm(char *w, int l) {
    CtEnt *e;
    if (l <= 3) return l;
    if ((e = ctm_get(&g_ct_ptirr, w, l))) { memcpy(w, e->v, e->vl); return e->vl; }
    if (ctm_get(&g_ct_ptk, w, l)) return l;
    if (w[l - 1] == 's' && l >= 4) {                                         /* plural */
        if (l >= 5 && (ct_ends(w, l, "oes") || ct_ends(w, l, "aes"))) { w[l - 3] = 'a'; w[l - 2] = 'o'; l--; }
        else if (l >= 5 && (ct_ends(w, l, "ais") || ct_ends(w, l, "eis") || ct_ends(w, l, "ois"))) { w[l - 2] = 'l'; l--; }
        else if (l >= 5 && w[l - 2] == 'n' && !ct_ends(w, l, "ions")) { w[l - 2] = 'm'; l--; }
        else if (l >= 5 && (ct_ends(w, l, "res") || ct_ends(w, l, "zes"))) l -= 2;
        else if (!(ct_ends(w, l, "ss") || ct_ends(w, l, "us") || ct_ends(w, l, "is"))) l--;
        if (l <= 3) return l;
        if ((e = ctm_get(&g_ct_ptirr, w, l))) { memcpy(w, e->v, e->vl); return e->vl; }
        if (ctm_get(&g_ct_ptk, w, l)) return l;
    }
    if (l >= 8 && (ct_ends(w, l, "zinho") || ct_ends(w, l, "zinha"))) l -= 5;                 /* diminutivos */
    else if (l >= 8 && (ct_ends(w, l, "inho") || ct_ends(w, l, "inha"))) { const char c = w[l - 1]; l -= 4; w[l++] = c; }
    if (l >= 9 && ct_ends(w, l, "mente") && l - 5 >= 4) { l -= 5; if (w[l - 1] == 'a') w[l - 1] = 'o'; return l; }   /* adverbio -> adjetivo */
    for (int i = 0; g_ct_ptsuf[i].suf; i++) {
        const int sl = (int)strlen(g_ct_ptsuf[i].suf);
        if (!ct_ends(w, l, g_ct_ptsuf[i].suf)) continue;
        const int st = l - g_ct_ptsuf[i].cut;
        if (st < 1) continue;
        const int vc = ct_vclass(w, st), kind = g_ct_ptsuf[i].kind;
        int cls = 0;
        (void)sl;
        if (kind == SX_STRONG) { if (st >= 4 || vc) cls = g_ct_ptsuf[i].cls; }
        else if (kind == SX_GE) { if (st >= 4 || vc) cls = (vc == 'e' || vc == 'i') ? vc : g_ct_ptsuf[i].cls; }
        else if (kind == SX_LISTED) { if (vc == 'e' || vc == 'i') cls = vc; }
        else { if (vc) cls = vc; else if (st >= 4 && (ct_ends(w, st, "iz") || ct_ends(w, st, "ific"))) cls = 'a'; }
        if (!cls) continue;
        w[st] = (char)cls; w[st + 1] = 'r';
        return st + 2;
    }
    return l;
}

static int ct_en_step(char *w, int *pl, int it) {
    int l = *pl;
    CtEnt *e = ctm_get(&g_ct_enk, w, l);
    const int kf = e ? e->f : 0;
    if (it == 0 && w[l - 1] == 's' && l >= 4 && !(kf & CK_NOS)) {                       /* plural */
        if (l >= 5 && ct_ends(w, l, "ies")) l -= 2;
        else if (ct_ends(w, l, "ss") || ct_ends(w, l, "us") || ct_ends(w, l, "is") || (ct_ends(w, l, "as") && l < 5)) return 0;
        else l--;
        *pl = l; return 1;
    }
    if (l >= 11 && ct_ends(w, l, "ification")) { l -= 9; memcpy(w + l, "ify", 3); *pl = l + 3; return 1; }
    if (l >= 8 && ct_ends(w, l, "ation")) { l -= 5; if (l < 4) { w[l++] = 'a'; w[l++] = 't'; } *pl = l; return 1; }
    if (l >= 9 && ct_ends(w, l, "ition")) { *pl = l - 5; return 1; }
    if (l >= 7 && ct_ends(w, l, "ion")) { *pl = l - 3; return 1; }
    if (l >= 8 && ct_ends(w, l, "ment")) { *pl = l - 4; return 1; }
    if (l >= 9 && ct_ends(w, l, "ically")) { *pl = l - 4; return 1; }
    if (l >= 6 && ct_ends(w, l, "ly") && l - 2 >= 4 && !(kf & CK_NOLY)) { *pl = l - 2; return 1; }
    if (l >= 8 && ct_ends(w, l, "able")) { *pl = l - 4; return 1; }
    if (l >= 6 && ct_ends(w, l, "ator")) { *pl = l - 2; return 1; }
    if (l >= 5 && ct_ends(w, l, "ing")) {
        const int r = l - 3;
        if (r >= 3 && ct_hasv(w, r)) { *pl = ct_undouble(w, r); return 1; }
        if (r == 2 && ct_hasv(w, r)) { w[2] = 'e'; *pl = 3; return 1; }
        return 0;
    }
    if (l >= 4 && ct_ends(w, l, "ed")) {
        if (l >= 5 && ct_ends(w, l, "ied")) { *pl = l - 2; return 1; }
        if (w[l - 3] == 'e') return 0;
        const int r = l - 2;
        if (r >= 3 && ct_hasv(w, r)) { *pl = ct_undouble(w, r); return 1; }
        if (l == 4 && ct_hasv(w, 3)) { *pl = 3; return 1; }
        return 0;
    }
    if (l >= 6 && ct_ends(w, l, "er") && !(kf & CK_NOER) && l - 2 >= 4) { *pl = ct_undouble(w, l - 2); return 1; }
    return 0;
}
static void ct_en_z(char *w, int l) {                                         /* -ise/-ising/-isation -> -ize... (so em padroes seguros) */
    static const char *const pre[] = {"alis", "ilis", "imis", "anis", "gnis", "tomis", "ronis", NULL};
    static const char *const tail[] = {"e", "ed", "es", "er", "ers", "ing", "ation", "ations", NULL};
    if (l < 7) return;
    for (int t = 0; tail[t]; t++) {
        const int tl = (int)strlen(tail[t]);
        if (!ct_ends(w, l, tail[t])) continue;
        for (int p = 0; pre[p]; p++) {
            const int pl = (int)strlen(pre[p]);
            if (l - tl >= pl && !memcmp(w + l - tl - pl, pre[p], (size_t)pl)) { w[l - tl - 1] = 'z'; return; }
        }
    }
}
static int ct_en_norm(char *w, int l) {
    CtEnt *e;
    if ((e = ctm_get(&g_ct_enirr, w, l))) { memcpy(w, e->v, e->vl); l = e->vl; }
    else if (l == 3 && w[2] == 's' && !ct_isv(w[1]) && w[1] != 'y') return 2;     /* ids -> id */
    if (l <= 3) return l;
    ct_en_z(w, l);
    for (int it = 0; it < 3 && l >= 3; it++) if (!ct_en_step(w, &l, it)) break;
    if (l >= 4 && w[l - 1] == 'e' && w[l - 2] != 'e') l--;
    if (l >= 6 && ct_ends(w, l, "at") && !ct_isv(w[l - 3])) { e = ctm_get(&g_ct_enk, w, l); if (!(e && (e->f & CK_NOAT))) l -= 2; }
    if (l >= 3 && w[l - 1] == 'y' && !ct_isv(w[l - 2])) w[l - 1] = 'i';
    return l;
}
/* normaliza UMA palavra (minuscula ASCII; maiusculas sao baixadas aqui); lang: CT_L_PT usa regras pt, os demais regras en. Retorna o tamanho (0 se vazia) */
static int coder_norm_word(const char *in, int lang, char *out, size_t cap) {
    char w[64]; int l = 0;
    ct_init();
    while (in[l] && l < 47) { w[l] = (char)tolower((unsigned char)in[l]); l++; }
    if (l == 0 || cap < 2) { if (cap) out[0] = 0; return 0; }
    if (isdigit((unsigned char)w[0])) { /* numero: inalterado */ }
    else l = lang == CT_L_PT ? ct_pt_norm(w, l) : ct_en_norm(w, l);
    if ((size_t)l >= cap) l = (int)cap - 1;
    memcpy(out, w, (size_t)l); out[l] = 0;
    return l;
}

/* ---- idioma por palavra (palavra minuscula ASCII; accented = veio com acento) ---- */
static int ct_detect(const char *w, int l, int accented) {
    int pt = 0, en = 0;
    const CtEnt *e = ctm_get(&g_ct_words, w, l);
    if (e) { pt = (e->f & (CW_PTSTOP | CW_PTREQ)) != 0; en = (e->f & (CW_ENSTOP | CW_ENREQ)) != 0; }
    if (accented) return CT_L_PT;
    if (pt && en) return CT_L_UNKNOWN;
    if (pt) return CT_L_PT;
    if (en) return CT_L_EN;
    if (ctm_get(&g_ct_abbr, w, l)) return CT_L_CODE;
    int dig = 1, v = 0;
    for (int i = 0; i < l; i++) { if (!isdigit((unsigned char)w[i])) dig = 0; if (ct_isv(w[i]) || w[i] == 'y') v = 1; }
    if (dig || l <= 2 || !v) return CT_L_CODE;
    static const char *const ptend[] = {"cao", "coes", "mente", "ando", "endo", "indo", "amento", "imento", "idade", "inho", "inha", "avel", "aes", "ario", "aria", "eiro", "eira", NULL};
    static const char *const enend[] = {"tion", "tions", "ion", "ions", "ing", "ings", "ness", "ous", "ful", "less", "ize", "ise", "izes", "able", "ible", "ity", "ities", "ship", "ly", NULL};
    for (int i = 0; ptend[i]; i++) if (l > (int)strlen(ptend[i]) + 1 && ct_ends(w, l, ptend[i])) return CT_L_PT;
    for (int i = 0; enend[i]; i++) if (l > (int)strlen(enend[i]) + 1 && ct_ends(w, l, enend[i])) return CT_L_EN;
    if (l >= 6 && (ct_ends(w, l, "ado") || ct_ends(w, l, "ada") || ct_ends(w, l, "ido") || ct_ends(w, l, "ida"))) return CT_L_PT;
    if (l >= 6 && ct_ends(w, l, "ed")) return CT_L_EN;
    return CT_L_UNKNOWN;
}
static int coder_detect_lang(const char *w) {
    char b[48]; int l = 0; ct_init();
    while (w[l] && l < 47) { b[l] = (char)tolower((unsigned char)w[l]); l++; }
    return ct_detect(b, l, 0);
}

/* ---- tokenizacao ---- */
static int ct_isrun(int c) { return isalnum(c) || c == '_' || c == '-' || c == '.' || c >= 0x80; }
static void ct_put(CoderTok *o, const char *norm, const char *orig, int ol, int pos, int field, int lang, int kind, int stop) {
    int nl = (int)strlen(norm); if (nl > 47) nl = 47;
    memcpy(o->norm, norm, (size_t)nl); o->norm[nl] = 0;
    if (ol > 47) ol = 47;
    memcpy(o->orig, orig, (size_t)ol); o->orig[ol] = 0;
    o->pos = pos; o->field = field; o->lang = lang; o->kind = kind; o->stop = stop;
}
/* processa uma corrida [rs,re); cnt != NULL: so conta evidencia de idioma (pt, en) */
static void ct_run(const char *text, int rs, int re, CoderTok *out, int *n, int cap, int field, int lang, int dflt, int *cnt) {
    char f[264]; int src[264], ps[64], pe[64];
    const int fl = ct_fold(text + rs, re - rs, f, src, 256), np = ct_spans(f, fl, ps, pe, 64);
    if (!np) return;
    const int mask = lang == CT_L_PT ? (CW_PTSTOP | CW_PTREQ) : lang == CT_L_EN ? (CW_ENSTOP | CW_ENREQ) : 15;
    if (!cnt && np > 1) {                                                      /* termo inteiro do identificador */
        char whole[64]; int wl = 0;
        for (int p = 0; p < np && wl < 47; p++) {
            if (p) whole[wl++] = '_';
            for (int i = ps[p]; i < pe[p] && wl < 47; i++) whole[wl++] = (char)tolower((unsigned char)f[i]);
        }
        whole[wl] = 0;
        if (*n < cap) { ct_put(&out[*n], whole, text + rs + src[ps[0]], re - rs - src[ps[0]] < 47 ? re - rs - src[ps[0]] : 47, rs + src[ps[0]], field, CT_L_CODE, 1, 0); (*n)++; }
    }
    for (int p = 0; p < np; p++) {
        char lw[48], nm[48]; int l = pe[p] - ps[p]; if (l > 47) l = 47;
        for (int i = 0; i < l; i++) lw[i] = (char)tolower((unsigned char)f[ps[p] + i]);
        lw[l] = 0;
        const int dig = isdigit((unsigned char)lw[0]) != 0;
        if (l == 1 && !dig) continue;
        const int acc = src[pe[p]] - src[ps[p]] > pe[p] - ps[p];
        const int det = dig ? CT_L_CODE : ct_detect(lw, l, acc);
        if (cnt) { if (det == CT_L_PT) cnt[0]++; else if (det == CT_L_EN) cnt[1]++; continue; }
        if (*n >= cap) return;
        const int eff = lang >= 0 ? lang : (det == CT_L_PT || det == CT_L_EN) ? det : dflt;
        if (dig) { memcpy(nm, lw, (size_t)l + 1); } else coder_norm_word(lw, eff, nm, sizeof nm);
        int stop = 0;
        if (np == 1 && !dig) { const CtEnt *e = ctm_get(&g_ct_words, lw, l); if (e && (e->f & mask)) stop = (e->f & mask & (CW_PTREQ | CW_ENREQ)) ? 2 : 1; }
        ct_put(&out[*n], nm, f + ps[p], l, rs + src[ps[p]], field, lang >= 0 ? lang : det, 0, stop); (*n)++;
        const CtEnt *ab = dig ? NULL : ctm_get(&g_ct_abbr, lw, l);                /* expansoes da abreviacao */
        if (ab && ab->v) {
            const char *q = ab->v, *end = ab->v + ab->vl;
            for (int k = 0; k < 3 && q < end && *n < cap; k++) {
                const char *c = (const char *)memchr(q, ',', (size_t)(end - q)); if (!c) c = end;
                char ex[48]; int el = (int)(c - q); if (el > 47) el = 47;
                memcpy(ex, q, (size_t)el); ex[el] = 0;
                if (el > 0) { coder_norm_word(ex, CT_L_EN, nm, sizeof nm); ct_put(&out[*n], nm, ex, el, rs + src[ps[p]], field, CT_L_EN, 2, 0); (*n)++; }
                q = c + 1;
            }
        }
    }
}
static int coder_tokens_ex(const char *text, CoderTok *out, int cap, int field, int lang) {
    ct_init();
    const int n = (int)strlen(text);
    int cnt = 0, dflt = lang == CT_L_PT ? CT_L_PT : CT_L_EN;
    if (lang == CT_AUTO && strchr(text, ' ')) {                               /* idioma dominante do texto */
        int c[2] = {0, 0};
        for (int i = 0; i < n;) {
            if (!ct_isrun((unsigned char)text[i])) { i++; continue; }
            int j = i; while (j < n && j - i < 255 && ct_isrun((unsigned char)text[j])) j++;
            ct_run(text, i, j, out, &cnt, cap, 0, CT_AUTO, dflt, c); i = j;
        }
        dflt = c[0] > c[1] ? CT_L_PT : CT_L_EN;
    }
    cnt = 0;
    for (int i = 0; i < n && cnt < cap;) {
        if (!ct_isrun((unsigned char)text[i])) { i++; continue; }
        int j = i; while (j < n && j - i < 255 && ct_isrun((unsigned char)text[j])) j++;
        ct_run(text, i, j, out, &cnt, cap, field, lang, dflt, NULL); i = j;
    }
    return cnt;
}
static int coder_tokens(const char *text, CoderTok *out, int cap) { return coder_tokens_ex(text, out, cap, 0, CT_AUTO); }

/* ---- katana coder tokens ---- */
static void ct_jstr(const char *s) {
    putchar('"');
    for (; *s; s++) { const unsigned char c = (unsigned char)*s; if (c == '"' || c == '\\') printf("\\%c", c); else if (c < 0x20) printf("\\u%04x", c); else putchar(c); }
    putchar('"');
}
static void ct_print(const char *text, const CoderTok *t, int n, int json) {
    static const char *const kn[] = {"part", "whole", "abbr"}, *const sn[] = {"", "stop", "request"};
    if (json) {
        printf("{\"text\":"); ct_jstr(text); printf(",\"tokens\":[");
        for (int i = 0; i < n; i++) {
            printf("%s{\"orig\":", i ? "," : ""); ct_jstr(t[i].orig); printf(",\"norm\":"); ct_jstr(t[i].norm);
            printf(",\"lang\":\"%s\",\"pos\":%d,\"kind\":\"%s\",\"stop\":\"%s\"}", g_ct_langname[t[i].lang], t[i].pos, kn[t[i].kind], sn[t[i].stop]);
        }
        printf("]}\n");
        return;
    }
    printf("%-24s %-22s %-8s %4s  %-6s %s\n", "orig", "norm", "lang", "pos", "kind", "flag");
    for (int i = 0; i < n; i++) printf("%-24s %-22s %-8s %4d  %-6s %s\n", t[i].orig, t[i].norm, g_ct_langname[t[i].lang], t[i].pos, kn[t[i].kind], sn[t[i].stop]);
}

/* ---- trechos de um arquivo que casam com o que o modelo procura (Read de arquivo grande sem faixa) ----
 * Tokeniza a intencao (consulta do Find ou pedido do usuario) e cada linha do arquivo com o mesmo tokenizador (identificadores
 * partidos, plural/verbo reduzidos); janelas de 14 linhas com passo 7 pontuam os termos DISTINTOS que contem, pesando mais os raros no
 * arquivo; devolve ate 2 janelas sem sobreposicao, com numero de linha. Devolve quantas janelas escreveu (0 = nada que valha) ---- */
static int rd_best_windows(const char *b, size_t n, const char *intent, long skip_lines, char *out, size_t cap) {
    if (!intent || !*intent || n == 0 || cap < 600) return 0;
    CoderTok tk[96]; char term[24][48]; int nt = 0;
    const int ni = coder_tokens(intent, tk, 96);
    for (int i = 0; i < ni && nt < 24; i++) {
        if (tk[i].stop || tk[i].kind == 2 || strlen(tk[i].norm) < 3) continue;
        int dup = 0; for (int k = 0; k < nt; k++) if (!strcmp(term[k], tk[i].norm)) dup = 1;
        if (!dup) snprintf(term[nt++], sizeof term[0], "%s", tk[i].norm);
    }
    if (nt == 0) return 0;
    size_t nl = 0; for (size_t i = 0; i < n; i++) if (b[i] == '\n') nl++;
    nl++;
    if (nl > 40000) return 0;
    uint32_t *mask = (uint32_t *)calloc(nl, sizeof *mask); size_t *off = (size_t *)malloc(sizeof *off * (nl + 1));
    if (!mask || !off) { free(mask); free(off); return 0; }
    int df[24] = {0};
    size_t li = 0; off[0] = 0;
    for (size_t s = 0; s < n && li < nl;) {
        const char *nlp = (const char *)memchr(b + s, '\n', n - s);
        size_t e = nlp ? (size_t)(nlp - b) : n;
        char ln[420]; size_t l = e - s < sizeof ln - 1 ? e - s : sizeof ln - 1;
        memcpy(ln, b + s, l); ln[l] = 0;
        const char *c0 = ln; while (*c0 == ' ' || *c0 == '\t') c0++;
        const int is_comment = (c0[0] == '/' && (c0[1] == '/' || c0[1] == '*')) || c0[0] == '*' || c0[0] == '#' || (c0[0] == '-' && c0[1] == '-');      /* comentario puro: o codigo vale mais */
        CoderTok lt[160]; const int nk = is_comment ? 0 : coder_tokens(ln, lt, 160);
        for (int i = 0; i < nk; i++) for (int k = 0; k < nt; k++) if (!strcmp(lt[i].norm, term[k])) mask[li] |= 1u << k;
        for (int k = 0; k < nt; k++) if (mask[li] & (1u << k)) df[k]++;
        li++; off[li] = nlp ? e + 1 : n; s = nlp ? e + 1 : n + 1;
    }
    const size_t N = li; double w[24];
    for (int k = 0; k < nt; k++) w[k] = df[k] ? 1.0 / (1.0 + log((double)df[k])) : 0;
    const int WIN = 14, STEP = 7;
    double bs[2] = {0, 0}; long bw[2] = {-1, -1};
    for (size_t a = (size_t)(skip_lines > 0 ? skip_lines : 0); a < N; a += STEP) {      /* depois das linhas que o Read ja mostra no comeco */
        uint32_t u = 0; int lines = 0;
        for (size_t i = a; i < a + WIN && i < N; i++) { u |= mask[i]; if (mask[i]) lines++; }
        int nd = 0; double sc = 0;
        for (int k = 0; k < nt; k++) if (u & (1u << k)) { nd++; sc += w[k]; }
        if (nd < (nt >= 2 ? 2 : 1)) continue;
        sc += 0.3 * lines;
        for (int r = 0; r < 2; r++) {
            if (bw[r] >= 0 && (long)a < bw[r] + WIN && (long)a + WIN > bw[r]) { if (sc > bs[r]) { bs[r] = sc; bw[r] = (long)a; } break; }      /* mesma regiao: fica a melhor */
            if (bw[r] < 0 || sc > bs[r]) {
                if (r == 0 && bw[0] >= 0) { bs[1] = bs[0]; bw[1] = bw[0]; }
                bs[r] = sc; bw[r] = (long)a; break;
            }
        }
    }
    if (bw[0] >= 0 && bw[1] >= 0 && bw[1] < bw[0] + WIN && bw[0] < bw[1] + WIN) bw[1] = -1;
    int wrote = 0; size_t o = 0;
    for (int r = 0; r < 2; r++) {
        if (bw[r] < 0) continue;
        size_t a = (size_t)bw[r], z = a + WIN < N ? a + WIN : N;
        if (o + 200 > cap) break;
        o += (size_t)snprintf(out + o, cap - o, "%s[lines %zu-%zu match what you look for]\n", wrote ? "" : "", a + 1, z);
        for (size_t i = a; i < z; i++) {
            size_t l = off[i + 1] > off[i] ? off[i + 1] - off[i] : 0; if (l && b[off[i] + l - 1] == '\n') l--;
            if (l > 200) l = 200;
            if (o + l + 12 > cap) break;
            o += (size_t)snprintf(out + o, cap - o, "%6zu\t%.*s\n", i + 1, (int)l, b + off[i]);
        }
        wrote++;
    }
    free(mask); free(off);
    return wrote;
}
static int coder_tokens_main(int argc, char **argv) {
    const char *text = NULL, *file = NULL, *kb = NULL; int lang = CT_AUTO, json = 0, bench = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--kb") && i + 1 < argc) kb = argv[++i];
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) {
            const char *v = argv[++i];
            lang = !strcmp(v, "pt") ? CT_L_PT : !strcmp(v, "en") ? CT_L_EN : CT_AUTO;
        } else if (!text) text = argv[i];
    }
    if (!text && !file) { fprintf(stderr, "uso: katana coder tokens \"texto ou identificador\" [--lang pt|en|auto] [--json] [--kb ARQ] | --file ARQ.txt [--bench]\n"); return 2; }
    ct_init();
    if (kb && coder_abbr_load(kb) < 0) fprintf(stderr, "aviso: nao foi possivel ler %s\n", kb);
    CoderTok *tk = (CoderTok *)malloc(4096 * sizeof(CoderTok));
    if (!tk) return 1;
    if (!file) { ct_print(text, tk, coder_tokens_ex(text, tk, 4096, 0, lang), json); free(tk); return 0; }
    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", file); free(tk); return 1; }
    if (bench) {
        char **ln = NULL; int nl = 0, cl = 0; static char buf[65536];
        while (fgets(buf, sizeof buf, f)) {
            size_t k = strlen(buf); while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
            if (nl == cl) { cl = cl ? cl * 2 : 1024; ln = (char **)realloc(ln, (size_t)cl * sizeof *ln); }
            ln[nl++] = strdup(buf);
        }
        fclose(f);
        long long ntok = 0; const clock_t t0 = clock();
        for (int rep = 0; rep < 20; rep++) for (int i = 0; i < nl; i++) ntok += coder_tokens_ex(ln[i], tk, 4096, 0, lang);
        const double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("%d linhas x 20, %lld tokens em %.3f s: %.2f M linhas/s, %.2f M tokens/s\n", nl, ntok, s, nl * 20.0 / s / 1e6, (double)ntok / s / 1e6);
        for (int i = 0; i < nl; i++) free(ln[i]);
        free(ln); free(tk); return 0;
    }
    static char line[65536];
    while (fgets(line, sizeof line, f)) {
        size_t k = strlen(line); while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
        if (!k) continue;
        const int c = coder_tokens_ex(line, tk, 4096, 0, lang);
        if (json) ct_print(line, tk, c, 1);
        else { printf("== %s\n", line); ct_print(line, tk, c, 0); }
    }
    fclose(f); free(tk);
    return 0;
}
