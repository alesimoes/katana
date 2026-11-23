/* ---------- coder/parse.c: analisador estrutural exato por familia de linguagem ----------
 * Para cada arquivo extrai SIMBOLOS (nome, assinatura curta, tipo, pai, linhas e bytes do CORPO COMPLETO) e REFERENCIAS
 * (chamadas, usos, imports, heranca) atribuidas ao simbolo mais interno que as contem. Sem bibliotecas, sem estado em disco.
 *
 * Arquitetura: (1) mascara: copia o texto e apaga (troca por espacos, mantendo \n e os offsets) comentarios, strings, regex,
 * here-docs; guarda tambem uma copia so sem comentarios (para assinaturas e caminhos de import). (2) tokenizador unico
 * (identificadores, numeros, pontuacao, bandeira de "primeiro da linha") e tabela de pares () [] {} tolerante a erro (fecho
 * faltando = ate o fim do arquivo, fecho sobrando = ignorado). (3) um reconhecedor de declaracoes por familia, que anda por
 * "instrucoes" e recursa nos blocos: C/C++, JS/TS, Java, Kotlin, Go, Rust, QML, PowerShell, shell. Python e Markdown sao
 * por linha/indentacao. (4) varredura de referencias sobre os tokens (CALL = identificador seguido de '(' que nao e palavra-chave
 * nem definicao; USE = nome de simbolo do proprio arquivo; IMPORT/EXTENDS/IMPLEMENTS saem dos reconhecedores).
 * Linhas sao 1-based; byte1 e exclusivo; o intervalo comeca no primeiro token do cabecalho (anotacoes/decoradores/template<>
 * na mesma instrucao inclusos) e termina no '}' que fecha (ou ';' ou fim do bloco indentado). Referencias sao deduplicadas por
 * (simbolo, tipo, nome): fica a primeira ocorrencia (linha). Referencia de lambda/callback anonimo vai para o simbolo que o contem;
 * funcao com nome aninhada em outra vira simbolo filho e leva as proprias referencias.
 *
 * O QUE CADA ANALISADOR NAO RESOLVE (falhas conhecidas, por projeto):
 *  - Todos: nao ha resolucao de nomes. Uma chamada e so o nome final (a.b.c() -> "c"); USE so casa nomes de simbolos do MESMO arquivo
 *    (variaveis locais com o mesmo nome de um simbolo global geram USE falso); chamadas por ponteiro/reflexao/macro nao aparecem.
 *    Template literal e interpolacao: tratados (JS, Kotlin); f-strings de Python e $(...) de shell nao (ficam em branco).
 *  - C/C++: #if/#else com cabecalhos diferentes e chaves desbalanceadas por ramo confundem; K&R antigo; macros que geram funcoes
 *    (X(foo) { ... }) viram funcao com o nome da macro; lambda e classe local dentro de funcao nao viram simbolo; o corpo de #define e
 *    varrido para referencias mas nao e analisado como declaracao; sobrecargas viram varios simbolos de mesmo nome; prototipos viram
 *    simbolo (sig termina em ';') so quando o arquivo nao tem a definicao.
 *  - JS/TS: ponto-e-virgula automatico e heuristico (quebra de linha); JSX e regex ambiguo (a / b / c) podem errar; objeto literal
 *    so e analisado em module.exports/export default; callbacks anonimos nao viram simbolo; tipos TS complexos em retorno podem
 *    enganar a procura do '{' do corpo.
 *  - Python: linhas continuadas por parenteses/barra sao tratadas; tabulacao conta como 8; "if x: def" em uma linha nao; atribuicoes
 *    so em escopo de modulo/classe; decoradores entram no intervalo.
 *  - Java/Kotlin: classes anonimas e lambdas nao viram simbolo; Kotlin sem ponto-e-virgula usa heuristica de quebra de linha;
 *    "get()/set()" de propriedade entram no intervalo da propriedade; records, sealed e annotation classes sao tratados como class.
 *  - Go: tipos genericos complexos, campos agrupados (a, b int) viram um VAR so com o primeiro nome; metodo e ligado ao pai pelo
 *    nome do receptor (pai pode estar fora do intervalo do tipo).
 *  - Rust: macros (foo!) viram CALL "foo!"; macro_rules e simbolo mas sem corpo analisado; impl vira CLASS com o nome do tipo.
 *  - Shell: so funcoes (name() {} e function name), variaveis de topo (NAME=...) e source/.; chamadas so a funcoes do mesmo arquivo;
 *    ${...}, aritmetica e case com ')' podem desbalancear; here-doc detectado pelo padrao <<[-]['"]PALAVRA.
 *  - QML: so componentes com id (e a raiz), propriedades, signals, funcoes e handlers onX; Behavior/State aninhados sem id nao viram simbolo.
 *  - PowerShell: chamadas = cmdlets Verbo-Substantivo e funcoes do arquivo; param() entra na assinatura; sem pipeline de objetos.
 *  - Markdown: titulos ATX e setext fora de blocos de codigo; refs = links relativos (IMPORT); nao entende HTML embutido.
 *  - Linguagens e formatos acrescentados depois (C#, Swift, PHP, Dart, Scala, Groovy/Gradle, Ruby, Lua, Elixir, Julia, Erlang, Haskell, OCaml/F#,
 *    Clojure/Lisp/Scheme/Racket, R, Perl, Objective-C, Zig, Nim, V, Fortran, COBOL, Assembly, SQL, JSON de projeto, YAML, TOML, Dockerfile,
 *    Makefile, CMake, HTML, XML, CSS/SCSS, Protobuf/Thrift, GraphQL, Terraform, Vue/Svelte, LaTeX, rst/AsciiDoc/Org, .properties/.env/.ini/.gitignore):
 *    o que cada um nao resolve esta no cabecalho de parse2.c (incluido logo depois deste arquivo, usa a mesma CP e as mesmas funcoes cp_*).
 */
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct { uint32_t a, b; uint8_t t, fl; } CpTok;
enum { CT_ID = 1, CT_NUM, CT_P };
enum { CF_NL = 1, CF_DIR = 2, CF_DEF = 4 };
enum { CM_TOP, CM_CLASS, CM_BODY, CM_OBJ, CM_ENUM, CM_COMP, CM_ARR };
typedef struct {
    int lang; const char *t, *c; size_t n;     /* t = sem comentarios (strings preservadas); c = sem comentarios nem strings */
    CpTok *tk; int nt, tcap; int *mt; unsigned char *isn;
    CoderFile *f; int scap; int *snt; unsigned char *sfl; char **sx;
    size_t *ln; int nln;
    int *rh; int rhn; int nested; int rcap;
    const char *path; int ux0, ux1;     /* usados pelas linguagens de parse2.c */
} CP;

/* linguagens acrescentadas depois: ver parse2.c (incluido logo depois deste arquivo) */
static void c2_mask(int L, const char *t, size_t n, char *cl, char *nc);
static int c2_gdecl(CP *c, int i, int b, int parent, int mode, int dp);
static int c2_istok(int L);
static int c2_isline(int L);
static void c2_parse_line(CP *c);
static void c2_sweep_line(CP *c);
static int c2_lang_of(const char *base, const char *ext);
static void c2_enrich(CP *c);
static void cp_blank(char *p, size_t a, size_t b) { if (b > a) for (size_t k = a; k < b; k++) if (p[k] != '\n' && p[k] != '\r') p[k] = ' '; }
static int cp_idc(int c) { return isalnum(c) || c == '_' || c == '$' || (c & 0x80); }
/* fim de string simples a partir de i (1o char depois da abertura): indice do fechamento, ou onde parou (quebra de linha/fim) */
static size_t cp_str_end(const char *t, size_t n, size_t i, char q, int esc, int ml, int dbl) {
    while (i < n) {
        char c = t[i];
        if (esc && c == '\\') { i += 2; continue; }
        if (c == q) { if (dbl && i + 1 < n && t[i + 1] == q) { i += 2; continue; } return i; }
        if (c == '\n' && !ml) return i;
        i++;
    }
    return n;
}
static size_t cp_charlit(const char *t, size_t n, size_t i) { /* t[i]=='\'': comprimento do literal de caractere, ou 0 */
    if (i + 1 >= n) return 0;
    if (t[i + 1] == '\\') { for (size_t k = i + 2; k < n && k < i + 14 && t[k] != '\n'; k++) if (t[k] == '\'') return k - i + 1; return 0; }
    unsigned char c = (unsigned char)t[i + 1];
    if (c == '\n' || c == '\'') return 0;
    size_t l = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return i + 1 + l < n && t[i + 1 + l] == '\'' ? l + 2 : 0;
}
/* fim de string de aspas triplas (q q q) a partir de i; devolve o indice do fechamento (ou n) */
static size_t cp_tri_end(const char *t, size_t n, size_t i, char q, int esc) {
    while (i < n) {
        if (esc && t[i] == '\\') { i += 2; continue; }
        if (t[i] == q && i + 2 < n + 0 + 1 && i + 2 <= n - 1 + 0 && t[i + 1] == q && t[i + 2] == q) return i;
        i++;
    }
    return n;
}
typedef struct { int kind, depth, dl, ml, esc; char delim; } CpStk;   /* kind 1 = dentro de string com interpolacao; 2 = codigo interpolado */
static int cp_regex_ok(char last, const char *w) {
    if (!last) return 1;
    if (strchr("(,=:[!&|?{};+-*%~^", last)) return 1;
    if (isalnum((unsigned char)last) || last == '_') {
        static const char *const K[] = {"return", "typeof", "case", "in", "of", "delete", "void", "throw", "new", "else", "do", "yield", "await", NULL};
        for (int i = 0; K[i]; i++) if (!strcmp(K[i], w)) return 1;
    }
    return 0;
}
static void cp_mask(int L, const char *t, size_t n, char *cl, char *nc) {
    memcpy(cl, t, n); memcpy(nc, t, n);
    const int slash = L == CL_C || L == CL_CPP || L == CL_JS || L == CL_TS || L == CL_KT || L == CL_JAVA || L == CL_GO || L == CL_RS || L == CL_QML;
    const int hash = L == CL_PY || L == CL_SH || L == CL_PS1;
    const int nest = L == CL_RS || L == CL_KT;
    const int js = L == CL_JS || L == CL_TS || L == CL_QML;
    size_t i = 0; CpStk st[24]; int sn = 0;
    char last = 0, lw[16] = ""; int lwl = 0;
    char hd[4][64]; int hdash[4], hn = 0;
    while (i < n) {
        char c = t[i];
        if (sn > 0 && st[sn - 1].kind == 1) {                      /* dentro de string com interpolacao */
            CpStk *s = &st[sn - 1];
            if (c == '\\' && s->esc) { cl[i] = ' '; if (i + 1 < n && t[i + 1] != '\n') cl[i + 1] = ' '; i += 2; continue; }
            if (s->dl == 3 ? !strncmp(t + i, "\"\"\"", 3) : c == s->delim) { i += s->dl == 3 ? 3 : 1; sn--; last = 'a'; continue; }
            if (c == '\n' && !s->ml) { sn--; continue; }
            if (c == '$' && i + 1 < n && t[i + 1] == '{') { cl[i] = ' '; if (sn < 23) st[sn++] = (CpStk){2, 0, 0, 0, 0, 0}; i += 2; last = 0; continue; }
            if (c == '$' && L == CL_KT) { cl[i] = ' '; i++; continue; }
            if (c != '\n' && c != '\r') cl[i] = ' ';
            i++; continue;
        }
        if (sn > 0 && st[sn - 1].kind == 2) {
            if (c == '{') st[sn - 1].depth++;
            else if (c == '}') { if (st[sn - 1].depth == 0) { sn--; i++; continue; } st[sn - 1].depth--; }
        }
        if (c == '\n') {
            i++;
            for (int h = 0; h < hn; h++) {                         /* corpo dos here-docs pendentes */
                size_t dl = strlen(hd[h]);
                while (i < n) {
                    size_t e = i; while (e < n && t[e] != '\n') e++;
                    size_t s = i; if (hdash[h]) while (s < e && t[s] == '\t') s++;
                    int hit = e - s >= dl && !memcmp(t + s, hd[h], dl) && (e - s == dl || (e - s == dl + 1 && t[s + dl] == '\r'));
                    cp_blank(cl, i, e); i = e < n ? e + 1 : n;
                    if (hit) break;
                }
            }
            hn = 0; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (L == CL_PS1 && c == '<' && i + 1 < n && t[i + 1] == '#') { size_t e = i + 2; while (e + 1 < n && !(t[e] == '#' && t[e + 1] == '>')) e++; e = e + 2 < n ? e + 2 : n; cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue; }
        if (slash && c == '/' && i + 1 < n && t[i + 1] == '/') { size_t e = i; while (e < n && t[e] != '\n') e++; cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue; }
        if (slash && c == '/' && i + 1 < n && t[i + 1] == '*') {
            size_t e = i + 2; int d = 1;
            while (e < n && d > 0) { if (t[e] == '*' && e + 1 < n && t[e + 1] == '/') { d--; e += 2; } else if (nest && t[e] == '/' && e + 1 < n && t[e + 1] == '*') { d++; e += 2; } else e++; }
            if (e > n) e = n;
            cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue;
        }
        if (hash && c == '#') {
            int ok = L == CL_PY || i == 0 || strchr(" \t\n\r;|&(", t[i - 1]);
            if (ok) { size_t e = i; while (e < n && t[e] != '\n') e++; cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue; }
        }
        if ((L == CL_SH || L == CL_PS1) && (c == '\\' || (L == CL_PS1 && c == '`'))) { if (i + 1 < n && t[i + 1] != '\n' && t[i + 1] != '\r') i += 2; else i++; last = 'a'; continue; }
        if (c == '"' || c == '\'') {
            char q = c;
            if (L == CL_PY) {
                int raw = (i > 0 && (t[i - 1] == 'r' || t[i - 1] == 'R')) || (i > 1 && (t[i - 2] == 'r' || t[i - 2] == 'R') && isalpha((unsigned char)t[i - 1]));
                if (i + 2 < n && t[i + 1] == q && t[i + 2] == q) {
                    size_t e = cp_tri_end(t, n, i + 3, q, !raw);
                    cp_blank(cl, i, e + 3 < n ? e + 3 : n); i = e + 3 < n ? e + 3 : n; last = 'a'; continue;
                }
                size_t e = cp_str_end(t, n, i + 1, q, !raw, 0, 0); if (e > n) e = n; cp_blank(cl, i + 1, e); i = e < n && t[e] == q ? e + 1 : e; last = 'a'; continue;
            }
            if (q == '"' && (L == CL_JAVA || L == CL_KT) && i + 2 < n && t[i + 1] == '"' && t[i + 2] == '"') {
                if (L == CL_KT && sn < 23) { st[sn++] = (CpStk){1, 0, 3, 1, 0, '"'}; i += 3; continue; }
                size_t e = cp_tri_end(t, n, i + 3, '"', 1);
                cp_blank(cl, i + 3, e); i = e + 3 < n ? e + 3 : n; last = 'a'; continue;
            }
            if (q == '"' && L == CL_KT && sn < 23) { st[sn++] = (CpStk){1, 0, 1, 0, 1, '"'}; i++; continue; }
            if (q == '\'' && (L == CL_C || L == CL_CPP || L == CL_JAVA || L == CL_KT || L == CL_GO || L == CL_RS)) {
                size_t l = (i > 0 && isdigit((unsigned char)t[i - 1])) ? 0 : cp_charlit(t, n, i);
                if (l) { cp_blank(cl, i + 1, i + l - 1); i += l; last = 'a'; } else i++;
                continue;
            }
            int ml = L == CL_SH || L == CL_PS1 || L == CL_RS, esc = !(L == CL_SH && q == '\'') && L != CL_PS1, dbl = L == CL_PS1;
            size_t e = cp_str_end(t, n, i + 1, q, esc, ml, dbl); if (e > n) e = n;
            cp_blank(cl, i + 1, e); i = e < n && t[e] == q ? e + 1 : e; last = 'a'; continue;
        }
        if (c == '`') {
            if (js && sn < 23) { st[sn++] = (CpStk){1, 0, 1, 1, 1, '`'}; i++; continue; }
            if (L == CL_GO) { size_t e = i + 1; while (e < n && t[e] != '`') e++; cp_blank(cl, i + 1, e); i = e < n ? e + 1 : n; last = 'a'; continue; }
            if (L == CL_KT) { size_t e = i + 1; while (e < n && t[e] != '`' && t[e] != '\n') e++; for (size_t k = i; k <= e && k < n; k++) if (!isalnum((unsigned char)t[k])) cl[k] = '_'; i = e < n ? e + 1 : n; last = 'a'; continue; }
        }
        if (L == CL_RS && (c == 'r' || (c == 'b' && i + 1 < n && t[i + 1] == 'r')) && (i == 0 || !cp_idc((unsigned char)t[i - 1]))) {
            size_t j = i + (c == 'b');
            if (j < n && t[j] == 'r') {
                size_t k = j + 1; int h = 0; while (k < n && t[k] == '#') { h++; k++; }
                if (k < n && t[k] == '"') {
                    size_t e = k + 1;
                    for (; e < n; e++) if (t[e] == '"') { int ok = 1; for (int z = 1; z <= h; z++) if (e + z >= n || t[e + z] != '#') { ok = 0; break; } if (ok) break; }
                    size_t end = e < n ? e + 1 + h : n; if (end > n) end = n;
                    cp_blank(cl, i, end); cl[k] = '"'; if (e < n) cl[e] = '"'; i = end; last = 'a'; continue;
                }
            }
        }
        if (L == CL_SH && c == '$' && i + 1 < n) {
            if (t[i + 1] == '{') { int d = 0; size_t e = i + 1; for (; e < n; e++) { if (t[e] == '{') d++; else if (t[e] == '}' && --d == 0) break; } cp_blank(cl, i, e < n ? e + 1 : n); cl[i] = '_'; i = e < n ? e + 1 : n; last = 'a'; continue; }
            if (t[i + 1] == '\'') { size_t e = cp_str_end(t, n, i + 2, '\'', 1, 1, 0); cp_blank(cl, i + 2, e); i = e < n ? e + 1 : n; last = 'a'; continue; }
        }
        if (L == CL_SH && c == '<' && i + 2 < n && t[i + 1] == '<' && t[i + 2] != '<' && (i == 0 || t[i - 1] != '<') && hn < 4) {
            size_t j = i + 2; int dash = 0; if (t[j] == '-') { dash = 1; j++; }
            while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;
            if (j < n && (t[j] == '\'' || t[j] == '"' || t[j] == '\\')) j++;
            size_t s = j; while (j < n && (isalnum((unsigned char)t[j]) || t[j] == '_' || t[j] == '.' || t[j] == '-')) j++;
            if (j > s && j - s < 63 && !isdigit((unsigned char)t[s])) { memcpy(hd[hn], t + s, j - s); hd[hn][j - s] = 0; hdash[hn] = dash; hn++; if (j < n && (t[j] == '\'' || t[j] == '"')) j++; i = j; continue; }
        }
        if (L == CL_PS1 && c == '@' && i + 1 < n && (t[i + 1] == '"' || t[i + 1] == '\'')) {
            char q = t[i + 1]; size_t j = i + 2; while (j < n && (t[j] == ' ' || t[j] == '\r')) j++;
            if (j < n && t[j] == '\n') { size_t e = j; while (e + 2 < n && !(t[e] == '\n' && t[e + 1] == q && t[e + 2] == '@')) e++; e = e + 3 < n ? e + 3 : n; cp_blank(cl, i, e); i = e; last = 'a'; continue; }
        }
        if (js && c == '/' && cp_regex_ok(last, lw)) {
            size_t e = i + 1; int cls = 0, okr = 0;
            for (; e < n && t[e] != '\n'; e++) {
                if (t[e] == '\\') { e++; continue; }
                if (t[e] == '[') cls = 1; else if (t[e] == ']') cls = 0; else if (t[e] == '/' && !cls) { okr = 1; break; }
            }
            if (okr && e > i + 1) { cp_blank(cl, i, e + 1); i = e + 1; last = 'a'; continue; }
        }
        if (isalnum((unsigned char)c) || c == '_') {
            if (i > 0 && (isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_')) { if (lwl < 15) lw[lwl++] = c; } else { lwl = 0; lw[lwl++] = c; }
            lw[lwl] = 0;
        }
        last = c; i++;
    }
}

/* ---------- palavras-chave ---------- */
static const char *const CP_NOCALL[] = {"if", "else", "while", "for", "switch", "case", "catch", "return", "sizeof", "typeof", "alignof", "decltype", "defined", "do", "try",
    "finally", "with", "elif", "except", "assert", "not", "and", "or", "in", "is", "lambda", "function", "func", "fun", "fn", "super", "this", "static_cast", "dynamic_cast",
    "reinterpret_cast", "const_cast", "static_assert", "_Static_assert", "alignas", "_Alignas", "noexcept", "throw", "throws", "synchronized", "using", "lock", "fixed", "foreach",
    "unless", "until", "select", "typedef", "extern", "__attribute__", "__attribute", "__declspec", "import", "require", "export", "elseif", "param", "trap", "when", "match",
    "await", "yield", "instanceof", "delete", "void", "new", "raise", "print_", "_Generic", "operator", "constructor", "sizeof...", "async", "typeid", "co_await", "co_return", NULL};
static const char *const CP_PRE[] = {"return", "new", "else", "case", "throw", "await", "yield", "typeof", "in", "of", "delete", "do", "not", "and", "or", "is", "print", "defer", "go",
    "if", "elif", "while", "for", "until", "unless", "with", "from", "assert", "raise", "match", "switch", "select", "when", "then", "lazy", "instanceof", "co_await", "co_return", NULL};
static int cp_inl(const char *s, size_t l, const char *const *L) {
    for (int i = 0; L[i]; i++) if (strlen(L[i]) == l && !memcmp(L[i], s, l)) return 1;
    return 0;
}

/* ---------- acesso a tokens ---------- */
#define TK(c, i) ((c)->tk[i])
static size_t cp_tl(const CP *c, int i) { return c->tk[i].b - c->tk[i].a; }
static int cp_is(const CP *c, int i, const char *s) { size_t l = strlen(s); return i >= 0 && i < c->nt && cp_tl(c, i) == l && !memcmp(c->c + c->tk[i].a, s, l); }
static int cp_isci(const CP *c, int i, const char *s) {
    size_t l = strlen(s); if (!(i >= 0 && i < c->nt && cp_tl(c, i) == l)) return 0;
    for (size_t k = 0; k < l; k++) if (tolower((unsigned char)c->c[c->tk[i].a + k]) != tolower((unsigned char)s[k])) return 0;
    return 1;
}
static int cp_p(const CP *c, int i, char ch) { return i >= 0 && i < c->nt && c->tk[i].t == CT_P && cp_tl(c, i) == 1 && c->c[c->tk[i].a] == ch; }
static int cp_idt(const CP *c, int i) { return i >= 0 && i < c->nt && c->tk[i].t == CT_ID; }
static int cp_dirt(const CP *c, int i) { return c->tk[i].fl & (CF_DIR | CF_DEF); }
static size_t cp_end(const CP *c, int close) { return close >= 0 && close < c->nt ? c->tk[close].b : c->n; }   /* fim em bytes de um fecho (ou do arquivo) */
static int cp_mt(const CP *c, int open) { int m = c->mt[open]; return m < 0 || m > c->nt ? c->nt : m; }
static int cp_open(const CP *c, int i) { return c->tk[i].t == CT_P && cp_tl(c, i) == 1 && strchr("([{", c->c[c->tk[i].a]); }
static int cp_closer(const CP *c, int i) { return c->tk[i].t == CT_P && cp_tl(c, i) == 1 && strchr(")]}", c->c[c->tk[i].a]); }
static int cp_skipgrp(const CP *c, int i, int b) { int m = cp_mt(c, i); return m + 1 > b ? b : m + 1; }   /* indice depois do grupo que abre em i */
static int cp_line(const CP *c, size_t off) { int lo = 0, hi = c->nln - 1; while (lo < hi) { int m = (lo + hi + 1) / 2; if (c->ln[m] <= off) lo = m; else hi = m - 1; } return lo + 1; }
static int cp_kwtok(const CP *c, int i, const char *const *L) { return cp_idt(c, i) && cp_inl(c->c + c->tk[i].a, cp_tl(c, i), L); }
static char *cp_dup(const char *s, size_t l) { char *r = (char *)malloc(l + 1); memcpy(r, s, l); r[l] = 0; return r; }
static char *cp_tokname(const CP *c, int i) {    /* texto do token sem crases (identificadores `assim` de Kotlin) */
    const char *s = c->t + c->tk[i].a; size_t l = cp_tl(c, i);
    if (l >= 2 && s[0] == '`') { s++; l -= 1; if (l && s[l - 1] == '`') l--; }
    return cp_dup(s, l);
}

/* ---------- tokenizador ---------- */
static void cp_tokadd(CP *c, size_t a, size_t b, int t, int fl) {
    if (c->nt >= c->tcap) { c->tcap = c->tcap * 2 + 64; c->tk = (CpTok *)realloc(c->tk, (size_t)c->tcap * sizeof(CpTok)); }
    c->tk[c->nt++] = (CpTok){(uint32_t)a, (uint32_t)b, (uint8_t)t, (uint8_t)fl};
}
static void cp_macro_end(CP *c, size_t b0, int nametok);
static void cp_tokenize(CP *c) {
    const char *s = c->c; size_t n = c->n; int L = c->lang;
    const int cdir = L == CL_C || L == CL_CPP, contbs = cdir || L == CL_SH || L == CL_PS1, sh = L == CL_SH, ps = L == CL_PS1;
    int nl = 1, bol = 1, dir = 0, cont = 0, defname = -1; size_t dirb0 = 0; int dirtok0 = 0;
    c->tcap = (int)(n / 4) + 64; c->tk = (CpTok *)malloc((size_t)c->tcap * sizeof(CpTok)); c->nt = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') {
            if (cont) cont = 0; else { nl = 1; bol = 1; if (dir) { if (dir == 2) cp_macro_end(c, dirb0, defname); dir = 0; defname = -1; } }
            i++; continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r') { i++; continue; }
        if (contbs && ((ch == '\\' && (L != CL_PS1)) || (ps && ch == '`')) && (i + 1 < n && (s[i + 1] == '\n' || (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n')))) { cont = 1; i++; continue; }
        int fl = (nl ? CF_NL : 0) | (dir == 2 ? CF_DEF : dir ? CF_DIR : 0);
        if (cdir && ch == '#' && bol) { dir = 1; dirb0 = i; dirtok0 = c->nt; fl |= CF_DIR; cp_tokadd(c, i, i + 1, CT_P, fl); i++; nl = 0; bol = 0; continue; }
        bol = 0;
        size_t a = i; int t;
        int idstart = isalpha(ch) || ch == '_' || ch >= 0x80 || (ch == '$' && !sh && !ps);
        if (ps && ch == '$' && i + 1 < n && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '_' || s[i + 1] == '?')) idstart = 1;
        if (idstart) {
            i++;
            for (;;) {
                if (i >= n) break;
                unsigned char d = (unsigned char)s[i];
                if (isalnum(d) || d == '_' || d >= 0x80 || (d == '$' && !sh && !ps)) { i++; continue; }
                if (sh && (d == '-' || d == '.') && i + 1 < n && (isalnum((unsigned char)s[i + 1]) || s[i + 1] == '_')) { i++; continue; }
                if (ps && d == '-' && i + 1 < n && isalpha((unsigned char)s[i + 1]) && isalnum((unsigned char)s[i - 1])) { i++; continue; }
                if (ps && d == ':' && s[a] == '$' && i + 1 < n && isalpha((unsigned char)s[i + 1])) { i++; continue; }
                break;
            }
            t = CT_ID;
            if (dir == 1 && c->nt == dirtok0 + 1 && i - a == 6 && !memcmp(s + a, "define", 6)) { dir = 2; fl = (fl & ~CF_DIR) | CF_DEF; c->tk[dirtok0].fl = (c->tk[dirtok0].fl & ~CF_DIR) | CF_DEF; defname = -2; }
            else if (dir == 2 && defname == -2 && c->nt == dirtok0 + 2) defname = c->nt;
        } else if (isdigit(ch) || (ch == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            i++; while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_' || (s[i] == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])))) i++;
            t = CT_NUM;
        } else {
            t = CT_P; i++;
            if (i < n) {
                char d = s[i];
                if ((ch == ':' && (d == ':' || (L == CL_GO && d == '='))) || (ch == '-' && (d == '>' || d == '-')) || (ch == '=' && (d == '>' || d == '=')) || (ch == '!' && d == '=') ||
                    (ch == '<' && d == '=') || (ch == '>' && d == '=') || (ch == '&' && d == '&') || (ch == '|' && d == '|') || (ch == '+' && d == '+') || (ch == '?' && d == '.') || (ch == '.' && d == '.')) i++;
            }
        }
        cp_tokadd(c, a, i, t, fl); nl = 0;
    }
    if (dir == 2) cp_macro_end(c, dirb0, defname);
}
static void cp_match(CP *c) {
    int *stk = (int *)malloc(sizeof(int) * (size_t)(c->nt + 1)); int sp = 0;
    c->mt = (int *)malloc(sizeof(int) * (size_t)(c->nt + 1));
    for (int i = 0; i < c->nt; i++) c->mt[i] = -1;
    for (int i = 0; i < c->nt; i++) {
        if (c->tk[i].fl & (CF_DIR | CF_DEF)) continue;
        if (c->tk[i].t != CT_P || cp_tl(c, i) != 1) continue;
        char ch = c->c[c->tk[i].a];
        if (ch == '(' || ch == '[' || ch == '{') { stk[sp++] = i; continue; }
        if (ch != ')' && ch != ']' && ch != '}') continue;
        char want = ch == ')' ? '(' : ch == ']' ? '[' : '{';
        int k = sp - 1; while (k >= 0 && c->c[c->tk[stk[k]].a] != want) k--;
        if (k < 0) continue;                                      /* fecho sobrando: ignorado */
        for (int z = sp - 1; z > k; z--) c->mt[stk[z]] = i;       /* aberturas internas sem fecho: terminam aqui */
        c->mt[stk[k]] = i; c->mt[i] = stk[k]; sp = k;
    }
    for (int z = 0; z < sp; z++) c->mt[stk[z]] = c->nt;
    free(stk);
}

/* ---------- simbolos e referencias ---------- */
static char *cp_sig(const CP *c, size_t a, size_t b, size_t ea, size_t eb) {
    char buf[200]; int o = 0, sp = 1;
    for (int pass = 0; pass < 2; pass++) {
        size_t x = pass ? ea : a, y = pass ? eb : b;
        if (pass && (eb <= ea)) break;
        if (pass && o < 190) { buf[o++] = ' '; sp = 1; }
        for (size_t k = x; k < y && o < 160; k++) {
            unsigned char ch = (unsigned char)c->t[k];
            if (ch == '\\' && k + 1 < y && (c->t[k + 1] == '\n' || c->t[k + 1] == '\r')) continue;
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { if (!sp) { buf[o++] = ' '; sp = 1; } continue; }
            buf[o++] = (char)ch; sp = 0;
        }
    }
    while (o > 0 && buf[o - 1] == ' ') o--;
    if (o >= 160) while (o > 0 && ((unsigned char)buf[o - 1] & 0xC0) == 0x80) o--;
    return cp_dup(buf, (size_t)o);
}
/* adiciona simbolo; nametok = token do nome (ou -1); nm/nl = nome explicito (ou NULL); sa..sb = trecho da assinatura (ea..eb extra) */
static int cp_symx(CP *c, int kind, int parent, size_t b0, size_t b1, int nametok, const char *nm, size_t nl, size_t sa, size_t sb, size_t ea, size_t eb) {
    CoderFile *f = c->f;
    if (f->nsym >= c->scap) {
        c->scap = c->scap * 2 + 32;
        f->sym = (CoderSym *)realloc(f->sym, (size_t)c->scap * sizeof(CoderSym)); c->snt = (int *)realloc(c->snt, (size_t)c->scap * sizeof(int));
        c->sfl = (unsigned char *)realloc(c->sfl, (size_t)c->scap); c->sx = (char **)realloc(c->sx, (size_t)c->scap * sizeof(char *));
    }
    if (b1 > c->n) b1 = c->n;
    if (b1 < b0) b1 = b0;
    CoderSym *s = &f->sym[f->nsym];
    s->name = nm ? cp_dup(nm, nl) : nametok >= 0 ? cp_tokname(c, nametok) : cp_dup("?", 1);
    s->sig = cp_sig(c, sa, sb > sa ? sb : sa, ea, eb);
    s->kind = kind; s->parent = parent; s->byte0 = b0; s->byte1 = b1;
    s->body0 = 0; s->vis = CV_UNKNOWN; s->flags = 0; s->conf = CC_EXACT; s->shape = 0;
    s->line0 = cp_line(c, b0); s->line1 = b1 > b0 ? cp_line(c, b1 - 1) : s->line0;
    c->snt[f->nsym] = nametok; c->sfl[f->nsym] = 0; c->sx[f->nsym] = NULL;
    if (nametok >= 0 && c->isn) c->isn[nametok] = 1;
    return f->nsym++;
}
static int cp_sym(CP *c, int kind, int parent, size_t b0, size_t b1, int nametok, size_t sa, size_t sb) { return cp_symx(c, kind, parent, b0, b1, nametok, NULL, 0, sa, sb, 0, 0); }
static void cp_setend(CP *c, int si, size_t b1) {
    CoderSym *s = &c->f->sym[si]; if (b1 > c->n) b1 = c->n; if (b1 < s->byte0) b1 = s->byte0;
    s->byte1 = b1; s->line1 = b1 > s->byte0 ? cp_line(c, b1 - 1) : s->line0;
}
static void cp_macro_end(CP *c, size_t b0, int nametok) {
    if (nametok < 0 || nametok >= c->nt) return;
    cp_sym(c, CK_MACRO, -1, b0, c->tk[c->nt - 1].b, nametok, b0, c->tk[c->nt - 1].b);
}
static uint32_t cp_hash(const char *s, size_t l, uint32_t h) { for (size_t i = 0; i < l; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; } return h; }
static void cp_rehash(CP *c) {
    free(c->rh); c->rhn = 1024; while (c->rhn < c->f->nref * 4) c->rhn *= 2;
    c->rh = (int *)calloc((size_t)c->rhn, sizeof(int));
    for (int r = 0; r < c->f->nref; r++) {
        const CoderRef *x = &c->f->ref[r]; uint32_t h = cp_hash(x->name, strlen(x->name), (uint32_t)(x->from + 7) * 2654435761u + (uint32_t)x->kind);
        size_t p = h & (size_t)(c->rhn - 1); while (c->rh[p]) p = (p + 1) & (size_t)(c->rhn - 1); c->rh[p] = r + 1;
    }
}
static void cp_ref(CP *c, int from, int kind, const char *nm, size_t nl, size_t off) {
    CoderFile *f = c->f; if (!nl) return;
    if (!c->rh || f->nref * 3 >= c->rhn) cp_rehash(c);
    uint32_t h = cp_hash(nm, nl, (uint32_t)(from + 7) * 2654435761u + (uint32_t)kind);
    size_t p = h & (size_t)(c->rhn - 1);
    while (c->rh[p]) { const CoderRef *x = &f->ref[c->rh[p] - 1]; if (x->from == from && x->kind == kind && strlen(x->name) == nl && !memcmp(x->name, nm, nl)) return; p = (p + 1) & (size_t)(c->rhn - 1); }
    if (f->nref >= c->rcap) { c->rcap = c->rcap * 2 + 16; f->ref = (CoderRef *)realloc(f->ref, (size_t)c->rcap * sizeof(CoderRef)); }
    { int ln = cp_line(c, off); size_t ls = ln >= 1 && ln <= c->nln ? c->ln[ln - 1] : 0; f->ref[f->nref] = (CoderRef){from, kind, ln, cp_dup(nm, nl), off >= ls ? (int)(off - ls) + 1 : 0, 0}; }
    c->rh[p] = ++f->nref;
}
static void cp_reft(CP *c, int from, int kind, int tok) { char *nm = cp_tokname(c, tok); cp_ref(c, from, kind, nm, strlen(nm), c->tk[tok].a); free(nm); }

/* ---------- motor de varredura por instrucoes ---------- */
static int cp_decl(CP *c, int i, int b, int parent, int mode, int dp);
static int cp_strpair(const CP *c, int j);
static void cp_scan(CP *c, int a, int b, int parent, int mode, int dp);
static const char *const CP_NOEND[] = {"in", "of", "instanceof", "typeof", "new", "delete", "void", "extends", "implements", "as", "is", "else", "do", "async", "await", "export", "default", "static",
    "get", "set", "public", "private", "protected", "readonly", "abstract", "declare", "function", "class", "const", "let", "var", "val", "fun", "type", "interface", "enum", "import", "throw",
    "case", "yield", "not", "and", "or", "return", "override", "open", "internal", "data", "sealed", "inline", "suspend", "operator", "infix", "companion", "object", "package", "namespace",
    "module", "where", "by", "if", "while", "for", "when", "try", "func", "struct", "fn", "pub", "mut", "then", "elif", "switch", "foreach", "until", "finally", "catch", NULL};
static const char *const CP_NOBEGIN[] = {"in", "of", "instanceof", "as", "is", "extends", "implements", "else", "catch", "finally", "where", "satisfies", "by", "and", "or", NULL};
static int cp_endsexpr(CP *c, int p) {
    const CpTok *x = &c->tk[p];
    if (x->t == CT_NUM) return 1;
    if (x->t == CT_ID) return !cp_inl(c->c + x->a, x->b - x->a, CP_NOEND);
    if (x->b - x->a == 1) return strchr(")]}\"'`", c->c[x->a]) != NULL;
    return cp_is(c, p, "++") || cp_is(c, p, "--");
}
/* ponto-e-virgula automatico: a quebra de linha antes do token j termina a instrucao? */
static int cp_asi(CP *c, int j) {
    int L = c->lang;
    if (!(L == CL_JS || L == CL_TS || L == CL_QML || L == CL_KT || L == CL_GO || L == CL_PS1 || L == CL_SH || L == CL_SWIFT || L == CL_SCALA || L == CL_GROOVY) || j <= 0 || !(c->tk[j].fl & CF_NL)) return 0;
    if (cp_dirt(c, j) || cp_dirt(c, j - 1) || cp_strpair(c, j - 1)) return 0;
    if (L == CL_SH) { const CpTok *x = &c->tk[j - 1]; if (x->t == CT_P && (strchr("|&(,{!", c->c[x->a]) || cp_is(c, j - 1, "&&") || cp_is(c, j - 1, "||"))) return 0; return 1; }
    if (!cp_endsexpr(c, j - 1)) return 0;
    const CpTok *y = &c->tk[j];
    if (y->t == CT_NUM) return 1;
    if (y->t == CT_ID) return !cp_inl(c->c + y->a, y->b - y->a, CP_NOBEGIN);
    return cp_p(c, j, '@');
}
static int cp_exprbrace(CP *c, int j, int i) {   /* '{' no indice j e abertura de expressao (objeto, lambda) e nao bloco? */
    if (j <= i) return 0;
    const CpTok *x = &c->tk[j - 1];
    if (x->t == CT_P) { if (x->b - x->a == 1) return strchr("=(,:?[!&|+-*/%<>^~\"'`", c->c[x->a]) != NULL; return cp_is(c, j - 1, "=>") || cp_is(c, j - 1, "->") || cp_is(c, j - 1, "&&") || cp_is(c, j - 1, "||") || cp_is(c, j - 1, "=="); }
    if (x->t == CT_ID) return cp_is(c, j - 1, "return") || cp_is(c, j - 1, "in") || cp_is(c, j - 1, "of") || cp_is(c, j - 1, "typeof") || cp_is(c, j - 1, "await") || cp_is(c, j - 1, "yield");
    return 0;
}
/* pula uma instrucao que nenhum reconhecedor quis: devolve o indice seguinte (> i) */
static int cp_skip(CP *c, int i, int b, int parent, int mode, int dp) {
    int j = i;
    while (j < b) {
        if (j > i && cp_asi(c, j)) return j;
        if (cp_dirt(c, j)) { j++; continue; }
        if (cp_p(c, j, ';')) return j + 1;
        if ((mode == CM_ARR || mode == CM_ENUM || mode == CM_OBJ) && cp_p(c, j, ',')) return j + 1;
        if (cp_p(c, j, '(') || cp_p(c, j, '[')) {
            int e = cp_mt(c, j);
            if (c->nested && dp < 200 && e > j + 1) cp_scan(c, j + 1, e < b ? e : b, parent, c->lang == CL_QML && cp_p(c, j, '[') ? CM_ARR : CM_BODY, dp + 1);
            j = e + 1 > b ? b : e + 1; continue;
        }
        if (cp_p(c, j, '{')) {
            int e = cp_mt(c, j), ex = cp_exprbrace(c, j, i);
            if (c->nested && dp < 200 && e > j + 1) cp_scan(c, j + 1, e < b ? e : b, parent, CM_BODY, dp + 1);
            j = e + 1 > b ? b : e + 1;
            if (ex) continue;
            return j;
        }
        j++;
    }
    return j > i ? j : i + 1;
}
static void cp_scan(CP *c, int a, int b, int parent, int mode, int dp) {
    if (dp > 200) return;
    int i = a;
    while (i < b) {
        if (cp_dirt(c, i) || cp_p(c, i, ';') || cp_p(c, i, '}') || cp_p(c, i, ')') || cp_p(c, i, ']')) { i++; continue; }
        if ((mode == CM_ARR || mode == CM_ENUM || mode == CM_OBJ) && cp_p(c, i, ',')) { i++; continue; }
        int e = cp_decl(c, i, b, parent, mode, dp);
        if (e > i) { i = e > b ? b : e; continue; }
        i = cp_skip(c, i, b, parent, mode, dp);
    }
}
static int cp_angle(CP *c, int j, int b) {   /* j em '<': indice depois do '>' correspondente (ou j+1 se nao fecha) */
    int d = 0;
    for (int m = j; m < b; m++) {
        if (cp_dirt(c, m)) continue;
        if (cp_p(c, m, '(') || cp_p(c, m, '[') || cp_p(c, m, '{')) { m = cp_mt(c, m); if (m >= b) break; continue; }
        if (cp_p(c, m, '<')) d++;
        else if (cp_p(c, m, '>')) { if (--d == 0) return m + 1; }
        else if (cp_p(c, m, ';')) break;
    }
    return j + 1;
}
static void cp_body(CP *c, int open, int b, int sym) { /* analisa o interior de um corpo '{' para declaracoes aninhadas (so linguagens com funcoes locais) */
    if (c->nested) { int e = cp_mt(c, open); cp_scan(c, open + 1, e < b ? e : b, sym, CM_BODY, 1); }
}

/* ---------- C / C++ ---------- */
static const char *const CP_CLASSKW[] = {"class", "struct", "union", "enum", NULL};
static const char *const CP_ATTR[] = {"__attribute__", "__attribute", "__declspec", "alignas", "_Alignas", "DECLSPEC_ALIGN", "__pragma", NULL};
static const char *const CP_NOTVAR[] = {"return", "goto", "break", "continue", "using", "friend", "static_assert", "public", "private", "protected", "case", "default", "delete", "throw", "if", "else", "while", "for", "do", NULL};
static int cp_c_funcptr(CP *c, int lo, int hi) {   /* acha o padrao (*nome) entre lo e hi; devolve o token do nome */
    for (int j = lo; j < hi; j++) if (cp_p(c, j, '(') && (cp_p(c, j + 1, '*') || cp_p(c, j + 1, '^'))) {
        int m = cp_mt(c, j), nm = -1;
        for (int q = j + 2; q < m && q < hi; q++) { if (cp_idt(c, q) && !cp_is(c, q, "const")) nm = q; else if (cp_p(c, q, '[')) break; }
        if (nm >= 0) return nm;
    }
    return -1;
}
static int cp_c_varname(CP *c, int lo, int hi) {   /* nome de variavel no intervalo [lo,hi) (hi = antes do '=' ou ';') */
    int j = hi - 1;
    while (j >= lo && cp_p(c, j, ']')) { int o = c->mt[j]; if (o < lo || o >= j) return -1; j = o - 1; }
    if (j >= lo && cp_idt(c, j)) return j;
    return cp_c_funcptr(c, lo, hi);
}
static int cp_c_enumerator(CP *c, int i, int b, int parent) {
    if (!cp_idt(c, i)) return 0;
    int j = i; while (j < b && !cp_p(c, j, ',')) { if (cp_open(c, j)) { j = cp_skipgrp(c, j, b); continue; } j++; }
    cp_sym(c, CK_CONST, parent, c->tk[i].a, c->tk[j > i ? j - 1 : i].b, i, c->tk[i].a, c->tk[j > i ? j - 1 : i].b);
    return j + 1;
}
static int cp_c_type(CP *c, int i, int k, int kw, int e, int b, int parent, int td, size_t b0, int dp) {
    int kind = cp_is(c, kw, "class") ? CK_CLASS : cp_is(c, kw, "enum") ? CK_ENUM : CK_STRUCT;
    int j = kw + 1, nm = -1;
    if (cp_is(c, j, "class") || cp_is(c, j, "struct")) j++;
    for (; j < e;) {
        if (cp_kwtok(c, j, CP_ATTR)) { j++; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, e); continue; }
        if (cp_p(c, j, '[')) { j = cp_skipgrp(c, j, e); continue; }
        if (cp_idt(c, j)) { if (!cp_is(c, j, "final")) nm = j; j++; continue; }
        if (cp_p(c, j, '<')) { j = cp_angle(c, j, e); continue; }
        if (cp_is(c, j, "::")) { j++; continue; }
        break;
    }
    int colon = cp_p(c, j, ':') ? j : -1;
    int close = cp_mt(c, e), after = close + 1;
    /* declaradores depois do '}' */
    int semi = -1, alias = -1, vnm = -1;
    if (after <= b) {
        int q = after; int n = 0;
        while (q < b && n < 24 && !cp_p(c, q, ';') && !cp_p(c, q, '{') && !cp_p(c, q, '}')) { if (alias < 0 && cp_idt(c, q) && !cp_kwtok(c, q, CP_ATTR)) alias = q; if (cp_p(c, q, '(') || cp_p(c, q, '[')) q = cp_skipgrp(c, q, b); else q++; n++; }
        if (q < b && cp_p(c, q, ';')) semi = q; else alias = -1;
    }
    if (alias >= 0 && !td) { vnm = alias; alias = -1; }
    size_t endb = close < c->nt ? c->tk[close].b : c->n;
    char nbuf[8];
    int tsym;
    if (nm < 0 && td && alias >= 0) {
        tsym = cp_symx(c, kind, parent, b0, c->tk[semi].b, alias, NULL, 0, b0, c->tk[e].a, 0, 0); alias = -1;
    } else if (nm < 0) { snprintf(nbuf, sizeof nbuf, "(anon)"); tsym = cp_symx(c, kind, parent, b0, endb, -1, nbuf, 6, b0, c->tk[e].a, 0, 0); }
    else tsym = cp_sym(c, kind, parent, b0, endb, nm, b0, c->tk[e].a);
    if (colon >= 0 && kind != CK_ENUM) {
        int last = -1, ang = 0;
        for (int m = colon + 1; m <= e; m++) {
            if (m < e && cp_p(c, m, '<')) { ang++; continue; }
            if (m < e && cp_p(c, m, '>')) { ang--; continue; }
            if (m < e && cp_p(c, m, '(')) { m = cp_mt(c, m); continue; }
            if (ang > 0) continue;
            if (m == e || cp_p(c, m, ',')) { if (last >= 0) cp_reft(c, tsym, CR_EXTENDS, last); last = -1; continue; }
            if (cp_idt(c, m) && !cp_is(c, m, "public") && !cp_is(c, m, "private") && !cp_is(c, m, "protected") && !cp_is(c, m, "virtual")) last = m;
        }
    }
    cp_scan(c, e + 1, close < b ? close : b, tsym, kind == CK_ENUM ? CM_ENUM : CM_CLASS, dp + 1);
    if (alias >= 0) cp_sym(c, CK_TYPEDEF, parent, c->tk[close < c->nt ? close : c->nt - 1].a, c->tk[semi].b, alias, c->tk[close < c->nt ? close : c->nt - 1].a, c->tk[semi].b);
    if (vnm >= 0) cp_sym(c, CK_VAR, parent, c->tk[close < c->nt ? close : c->nt - 1].a, c->tk[semi].b, vnm, c->tk[vnm].a, c->tk[vnm].b);
    return semi >= 0 ? semi + 1 : after;
}
static int cp_c_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    if (mode == CM_BODY) return 0;
    if (mode == CM_ENUM) return cp_c_enumerator(c, i, b, parent);
    int k = i;
    if (mode == CM_CLASS && cp_idt(c, k)) {
        int e = k; while (cp_idt(c, e) && e < k + 3) e++;
        if (cp_p(c, e, ':')) {
            int ok = 1;
            for (int m = k; m < e; m++) if (!(cp_is(c, m, "public") || cp_is(c, m, "private") || cp_is(c, m, "protected") || cp_is(c, m, "signals") || cp_is(c, m, "slots") || cp_is(c, m, "Q_SIGNALS") || cp_is(c, m, "Q_SLOTS"))) ok = 0;
            if (ok) return e + 1;
        }
    }
    while (cp_is(c, k, "template") && cp_p(c, k + 1, '<')) { int m = cp_angle(c, k + 1, b); if (m <= k + 1) break; k = m; }
    int kk = k;
    if (cp_is(c, kk, "inline") && cp_is(c, kk + 1, "namespace")) kk++;
    if (cp_is(c, kk, "namespace")) {
        int j = kk + 1; while (j < b && (cp_idt(c, j) || cp_is(c, j, "::"))) j++;
        if (cp_p(c, j, '{')) {
            int close = cp_mt(c, j); const char *nmx = j > kk + 1 ? c->t + c->tk[kk + 1].a : "(anon)"; size_t nl = j > kk + 1 ? c->tk[j - 1].b - c->tk[kk + 1].a : 6;
            int s = cp_symx(c, CK_NAMESPACE, parent, c->tk[i].a, cp_end(c, close), -1, nmx, nl, c->tk[i].a, c->tk[j].a, 0, 0);
            cp_scan(c, j + 1, close < b ? close : b, s, CM_TOP, dp + 1);
            return close + 1;
        }
        return 0;
    }
    if (cp_is(c, kk, "extern") && cp_p(c, kk + 1, '"') && cp_p(c, kk + 2, '"')) {
        if (cp_p(c, kk + 3, '{')) { int close = cp_mt(c, kk + 3); cp_scan(c, kk + 4, close < b ? close : b, parent, CM_TOP, dp + 1); return close + 1; }
        k = kk + 3;
    }
    if (cp_is(c, k, "using") && cp_idt(c, k + 1) && cp_p(c, k + 2, '=')) {
        int j = k; while (j < b && !cp_p(c, j, ';')) { if (cp_open(c, j)) { j = cp_skipgrp(c, j, b); continue; } j++; }
        cp_sym(c, CK_TYPEDEF, parent, c->tk[i].a, j < b ? c->tk[j].b : c->tk[b - 1].b, k + 1, c->tk[i].a, c->tk[k + 2].a);
        return j + 1;
    }
    int eq = -1, kw = -1, term = 0, e = b, ctor = 0, pure = 0, oplo = -1, ophi = -1, np = 0, po[8], pc[8];
    for (int j = k; j < b;) {
        if (cp_dirt(c, j)) { j++; continue; }
        if (cp_p(c, j, ';')) { term = ';'; e = j; break; }
        if (cp_p(c, j, '{')) {
            if (eq >= 0 || (ctor && (cp_idt(c, j - 1) || cp_p(c, j - 1, '>')))) { j = cp_skipgrp(c, j, b); continue; }
            term = '{'; e = j; break;
        }
        if (cp_p(c, j, '}')) { term = '}'; e = j; break; }
        if (cp_p(c, j, '(')) { if (np < 8) { po[np] = j; pc[np] = cp_mt(c, j); np++; } j = cp_skipgrp(c, j, b); continue; }
        if (cp_p(c, j, '[')) { j = cp_skipgrp(c, j, b); continue; }
        if (cp_idt(c, j)) {
            if (kw < 0 && cp_kwtok(c, j, CP_CLASSKW)) kw = j;
            if (cp_is(c, j, "operator")) {
                oplo = j; j++;
                if (cp_p(c, j, '(') && cp_p(c, j + 1, ')')) j += 2; else { int cnt = 0; while (j < b && !cp_p(c, j, '(') && cnt < 8) { j++; cnt++; } }
                ophi = j; continue;
            }
            j++; continue;
        }
        if (cp_p(c, j, '=')) {
            if (np > 0 && eq < 0 && (cp_p(c, j - 1, ')') || cp_is(c, j - 1, "const") || cp_is(c, j - 1, "noexcept") || cp_is(c, j - 1, "override") || cp_is(c, j - 1, "final"))) pure = 1;
            else if (eq < 0) eq = j;
            j++; continue;
        }
        if (cp_p(c, j, ':') && np > 0 && kw < 0 && !ctor && (cp_p(c, j - 1, ')') || cp_idt(c, j - 1))) ctor = 1;
        j++;
    }
    size_t b0 = c->tk[i].a;
    int td = cp_is(c, i, "typedef");
    if (kw >= 0 && eq < 0 && term == '{') {
        int real = 0; for (int m = 0; m < np; m++) if (po[m] > kw && !cp_kwtok(c, po[m] - 1, CP_ATTR)) real = 1;
        if (!real) return cp_c_type(c, i, k, kw, e, b, parent, td, b0, dp);
    }
    size_t endb = term == ';' ? c->tk[e].b : e > k ? c->tk[e - 1].b : c->tk[k].b;
    if (td && term != '{') {
        int nm = cp_c_funcptr(c, k, e);
        if (nm < 0) nm = cp_c_varname(c, k + 1, e);
        if (nm >= 0) cp_sym(c, CK_TYPEDEF, parent, b0, endb, nm, b0, endb);
        return term == ';' ? e + 1 : e > i ? e : 0;
    }
    if (eq >= 0) {
        int nm = cp_c_varname(c, k, eq);
        if (nm < 0 || cp_kwtok(c, nm, CP_NOTVAR)) return 0;
        int cst = 0; for (int m = k; m < eq; m++) if (cp_is(c, m, "const") || cp_is(c, m, "constexpr") || cp_is(c, m, "constinit")) cst = 1;
        cp_sym(c, cst ? CK_CONST : CK_VAR, parent, b0, endb, nm, b0, c->tk[eq].a);
        return term == ';' ? e + 1 : e > i ? e : 0;
    }
    if (np > 0) {
        int fp = cp_c_funcptr(c, k, e);
        if (fp >= 0 && term != '{') { cp_sym(c, CK_VAR, parent, b0, endb, fp, b0, endb); return term == ';' ? e + 1 : e > i ? e : 0; }
        int pick = -1, skipend = -1;
        for (int m = 0; m < np; m++) {
            int o = po[m], pv = o - 1, ok = 0;
            if (oplo >= 0 && o == ophi) ok = 2;
            else if (pv >= k && cp_idt(c, pv) && !cp_kwtok(c, pv, CP_NOCALL) && !cp_kwtok(c, pv, CP_ATTR)) ok = 1;
            if (!ok) continue;
            int nx = pc[m] + 1;
            if (nx < e && cp_idt(c, nx) && !(cp_is(c, nx, "const") || cp_is(c, nx, "noexcept") || cp_is(c, nx, "override") || cp_is(c, nx, "final") || cp_is(c, nx, "volatile") || cp_is(c, nx, "mutable") ||
                                            cp_is(c, nx, "throw") || cp_is(c, nx, "requires") || cp_is(c, nx, "try") || cp_kwtok(c, nx, CP_ATTR))) { skipend = nx; continue; }
            if (nx < e && cp_p(c, nx, '*')) continue;
            pick = m; break;
        }
        if (pick < 0) return 0;
        int o = po[pick], cl_ = pc[pick], nameTok = o - 1, qual = -1, tilde = 0, startName;
        if (oplo >= 0 && o == ophi) nameTok = oplo;
        if (nameTok > k && cp_p(c, nameTok - 1, '~')) tilde = 1;
        startName = tilde ? nameTok - 1 : nameTok;
        int j = startName - 1;
        if (j >= k && cp_is(c, j, "::")) {
            j--;
            if (j >= k && cp_p(c, j, '>')) { int d = 0; while (j >= k) { if (cp_p(c, j, '>')) d++; else if (cp_p(c, j, '<') && --d == 0) break; j--; } j--; }
            if (j >= k && cp_idt(c, j)) qual = j;
        }
        int first = (qual < 0 && startName == k) || (qual >= 0 && 0);
        char *nmtxt = NULL; size_t nml = 0; const char *nm = NULL;
        if (nameTok == oplo) { nm = c->t + c->tk[oplo].a; nml = c->tk[ophi - 1].b - c->tk[oplo].a; }
        else if (tilde) { nm = c->t + c->tk[nameTok - 1].a; nml = c->tk[nameTok].b - c->tk[nameTok - 1].a; }
        (void)nmtxt;
        if (first) {
            if (mode == CM_TOP && term != '{') return 0;
            if (mode == CM_CLASS) {
                if (parent < 0) return 0;
                const char *pn = c->f->sym[parent].name;
                if (strlen(pn) != cp_tl(c, nameTok) || memcmp(pn, c->c + c->tk[nameTok].a, cp_tl(c, nameTok))) return 0;
            }
            if (mode == CM_TOP && term == '{' && cp_kwtok(c, nameTok, CP_NOTVAR)) return 0;
        }
        if (skipend >= 0 && skipend <= startName) { b0 = c->tk[skipend].a; first = 0; }
        int par = parent;
        if (qual >= 0 && mode != CM_CLASS) {
            size_t ql = cp_tl(c, qual);
            for (int s = c->f->nsym - 1; s >= 0; s--) { int kd = c->f->sym[s].kind; if ((kd == CK_CLASS || kd == CK_STRUCT) && strlen(c->f->sym[s].name) == ql && !memcmp(c->f->sym[s].name, c->c + c->tk[qual].a, ql)) { par = s; break; } }
        }
        int lastq = cl_ < c->nt ? cl_ : c->nt - 1;
        for (int q = cl_ + 1; q < e && q < cl_ + 8; q++) { if (cp_is(c, q, "const") || cp_is(c, q, "noexcept") || cp_is(c, q, "override") || cp_is(c, q, "final") || cp_is(c, q, "volatile") || cp_p(c, q, '&') || cp_is(c, q, "&&")) lastq = q; else break; }
        size_t b1 = term == '{' ? cp_end(c, cp_mt(c, e)) : endb;
        int s = cp_symx(c, (mode == CM_CLASS || qual >= 0) ? CK_METHOD : CK_FUNC, par, b0, b1, nm ? -1 : nameTok, nm, nml, b0, c->tk[lastq].b, 0, 0);
        if (nm) { c->snt[s] = nameTok; c->isn[nameTok] = 1; }
        if (term != '{') c->sfl[s] |= 1;
        if (term == '{') return cp_mt(c, e) + 1 > b ? b : cp_mt(c, e) + 1;
        return term == ';' ? e + 1 : e > i ? e : 0;
    }
    if (term == ';' || (term == 0 && e > k)) {
        int hi = e;
        for (int m = k; m < e; m++) { if (cp_p(c, m, '[')) { m = cp_mt(c, m); continue; } if (cp_p(c, m, ',') || (cp_p(c, m, ':') && m > k)) { hi = m; break; } }
        int nm = cp_c_varname(c, k, hi);
        if (nm < 0 || nm == k || cp_kwtok(c, k, CP_NOTVAR) || cp_kwtok(c, nm, CP_NOTVAR)) return term == ';' ? e + 1 : 0;
        if (nm - 1 == kw) return e + 1;
        int cst = 0; for (int m = k; m < hi; m++) if (cp_is(c, m, "const") || cp_is(c, m, "constexpr")) cst = 1;
        cp_sym(c, cst ? CK_CONST : CK_VAR, parent, b0, endb, nm, b0, endb);
        return term == ';' ? e + 1 : e;
    }
    return 0;
}

/* ---------- utilidades de texto/tokens para os reconhecedores ---------- */
static int cp_tokat(const CP *c, size_t off) { int lo = 0, hi = c->nt; while (lo < hi) { int m = (lo + hi) / 2; if (c->tk[m].a < off) lo = m + 1; else hi = m; } return lo; }
static int cp_strpair(const CP *c, int j) {   /* tokens j,j+1 = aspas simples/duplas/crase de uma string sem interpolacao */
    return j >= 0 && j + 1 < c->nt && c->tk[j].t == CT_P && cp_tl(c, j) == 1 && strchr("\"'`", c->c[c->tk[j].a]) && cp_tl(c, j + 1) == 1 && c->c[c->tk[j + 1].a] == c->c[c->tk[j].a];
}
static void cp_ref_range(CP *c, int from, int kind, size_t a, size_t b, int nospace) {   /* referencia com o texto de [a,b) (sem espacos nas pontas; opcional sem espacos internos) */
    char buf[256]; size_t o = 0;
    while (a < b && isspace((unsigned char)c->t[a])) a++;
    while (b > a && isspace((unsigned char)c->t[b - 1])) b--;
    for (size_t k = a; k < b && o < 250; k++) { if (nospace && isspace((unsigned char)c->t[k])) continue; buf[o++] = c->t[k]; }
    if (o) cp_ref(c, from, kind, buf, o, a);
}
static void cp_import_str(CP *c, int from, int j) { if (cp_strpair(c, j)) cp_ref_range(c, from, CR_IMPORT, c->tk[j].b, c->tk[j + 1].a, 0); }
/* lista de nomes de heranca em [lo,hi): separados por ',' (profundidade de '<>' zero); cada item vira a ref com o ultimo identificador */
static void cp_heritage(CP *c, int lo, int hi, int sym, int kind) {
    int last = -1, ang = 0;
    for (int m = lo; m <= hi; m++) {
        if (m < hi && cp_p(c, m, '<')) { ang++; continue; }
        if (m < hi && cp_p(c, m, '>')) { if (ang > 0) ang--; continue; }
        if (m < hi && (cp_p(c, m, '(') || cp_p(c, m, '['))) { m = cp_mt(c, m); continue; }
        if (ang > 0) continue;
        if (m == hi || cp_p(c, m, ',')) { if (last >= 0) cp_reft(c, sym, kind, last); last = -1; continue; }
        if (cp_idt(c, m) && !cp_is(c, m, "public") && !cp_is(c, m, "private") && !cp_is(c, m, "protected") && !cp_is(c, m, "virtual")) last = m;
    }
}

/* ---------- Python (por linha e indentacao) ---------- */
static void cp_py(CP *c) {
    const char *s = c->c; size_t n = c->n, pos = 0, lastend = 0, decstart = (size_t)-1;
    struct { int indent, sym; } stk[160]; int sp = 0;
    while (pos < n) {
        size_t ls = pos; int ind = 0;
        while (ls < n && (s[ls] == ' ' || s[ls] == '\t')) { ind += s[ls] == '\t' ? 8 - (ind % 8) : 1; ls++; }
        if (ls >= n) break;
        if (s[ls] == '\n' || s[ls] == '\r') { pos = ls + 1; continue; }
        size_t e = ls; int depth = 0;
        while (e < n) {
            char ch = s[e];
            if (ch == '(' || ch == '[' || ch == '{') depth++;
            else if (ch == ')' || ch == ']' || ch == '}') { if (depth > 0) depth--; }
            else if (ch == '\\' && e + 1 < n && (s[e + 1] == '\n' || s[e + 1] == '\r')) { e += 2; if (e < n && s[e - 1] == '\r' && s[e] == '\n') e++; continue; }
            else if (ch == '\n' && depth == 0) break;
            e++;
        }
        size_t ce = e; while (ce > ls && isspace((unsigned char)s[ce - 1])) ce--;
        pos = e < n ? e + 1 : n;
        if (ce <= ls) continue;
        while (sp > 0 && stk[sp - 1].indent >= ind) { cp_setend(c, stk[sp - 1].sym, lastend); sp--; }
        int parent = sp > 0 ? stk[sp - 1].sym : -1;
        const char *p = s + ls; size_t ll = ce - ls;
        if (p[0] == '@') { if (decstart == (size_t)-1) decstart = ls; lastend = ce; continue; }
        size_t dp0 = ls; int isasync = 0;
        if (ll > 6 && !strncmp(p, "async", 5) && (p[5] == ' ' || p[5] == '\t')) { isasync = 1; size_t q = 5; while (q < ll && (p[q] == ' ' || p[q] == '\t')) q++; p += q; ll -= q; dp0 = ls + q; }
        (void)isasync;
        int isdef = ll > 4 && !strncmp(p, "def", 3) && (p[3] == ' ' || p[3] == '\t'), iscls = ll > 6 && !strncmp(p, "class", 5) && (p[5] == ' ' || p[5] == '\t');
        if (isdef || iscls) {
            size_t q = isdef ? 3 : 5; while (q < ll && (p[q] == ' ' || p[q] == '\t')) q++;
            size_t nb = q; while (q < ll && cp_idc((unsigned char)p[q])) q++;
            if (q > nb) {
                size_t nameoff = dp0 + nb, hc = 0; int d = 0;
                for (size_t z = q; z < ll; z++) { char ch = p[z]; if (ch == '(' || ch == '[' || ch == '{') d++; else if (ch == ')' || ch == ']' || ch == '}') d--; else if (ch == ':' && d <= 0) { hc = z; break; } }
                size_t b0 = decstart != (size_t)-1 ? decstart : ls;
                int kind = iscls ? CK_CLASS : (parent >= 0 && c->f->sym[parent].kind == CK_CLASS ? CK_METHOD : CK_FUNC);
                int nt0 = cp_tokat(c, nameoff);
                int sym = cp_sym(c, kind, parent, b0, ce, nt0 < c->nt ? nt0 : -1, dp0, hc ? dp0 + hc : ce);
                if (iscls) {
                    int op = cp_tokat(c, dp0 + q);
                    if (op < c->nt && cp_p(c, op, '(') && c->tk[op].a < ce) { int cl_ = cp_mt(c, op); int lo = op + 1, last = -1, hasq = 0;
                        for (int m = lo; m <= cl_ && m < c->nt; m++) {
                            if (m < cl_ && (cp_p(c, m, '(') || cp_p(c, m, '['))) { m = cp_mt(c, m); continue; }
                            if (m == cl_ || cp_p(c, m, ',')) { if (last >= 0 && !hasq) cp_reft(c, sym, CR_EXTENDS, last); last = -1; hasq = 0; continue; }
                            if (cp_p(c, m, '=')) hasq = 1; else if (cp_idt(c, m)) last = m;
                        } }
                }
                decstart = (size_t)-1;
                int one = 0; if (hc) for (size_t z = hc + 1; z < ll; z++) if (!isspace((unsigned char)p[z])) { one = 1; break; }
                if (!one && sp < 158) { stk[sp].indent = ind; stk[sp].sym = sym; sp++; }
                lastend = ce; continue;
            }
        }
        decstart = (size_t)-1;
        if (ll > 7 && !strncmp(p, "import", 6) && (p[6] == ' ' || p[6] == '\t')) {
            size_t q = 6;
            while (q < ll) {
                while (q < ll && (p[q] == ' ' || p[q] == '\t' || p[q] == ',' || p[q] == '(')) q++;
                size_t a = q; while (q < ll && (cp_idc((unsigned char)p[q]) || p[q] == '.')) q++;
                if (q > a) cp_ref(c, parent, CR_IMPORT, p + a, q - a, ls + a);
                if (q + 3 < ll && !strncmp(p + q, " as ", 4)) { q += 4; while (q < ll && cp_idc((unsigned char)p[q])) q++; }
                else if (q >= ll || (p[q] != ',' && p[q] != ' ')) break;
            }
        } else if (ll > 5 && !strncmp(p, "from", 4) && (p[4] == ' ' || p[4] == '\t')) {
            size_t q = 4; while (q < ll && (p[q] == ' ' || p[q] == '\t')) q++;
            size_t a = q; while (q < ll && (cp_idc((unsigned char)p[q]) || p[q] == '.')) q++;
            if (q > a) cp_ref(c, parent, CR_IMPORT, p + a, q - a, ls + a);
        } else if (parent < 0 || c->f->sym[parent].kind == CK_CLASS) {
            size_t q = 0; while (q < ll && cp_idc((unsigned char)p[q])) q++;
            if (q > 0 && !isdigit((unsigned char)p[0])) {
                size_t r = q; while (r < ll && (p[r] == ' ' || p[r] == '\t')) r++;
                int asg = r < ll && ((p[r] == '=' && !(r + 1 < ll && p[r + 1] == '=')) || (p[r] == ':' && r + 1 >= ll ? 0 : p[r] == ':'));
                int kwx = (q == 4 && !strncmp(p, "else", 4)) || (q == 3 && !strncmp(p, "try", 3)) || (q == 7 && !strncmp(p, "finally", 7));
                if (asg && !kwx) {
                    int up = 1; for (size_t z = 0; z < q; z++) if (islower((unsigned char)p[z])) up = 0;
                    int nt0 = cp_tokat(c, ls);
                    cp_sym(c, up ? CK_CONST : CK_VAR, parent, ls, ce, nt0 < c->nt ? nt0 : -1, ls, ce);
                }
            }
        }
        lastend = ce;
    }
    while (sp > 0) { cp_setend(c, stk[sp - 1].sym, lastend); sp--; }
}

/* ---------- Markdown ---------- */
static void cp_md(CP *c) {
    const char *s = c->t; size_t n = c->n, pos = 0;
    int lv[256]; int stk[8], sp = 0, cur = -1; (void)lv;
    int prevtext = 0; size_t pls = 0, ple = 0; char fence = 0; size_t fl = 0; int first = 1; int nh = 0;
    int *hl = NULL; int hcap = 0;
    while (pos < n) {
        size_t ls = pos, le = pos; while (le < n && s[le] != '\n') le++;
        pos = le < n ? le + 1 : n;
        size_t te = le; while (te > ls && (s[te - 1] == '\r' || s[te - 1] == ' ' || s[te - 1] == '\t')) te--;
        size_t ts = ls; int sp3 = 0; while (ts < te && s[ts] == ' ' && sp3 < 3) { ts++; sp3++; }
        if (first) { first = 0; if (te - ts == 3 && !strncmp(s + ts, "---", 3)) { size_t q = pos; int found = 0; while (q < n) { size_t qe = q; while (qe < n && s[qe] != '\n') qe++; if (qe - q >= 3 && !strncmp(s + q, "---", 3)) { found = 1; pos = qe < n ? qe + 1 : n; break; } q = qe < n ? qe + 1 : n; } if (found) continue; } }
        if (fence) { if (te - ts >= fl && s[ts] == fence) { size_t z = ts; while (z < te && s[z] == fence) z++; if (z - ts >= fl && z == te) fence = 0; } prevtext = 0; continue; }
        if (te - ts >= 3 && (s[ts] == '`' || s[ts] == '~')) { size_t z = ts; while (z < te && s[z] == s[ts]) z++; if (z - ts >= 3) { fence = s[ts]; fl = z - ts; prevtext = 0; continue; } }
        if (te == ts) { prevtext = 0; continue; }
        int level = 0; size_t ns = 0, ne = 0, b0 = ls;
        if (s[ts] == '#') {
            size_t z = ts; while (z < te && s[z] == '#') z++;
            if (z - ts <= 6 && (z == te || s[z] == ' ' || s[z] == '\t')) { level = (int)(z - ts); ns = z; while (ns < te && (s[ns] == ' ' || s[ns] == '\t')) ns++; ne = te; while (ne > ns && s[ne - 1] == '#') ne--; while (ne > ns && (s[ne - 1] == ' ' || s[ne - 1] == '\t')) ne--; b0 = ts; }
        }
        if (!level && prevtext) {
            size_t z = ts; char u = s[ts];
            if (u == '=' || u == '-') { while (z < te && s[z] == u) z++; if (z == te && (u == '=' || te - ts >= 2)) { level = u == '=' ? 1 : 2; b0 = pls; ns = pls; ne = ple; while (ns < ne && s[ns] == ' ') ns++; } }
        }
        if (level) {
            while (sp > 0 && hl[stk[sp - 1]] >= level) sp--;
            int parent = sp > 0 ? stk[sp - 1] : -1;
            int si = cp_symx(c, CK_HEADING, parent, b0, te, -1, s + ns, ne - ns, b0, te, 0, 0);
            if (nh >= hcap) { hcap = hcap * 2 + 16; hl = (int *)realloc(hl, (size_t)hcap * sizeof(int)); }
            hl[si] = level; nh = si + 1;
            if (sp < 8) stk[sp++] = si;
            cur = si; prevtext = 0; continue;
        }
        { /* links relativos */
            for (size_t z = ts; z + 1 < te; z++) if (s[z] == ']' && s[z + 1] == '(') {
                size_t a = z + 2, e = a; while (e < te && s[e] != ')' && s[e] != ' ') e++;
                if (e > a && s[a] != '#' && strncmp(s + a, "http", 4) && strncmp(s + a, "mailto", 6)) { size_t h = a; while (h < e && s[h] != '#') h++; if (h > a) cp_ref(c, cur, CR_IMPORT, s + a, h - a, a); }
            }
        }
        prevtext = !(s[ts] == '>' || s[ts] == '|' || s[ts] == '-' || s[ts] == '*' || s[ts] == '+' || isdigit((unsigned char)s[ts]));
        pls = ls; ple = te;
    }
    for (int k = 0; k < c->f->nsym; k++) {   /* fim = antes do proximo titulo de nivel <= */
        size_t e = n; for (int j = k + 1; j < c->f->nsym; j++) if (hl[j] <= hl[k]) { e = c->f->sym[j].byte0; break; }
        while (e > c->f->sym[k].byte0 && isspace((unsigned char)s[e - 1])) e--;
        cp_setend(c, k, e);
    }
    free(hl);
}

/* ---------- JavaScript / TypeScript ---------- */
static const char *const CP_JSMOD[] = {"export", "default", "declare", "abstract", "async", "static", "public", "private", "protected", "readonly", "override", "accessor", NULL};
static int cp_js_modnext(CP *c, int j) { /* o token depois do modificador indica que ele e mesmo modificador */
    if (j >= c->nt) return 0;
    if (c->tk[j].t == CT_P) return cp_p(c, j, '[') || cp_p(c, j, '*') || cp_p(c, j, '#') || cp_p(c, j, '{') || cp_p(c, j, '"') || cp_p(c, j, '\'') || cp_p(c, j, '@');
    return 1;
}
static int cp_js_arrow(CP *c, int j, int b) {   /* o lado direito em j e funcao (1) ou classe (2)? */
    if (cp_is(c, j, "async") && !cp_p(c, j + 1, '=')) j++;
    if (cp_is(c, j, "function")) return 1;
    if (cp_is(c, j, "class")) return 2;
    if (cp_p(c, j, '<')) j = cp_angle(c, j, b);
    if (cp_idt(c, j) && cp_is(c, j + 1, "=>")) return 1;
    if (cp_p(c, j, '(')) {
        int m = cp_mt(c, j); if (m >= b) return 0;
        int q = m + 1; if (cp_is(c, q, "=>")) return 1;
        if (cp_p(c, q, ':')) for (int z = q; z < b && z < q + 60; z++) { if (cp_is(c, z, "=>")) return 1; if (cp_p(c, z, ';') || cp_p(c, z, '=')) break; if (cp_open(c, z)) z = cp_mt(c, z); }
    }
    return 0;
}
static int cp_js_bodybrace(CP *c, int from, int b) {   /* '{' que abre o corpo, ou -1 se nao ha */
    for (int j = from; j < b;) {
        if (j > from && cp_asi(c, j)) return -1;
        if (cp_p(c, j, ';')) return -1;
        if (cp_p(c, j, '{')) {
            int pv = j - 1;
            if (pv >= from && (cp_p(c, pv, ':') || cp_p(c, pv, '|') || cp_p(c, pv, '&') || cp_p(c, pv, '<') || cp_p(c, pv, ',') || cp_is(c, pv, "=>") || cp_is(c, pv, "keyof") || cp_is(c, pv, "extends"))) { j = cp_skipgrp(c, j, b); continue; }
            return j;
        }
        if (cp_p(c, j, '(') || cp_p(c, j, '[')) { j = cp_skipgrp(c, j, b); continue; }
        j++;
    }
    return -1;
}
static int cp_js_name_at(CP *c, int j, int *nt, char *nbuf, size_t *nl) {   /* nome de membro: identificador, string ou [computado]; devolve indice seguinte ou -1 */
    *nt = -1; *nl = 0;
    if (cp_p(c, j, '#') && cp_idt(c, j + 1)) { *nt = j + 1; return j + 2; }
    if (cp_idt(c, j)) { *nt = j; return j + 1; }
    if (cp_strpair(c, j)) { size_t l = c->tk[j + 1].a - c->tk[j].b; if (l > 100) l = 100; memcpy(nbuf, c->t + c->tk[j].b, l); *nl = l; return j + 2; }
    if (cp_p(c, j, '[')) { int m = cp_mt(c, j); size_t l = c->tk[m < c->nt ? m : c->nt - 1].b - c->tk[j].a; if (l > 100) l = 100; memcpy(nbuf, c->t + c->tk[j].a, l); *nl = l; return m + 1; }
    if (c->tk[j].t == CT_NUM) { *nt = j; return j + 1; }
    return -1;
}
static int cp_js_enum_members(CP *c, int open, int b, int sym) {
    int close = cp_mt(c, open);
    for (int j = open + 1; j < close && j < b;) {
        int e = j; while (e < close && !cp_p(c, e, ',')) { if (cp_open(c, e)) { e = cp_skipgrp(c, e, close); continue; } e++; }
        int nt = -1; char nb[104]; size_t nl; int nx = cp_js_name_at(c, j, &nt, nb, &nl);
        if (nx > 0 && e > j) cp_symx(c, CK_CONST, sym, c->tk[j].a, c->tk[e - 1].b, nt, nl ? nb : NULL, nl, c->tk[j].a, c->tk[e - 1].b, 0, 0);
        j = e + 1;
    }
    return close + 1;
}
static int cp_js_member(CP *c, int i, int j, size_t b0, int b, int parent, int mode, int dp) {
    int nt; char nb[104]; size_t nl;
    int n = cp_js_name_at(c, j, &nt, nb, &nl);
    if (n < 0) return 0;
    if (cp_p(c, n, '?') || cp_p(c, n, '!')) n++;
    if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
    const char *nm = nl ? nb : NULL;
    if (cp_p(c, n, '(')) {
        int pc = cp_mt(c, n), bo = cp_js_bodybrace(c, pc + 1, b);
        if (bo >= 0) {
            int cl_ = cp_mt(c, bo);
            int s = cp_symx(c, CK_METHOD, parent, b0, cp_end(c, cl_), nt, nm, nl, b0, c->tk[bo].a, 0, 0);
            cp_body(c, bo, b, s); return cl_ + 1 > b ? b : cl_ + 1;
        }
        int se = cp_skip(c, n, b, parent, mode, 250);
        int s = cp_symx(c, CK_METHOD, parent, b0, c->tk[se - 1].b, nt, nm, nl, b0, c->tk[se - 1].b, 0, 0); c->sfl[s] |= 1;
        return se;
    }
    if (cp_p(c, n, '=') || cp_p(c, n, ':')) {
        int rhs = n + 1, kd = cp_js_arrow(c, rhs, b);
        if (kd == 1) {
            int s = cp_symx(c, CK_METHOD, parent, b0, c->tk[n].b, nt, nm, nl, b0, c->tk[n].a, 0, 0);
            int se = cp_skip(c, rhs - 1, b, s, mode == CM_OBJ ? CM_OBJ : CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
        }
        if (mode == CM_CLASS) {
            int s = cp_symx(c, CK_VAR, parent, b0, c->tk[n].b, nt, nm, nl, b0, c->tk[n].a, 0, 0);
            int se = cp_skip(c, rhs - 1, b, s, mode, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
        }
        return cp_skip(c, n, b, parent, mode, dp);
    }
    if (mode == CM_CLASS && (n >= b || cp_p(c, n, ';') || cp_asi(c, n))) {
        int s = cp_symx(c, CK_VAR, parent, b0, c->tk[n - 1].b, nt, nm, nl, b0, c->tk[n - 1].b, 0, 0); (void)s;
        return n < b && cp_p(c, n, ';') ? n + 1 : n;
    }
    return 0;
}
static int cp_js_heritage(CP *c, int lo, int bo, int sym, int iface) {   /* extends/implements entre lo e bo */
    int mode = 0, last = -1;
    for (int z = lo; z <= bo; z++) {
        int end = z == bo;
        if (!end && (cp_is(c, z, "extends") || cp_is(c, z, "implements"))) { if (last >= 0) cp_reft(c, sym, mode == 2 ? CR_IMPLEMENTS : CR_EXTENDS, last); last = -1; mode = cp_is(c, z, "extends") ? 1 : 2; continue; }
        if (end || cp_p(c, z, ',')) { if (last >= 0 && mode) cp_reft(c, sym, mode == 2 ? CR_IMPLEMENTS : CR_EXTENDS, last); last = -1; continue; }
        if (cp_p(c, z, '<')) { z = cp_angle(c, z, bo) - 1; continue; }
        if (cp_p(c, z, '(') || cp_p(c, z, '[')) { z = cp_mt(c, z); continue; }
        if (cp_idt(c, z)) last = z;
    }
    (void)iface; return 0;
}
static int cp_js_modstr(CP *c, int j, int b, int parent) {   /* import/export ... from "x": registra o caminho e devolve o indice seguinte */
    int m = j + 1;
    for (; m < b; m++) {
        if (cp_p(c, m, ';')) return m + 1;
        if (m > j + 1 && cp_asi(c, m)) return m;
        if (cp_open(c, m)) { m = cp_skipgrp(c, m, b) - 1; continue; }
        if (cp_strpair(c, m)) { cp_import_str(c, parent, m); m += 2; if (m < b && cp_p(c, m, ';')) m++; return m; }
    }
    return m;
}
static int cp_js_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    int j = i; size_t b0 = c->tk[i].a;
    int member = mode == CM_CLASS || mode == CM_OBJ;
    for (;;) {
        if (cp_p(c, j, '@')) { j++; if (cp_idt(c, j)) j++; while (cp_p(c, j, '.') && cp_idt(c, j + 1)) j += 2; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, b); continue; }
        if (cp_kwtok(c, j, CP_JSMOD) && cp_js_modnext(c, j + 1)) { j++; continue; }
        if (member && (cp_is(c, j, "get") || cp_is(c, j, "set")) && (cp_idt(c, j + 1) || cp_p(c, j + 1, '[') || cp_p(c, j + 1, '"') || cp_p(c, j + 1, '\''))) { j++; continue; }
        if (member && cp_p(c, j, '*')) { j++; continue; }
        break;
    }
    if (j >= b) return 0;
    if (member) return cp_js_member(c, i, j, b0, b, parent, mode, dp);
    if (cp_is(c, j, "import") && !cp_p(c, j + 1, '(') && !cp_p(c, j + 1, '.')) {
        return cp_js_modstr(c, j, b, parent);
    }
    if (j > i && (cp_p(c, j, '{') || cp_p(c, j, '*'))) {
        if (cp_p(c, j, '{') && !cp_is(c, cp_skipgrp(c, j, b), "from")) { int cl_ = cp_mt(c, j); cp_scan(c, j + 1, cl_ < b ? cl_ : b, parent, CM_OBJ, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; }
        return cp_js_modstr(c, j, b, parent);
    }
    if (cp_is(c, j, "function")) {
        int n = j + 1; if (cp_p(c, n, '*')) n++;
        int nm = -1; if (cp_idt(c, n) && !cp_p(c, n + 1, '=')) { nm = n; n++; }
        if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        if (!cp_p(c, n, '(')) return 0;
        int pc = cp_mt(c, n), bo = cp_js_bodybrace(c, pc + 1, b);
        if (bo < 0) { int se = cp_skip(c, n, b, parent, mode, 250); int s = cp_symx(c, CK_FUNC, parent, b0, c->tk[se - 1].b, nm, nm < 0 ? "default" : NULL, 7, b0, c->tk[se - 1].b, 0, 0); c->sfl[s] |= 1; return se; }
        int cl_ = cp_mt(c, bo);
        int s = cp_symx(c, CK_FUNC, parent, b0, cp_end(c, cl_), nm, nm < 0 ? "default" : NULL, 7, b0, c->tk[bo].a, 0, 0);
        cp_body(c, bo, b, s); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_is(c, j, "class") || cp_is(c, j, "interface")) {
        int iface = cp_is(c, j, "interface"), n = j + 1, nm = -1;
        if (cp_idt(c, n) && !cp_is(c, n, "extends") && !cp_is(c, n, "implements")) { nm = n; n++; }
        if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int bo = -1; for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { bo = z; break; } if (cp_p(c, z, ';') || cp_asi(c, z) ) break; if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); else if (cp_p(c, z, '<')) z = cp_angle(c, z, b) - 1; }
        if (bo < 0) return 0;
        int cl_ = cp_mt(c, bo);
        int s = cp_symx(c, iface ? CK_IFACE : CK_CLASS, parent, b0, cp_end(c, cl_), nm, nm < 0 ? "default" : NULL, 7, b0, c->tk[bo].a, 0, 0);
        cp_js_heritage(c, n, bo, s, iface);
        cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_is(c, j, "type") && cp_idt(c, j + 1) && (cp_p(c, j + 2, '=') || cp_p(c, j + 2, '<'))) {
        int se = cp_skip(c, j, b, parent, mode, 250); cp_sym(c, CK_TYPEDEF, parent, b0, c->tk[se - 1].b, j + 1, b0, c->tk[se - 1].b); return se;
    }
    if ((cp_is(c, j, "enum") || (cp_is(c, j, "const") && cp_is(c, j + 1, "enum"))) ) {
        int e0 = cp_is(c, j, "const") ? j + 1 : j;
        if (cp_idt(c, e0 + 1) && cp_p(c, e0 + 2, '{')) { int cl_ = cp_mt(c, e0 + 2); int s = cp_sym(c, CK_ENUM, parent, b0, cp_end(c, cl_), e0 + 1, b0, c->tk[e0 + 2].a); return cp_js_enum_members(c, e0 + 2, b, s); }
    }
    if ((cp_is(c, j, "namespace") || cp_is(c, j, "module") || (cp_is(c, j, "global") && j > i)) ) {
        int n = j + 1; while (n < b && (cp_idt(c, n) || cp_p(c, n, '.') || cp_strpair(c, n) || cp_p(c, n, '"') || cp_p(c, n, '\''))) n++;
        if (cp_p(c, n, '{')) {
            int cl_ = cp_mt(c, n); size_t a = n > j + 1 ? c->tk[j + 1].a : c->tk[j].a, e = n > j + 1 ? c->tk[n - 1].b : c->tk[j].b;
            int s = cp_symx(c, CK_NAMESPACE, parent, b0, cp_end(c, cl_), -1, c->t + a, e - a, b0, c->tk[n].a, 0, 0);
            cp_scan(c, n + 1, cl_ < b ? cl_ : b, s, CM_TOP, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
        }
    }
    if (cp_is(c, j, "const") || cp_is(c, j, "let") || cp_is(c, j, "var")) {
        int nm = j + 1; if (!cp_idt(c, nm)) return 0;
        int eqi = -1;
        for (int z = nm + 1; z < b; z++) { if (cp_asi(c, z) || cp_p(c, z, ';')) break; if (cp_p(c, z, '=')) { eqi = z; break; } if (cp_open(c, z)) z = cp_mt(c, z); }
        if (eqi < 0) return 0;
        int rhs = eqi + 1, kd = cp_js_arrow(c, rhs, b);
        if (kd) {
            int s = cp_sym(c, kd == 2 ? CK_CLASS : CK_FUNC, parent, b0, c->tk[eqi].b, nm, b0, c->tk[eqi].a);
            int se = cp_skip(c, rhs - 1, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
        }
        if (cp_is(c, rhs, "require") || (cp_is(c, rhs, "await") && cp_is(c, rhs + 1, "import"))) return 0;
        if (mode == CM_TOP) {
            int s = cp_sym(c, cp_is(c, j, "const") ? CK_CONST : CK_VAR, parent, b0, c->tk[eqi].b, nm, b0, c->tk[eqi].a);
            int se = cp_skip(c, rhs - 1, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
        }
        return 0;
    }
    if (cp_idt(c, j) && !cp_kwtok(c, j, CP_NOCALL)) {
        int n = j, last = j, prot = -1;
        while (cp_idt(c, n)) { last = n; if (cp_is(c, n + 1, ".") || cp_p(c, n + 1, '.')) { if (cp_is(c, n, "prototype")) prot = n; if (cp_idt(c, n + 2)) { n += 2; continue; } } n++; break; }
        if (n > j + 1 && cp_p(c, n, '=') && !cp_p(c, n + 1, '=')) {
            int rhs = n + 1;
            if ((cp_is(c, j, "module") || cp_is(c, j, "exports")) && cp_p(c, rhs, '{')) { int cl_ = cp_mt(c, rhs); cp_scan(c, rhs + 1, cl_ < b ? cl_ : b, parent, CM_OBJ, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; }
            int kd = cp_js_arrow(c, rhs, b);
            if (kd) {
                int par = parent, isproto = prot >= 0 && prot + 1 < last;
                if (isproto) { size_t ql = cp_tl(c, j); for (int s = c->f->nsym - 1; s >= 0; s--) if ((c->f->sym[s].kind == CK_CLASS || c->f->sym[s].kind == CK_FUNC) && strlen(c->f->sym[s].name) == ql && !memcmp(c->f->sym[s].name, c->c + c->tk[j].a, ql)) { par = s; break; } }
                int s = cp_sym(c, kd == 2 ? CK_CLASS : isproto ? CK_METHOD : CK_FUNC, par, b0, c->tk[n].b, last, b0, c->tk[n].a);
                int se = cp_skip(c, rhs - 1, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
            }
        }
    }
    return 0;
}

/* ---------- Java ---------- */
static const char *const CP_JMOD[] = {"public", "private", "protected", "static", "final", "abstract", "native", "synchronized", "transient", "volatile", "strictfp", "default", "sealed", NULL};
static int cp_enum_body(CP *c, int open, int b, int sym, int lang_kt) {   /* constantes de enum ate ';' e depois membros */
    int close = cp_mt(c, open), j = open + 1;
    (void)lang_kt;
    while (j < close && j < b) {
        while (cp_p(c, j, '@')) { j++; if (cp_idt(c, j)) j++; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, close); }
        if (cp_p(c, j, ';')) { j++; break; }
        if (cp_p(c, j, ',')) { j++; continue; }
        if (!cp_idt(c, j)) { j = cp_skip(c, j, close, sym, CM_BODY, 250); continue; }
        int st = j, e = j + 1;
        while (e < close && !cp_p(c, e, ',') && !cp_p(c, e, ';')) { if (cp_open(c, e)) { e = cp_skipgrp(c, e, close); continue; } e++; }
        cp_sym(c, CK_CONST, sym, c->tk[st].a, c->tk[e - 1].b, st, c->tk[st].a, c->tk[e - 1].b);
        j = e;
    }
    if (j < close) cp_scan(c, j, close < b ? close : b, sym, CM_CLASS, 2);
    return close + 1 > b ? b : close + 1;
}
static int cp_java_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    if (mode == CM_BODY) return 0;
    int j = i; size_t b0 = c->tk[i].a;
    for (;;) {
        if (cp_p(c, j, '@') && !cp_is(c, j + 1, "interface")) { j++; if (cp_idt(c, j)) j++; while (cp_p(c, j, '.') && cp_idt(c, j + 1)) j += 2; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, b); continue; }
        if (cp_kwtok(c, j, CP_JMOD) && !cp_p(c, j + 1, '(') && !cp_p(c, j + 1, '=') && !cp_p(c, j + 1, ';')) { j++; continue; }
        if (cp_is(c, j, "non") && cp_p(c, j + 1, '-') && cp_is(c, j + 2, "sealed")) { j += 3; continue; }
        break;
    }
    if (j >= b) return 0;
    if (cp_is(c, j, "package")) { int e = j; while (e < b && !cp_p(c, e, ';')) e++; return e + 1; }
    if (cp_is(c, j, "import")) {
        int n = j + 1; if (cp_is(c, n, "static")) n++;
        int e = n; while (e < b && !cp_p(c, e, ';')) e++;
        if (e > n) cp_ref_range(c, -1, CR_IMPORT, c->tk[n].a, c->tk[e - 1].b, 1);
        return e + 1;
    }
    int at = cp_p(c, j, '@') && cp_is(c, j + 1, "interface");
    int tk = at ? j + 1 : j;
    int isrec = cp_is(c, tk, "record") && cp_idt(c, tk + 1) && (cp_p(c, tk + 2, '(') || cp_p(c, tk + 2, '<'));
    if (cp_is(c, tk, "class") || cp_is(c, tk, "interface") || cp_is(c, tk, "enum") || isrec) {
        int kind = cp_is(c, tk, "interface") ? CK_IFACE : cp_is(c, tk, "enum") ? CK_ENUM : isrec ? CK_STRUCT : CK_CLASS;
        int nm = tk + 1; if (!cp_idt(c, nm)) return 0;
        int n = nm + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int bo = -1; for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { bo = z; break; } if (cp_p(c, z, ';')) break; if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); else if (cp_p(c, z, '<')) z = cp_angle(c, z, b) - 1; }
        if (bo < 0) return 0;
        int cl_ = cp_mt(c, bo), s = cp_sym(c, kind, parent, b0, cp_end(c, cl_), nm, b0, c->tk[bo].a);
        int mode2 = 0, last = -1;
        for (int z = n; z <= bo; z++) {
            int end = z == bo;
            if (!end && (cp_is(c, z, "extends") || cp_is(c, z, "implements") || cp_is(c, z, "permits"))) { if (last >= 0 && mode2) cp_reft(c, s, mode2 == 2 ? CR_IMPLEMENTS : CR_EXTENDS, last); last = -1; mode2 = cp_is(c, z, "extends") ? 1 : cp_is(c, z, "implements") ? 2 : 0; continue; }
            if (end || cp_p(c, z, ',')) { if (last >= 0 && mode2) cp_reft(c, s, mode2 == 2 ? CR_IMPLEMENTS : CR_EXTENDS, last); last = -1; continue; }
            if (cp_p(c, z, '<')) { z = cp_angle(c, z, bo) - 1; continue; }
            if (cp_p(c, z, '(')) { z = cp_mt(c, z); continue; }
            if (cp_idt(c, z)) last = z;
        }
        if (kind == CK_ENUM) return cp_enum_body(c, bo, b, s, 0);
        cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
    }
    int eq = -1, par = -1, term = 0, e = b;
    for (int z = j; z < b;) {
        if (cp_p(c, z, ';')) { term = ';'; e = z; break; }
        if (cp_p(c, z, '{')) { if (eq >= 0) { z = cp_skipgrp(c, z, b); continue; } term = '{'; e = z; break; }
        if (cp_p(c, z, '}')) { term = '}'; e = z; break; }
        if (cp_p(c, z, '(')) { if (par < 0 && eq < 0) par = z; z = cp_skipgrp(c, z, b); continue; }
        if (cp_p(c, z, '[')) { z = cp_skipgrp(c, z, b); continue; }
        if (cp_p(c, z, '<') && par < 0 && eq < 0) { z = cp_angle(c, z, b); continue; }
        if (cp_p(c, z, '=') && eq < 0 && par < 0) eq = z;
        z++;
    }
    if (par >= 0 && eq < 0 && par - 1 >= j && cp_idt(c, par - 1) && !cp_kwtok(c, par - 1, CP_NOCALL)) {
        int nm = par - 1;
        if (term == '{') { int cl_ = cp_mt(c, e); int s = cp_sym(c, CK_METHOD, parent, b0, cp_end(c, cl_), nm, b0, c->tk[e].a); (void)s; return cl_ + 1 > b ? b : cl_ + 1; }
        int s = cp_sym(c, CK_METHOD, parent, b0, term == ';' ? c->tk[e].b : c->tk[e - 1].b, nm, b0, term == ';' ? c->tk[e].a : c->tk[e - 1].b); c->sfl[s] |= 1;
        return term == ';' ? e + 1 : e > i ? e : 0;
    }
    if ((eq >= 0 || par < 0) && (term == ';')) {
        int hi = eq >= 0 ? eq : e;
        for (int m = j; m < hi; m++) { if (cp_p(c, m, ',')) { hi = m; break; } }
        int nm = hi - 1; while (nm >= j && cp_p(c, nm, ']')) nm = c->mt[nm] - 1;
        if (nm > j && cp_idt(c, nm) && !cp_kwtok(c, j, CP_NOTVAR)) {
            int fin = 0; for (int m = i; m < nm; m++) if (cp_is(c, m, "final")) fin = 1;
            cp_sym(c, fin ? CK_CONST : CK_VAR, parent, b0, c->tk[e].b, nm, b0, c->tk[hi].a);
        }
        return e + 1;
    }
    return 0;
}

/* ---------- Kotlin ---------- */
static const char *const CP_KTMOD[] = {"public", "private", "protected", "internal", "open", "final", "abstract", "sealed", "data", "inner", "annotation", "override", "lateinit", "const", "suspend",
    "inline", "noinline", "crossinline", "tailrec", "operator", "infix", "external", "expect", "actual", "vararg", "value", "enum", "companion", NULL};
static int cp_kt_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    if (mode == CM_ENUM) return 0;
    int j = i; size_t b0 = c->tk[i].a; int cst = 0, comp = 0, enm = 0;
    for (;;) {
        if (cp_p(c, j, '@')) { j++; if (cp_idt(c, j) && cp_p(c, j + 1, ':')) j += 2; if (cp_idt(c, j)) j++; while (cp_p(c, j, '.') && cp_idt(c, j + 1)) j += 2; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, b); continue; }
        if (cp_kwtok(c, j, CP_KTMOD) && cp_idt(c, j + 1)) { if (cp_is(c, j, "const")) cst = 1; if (cp_is(c, j, "companion")) comp = 1; if (cp_is(c, j, "enum")) enm = 1; j++; continue; }
        break;
    }
    if (j >= b) return 0;
    if (cp_is(c, j, "package")) { int e = j + 1; while (e < b && !(c->tk[e].fl & CF_NL)) e++; return e; }
    if (cp_is(c, j, "import")) {
        int e = j + 1; while (e < b && !(c->tk[e].fl & CF_NL)) e++;
        int ae = e; for (int m = j + 1; m < e; m++) if (cp_is(c, m, "as")) { ae = m; break; }
        if (ae > j + 1) cp_ref_range(c, -1, CR_IMPORT, c->tk[j + 1].a, c->tk[ae - 1].b, 1);
        return e;
    }
    if (cp_is(c, j, "typealias") && cp_idt(c, j + 1)) { int se = cp_skip(c, j, b, parent, mode, 250); cp_sym(c, CK_TYPEDEF, parent, b0, c->tk[se - 1].b, j + 1, b0, c->tk[se - 1].b); return se; }
    if (cp_is(c, j, "fun") && cp_is(c, j + 1, "interface")) j++;
    int isclass = cp_is(c, j, "class"), isif = cp_is(c, j, "interface"), isobj = cp_is(c, j, "object");
    if (isclass || isif || isobj) {
        int nm = j + 1; if (isclass && cp_p(c, nm, '<')) { /* generico antes do nome: raro */ }
        char cname[16]; const char *nmx = NULL; size_t nml = 0;
        if (!cp_idt(c, nm) || cp_is(c, nm, "where")) { if (isobj && comp) { nm = -1; nmx = "Companion"; nml = 9; } else return 0; }
        else if (isobj && cp_p(c, nm + 1, ':') && 0) return 0;
        (void)cname;
        int n = (nm < 0 ? j + 1 : nm + 1);
        if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        if (cp_is(c, n, "constructor") && cp_p(c, n + 1, '(')) n++;
        if (cp_p(c, n, '(')) n = cp_skipgrp(c, n, b);
        int colon = -1, bo = -1, z = n;
        for (; z < b; z++) {
            if (z > n && cp_asi(c, z) && !cp_p(c, z, '{')) break;
            if (cp_p(c, z, ':') && colon < 0) colon = z;
            if (cp_p(c, z, '{')) { bo = z; break; }
            if (cp_p(c, z, '(') || cp_p(c, z, '[')) { z = cp_mt(c, z); continue; }
            if (cp_p(c, z, '<')) { z = cp_angle(c, z, b) - 1; continue; }
            if (cp_p(c, z, ';')) break;
        }
        int hend = bo >= 0 ? bo : z;
        int cl_ = bo >= 0 ? cp_mt(c, bo) : -1;
        size_t b1 = bo >= 0 ? cp_end(c, cl_) : c->tk[hend > n ? hend - 1 : n - 1 < 0 ? 0 : n - 1].b;
        int kind = isif ? CK_IFACE : enm ? CK_ENUM : CK_CLASS;
        int s = cp_symx(c, kind, parent, b0, b1, nm, nmx, nml, b0, hend > 0 && hend <= b ? c->tk[hend - 1].b : b0, 0, 0);
        if (colon >= 0) {
            int last = -1, ang = 0, hasp = 0;
            for (int m = colon + 1; m <= hend; m++) {
                int end = m == hend;
                if (!end && cp_p(c, m, '<')) { ang++; continue; }
                if (!end && cp_p(c, m, '>')) { if (ang > 0) ang--; continue; }
                if (!end && cp_p(c, m, '(')) { hasp = 1; m = cp_mt(c, m); continue; }
                if (ang > 0) continue;
                if (!end && cp_is(c, m, "by")) { while (m + 1 < hend && !cp_p(c, m + 1, ',')) m++; continue; }
                if (end || cp_p(c, m, ',')) { if (last >= 0) cp_reft(c, s, hasp && !isif ? CR_EXTENDS : (isif ? CR_EXTENDS : CR_IMPLEMENTS), last); last = -1; hasp = 0; continue; }
                if (cp_idt(c, m)) last = m;
            }
        }
        if (bo < 0) return hend;
        if (kind == CK_ENUM) return cp_enum_body(c, bo, b, s, 1);
        cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_is(c, j, "fun") || (cp_is(c, j, "constructor") && cp_p(c, j + 1, '(') && mode == CM_CLASS)) {
        int nm = -1, n = j + 1, pc;
        if (cp_is(c, j, "constructor")) { nm = j; n = j + 1; }
        else {
            if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
            for (; n < b; n++) {
                if (cp_p(c, n, '(')) break;
                if (cp_p(c, n, '<')) { n = cp_angle(c, n, b) - 1; continue; }
                if (cp_idt(c, n)) nm = n; else if (!cp_p(c, n, '.') && !cp_p(c, n, '?')) return 0;
            }
            if (nm < 0 || !cp_p(c, n, '(')) return 0;
        }
        pc = cp_mt(c, n);
        int z = pc + 1, bo = -1, eqi = -1;
        for (; z < b; z++) {
            if (z > pc + 1 && cp_asi(c, z)) break;
            if (cp_p(c, z, '{')) { bo = z; break; }
            if (cp_p(c, z, '=')) { eqi = z; break; }
            if (cp_p(c, z, '(') || cp_p(c, z, '[')) { z = cp_mt(c, z); continue; }
            if (cp_p(c, z, '<')) { z = cp_angle(c, z, b) - 1; continue; }
        }
        int kind = mode == CM_CLASS ? CK_METHOD : CK_FUNC;
        if (bo >= 0) { int cl_ = cp_mt(c, bo); int s = cp_sym(c, kind, parent, b0, cp_end(c, cl_), nm, b0, c->tk[bo].a); cp_body(c, bo, b, s); return cl_ + 1 > b ? b : cl_ + 1; }
        if (eqi >= 0) { int s = cp_sym(c, kind, parent, b0, c->tk[eqi].b, nm, b0, c->tk[eqi].a); int se = cp_skip(c, eqi, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se; }
        int s = cp_sym(c, kind, parent, b0, c->tk[z > pc ? z - 1 : pc].b, nm, b0, c->tk[z > pc ? z - 1 : pc].b); c->sfl[s] |= 1;
        return z;
    }
    if ((cp_is(c, j, "val") || cp_is(c, j, "var")) && mode != CM_BODY) {
        int n = j + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int nm = -1; for (; n < b; n++) { if (cp_idt(c, n)) nm = n; else if (!cp_p(c, n, '.') && !cp_p(c, n, '?')) break; else continue; if (cp_p(c, n + 1, '.')) continue; break; }
        if (nm < 0) return 0;
        int s = cp_sym(c, cst ? CK_CONST : CK_VAR, parent, b0, c->tk[nm].b, nm, b0, c->tk[nm].b);
        int se = cp_skip(c, nm + 1, b, s, CM_BODY, dp);
        while (se < b && (cp_is(c, se, "get") || cp_is(c, se, "set") || ((cp_is(c, se, "private") || cp_is(c, se, "internal") || cp_is(c, se, "protected")) && (cp_is(c, se + 1, "set") || cp_is(c, se + 1, "get"))))) se = cp_skip(c, se, b, s, CM_BODY, dp);
        cp_setend(c, s, c->tk[se - 1].b); return se;
    }
    return 0;
}

/* ---------- Go ---------- */
static void cp_go_struct(CP *c, int open, int b, int sym, int iface) {
    int close = cp_mt(c, open);
    for (int j = open + 1; j < close && j < b;) {
        if (cp_p(c, j, ';')) { j++; continue; }
        int e = cp_skip(c, j, close, sym, CM_TOP, 250), s = j, ee = e;
        if (ee - 2 > j && cp_strpair(c, ee - 2)) ee -= 2;
        if (ee > s && cp_p(c, ee - 1, ';')) ee--;
        if (cp_p(c, s, '*') || cp_p(c, s, '~')) s++;
        if (ee > s && cp_idt(c, s)) {
            if (ee - s == 1) cp_reft(c, sym, CR_EXTENDS, s);
            else if (ee - s == 3 && cp_p(c, s + 1, '.') && cp_idt(c, s + 2)) cp_reft(c, sym, CR_EXTENDS, s + 2);
            else if (iface && cp_p(c, s + 1, '(')) { int pc = cp_mt(c, s + 1); cp_sym(c, CK_METHOD, sym, c->tk[j].a, c->tk[ee - 1].b, s, c->tk[j].a, c->tk[ee - 1].b); (void)pc; }
            else if (!iface && (cp_idt(c, s + 1) || cp_p(c, s + 1, '*') || cp_p(c, s + 1, '[') || cp_p(c, s + 1, ',')) && !cp_is(c, s, "_")) cp_sym(c, CK_VAR, sym, c->tk[j].a, c->tk[ee - 1].b, s, c->tk[j].a, c->tk[ee - 1].b);
        }
        j = e;
    }
}
static int cp_go_typespec(CP *c, int j, int b, int parent, size_t b0) {
    if (!cp_idt(c, j)) return cp_skip(c, j, b, parent, CM_TOP, 250);
    int nm = j, n = j + 1;
    if (cp_p(c, n, '[') && cp_idt(c, n + 1) && !cp_p(c, n + 2, ']')) n = cp_skipgrp(c, n, b);
    if (cp_p(c, n, '=')) n++;
    int st = cp_is(c, n, "struct"), it = cp_is(c, n, "interface");
    if ((st || it) && cp_p(c, n + 1, '{')) {
        int cl_ = cp_mt(c, n + 1), s = cp_sym(c, st ? CK_STRUCT : CK_IFACE, parent, b0, cp_end(c, cl_), nm, b0, c->tk[n + 1].a);
        cp_go_struct(c, n + 1, b, s, it); return cl_ + 1 > b ? b : cl_ + 1;
    }
    int se = cp_skip(c, j, b, parent, CM_TOP, 250);
    cp_sym(c, CK_TYPEDEF, parent, b0, c->tk[se - 1].b, nm, b0, c->tk[se - 1].b); return se;
}
static int cp_go_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    if (mode == CM_BODY) return 0;
    size_t b0 = c->tk[i].a;
    if (cp_is(c, i, "package")) return cp_skip(c, i, b, parent, mode, 250);
    if (cp_is(c, i, "import")) {
        if (cp_p(c, i + 1, '(')) { int cl_ = cp_mt(c, i + 1); for (int m = i + 2; m < cl_ && m < b; m++) if (cp_strpair(c, m)) { cp_import_str(c, -1, m); m++; } return cl_ + 1 > b ? b : cl_ + 1; }
        int se = cp_skip(c, i, b, parent, mode, 250); for (int m = i + 1; m < se; m++) if (cp_strpair(c, m)) { cp_import_str(c, -1, m); break; }
        return se;
    }
    if (cp_is(c, i, "func")) {
        int n = i + 1, rt = -1;
        if (cp_p(c, n, '(')) { int rc = cp_mt(c, n); for (int m = n + 1; m < rc; m++) { if (cp_p(c, m, '[')) { m = cp_mt(c, m); continue; } if (cp_idt(c, m)) rt = m; } n = rc + 1; }
        if (!cp_idt(c, n)) return 0;
        int nm = n; n++;
        if (cp_p(c, n, '[')) n = cp_skipgrp(c, n, b);
        if (!cp_p(c, n, '(')) return 0;
        int pc = cp_mt(c, n), bo = -1, z = pc + 1;
        for (; z < b; z++) {
            if (z > pc + 1 && cp_asi(c, z)) break;
            if (cp_p(c, z, '{')) { if (cp_is(c, z - 1, "struct") || cp_is(c, z - 1, "interface")) { z = cp_mt(c, z); continue; } bo = z; break; }
            if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z);
        }
        int kind = rt >= 0 ? CK_METHOD : CK_FUNC, s;
        if (bo >= 0) { int cl_ = cp_mt(c, bo); s = cp_sym(c, kind, parent, b0, cp_end(c, cl_), nm, b0, c->tk[bo].a); if (rt >= 0) c->sx[s] = cp_tokname(c, rt); return cl_ + 1 > b ? b : cl_ + 1; }
        s = cp_sym(c, kind, parent, b0, c->tk[z > pc ? z - 1 : pc].b, nm, b0, c->tk[z > pc ? z - 1 : pc].b); c->sfl[s] |= 1; if (rt >= 0) c->sx[s] = cp_tokname(c, rt);
        return z;
    }
    if (cp_is(c, i, "type")) {
        if (cp_p(c, i + 1, '(')) { int cl_ = cp_mt(c, i + 1); for (int j = i + 2; j < cl_ && j < b;) { if (cp_p(c, j, ';')) { j++; continue; } j = cp_go_typespec(c, j, cl_, parent, c->tk[j].a); } return cl_ + 1 > b ? b : cl_ + 1; }
        return cp_go_typespec(c, i + 1, b, parent, b0);
    }
    if (cp_is(c, i, "var") || cp_is(c, i, "const")) {
        int kind = cp_is(c, i, "const") ? CK_CONST : CK_VAR;
        if (cp_p(c, i + 1, '(')) {
            int cl_ = cp_mt(c, i + 1);
            for (int j = i + 2; j < cl_ && j < b;) {
                if (cp_p(c, j, ';')) { j++; continue; }
                int e = cp_skip(c, j, cl_, parent, CM_TOP, 250);
                if (cp_idt(c, j) && !cp_is(c, j, "_")) cp_sym(c, kind, parent, c->tk[j].a, c->tk[e - 1].b, j, c->tk[j].a, c->tk[e - 1].b);
                j = e;
            }
            return cl_ + 1 > b ? b : cl_ + 1;
        }
        int se = cp_skip(c, i, b, parent, mode, 250);
        if (cp_idt(c, i + 1) && !cp_is(c, i + 1, "_")) cp_sym(c, kind, parent, b0, c->tk[se - 1].b, i + 1, b0, c->tk[se - 1].b);
        return se;
    }
    return 0;
}

/* ---------- Rust ---------- */
static int cp_rs_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    int j = i; size_t b0 = c->tk[i].a;
    if (mode == CM_ENUM || mode == CM_ARR) {
        for (;;) { if (cp_p(c, j, '#')) { j++; if (cp_p(c, j, '[')) j = cp_skipgrp(c, j, b); continue; } if (cp_is(c, j, "pub")) { j++; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, b); continue; } break; }
        if (!cp_idt(c, j)) return 0;
        int e = j + 1, ang = 0;
        while (e < b && !(ang == 0 && cp_p(c, e, ','))) { if (cp_open(c, e)) { e = cp_skipgrp(c, e, b); continue; } if (cp_p(c, e, '<')) ang++; else if (cp_p(c, e, '>') && ang > 0) ang--; e++; }
        cp_sym(c, mode == CM_ENUM ? CK_CONST : CK_VAR, parent, b0, c->tk[e - 1].b, j, c->tk[j].a, c->tk[e - 1].b);
        return e + 1;
    }
    for (;;) {
        if (cp_p(c, j, '#')) { j++; if (cp_p(c, j, '!')) j++; if (cp_p(c, j, '[')) j = cp_skipgrp(c, j, b); continue; }
        if (cp_is(c, j, "pub")) { j++; if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, b); continue; }
        if (cp_is(c, j, "unsafe") || cp_is(c, j, "async") || cp_is(c, j, "default") || (cp_is(c, j, "const") && (cp_is(c, j + 1, "fn") || cp_is(c, j + 1, "unsafe") || cp_is(c, j + 1, "async")))) { j++; continue; }
        if (cp_is(c, j, "extern")) {
            if (cp_strpair(c, j + 1)) { if (cp_p(c, j + 3, '{')) { int cl_ = cp_mt(c, j + 3); cp_scan(c, j + 4, cl_ < b ? cl_ : b, parent, CM_TOP, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; } j += 3; continue; }
            if (cp_is(c, j + 1, "crate")) return cp_skip(c, j, b, parent, mode, 250);
            j++; continue;
        }
        break;
    }
    if (j >= b) return 0;
    if (cp_is(c, j, "fn") && cp_idt(c, j + 1)) {
        int nm = j + 1, n = nm + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int term = 0, e = b;
        for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { term = '{'; e = z; break; } if (cp_p(c, z, ';')) { term = ';'; e = z; break; } if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); }
        int kind = mode == CM_CLASS ? CK_METHOD : CK_FUNC;
        if (term == '{') { int cl_ = cp_mt(c, e); int s = cp_sym(c, kind, parent, b0, cp_end(c, cl_), nm, b0, c->tk[e].a); cp_body(c, e, b, s); return cl_ + 1 > b ? b : cl_ + 1; }
        int s = cp_sym(c, kind, parent, b0, term ? c->tk[e].b : c->tk[e - 1].b, nm, b0, term ? c->tk[e].a : c->tk[e - 1].b); c->sfl[s] |= 1;
        return term ? e + 1 : e;
    }
    if ((cp_is(c, j, "struct") || cp_is(c, j, "union") || cp_is(c, j, "enum")) && cp_idt(c, j + 1)) {
        int en = cp_is(c, j, "enum"), nm = j + 1, n = nm + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int term = 0, e = b;
        for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { term = '{'; e = z; break; } if (cp_p(c, z, ';')) { term = ';'; e = z; break; } if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); }
        if (term == '{') { int cl_ = cp_mt(c, e); int s = cp_sym(c, en ? CK_ENUM : CK_STRUCT, parent, b0, cp_end(c, cl_), nm, b0, c->tk[e].a); cp_scan(c, e + 1, cl_ < b ? cl_ : b, s, en ? CM_ENUM : CM_ARR, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; }
        cp_sym(c, en ? CK_ENUM : CK_STRUCT, parent, b0, term ? c->tk[e].b : c->tk[e - 1].b, nm, b0, term ? c->tk[e].b : c->tk[e - 1].b);
        return term ? e + 1 : e;
    }
    if (cp_is(c, j, "trait") && cp_idt(c, j + 1)) {
        int nm = j + 1, n = nm + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int bo = -1; for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { bo = z; break; } if (cp_p(c, z, ';')) break; if (cp_p(c, z, '(')) z = cp_mt(c, z); }
        if (bo < 0) return 0;
        int cl_ = cp_mt(c, bo), s = cp_sym(c, CK_IFACE, parent, b0, cp_end(c, cl_), nm, b0, c->tk[bo].a);
        if (cp_p(c, n, ':')) { int wh = bo; for (int z = n; z < bo; z++) if (cp_is(c, z, "where")) { wh = z; break; } int last = -1; for (int z = n + 1; z <= wh; z++) { if (z == wh || cp_p(c, z, '+')) { if (last >= 0) cp_reft(c, s, CR_EXTENDS, last); last = -1; continue; } if (cp_p(c, z, '<')) { z = cp_angle(c, z, wh) - 1; continue; } if (cp_idt(c, z) && !cp_is(c, z, "dyn")) last = z; } }
        cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_is(c, j, "impl")) {
        int n = j + 1; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        int bo = -1; for (int z = n; z < b; z++) { if (cp_p(c, z, '{')) { bo = z; break; } if (cp_p(c, z, ';')) break; if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); }
        if (bo < 0) return 0;
        int tr = -1, ty = -1, fo = 0, ang = 0;
        for (int z = n; z < bo; z++) {
            if (cp_is(c, z, "where")) break;
            if (cp_p(c, z, '<')) { ang++; continue; } if (cp_p(c, z, '>')) { if (ang > 0) ang--; continue; }
            if (ang) continue;
            if (cp_is(c, z, "for")) { fo = 1; tr = ty; ty = -1; continue; }
            if (cp_idt(c, z) && !cp_is(c, z, "dyn")) ty = z;
        }
        if (ty < 0) return 0;
        int cl_ = cp_mt(c, bo), s = cp_sym(c, CK_CLASS, parent, b0, cp_end(c, cl_), ty, b0, c->tk[bo].a);
        if (fo && tr >= 0) cp_reft(c, s, CR_IMPLEMENTS, tr);
        cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_is(c, j, "mod") && cp_idt(c, j + 1)) {
        if (cp_p(c, j + 2, '{')) { int cl_ = cp_mt(c, j + 2), s = cp_sym(c, CK_NAMESPACE, parent, b0, cp_end(c, cl_), j + 1, b0, c->tk[j + 2].a); cp_scan(c, j + 3, cl_ < b ? cl_ : b, s, CM_TOP, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; }
        int se = cp_skip(c, j, b, parent, mode, 250); cp_sym(c, CK_NAMESPACE, parent, b0, c->tk[se - 1].b, j + 1, b0, c->tk[se - 1].b); return se;
    }
    if (cp_is(c, j, "use")) {
        int e = j + 1; while (e < b && !cp_p(c, e, ';')) { if (cp_open(c, e)) { e = cp_skipgrp(c, e, b); continue; } e++; }
        if (e > j + 1) cp_ref_range(c, parent, CR_IMPORT, c->tk[j + 1].a, c->tk[e - 1].b, 1);
        return e + 1;
    }
    if (cp_is(c, j, "type") && cp_idt(c, j + 1)) { int se = cp_skip(c, j, b, parent, mode, 250); cp_sym(c, CK_TYPEDEF, parent, b0, c->tk[se - 1].b, j + 1, b0, c->tk[se - 1].b); return se; }
    if ((cp_is(c, j, "const") || cp_is(c, j, "static")) && mode != CM_BODY) {
        int n = j + 1; if (cp_is(c, n, "mut")) n++;
        int se = cp_skip(c, j, b, parent, mode, 250);
        if (cp_idt(c, n) && !cp_is(c, n, "_") && !cp_is(c, n, "fn")) cp_sym(c, cp_is(c, j, "const") ? CK_CONST : CK_VAR, parent, b0, c->tk[se - 1].b, n, b0, c->tk[se - 1].b);
        return se;
    }
    if (cp_is(c, j, "macro_rules") && cp_p(c, j + 1, '!') && cp_idt(c, j + 2) && cp_p(c, j + 3, '{')) { int cl_ = cp_mt(c, j + 3); cp_sym(c, CK_MACRO, parent, b0, cp_end(c, cl_), j + 2, b0, c->tk[j + 3].a); return cl_ + 1 > b ? b : cl_ + 1; }
    return 0;
}

/* ---------- shell ---------- */
static int cp_sh_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    if (mode == CM_BODY) return 0;
    size_t b0 = c->tk[i].a;
    int nm = -1, n = -1;
    if (cp_is(c, i, "function") && cp_idt(c, i + 1)) { nm = i + 1; n = nm + 1; if (cp_p(c, n, '(') && cp_p(c, n + 1, ')')) n += 2; }
    else if (cp_idt(c, i) && cp_p(c, i + 1, '(') && cp_p(c, i + 2, ')')) { nm = i; n = i + 3; }
    if (nm >= 0 && (cp_p(c, n, '{') || cp_p(c, n, '('))) { int cl_ = cp_mt(c, n); cp_sym(c, CK_FUNC, parent, b0, cp_end(c, cl_), nm, b0, c->tk[n].a); return cl_ + 1 > b ? b : cl_ + 1; }
    if (cp_is(c, i, "source") || (cp_p(c, i, '.') && !(c->tk[i + 1].fl & CF_NL))) {
        if (i + 1 < b && !(c->tk[i + 1].fl & CF_NL) && cp_tl(c, i) <= 6) {
            size_t a = c->tk[i + 1].a, e = a; while (e < c->n && !strchr(" \t\r\n;|&)", c->t[e])) e++;
            while (a < e && (c->t[a] == '"' || c->t[a] == '\'')) a++;
            while (e > a && (c->t[e - 1] == '"' || c->t[e - 1] == '\'')) e--;
            if (e > a) cp_ref_range(c, parent, CR_IMPORT, a, e, 0);
        }
        return 0;
    }
    int j = i; if (cp_is(c, j, "export") || cp_is(c, j, "readonly")) j++;
    if (mode == CM_TOP && cp_idt(c, j) && cp_p(c, j + 1, '=') && c->tk[j + 1].a == c->tk[j].b) {
        int se = cp_skip(c, i, b, parent, mode, 250); int up = 1; for (size_t z = c->tk[j].a; z < c->tk[j].b; z++) if (islower((unsigned char)c->c[z])) up = 0;
        cp_sym(c, up ? CK_CONST : CK_VAR, parent, b0, c->tk[se - 1].b, j, b0, c->tk[se - 1].b); return se;
    }
    return 0;
}

/* ---------- PowerShell ---------- */
static int cp_ps_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    size_t b0 = c->tk[i].a;
    if (mode == CM_CLASS) {
        int j = i; while (cp_isci(c, j, "hidden") || cp_isci(c, j, "static")) j++;
        while (cp_p(c, j, '[')) j = cp_skipgrp(c, j, b);
        if (cp_idt(c, j) && cp_p(c, j + 1, '(') && c->c[c->tk[j].a] != '$') {
            int pc = cp_mt(c, j + 1), bo = pc + 1; if (!cp_p(c, bo, '{')) return 0;
            int cl_ = cp_mt(c, bo); int s = cp_sym(c, CK_METHOD, parent, b0, cp_end(c, cl_), j, b0, c->tk[bo].a); cp_body(c, bo, b, s); return cl_ + 1 > b ? b : cl_ + 1;
        }
        if (cp_idt(c, j) && c->c[c->tk[j].a] == '$') { int se = cp_skip(c, j, b, parent, mode, 250); cp_sym(c, CK_VAR, parent, b0, c->tk[se - 1].b, j, b0, c->tk[se - 1].b); return se; }
        return 0;
    }
    if ((cp_isci(c, i, "function") || cp_isci(c, i, "filter") || cp_isci(c, i, "workflow")) && cp_idt(c, i + 1)) {
        int nm = i + 1, n = nm + 1; if (cp_p(c, n, '(')) n = cp_skipgrp(c, n, b);
        if (!cp_p(c, n, '{')) return 0;
        int cl_ = cp_mt(c, n), z = n + 1; while (cp_p(c, z, '[')) z = cp_skipgrp(c, z, b);
        size_t ea = 0, eb = 0; if (cp_isci(c, z, "param") && cp_p(c, z + 1, '(')) { int pc = cp_mt(c, z + 1); ea = c->tk[z].a; eb = cp_end(c, pc); }
        int s = cp_symx(c, CK_FUNC, parent, b0, cp_end(c, cl_), nm, NULL, 0, b0, c->tk[n].a, ea, eb);
        cp_body(c, n, b, s); return cl_ + 1 > b ? b : cl_ + 1;
    }
    if ((cp_isci(c, i, "class") || cp_isci(c, i, "enum")) && cp_idt(c, i + 1)) {
        int en = cp_isci(c, i, "enum"), nm = i + 1, bo = -1;
        for (int z = nm + 1; z < b; z++) { if (cp_p(c, z, '{')) { bo = z; break; } if (cp_p(c, z, '(') || cp_p(c, z, '[')) z = cp_mt(c, z); }
        if (bo < 0) return 0;
        int cl_ = cp_mt(c, bo), s = cp_sym(c, en ? CK_ENUM : CK_CLASS, parent, b0, cp_end(c, cl_), nm, b0, c->tk[bo].a);
        if (!en && cp_p(c, nm + 1, ':')) { int first = 1, last = -1; for (int z = nm + 2; z <= bo; z++) { if (z == bo || cp_p(c, z, ',')) { if (last >= 0) cp_reft(c, s, first ? CR_EXTENDS : CR_IMPLEMENTS, last); first = 0; last = -1; continue; } if (cp_idt(c, z)) last = z; } }
        if (!en) cp_scan(c, bo + 1, cl_ < b ? cl_ : b, s, CM_CLASS, dp + 1);
        return cl_ + 1 > b ? b : cl_ + 1;
    }
    if (cp_isci(c, i, "param") && cp_p(c, i + 1, '(') && mode == CM_TOP) { int pc = cp_mt(c, i + 1); cp_symx(c, CK_VAR, parent, b0, cp_end(c, pc), i, NULL, 0, b0, cp_end(c, pc), 0, 0); return pc + 1 > b ? b : pc + 1; }
    if (cp_isci(c, i, "import-module") && cp_idt(c, i + 1)) { cp_reft(c, parent, CR_IMPORT, i + 1); return 0; }
    if (cp_isci(c, i, "using") && cp_isci(c, i + 1, "module") && cp_idt(c, i + 2)) { cp_reft(c, parent, CR_IMPORT, i + 2); return 0; }
    if (cp_p(c, i, '.') && !(c->tk[i + 1].fl & CF_NL) && i + 1 < b) {
        size_t a = c->tk[i + 1].a, e = a; while (e < c->n && !strchr(" \t\r\n;|&)", c->t[e])) e++;
        while (a < e && (c->t[a] == '"' || c->t[a] == '\'')) a++;
        while (e > a && (c->t[e - 1] == '"' || c->t[e - 1] == '\'')) e--;
        if (e > a) cp_ref_range(c, parent, CR_IMPORT, a, e, 0);
    }
    return 0;
}

/* ---------- QML ---------- */
static int cp_qml_comp(CP *c, int i, int b, int parent, int dp) {
    int j = i; while (cp_idt(c, j) || cp_p(c, j, '.')) j++;
    if (cp_is(c, j - 1, "on") ) return 0;
    if (cp_is(c, j, "on") ) { j++; if (cp_idt(c, j)) j++; }
    if (!cp_p(c, j, '{') || j == i) return 0;
    int close = cp_mt(c, j), idtok = -1;
    for (int z = j + 1; z < close && z < b; z++) { if (cp_is(c, z, "id") && cp_p(c, z + 1, ':') && cp_idt(c, z + 2)) { idtok = z; break; } if (cp_open(c, z)) z = cp_mt(c, z); }
    int hasroot = 0; for (int s = 0; s < c->f->nsym; s++) if (c->f->sym[s].kind == CK_CLASS) { hasroot = 1; break; }
    int s = parent;
    if (idtok >= 0 || !hasroot) {
        size_t a = c->tk[i].a, e = c->tk[j - 1].b;
        s = cp_symx(c, CK_CLASS, parent, c->tk[i].a, cp_end(c, close), -1, c->t + a, e - a, c->tk[i].a, c->tk[j].a, idtok >= 0 ? c->tk[idtok].a : 0, idtok >= 0 ? c->tk[idtok + 2].b : 0);
    }
    cp_scan(c, j + 1, close < b ? close : b, s, CM_COMP, dp + 1);
    return close + 1 > b ? b : close + 1;
}
static int cp_qml_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    size_t b0 = c->tk[i].a;
    if (cp_is(c, i, "import")) {
        int e = i + 1; while (e < b && !(c->tk[e].fl & CF_NL)) e++;
        if (e > i + 1) cp_ref_range(c, -1, CR_IMPORT, c->tk[i + 1].a, c->tk[e - 1].b, 0);
        return e;
    }
    if (cp_is(c, i, "pragma")) { int e = i + 1; while (e < b && !(c->tk[e].fl & CF_NL)) e++; return e; }
    int j = i; while (cp_is(c, j, "readonly") || cp_is(c, j, "default") || cp_is(c, j, "required")) j++;
    if (cp_is(c, j, "property")) {
        int n = j + 1; if (cp_idt(c, n)) n++; if (cp_p(c, n, '<')) n = cp_angle(c, n, b);
        if (!cp_idt(c, n)) return 0;
        int nm = n; int s = cp_sym(c, CK_VAR, parent, b0, c->tk[nm].b, nm, b0, c->tk[nm].b);
        if (cp_p(c, nm + 1, ':')) {
            int v = nm + 2, k = v; while (cp_idt(c, k) || cp_p(c, k, '.')) k++;
            int se;
            if (k > v && cp_p(c, k, '{') && !cp_kwtok(c, v, CP_NOCALL)) { se = cp_qml_comp(c, v, b, s, dp); if (!se) se = cp_skip(c, v - 1, b, s, CM_BODY, dp); }
            else se = cp_skip(c, v - 1, b, s, CM_BODY, dp);
            cp_setend(c, s, c->tk[se - 1].b); return se;
        }
        return nm + 1;
    }
    if (cp_is(c, j, "signal") && cp_idt(c, j + 1)) {
        int nm = j + 1, e = nm; if (cp_p(c, nm + 1, '(')) e = cp_mt(c, nm + 1);
        int s = cp_sym(c, CK_METHOD, parent, b0, cp_end(c, e), nm, b0, cp_end(c, e)); (void)s; return e + 1 > b ? b : e + 1;
    }
    if (cp_is(c, j, "function") || cp_is(c, j, "enum")) { int ns = c->f->nsym; int r = cp_js_decl(c, i, b, parent, CM_TOP, dp); if (r > i && c->f->nsym > ns && c->f->sym[ns].kind == CK_FUNC) c->f->sym[ns].kind = CK_METHOD; return r; }
    if (cp_is(c, j, "component") && cp_idt(c, j + 1) && cp_p(c, j + 2, ':')) {
        int s = cp_sym(c, CK_CLASS, parent, b0, c->tk[j + 1].b, j + 1, b0, c->tk[j + 1].b); int se = cp_qml_comp(c, j + 3, b, s, dp); if (!se) se = cp_skip(c, j + 3, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
    }
    if (cp_idt(c, j)) {   /* binding: a.b.c: valor */
        int n = j; while (cp_idt(c, n) && cp_p(c, n + 1, '.')) n += 2;
        if (cp_idt(c, n) && cp_p(c, n + 1, ':')) {
            const char *last = c->c + c->tk[n].a; int handler = cp_tl(c, n) > 2 && last[0] == 'o' && last[1] == 'n' && isupper((unsigned char)last[2]);
            int v = n + 2;
            if (handler) {
                size_t a = c->tk[j].a, e = c->tk[n].b;
                int s = cp_symx(c, CK_METHOD, parent, b0, c->tk[n + 1].b, -1, c->t + a, e - a, b0, c->tk[n + 1].b, 0, 0);
                if (cp_p(c, v, '{')) { int cl_ = cp_mt(c, v); cp_setend(c, s, cp_end(c, cl_)); cp_body(c, v, b, s); return cl_ + 1 > b ? b : cl_ + 1; }
                int se = cp_skip(c, v - 1, b, s, CM_BODY, dp); cp_setend(c, s, c->tk[se - 1].b); return se;
            }
            if (cp_is(c, j, "id")) return cp_skip(c, v - 1, b, parent, CM_BODY, 250);
            int k = v; while (cp_idt(c, k) || cp_p(c, k, '.')) k++;
            if (k > v && cp_p(c, k, '{') && isupper((unsigned char)c->c[c->tk[v].a])) { int r = cp_qml_comp(c, v, b, parent, dp); if (r) return r; }
            if (cp_p(c, v, '[')) { int cl_ = cp_mt(c, v); cp_scan(c, v + 1, cl_ < b ? cl_ : b, parent, CM_ARR, dp + 1); return cl_ + 1 > b ? b : cl_ + 1; }
            return cp_skip(c, v - 1, b, parent, CM_BODY, dp);
        }
        if (isupper((unsigned char)c->c[c->tk[j].a])) return cp_qml_comp(c, j, b, parent, dp);
    }
    return 0;
}

/* ---------- despacho ---------- */
static int cp_decl(CP *c, int i, int b, int parent, int mode, int dp) {
    switch (c->lang) {
    case CL_C: case CL_CPP: return cp_c_decl(c, i, b, parent, mode, dp);
    case CL_JS: case CL_TS: return cp_js_decl(c, i, b, parent, mode, dp);
    case CL_JAVA: return cp_java_decl(c, i, b, parent, mode, dp);
    case CL_KT: return cp_kt_decl(c, i, b, parent, mode, dp);
    case CL_GO: return cp_go_decl(c, i, b, parent, mode, dp);
    case CL_RS: return cp_rs_decl(c, i, b, parent, mode, dp);
    case CL_SH: return cp_sh_decl(c, i, b, parent, mode, dp);
    case CL_PS1: return cp_ps_decl(c, i, b, parent, mode, dp);
    case CL_QML: return mode == CM_BODY ? cp_js_decl(c, i, b, parent, mode, dp) : cp_qml_decl(c, i, b, parent, mode, dp);
    case CL_CS: case CL_SWIFT: case CL_PHP: case CL_DART: case CL_SCALA: case CL_GROOVY: return c2_gdecl(c, i, b, parent, mode, dp);
    }
    return 0;
}

/* ---------- varredura de referencias ---------- */
static int cp_usable(const CoderFile *f, int s, int lang) {
    const CoderSym *x = &f->sym[s]; int pk = x->parent >= 0 ? f->sym[x->parent].kind : -1;
    if (strlen(x->name) < 2 || x->name[0] == '(') return 0;
    switch (x->kind) {
    case CK_STRUCT: case CK_CLASS: case CK_ENUM: case CK_IFACE: case CK_TYPEDEF: case CK_MACRO: return !(x->kind == CK_CLASS && lang == CL_QML);
    case CK_VAR: case CK_CONST: case CK_FUNC: return x->parent < 0 || pk == CK_NAMESPACE || pk == CK_ENUM;
    }
    return 0;
}
static void cp_sweep(CP *c) {
    CoderFile *f = c->f; int L = c->lang, nsym = f->nsym;
    int hn = 64; while (hn < nsym * 3) hn *= 2;
    int *ht = (int *)calloc((size_t)hn, sizeof(int)); unsigned char *isf = (unsigned char *)calloc((size_t)(nsym + 1), 1);
    for (int s = 0; s < nsym; s++) if (cp_usable(f, s, L)) {
        uint32_t h = cp_hash(f->sym[s].name, strlen(f->sym[s].name), 2166136261u); size_t p = h & (size_t)(hn - 1);
        while (ht[p]) p = (p + 1) & (size_t)(hn - 1);
        ht[p] = s + 1; if (f->sym[s].kind == CK_FUNC) isf[s] = 1;
    }
    int *stk = (int *)malloc(sizeof(int) * (size_t)(nsym + 2)); int sp = 0, ni = 0;
    const int shps = L == CL_SH || L == CL_PS1, gen = L == CL_TS || L == CL_KT || L == CL_JAVA || L == CL_RS || L == CL_CPP || L == CL_GO || L == CL_JS || L == CL_CS || L == CL_DART || L == CL_SWIFT || L == CL_SCALA;
    const int nlfresh = !(L == CL_C || L == CL_CPP || L == CL_JAVA || L == CL_RS || L == CL_CS || L == CL_DART || L == CL_PHP);
    for (int i = 0; i < c->nt; i++) {
        const CpTok *t = &c->tk[i]; size_t off = t->a;
        while (sp > 0 && off >= f->sym[stk[sp - 1]].byte1) sp--;
        while (ni < nsym && f->sym[ni].byte0 <= off) { if (off < f->sym[ni].byte1) stk[sp++] = ni; ni++; }
        if ((t->fl & CF_DIR) || t->t != CT_ID || c->isn[i]) continue;
        if (L == CL_RS && i >= 2 && cp_p(c, i - 1, '[') && (cp_p(c, i - 2, '#') || cp_p(c, i - 2, '!'))) continue;
        int cur = sp > 0 ? stk[sp - 1] : -1, j = i + 1;
        const char *nm = c->c + off; size_t nl = t->b - t->a;
        if (shps) {
            int cmd = i == 0 || (t->fl & CF_NL);
            if (!cmd && i > 0) { const CpTok *p = &c->tk[i - 1]; cmd = p->t == CT_P ? (p->b - p->a == 1 && strchr(";|&({!", c->c[p->a]) != NULL) || cp_is(c, i - 1, "&&") || cp_is(c, i - 1, "||") : cp_kwtok(c, i - 1, CP_PRE) || cp_is(c, i - 1, "then") || cp_is(c, i - 1, "do") || cp_is(c, i - 1, "else"); }
            int isverb = L == CL_PS1 && isupper((unsigned char)nm[0]) && memchr(nm, '-', nl) != NULL;
            if (cmd || isverb) {
                uint32_t h = cp_hash(nm, nl, 2166136261u); size_t p = h & (size_t)(hn - 1); int hit = 0;
                while (ht[p]) { int s = ht[p] - 1; if (isf[s] && strlen(f->sym[s].name) == nl && !memcmp(f->sym[s].name, c->t + off, nl)) { hit = 1; break; } p = (p + 1) & (size_t)(hn - 1); }
                if ((hit || isverb) && !(cmd && L == CL_PS1 && cp_isci(c, i, "function"))) cp_reft(c, cur, CR_CALL, i);
            }
            if (!(L == CL_PS1 && i > 0 && (cp_p(c, i - 1, '.') || cp_is(c, i - 1, "::")))) continue;
        }
        if (L == CL_JS || L == CL_TS || L == CL_QML) {
            if ((cp_is(c, i, "require") || cp_is(c, i, "import")) && cp_p(c, j, '(') && cp_strpair(c, j + 1)) { cp_import_str(c, cur, j + 1); continue; }
        }
        if (L == CL_RS && cp_p(c, j, '!') && c->tk[j].a == t->b && (cp_p(c, j + 1, '(') || cp_p(c, j + 1, '[') || cp_p(c, j + 1, '{'))) {
            char buf[80]; if (nl < 78) { memcpy(buf, nm, nl); buf[nl] = '!'; cp_ref(c, cur, CR_CALL, buf, nl + 1, off); }
            continue;
        }
        int g = j;
        if (gen) {
            if (L == CL_RS && cp_is(c, g, "::") && cp_p(c, g + 1, '<')) g++;
            if (cp_p(c, g, '<') && c->tk[g].a == c->tk[g - 1].b) {
                int k = cp_angle(c, g, c->nt < g + 48 ? c->nt : g + 48), ok = k > g + 1 && cp_p(c, k, '(');
                for (int z = g + 1; ok && z < k - 1; z++) if (!(c->tk[z].t != CT_P || strchr(",.*&[]?<>:", c->c[c->tk[z].a]) || cp_is(c, z, "::"))) ok = 0;
                if (ok) g = k;
            }
        }
        if (cp_p(c, g, '(')) {
            if (cp_inl(nm, nl, CP_NOCALL)) continue;
            if (i > 0 && c->tk[i - 1].t == CT_ID && !(c->tk[i - 1].fl & CF_DIR) && !((t->fl & CF_NL) && nlfresh) && !cp_kwtok(c, i - 1, CP_PRE) && !(L == CL_PY && (t->fl & CF_NL))) continue;
            cp_reft(c, cur, CR_CALL, i);
            continue;
        }
        if (nl < 2 || (i > 0 && (cp_p(c, i - 1, '.') || cp_is(c, i - 1, "->") || cp_is(c, i - 1, "?.")))) continue;
        uint32_t h = cp_hash(nm, nl, 2166136261u); size_t p = h & (size_t)(hn - 1);
        while (ht[p]) { int s = ht[p] - 1; if (strlen(f->sym[s].name) == nl && !memcmp(f->sym[s].name, c->t + off, nl)) { cp_reft(c, cur, CR_USE, i); break; } p = (p + 1) & (size_t)(hn - 1); }
    }
    free(ht); free(isf); free(stk);
}

/* ---------- pos-processamento: prototipos repetidos, receptor Go, ordenacao ---------- */
static CoderSym *g_cp_sortsym;
static int cp_cmp(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y; const CoderSym *s = g_cp_sortsym;
    if (s[a].byte0 != s[b].byte0) return s[a].byte0 < s[b].byte0 ? -1 : 1;
    if (s[a].byte1 != s[b].byte1) return s[a].byte1 > s[b].byte1 ? -1 : 1;
    return a - b;
}
static void cp_post(CP *c) {
    CoderFile *f = c->f; int n = f->nsym; if (n == 0) return;
    int *map = (int *)malloc(sizeof(int) * (size_t)n);
    /* prototipos que tem definicao no mesmo arquivo somem */
    unsigned char *drop = (unsigned char *)calloc((size_t)n, 1); int any = 0;
    if (c->lang == CL_C || c->lang == CL_CPP) for (int s = 0; s < n; s++) if ((c->sfl[s] & 1) && (f->sym[s].kind == CK_FUNC || f->sym[s].kind == CK_METHOD)) {
        for (int d = 0; d < n; d++) if (!(c->sfl[d] & 1) && f->sym[d].kind == f->sym[s].kind && !strcmp(f->sym[d].name, f->sym[s].name)) { drop[s] = 1; any = 1; break; }
        if (n > 4000) break;      /* arquivo enorme: nao paga O(n^2) */
    }
    if (any) {
        int w = 0;
        for (int s = 0; s < n; s++) { if (drop[s]) { map[s] = -1; free(f->sym[s].name); free(f->sym[s].sig); free(c->sx[s]); continue; } map[s] = w; f->sym[w] = f->sym[s]; c->snt[w] = c->snt[s]; c->sfl[w] = c->sfl[s]; c->sx[w] = c->sx[s]; w++; }
        for (int s = 0; s < w; s++) if (f->sym[s].parent >= 0) f->sym[s].parent = map[f->sym[s].parent];
        for (int r = 0; r < f->nref; r++) if (f->ref[r].from >= 0) f->ref[r].from = map[f->ref[r].from];
        f->nsym = n = w;
    }
    free(drop);
    if (c->lang == CL_GO) for (int s = 0; s < n; s++) if (c->sx[s]) for (int d = 0; d < n; d++) { int k = f->sym[d].kind; if ((k == CK_STRUCT || k == CK_IFACE || k == CK_TYPEDEF) && !strcmp(f->sym[d].name, c->sx[s])) { f->sym[s].parent = d; break; } }
    int sorted = 1; for (int s = 1; s < n; s++) if (f->sym[s].byte0 < f->sym[s - 1].byte0) { sorted = 0; break; }
    if (!sorted) {
        int *ord = (int *)malloc(sizeof(int) * (size_t)n); for (int s = 0; s < n; s++) ord[s] = s;
        g_cp_sortsym = f->sym; qsort(ord, (size_t)n, sizeof(int), cp_cmp);
        CoderSym *ns = (CoderSym *)malloc(sizeof(CoderSym) * (size_t)n); char **nx = (char **)malloc(sizeof(char *) * (size_t)n); unsigned char *nf = (unsigned char *)malloc((size_t)n);
        for (int k = 0; k < n; k++) { map[ord[k]] = k; ns[k] = f->sym[ord[k]]; nx[k] = c->sx[ord[k]]; nf[k] = c->sfl[ord[k]]; }
        for (int k = 0; k < n; k++) if (ns[k].parent >= 0) ns[k].parent = map[ns[k].parent];
        for (int r = 0; r < f->nref; r++) if (f->ref[r].from >= 0) f->ref[r].from = map[f->ref[r].from];
        memcpy(f->sym, ns, sizeof(CoderSym) * (size_t)n); memcpy(c->sx, nx, sizeof(char *) * (size_t)n); memcpy(c->sfl, nf, (size_t)n);
        free(ns); free(nx); free(nf); free(ord);
    }
    free(map);
}

/* ---------- API ---------- */
static int coder_lang_sniff(const char *t, size_t n) {
    if (n < 3 || t[0] != '#' || t[1] != '!') return CL_UNKNOWN;
    size_t e = 0; while (e < n && e < 200 && t[e] != '\n') e++;
    char l[210]; memcpy(l, t, e); l[e] = 0;
    if (strstr(l, "python")) return CL_PY;
    if (strstr(l, "node") || strstr(l, "deno") || strstr(l, "bun")) return CL_JS;
    if (strstr(l, "pwsh") || strstr(l, "powershell")) return CL_PS1;
    if (strstr(l, "bash") || strstr(l, "zsh") || strstr(l, "ksh") || strstr(l, "dash") || strstr(l, "/sh") || strstr(l, " sh")) return CL_SH;
    return CL_UNKNOWN;
}
static int coder_lang_of(const char *path) {
    const char *base = path, *dot = NULL;
    for (const char *p = path; *p; p++) { if (*p == '/' || *p == '\\') { base = p + 1; dot = NULL; } else if (*p == '.') dot = p; }
    { int x = c2_lang_of(base, dot && dot > base ? dot + 1 : NULL); if (x != CL_UNKNOWN) return x; }
    if (dot && dot > base) {
        char e[12]; size_t l = strlen(dot + 1); if (l > 8) return CL_UNKNOWN;
        for (size_t k = 0; k <= l; k++) e[k] = (char)tolower((unsigned char)dot[1 + k]);
        static const struct { const char *e; int l; } T[] = {{"c", CL_C}, {"h", CL_C}, {"cpp", CL_CPP}, {"cc", CL_CPP}, {"cxx", CL_CPP}, {"hpp", CL_CPP}, {"hh", CL_CPP}, {"hxx", CL_CPP}, {"inl", CL_CPP}, {"ipp", CL_CPP}, {"tpp", CL_CPP},
            {"js", CL_JS}, {"mjs", CL_JS}, {"cjs", CL_JS}, {"jsx", CL_JS}, {"ts", CL_TS}, {"tsx", CL_TS}, {"mts", CL_TS}, {"cts", CL_TS}, {"py", CL_PY}, {"pyw", CL_PY}, {"kt", CL_KT}, {"kts", CL_KT},
            {"java", CL_JAVA}, {"go", CL_GO}, {"rs", CL_RS}, {"sh", CL_SH}, {"bash", CL_SH}, {"zsh", CL_SH}, {"ksh", CL_SH}, {"qml", CL_QML}, {"ps1", CL_PS1}, {"psm1", CL_PS1}, {"psd1", CL_PS1},
            {"md", CL_MD}, {"markdown", CL_MD}, {"mdx", CL_MD}, {"json", CL_CONFIG}, {NULL, 0}};
        for (int k = 0; T[k].e; k++) if (!strcmp(T[k].e, e)) return T[k].l;
        return CL_UNKNOWN;
    }
    FILE *fp = fopen(path, "rb"); if (!fp) return CL_UNKNOWN;
    char h[256]; size_t n = fread(h, 1, sizeof h, fp); fclose(fp);
    return coder_lang_sniff(h, n);
}
static void coder_file_free(CoderFile *f) {
    if (!f) return;
    for (int i = 0; i < f->nsym; i++) { free(f->sym[i].name); free(f->sym[i].sig); }
    for (int i = 0; i < f->nref; i++) free(f->ref[i].name);
    free(f->sym); free(f->ref); free(f->guard); memset(f, 0, sizeof *f);
}
/* preenche os campos novos que a analise sintatica ja permite: corpo, visibilidade, flags e confianca do intervalo. O que nao da para saber fica em zero (desconhecido). */
static int cp_sig_has(const char *sig, const char *w) {
    size_t wl = strlen(w);
    for (const char *p = sig; (p = strstr(p, w)); p += wl) {
        int pre = p == sig || !cp_idc((unsigned char)p[-1]), post = !cp_idc((unsigned char)p[wl]);
        if (pre && post) return 1;
    }
    return 0;
}
/* C/C++: #include "x.h" e #include <x.h> viram referencias de import no nivel do arquivo (so o texto entre aspas ou colchetes) */
static void cp_c_includes(CP *c) {
    const char *t = c->t; size_t n = c->n;
    for (size_t i = 0; i < n; ) {
        size_t e = i; while (e < n && t[e] != '\n') e++;
        size_t k = i; while (k < e && (t[k] == ' ' || t[k] == '\t')) k++;
        if (k < e && t[k] == '#') {
            k++; while (k < e && (t[k] == ' ' || t[k] == '\t')) k++;
            if (e - k > 7 && !strncmp(t + k, "include", 7)) {
                k += 7; while (k < e && (t[k] == ' ' || t[k] == '\t')) k++;
                if (k < e && (t[k] == '"' || t[k] == '<')) {
                    char close = t[k] == '"' ? '"' : '>'; size_t a = ++k;
                    while (k < e && t[k] != close) k++;
                    if (k < e && k > a) cp_ref(c, -1, CR_IMPORT, t + a, k - a, a);
                }
            }
        }
        i = e + 1;
    }
}
static void cp_enrich(CP *c) {
    if (c2_isline(c->lang)) { c2_enrich(c); return; }
    if (c->lang == CL_C || c->lang == CL_CPP) cp_c_includes(c);
    CoderFile *f = c->f; int L = c->lang;
    for (int s = 0; s < f->nsym; s++) {
        CoderSym *y = &f->sym[s]; const char *sg = y->sig ? y->sig : "";
        if (c->sfl[s] & 1) y->flags |= CSF_DECL;
        if (L != CL_MD && y->kind != CK_HEADING && !(y->flags & CSF_DECL)) {
            if (L == CL_PY) {        /* corpo = depois da primeira quebra de linha do cabecalho */
                for (size_t k = y->byte0; k < y->byte1 && k < c->n; k++) if (c->c[k] == '\n') { if (k + 1 < y->byte1) y->body0 = k + 1; break; }
            } else for (size_t k = y->byte0; k < y->byte1 && k < c->n; k++) if (c->c[k] == '{') { y->body0 = k; break; }
        }
        if (y->kind == CK_MACRO) y->conf = CC_HEUR;
        if (cp_sig_has(sg, "async")) y->flags |= CSF_ASYNC;
        if (cp_sig_has(sg, "static")) y->flags |= CSF_STATIC;
        if (L == CL_C || L == CL_CPP) { if (y->flags & CSF_STATIC) y->vis = CV_FILE; else if (y->parent < 0) y->vis = CV_PUBLIC; }
        else if (L == CL_JS || L == CL_TS) { if (cp_sig_has(sg, "export")) y->vis = CV_EXPORTED; else if (y->name[0] == '#') y->vis = CV_PRIVATE; else if (y->parent < 0) y->vis = CV_FILE; }
        else if (L == CL_PY) { if (y->name[0] == '_' && y->name[1] != '_') y->vis = CV_FILE; else if (y->parent < 0) y->vis = CV_PUBLIC; }
        else if (L == CL_GO) y->vis = isupper((unsigned char)y->name[0]) ? CV_EXPORTED : CV_FILE;
        else if (L == CL_RS) y->vis = cp_sig_has(sg, "pub") ? CV_EXPORTED : CV_FILE;
        if (L == CL_JAVA || L == CL_KT || L == CL_CPP || L == CL_TS || c2_istok(L)) {
            if (cp_sig_has(sg, "private")) y->vis = CV_PRIVATE; else if (cp_sig_has(sg, "protected")) y->vis = CV_PROTECTED; else if (cp_sig_has(sg, "public")) y->vis = CV_PUBLIC;
        }
    }
}
static int coder_parse(const char *path, const char *text, size_t n, CoderFile *out) {
    memset(out, 0, sizeof *out);
    int L = coder_lang_of(path);
    if (L == CL_UNKNOWN) L = coder_lang_sniff(text, n);
    out->lang = L;
    if (L == CL_UNKNOWN || L == CL_CONFIG || n == 0 || n >= 0xFFFFFFF0u) return 0;
    if (L == CL_C) { size_t pl = strlen(path); if (pl > 2 && path[pl - 2] == '.' && (path[pl - 1] == 'h' || path[pl - 1] == 'H')) { size_t m = n < 65536 ? n : 65536; for (size_t k = 0; k + 8 < m; k++) if ((text[k] == 'c' && !strncmp(text + k, "class ", 6)) || !strncmp(text + k, "namespace ", 10) || !strncmp(text + k, "template<", 9) || !strncmp(text + k, "template <", 10)) { L = out->lang = CL_CPP; break; } } }
    CP c; memset(&c, 0, sizeof c);
    c.lang = L; c.f = out; c.n = n; c.path = path;
    char *cl = (char *)malloc(n + 1), *nc = (char *)malloc(n + 1);
    if (L == CL_MD) { memcpy(cl, text, n); memcpy(nc, text, n); } else if (c2_isline(L) || c2_istok(L)) c2_mask(L, text, n, cl, nc); else cp_mask(L, text, n, cl, nc);
    cl[n] = nc[n] = 0; c.c = cl; c.t = nc;
    c.nln = 1; for (size_t k = 0; k < n; k++) if (text[k] == '\n') c.nln++;
    c.ln = (size_t *)malloc(sizeof(size_t) * (size_t)c.nln); { int q = 0; c.ln[q++] = 0; for (size_t k = 0; k < n; k++) if (text[k] == '\n') c.ln[q++] = k + 1; }
    c.nested = L == CL_JS || L == CL_TS || L == CL_QML || L == CL_KT || L == CL_RS || L == CL_PS1;
    if (L != CL_MD && !c2_isline(L)) {
        cp_tokenize(&c); cp_match(&c);
        c.isn = (unsigned char *)calloc((size_t)c.nt + 1, 1);
        for (int s = 0; s < out->nsym; s++) if (c.snt[s] >= 0 && c.snt[s] < c.nt) c.isn[c.snt[s]] = 1;
    } else { c.isn = (unsigned char *)calloc(1, 1); c.mt = (int *)malloc(sizeof(int)); }
    if (L == CL_PY) cp_py(&c); else if (L == CL_MD) cp_md(&c); else if (c2_isline(L)) c2_parse_line(&c); else cp_scan(&c, 0, c.nt, -1, CM_TOP, 0);
    cp_post(&c);
    cp_enrich(&c);
    if (c2_isline(L)) c2_sweep_line(&c); else if (L != CL_MD) { cp_rehash(&c); cp_sweep(&c); }
    if (c.sx) { for (int s = 0; s < out->nsym; s++) free(c.sx[s]); }
    free(c.sx); free(c.snt); free(c.sfl); free(c.rh); free(c.tk); free(c.mt); free(c.isn); free(c.ln); free(cl); free(nc);
    return 0;
}

/* ---------- katana coder symbols <arquivo|pasta> [--refs] [--json] ---------- */
static const char *const CP_KINDN[] = {"file", "func", "method", "class", "struct", "enum", "iface", "var", "const", "macro", "typedef", "namespace", "heading", "ctor", "union", "enumerator", "field", "property", "signal", "section"};
static const char *const CP_LANGN[] = {"?", "c", "c++", "js", "ts", "python", "kotlin", "java", "go", "rust", "shell", "qml", "powershell", "markdown", "config",
    "c#", "swift", "php", "ruby", "lua", "dart", "scala", "sql", "json", "yaml", "toml", "dockerfile", "makefile", "cmake", "html", "css",
    "clojure", "lisp", "elixir", "erlang", "haskell", "ocaml", "r", "julia", "perl", "groovy", "objc", "zig", "nim", "v", "fortran", "cobol", "asm",
    "proto", "graphql", "terraform", "vue", "latex", "markup-docs", "xml", "properties"};
static const char *const CP_REFN[] = {"call", "use", "import", "extends", "implements", "type", "write", "literal"};
_Static_assert(sizeof(CP_LANGN) / sizeof(CP_LANGN[0]) == CL_N, "CP_LANGN deve ter um nome por CoderLang");
_Static_assert(sizeof(CP_KINDN) / sizeof(CP_KINDN[0]) == CK_N, "CP_KINDN deve ter um nome por CoderKind");
typedef struct { int refs, json, nfiles, nsym, nref; } CpOut;
static void cp_jstr(const char *s) {
    putchar('"');
    for (; *s; s++) { unsigned char ch = (unsigned char)*s; if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); } else if (ch < 0x20) printf("\\u%04x", ch); else putchar(ch); }
    putchar('"');
}
static void cp_print(const char *path, const CoderFile *f, CpOut *o) {
    o->nfiles++; o->nsym += f->nsym; o->nref += f->nref;
    if (o->json) {
        printf("{\"path\":"); cp_jstr(path); printf(",\"lang\":\"%s\",\"symbols\":[", CP_LANGN[f->lang]);
        for (int i = 0; i < f->nsym; i++) { const CoderSym *s = &f->sym[i]; printf("%s{\"i\":%d,\"name\":", i ? "," : "", i); cp_jstr(s->name); printf(",\"kind\":\"%s\",\"sig\":", CP_KINDN[s->kind]); cp_jstr(s->sig); printf(",\"parent\":%d,\"line0\":%d,\"line1\":%d,\"byte0\":%zu,\"byte1\":%zu}", s->parent, s->line0, s->line1, s->byte0, s->byte1); }
        printf("],\"refs\":[");
        for (int i = 0; i < f->nref; i++) { const CoderRef *r = &f->ref[i]; printf("%s{\"from\":%d,\"kind\":\"%s\",\"name\":", i ? "," : "", r->from, CP_REFN[r->kind]); cp_jstr(r->name); printf(",\"line\":%d}", r->line); }
        printf("]}\n"); return;
    }
    printf("%s  [%s]  %d symbols\n", path, CP_LANGN[f->lang], f->nsym);
    int *cnt = NULL, *idx = NULL;
    if (o->refs) {
        cnt = (int *)calloc((size_t)f->nsym + 2, sizeof(int)); idx = (int *)malloc(sizeof(int) * (size_t)(f->nref + 1));
        for (int r = 0; r < f->nref; r++) cnt[f->ref[r].from + 1 + 1]++;
        for (int s = 1; s < f->nsym + 2; s++) cnt[s] += cnt[s - 1];
        int *pos = (int *)malloc(sizeof(int) * (size_t)(f->nsym + 2)); memcpy(pos, cnt, sizeof(int) * (size_t)(f->nsym + 2));
        for (int r = 0; r < f->nref; r++) idx[pos[f->ref[r].from + 1]++] = r;
        free(pos);
    }
#define PRINTREFS(slot, pad) do { int any = 0; for (int k = 0; k < CR_N; k++) { int first = 1; \
        for (int q = cnt[slot]; q < cnt[(slot) + 1]; q++) { const CoderRef *r = &f->ref[idx[q]]; if (r->kind != k) continue; \
            if (first) { printf("%*s%s:", pad, "", CP_REFN[k]); first = 0; any = 1; } printf(" %s", r->name); } if (!first) printf("\n"); } (void)any; } while (0)
    if (o->refs && cnt[1] > cnt[0]) { printf("  (file)\n"); PRINTREFS(0, 4); }
    for (int i = 0; i < f->nsym; i++) {
        const CoderSym *s = &f->sym[i]; int d = 0; for (int p = s->parent; p >= 0 && d < 40; p = f->sym[p].parent) d++;
        printf("%*s%d-%d %s %s  %s\n", 2 + d * 2, "", s->line0, s->line1, CP_KINDN[s->kind], s->name, s->sig);
        if (o->refs) PRINTREFS(i + 1, 4 + d * 2 + 2);
    }
    free(cnt); free(idx);
}
static void cp_do_file(const char *path, CpOut *o, size_t maxb) {
    FILE *fp = fopen(path, "rb"); if (!fp) { fprintf(stderr, "coder symbols: nao abre %s\n", path); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > maxb) { fclose(fp); return; }
    char *buf = (char *)malloc((size_t)sz + 1); size_t n = fread(buf, 1, (size_t)sz, fp); fclose(fp); buf[n] = 0;
    if (memchr(buf, 0, n < 8000 ? n : 8000)) { free(buf); return; }
    CoderFile f; coder_parse(path, buf, n, &f);
    if (f.lang != CL_UNKNOWN && f.lang != CL_CONFIG) cp_print(path, &f, o);
    coder_file_free(&f); free(buf);
}
static int cp_namecmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void cp_walk(const char *dir, CpOut *o) {
    DIR *d = opendir(dir); if (!d) return;
    char **names = NULL; int nn = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; if (nn >= cap) { cap = cap * 2 + 16; names = (char **)realloc(names, (size_t)cap * sizeof(char *)); } names[nn++] = cp_dup(e->d_name, strlen(e->d_name)); }
    closedir(d);
    qsort(names, (size_t)nn, sizeof(char *), cp_namecmp);
    static const char *const SKIP[] = {".git", "node_modules", "build", "dist", "vendor", ".katana", ".gradle", "__pycache__", ".idea", NULL};
    for (int i = 0; i < nn; i++) {
        char *p = (char *)malloc(strlen(dir) + strlen(names[i]) + 2); sprintf(p, "%s/%s", dir, names[i]);
        struct stat st;
#ifdef _WIN32
        int ok = stat(p, &st) == 0;
#else
        int ok = lstat(p, &st) == 0;
#endif
        int skip = 0; for (int k = 0; SKIP[k]; k++) if (!strcmp(names[i], SKIP[k])) skip = 1;
        if (ok && !skip) {
#ifdef S_ISLNK
            if (S_ISLNK(st.st_mode)) { free(p); free(names[i]); continue; }
#endif
            if (S_ISDIR(st.st_mode)) cp_walk(p, o);
            else if (S_ISREG(st.st_mode) && coder_lang_of(p) != CL_UNKNOWN) cp_do_file(p, o, 1u << 20);
        }
        free(p); free(names[i]);
    }
    free(names);
}
static int coder_symbols_main(int argc, char **argv) {
    CpOut o; memset(&o, 0, sizeof o); const char *path = NULL;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--refs")) o.refs = 1; else if (!strcmp(argv[i], "--json")) o.json = 1; else path = argv[i]; }
    if (!path) { fprintf(stderr, "uso: katana coder symbols <arquivo|pasta> [--refs] [--json]\n"); return 2; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "coder symbols: %s nao existe\n", path); return 1; }
    if (S_ISDIR(st.st_mode)) { size_t l = strlen(path); char *d = cp_dup(path, l); while (l > 1 && d[l - 1] == '/') d[--l] = 0; cp_walk(d, &o); free(d); }
    else cp_do_file(path, &o, 1u << 20);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "%d arquivos, %d simbolos, %d referencias, %.0f ms\n", o.nfiles, o.nsym, o.nref, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    return 0;
}
