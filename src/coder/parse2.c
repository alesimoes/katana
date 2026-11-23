/* ---------- coder/parse2.c: linguagens e formatos acrescentados (mesma infraestrutura de parse.c) ----------
 * Duas familias novas, sobre a mesma CP/CoderFile de parse.c:
 *  (A) por TOKENS (reutiliza tokenizador, tabela de pares, cp_scan, cp_sweep): C#, Swift, PHP, Dart, Scala, Groovy/Gradle.
 *      Um unico reconhecedor parametrizado (c2_gdecl) com uma tabela de palavras-chave por linguagem.
 *  (B) por LINHA / indentacao / parenteses sobre o texto mascarado: Ruby, Lua, Elixir, Julia, Erlang, Haskell, OCaml/F#, R, Perl,
 *      Objective-C, Zig, Nim, V, Fortran, COBOL, Assembly, Clojure/Lisp/Scheme/Racket, SQL, JSON de projeto, YAML, TOML, Dockerfile,
 *      Makefile, CMake, HTML, CSS/SCSS, Protobuf/Thrift, GraphQL, Terraform, Vue/Svelte, LaTeX, rst/AsciiDoc/Org, XML/XAML/pom,
 *      .properties/.env/.ini/.gitignore.
 * A mascara (comentarios, strings, heredocs, raw strings) e propria (c2_mask) e so roda nessas linguagens: as antigas nao mudam.
 *
 * O QUE CADA UM NAO RESOLVE (falhas conhecidas):
 *  - Todos: sem resolucao de nomes; chamadas sao so o nome final; (B) so atribui chamadas a `nome(` (sem parenteses = nao aparece).
 *  - C#: expressoes lambda/locais nao viram simbolo; regioes #if/#region sao ignoradas como linhas de diretiva; strings
 *    interpoladas com aspas internas ($"{a["k"]}") podem desbalancear; top-level statements nao viram simbolo; operadores
 *    sobrecarregados e indexadores (this[...]) sao ignorados.
 *  - Swift: closures e funcoes locais nao viram simbolo; interpolacao \( ) com aspas aninhadas pode desbalancear; #if e ignorado;
 *    `extension` leva o nome do tipo estendido (o conformances viram EXTENDS/IMPLEMENTS).
 *  - PHP: HTML fora de <?php ?> e apagado; closures/arrow functions nao viram simbolo; heredoc com interpolacao complexa; "use"
 *    dentro de classe (trait) conta como import.
 *  - Dart: genericos com funcoes como tipo, records e patterns (3.x) podem enganar a procura do nome; extensoes sem nome viram "on Tipo".
 *  - Scala: so sintaxe com chaves/`=`; Scala 3 por indentacao (`class A:`) nao tem corpo reconhecido; implicits/givens por nome simples.
 *  - Groovy/Gradle: closures de DSL (`dependencies {}`) viram secao so no nivel de topo; metodos sem tipo nem `def` nao aparecem.
 *  - Ruby: `end` e contado por palavra-chave (modificadores postfix if/unless/while sao distinguidos por posicao na linha, nao por
 *    gramatica); %w/%q e regex literais nao sao mascarados; heredoc <<~X e <<-X sim; chamadas sem parenteses nao aparecem.
 *  - Lua: blocos por function/if/do/for/while/repeat..end, until; metodos `function A.b:c()` viram "A.b:c" (so o nome final).
 *  - Elixir: blocos do/end e fn/end; `def f, do: x` em uma linha; sigils (~r//) nao mascarados; macros que geram def nao aparecem.
 *  - Julia, Fortran: fim por palavra-chave; comprehensions e `end` em indices sao ignorados dentro de colchetes.
 *  - Erlang: clausulas consecutivas de mesmo nome viram UM simbolo; fim = ponto final seguido de espaco (pode errar com ponto em atomo).
 *  - Haskell: so declaracoes em coluna 0 (e membros indentados de class/instance); guardas, where e let locais nao viram simbolo; operadores
 *    definidos infixos nao aparecem; chamadas nao sao extraidas (sem parenteses).
 *  - OCaml/F#: `let` local nao e simbolo; fim por indentacao (corpo no mesmo nivel do let confunde); F# ativo por offside, `module X =` aninhado so por indentacao.
 *  - R: so `nome <- function`/`nome = function` e atribuicoes de topo; S4/R6 (setClass/R6Class) viram so a atribuicao.
 *  - Perl: sub/package/use/require; prototipos e subs anonimas nao; pod apagado; regex e q()/qq() nao mascarados.
 *  - Objective-C: @interface/@implementation/@protocol e metodos -/+ ; blocos e funcoes C no mesmo arquivo viram so metodos/funcoes simples.
 *  - Zig/Nim/V: por palavra-chave (fn/pub fn, proc/func/method/template/macro, fn) e struct/enum/union/type; comptime e pragmas nao.
 *  - COBOL e Assembly: heuristicos (divisoes/secoes/paragrafos; rotulos); sem expansao de COPY/macros.
 *  - Clojure/Lisp/Scheme/Racket: so formas de topo conhecidas (defn, def, defmacro, defprotocol, defrecord, deftype, defmulti, defmethod, ns...
 *    em Clojure; defun, defmacro, defvar, defclass, defstruct, define... nos demais); macros do usuario que expandem em def nao aparecem;
 *    #_ so descarta a forma seguinte no topo; condicionais de leitor #?( ) sao analisados como se todos os ramos valessem.
 *  - SQL: dialetos so por palavra-chave; corpo de procedure com BEGIN/END e CASE/END contados; referencias de tabela = nome apos
 *    from/join/into/update/table/references (aliases e CTEs viram referencias).
 *  - JSON: so package.json/composer.json/tsconfig e afins (outros JSON continuam sem simbolos); YAML: so chaves de primeiro nivel e as
 *    de segundo nivel de mapas; ancoras/merge e multi-documento nao; TOML: tabelas [a.b], [[a]] e chaves de topo.
 *  - Dockerfile: estagios (FROM), ARG/ENV, EXPOSE/ENTRYPOINT/CMD como constantes; heredocs de RUN pulados. Makefile: alvos, variaveis,
 *    include; regras geradas por eval/define nao. CMake: project, add_executable, add_library, target_link_libraries/function/macro/set/option; generator expressions sao texto.
 *  - HTML/XML: nao ha DOM; so tags com id/name e imports (script src, link href, include); elementos nao fechados (void) tolerados por pilha limitada.
 *  - CSS/SCSS: seletores de classe/id (uma entrada por classe) e @mixin/@function/@keyframes/variaveis; selecionadores dentro de :is()/:not() contam;
 *    aninhamento `&-sufixo` nao gera nome.
 *  - Protobuf/Thrift/GraphQL/Terraform: por palavra-chave e chaves; Terraform: bloco resource/data/module/variable/output/locals/provider.
 *  - Vue/Svelte: secoes <script>/<template>/<style> e analise do script como JS/TS (offsets traduzidos); expressoes do template nao.
 *  - LaTeX: \section..\subparagraph, \newcommand/\def, \usepackage/\input/\include; rst/AsciiDoc/Org: titulos apenas.
 *  - .properties/.env/.ini/.gitignore: chaves/padroes de nivel unico (capados em 400).
 * Tudo e tolerante: laco sempre avanca, recursao limitada, tabelas com capacidade fixa; arquivo malformado termina no fim do texto.
 */

/* ---------- configuracao da mascara por linguagem ---------- */
typedef struct {
    const char *lc[3];            /* inicios de comentario de linha */
    const char *bo, *bc; int nest;   /* comentario de bloco */
    int sq, dq, bt, tri;          /* aspas simples: 0 nada, 1 string, 2 so literal de char; dq string; bt crase = string; tri = """ """ (e ''' ''' se sq==1) */
    int hd;                       /* heredoc <<ID (1 sim, 2 so <<<ID / <<~ID / <<-ID) */
    const char *lb, *le;          /* bloco que comeca no inicio de linha (=begin ... =end) */
    int sw;                       /* strings cruas #"..."# (Swift) */
    int bs;                       /* barra invertida + char fora de string some (Lisp: \( ) */
} C2M;
static C2M c2_mcfg(int L) {
    C2M m; memset(&m, 0, sizeof m); m.dq = 1;
    switch (L) {
    case CL_CS: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; m.tri = 1; break;
    case CL_SWIFT: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.nest = 1; m.tri = 1; m.sw = 1; break;
    case CL_PHP: m.lc[0] = "//"; m.lc[1] = "#"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; m.hd = 2; break;
    case CL_DART: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.nest = 1; m.sq = 1; m.tri = 1; break;
    case CL_SCALA: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.nest = 1; m.sq = 2; m.tri = 1; break;
    case CL_GROOVY: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; m.tri = 1; break;
    case CL_RB: m.lc[0] = "#"; m.sq = 1; m.bt = 1; m.hd = 2; m.lb = "=begin"; m.le = "=end"; break;
    case CL_LUA: m.lc[0] = "--"; m.sq = 1; break;
    case CL_EX: m.lc[0] = "#"; m.sq = 1; m.tri = 1; break;
    case CL_JL: m.lc[0] = "#"; m.bo = "#="; m.bc = "=#"; m.nest = 1; m.sq = 2; m.tri = 1; break;
    case CL_ERL: m.lc[0] = "%"; m.sq = 1; break;
    case CL_HS: m.lc[0] = "--"; m.bo = "{-"; m.bc = "-}"; m.nest = 1; m.sq = 2; break;
    case CL_ML: m.lc[0] = "//"; m.bo = "(*"; m.bc = "*)"; m.nest = 1; m.sq = 2; break;
    case CL_R: m.lc[0] = "#"; m.sq = 1; break;
    case CL_PERL: m.lc[0] = "#"; m.sq = 1; m.hd = 1; m.lb = "=pod"; m.le = "=cut"; break;
    case CL_OBJC: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; break;
    case CL_ZIG: m.lc[0] = "//"; m.sq = 1; break;
    case CL_NIM: m.lc[0] = "#"; m.bo = "#["; m.bc = "]#"; m.nest = 1; m.sq = 2; m.tri = 1; break;
    case CL_VLANG: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.nest = 1; m.sq = 1; break;
    case CL_FORTRAN: m.lc[0] = "!"; m.sq = 1; break;
    case CL_COBOL: m.lc[0] = "*>"; m.sq = 1; break;
    case CL_ASM: m.lc[0] = ";"; m.lc[1] = "//"; m.lc[2] = "#"; m.bo = "/*"; m.bc = "*/"; m.dq = 1; m.sq = 2; break;
    case CL_CLJ: case CL_LISP: m.lc[0] = ";"; m.bo = "#|"; m.bc = "|#"; m.nest = 1; m.bs = 1; break;
    case CL_SQL: m.lc[0] = "--"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; m.dq = 0; break;
    case CL_PROTO: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; break;
    case CL_GQL: m.lc[0] = "#"; m.tri = 1; break;
    case CL_CMAKE: m.lc[0] = "#"; m.bo = "#[["; m.bc = "]]"; break;
    case CL_TF: m.lc[0] = "#"; m.lc[1] = "//"; m.bo = "/*"; m.bc = "*/"; m.hd = 1; break;
    case CL_CSS: m.lc[0] = "//"; m.bo = "/*"; m.bc = "*/"; m.sq = 1; break;
    case CL_HTML: case CL_XML: m.bo = "<!--"; m.bc = "-->"; m.dq = 0; break;
    case CL_TEX: m.lc[0] = "%"; m.dq = 0; break;
    default: m.dq = 0; break;
    }
    return m;
}
static int c2_isword(int ch) { return isalnum(ch) || ch == '_' || (ch & 0x80); }
/* hash simples: devolve 1 se o texto em t+i comeca com a (nao vazio) */
static int c2_at(const char *t, size_t n, size_t i, const char *a) { size_t l = strlen(a); return l && i + l <= n && !memcmp(t + i, a, l); }
/* apaga [a,b) na copia sem comentarios (nc) e na copia sem strings (cl) */
static void c2_mask(int L, const char *t, size_t n, char *cl, char *nc) {
    memcpy(cl, t, n); memcpy(nc, t, n);
    C2M m = c2_mcfg(L);
    size_t i = 0; int php_html = L == CL_PHP; char hd[4][64]; int hdash[4], hn = 0; char last = 0; char lw[16] = ""; int lwl = 0;
    if (L == CL_PHP) { size_t k = 0; int any = 0; while (k + 1 < n) { if (t[k] == '<' && t[k + 1] == '?') { any = 1; break; } k++; } if (!any) php_html = 0; }
    while (i < n) {
        char c = t[i];
        if (php_html) {                               /* texto fora de <?php ... ?> */
            size_t e = i; while (e + 1 < n && !(t[e] == '<' && t[e + 1] == '?')) e++;
            if (e + 1 >= n) e = n;
            cp_blank(cl, i, e); cp_blank(nc, i, e); i = e;
            if (i < n) { size_t m0 = i; i += 2; if (c2_at(t, n, i, "php")) i += 3; else if (i < n && t[i] == '=') i++; cp_blank(cl, m0, i); cp_blank(nc, m0, i); php_html = 0; }
            continue;
        }
        if (L == CL_PHP && c == '?' && i + 1 < n && t[i + 1] == '>') { cp_blank(cl, i, i + 2); cp_blank(nc, i, i + 2); i += 2; php_html = 1; continue; }
        if (c == '\n') {
            i++;
            for (int h = 0; h < hn; h++) {            /* corpo dos heredocs pendentes */
                size_t dl = strlen(hd[h]);
                while (i < n) {
                    size_t e = i; while (e < n && t[e] != '\n') e++;
                    size_t s = i; if (hdash[h] || 1) while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
                    int hit = e - s >= dl && !memcmp(t + s, hd[h], dl) && (e - s == dl || t[s + dl] == ';' || t[s + dl] == ',' || t[s + dl] == ')' || t[s + dl] == '\r');
                    if (hit) { cp_blank(cl, i, s + dl); i = s + dl; break; }
                    cp_blank(cl, i, e); i = e < n ? e + 1 : n;
                }
            }
            hn = 0;
            if (m.lb && i < n && c2_at(t, n, i, m.lb)) {
                size_t e = i;
                while (e < n) { size_t le_ = e; while (le_ < n && t[le_] != '\n') le_++; int hit = c2_at(t, n, e, m.le); e = le_ < n ? le_ + 1 : n; if (hit) break; }
                cp_blank(cl, i, e); cp_blank(nc, i, e); i = e;
            }
            if ((L == CL_RB || L == CL_PERL) && i < n && c2_at(t, n, i, "__END__") && (i + 7 >= n || t[i + 7] == '\n' || t[i + 7] == '\r')) { cp_blank(cl, i, n); cp_blank(nc, i, n); i = n; }
            if (L == CL_COBOL && i + 6 < n && (t[i + 6] == '*' || t[i + 6] == '/')) { size_t e = i; while (e < n && t[e] != '\n') e++; cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; }
            continue;
        }
        if (i == 0) {
            if (m.lb && c2_at(t, n, 0, m.lb)) { size_t e = 0; while (e < n) { size_t le_ = e; while (le_ < n && t[le_] != '\n') le_++; int hit = c2_at(t, n, e, m.le); e = le_ < n ? le_ + 1 : n; if (hit) break; } cp_blank(cl, 0, e); cp_blank(nc, 0, e); i = e; continue; }
            if (L == CL_COBOL && n > 6 && (t[6] == '*' || t[6] == '/')) { size_t e = 0; while (e < n && t[e] != '\n') e++; cp_blank(cl, 0, e); cp_blank(nc, 0, e); i = e; continue; }
            if ((L == CL_CLJ || L == CL_LISP) && c2_at(t, n, 0, "#lang")) { size_t e = 0; while (e < n && t[e] != '\n') e++; cp_blank(cl, 0, e); cp_blank(nc, 0, e); i = e; continue; }
        }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (L == CL_LUA && c == '-' && c2_at(t, n, i, "--")) {      /* --[[ ]] e --[==[ ]==] */
            size_t k = i + 2; if (k < n && t[k] == '[') { size_t q = k + 1; int lv = 0; while (q < n && t[q] == '=') { lv++; q++; } if (q < n && t[q] == '[') { size_t e = q + 1; for (; e < n; e++) if (t[e] == ']') { size_t z = e + 1; int ok = 1; for (int w = 0; w < lv; w++) { if (z >= n || t[z] != '=') { ok = 0; break; } z++; } if (ok && z < n && t[z] == ']') { e = z + 1; break; } } if (e > n) e = n; cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue; } }
        }
        if (m.bo && c2_at(t, n, i, m.bo)) {
            size_t bl = strlen(m.bo), cl_ = strlen(m.bc), e = i + bl; int d = 1;
            while (e < n && d > 0) { if (c2_at(t, n, e, m.bc)) { d--; e += cl_; } else if (m.nest && c2_at(t, n, e, m.bo)) { d++; e += bl; } else e++; }
            if (e > n) e = n;
            cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue;
        }
        int islc = 0;
        for (int k = 0; k < 3 && m.lc[k]; k++) if (c2_at(t, n, i, m.lc[k])) {
            if (L == CL_PHP && m.lc[k][0] == '#' && i + 1 < n && t[i + 1] == '[') continue;
            if (L == CL_PERL && i > 0 && t[i - 1] == '$') continue;
            if (L == CL_RB && i > 0 && t[i - 1] == '$') continue;
            if (L == CL_HS && i + 2 < n && strchr("!#$%&*+./<=>?@\\^|~:", t[i + 2]) && t[i + 2] != '-') continue;
            if (L == CL_HS && i > 0 && strchr("!#$%&*+./<=>?@\\^|~:", t[i - 1])) continue;
            if (L == CL_ASM && m.lc[k][0] == '#' && i > 0 && !isspace((unsigned char)t[i - 1])) continue;
            if (L == CL_CSS && m.lc[k][0] == '/' && ((i > 0 && t[i - 1] == ':') )) continue;
            if (L == CL_ERL && i > 0 && t[i - 1] == '$') continue;
            if (L == CL_FORTRAN && 0) continue;
            islc = 1; break;
        }
        if (islc) {
            size_t e = i; while (e < n && t[e] != '\n') { if (L == CL_PHP && t[e] == '?' && e + 1 < n && t[e + 1] == '>') break; e++; }
            cp_blank(cl, i, e); cp_blank(nc, i, e); i = e; continue;
        }
        if (L == CL_ZIG && c == '\\' && i + 1 < n && t[i + 1] == '\\') { size_t e = i; while (e < n && t[e] != '\n') e++; cp_blank(cl, i, e); i = e; continue; }
        if (m.bs && c == '\\' && i + 1 < n) { cp_blank(cl, i, i + 2); cp_blank(nc, i, i + 2); i += 2; last = 'a'; continue; }
        if (L == CL_ERL && c == '$' && i + 1 < n) { cp_blank(cl, i, i + 2); cp_blank(nc, i, i + 2); i += 2; last = 'a'; continue; }
        if (L == CL_TEX && c == '\\' && i + 1 < n) { i += 2; continue; }
        if (L == CL_SQL && c == '$') {                 /* $$ ... $$ e $tag$ ... $tag$ */
            size_t k = i + 1; while (k < n && (isalnum((unsigned char)t[k]) || t[k] == '_')) k++;
            if (k < n && t[k] == '$' && !(k > i + 1 && isdigit((unsigned char)t[i + 1]))) {
                char tag[40]; size_t tl = k - i + 1; if (tl < 38) { memcpy(tag, t + i, tl); tag[tl] = 0; size_t e = k + 1; while (e < n && !c2_at(t, n, e, tag)) e++; size_t end = e < n ? e + tl : n; cp_blank(cl, i + tl, e < n ? e : n); i = end; last = 'a'; continue; }
            }
        }
        if (L == CL_SQL && (c == '`' || c == '[' || c == ']')) { cl[i] = ' '; i++; continue; }
        if (L == CL_SQL && c == '"') { cl[i] = ' '; i++; continue; }
        if (m.sw && c == '#') {                        /* Swift: #"..."# */
            size_t k = i; int h = 0; while (k < n && t[k] == '#') { h++; k++; }
            if (k < n && t[k] == '"') {
                size_t e = k + 1;
                for (; e < n; e++) if (t[e] == '"') { int ok = 1; for (int z = 1; z <= h; z++) if (e + z >= n || t[e + z] != '#') { ok = 0; break; } if (ok) break; }
                size_t end = e < n ? e + 1 + h : n; cp_blank(cl, k + 1, e < n ? e : n); i = end; last = 'a'; continue;
            }
        }
        if (m.hd && c == '<' && i + 2 < n && t[i + 1] == '<' && hn < 4) {
            size_t j = i + 2; int dash = 0;
            if (L == CL_PHP || L == CL_RB || L == CL_PERL || L == CL_TF) {
                if (L == CL_PHP) { if (j < n && t[j] == '<') j++; else j = 0; }
                else { if (j < n && (t[j] == '-' || t[j] == '~')) { dash = 1; j++; } else if (m.hd == 2) { /* Ruby: sem - ou ~ so se aspas ou maiuscula e nao apos identificador */ if (!(j < n && (t[j] == '"' || t[j] == '\'' || isupper((unsigned char)t[j])))) j = 0; else if (i > 0 && c2_isword((unsigned char)t[i - 1])) j = 0; } }
                if (j && j < n) {
                    size_t q = j; char qc = 0; if (t[q] == '\'' || t[q] == '"') { qc = t[q]; q++; }
                    size_t s = q; while (q < n && (isalnum((unsigned char)t[q]) || t[q] == '_')) q++;
                    if (q > s && q - s < 63 && !isdigit((unsigned char)t[s]) && (!qc || (q < n && t[q] == qc))) { memcpy(hd[hn], t + s, q - s); hd[hn][q - s] = 0; hdash[hn] = dash; hn++; i = qc ? q + 1 : q; continue; }
                }
            }
        }
        if (c == '"' || c == '\'' || (c == '`' && m.bt)) {
            char q = c;
            if (q == '\'' && !m.sq) { i++; last = c; continue; }
            if (q == '"' && !m.dq) { i++; last = c; continue; }
            if (m.tri && i + 2 < n && t[i + 1] == q && t[i + 2] == q && (q == '"' || m.sq == 1)) {
                size_t e = cp_tri_end(t, n, i + 3, q, q == '"' ? 1 : 0);
                cp_blank(cl, i + 3, e < n ? e : n); i = e + 3 < n ? e + 3 : n; last = 'a'; continue;
            }
            if (q == '\'' && m.sq == 2) {
                size_t l = (i > 0 && (isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_' || t[i - 1] == '\'')) ? 0 : cp_charlit(t, n, i);
                if (l) { cp_blank(cl, i + 1, i + l - 1); i += l; last = 'a'; } else i++;
                continue;
            }
            int esc = !(L == CL_SQL) && !(L == CL_FORTRAN) && !(L == CL_PHP && q == '\'' && 0);
            int ml = L == CL_RB || L == CL_EX || L == CL_PHP || L == CL_PERL || L == CL_CLJ || L == CL_LISP || L == CL_SQL || L == CL_R || L == CL_ML || L == CL_ERL || L == CL_DART || L == CL_GROOVY;
            size_t e = cp_str_end(t, n, i + 1, q, esc, ml, 0); if (e > n) e = n;
            /* "..." sem fechamento ate o fim da linha nao devora o resto: cp_str_end ja para na quebra quando !ml */
            cp_blank(cl, i + 1, e); i = e < n && t[e] == q ? e + 1 : e; last = 'a'; continue;
        }
        if (c == '[' && L == CL_LUA) {                 /* string longa [[ ]] / [==[ ]==] */
            size_t q = i + 1; int lv = 0; while (q < n && t[q] == '=') { lv++; q++; }
            if (q < n && t[q] == '[') {
                size_t e = q + 1; for (; e < n; e++) if (t[e] == ']') { size_t z = e + 1; int ok = 1; for (int w = 0; w < lv; w++) { if (z >= n || t[z] != '=') { ok = 0; break; } z++; } if (ok && z < n && t[z] == ']') break; }
                size_t end = e < n ? e + lv + 2 : n; if (end > n) end = n; cp_blank(cl, i, end); i = end; last = 'a'; continue;
            }
        }
        if (isalnum((unsigned char)c) || c == '_') {
            if (i > 0 && (isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_')) { if (lwl < 15) lw[lwl++] = c; } else { lwl = 0; lw[lwl++] = c; }
            lw[lwl] = 0;
        }
        (void)last; last = c; i++;
    }
}

/* ---------- familia A: reconhecedor por tokens parametrizado (C#, Swift, PHP, Dart, Scala, Groovy) ---------- */
typedef struct { const char *ty[10], *fn[6], *im[8], *ns[3], *vr[6], *ta[3]; int cs, nl; } C2G;
static const C2G *c2_gspec(int L) {
    static const C2G cs = {{"class", "struct", "interface", "enum", "record", 0}, {0}, {"using", 0}, {"namespace", 0}, {0}, {"delegate", 0}, 1, 0};
    static const C2G sw = {{"class", "struct", "enum", "protocol", "extension", "actor", 0}, {"func", "init", "deinit", "subscript", 0}, {"import", 0}, {0}, {"var", "let", 0}, {"typealias", 0}, 0, 1};
    static const C2G ph = {{"class", "interface", "trait", "enum", 0}, {"function", 0}, {"use", "require", "require_once", "include", "include_once", 0}, {"namespace", 0}, {"const", 0}, {0}, 0, 0};
    static const C2G da = {{"class", "mixin", "enum", "extension", 0}, {0}, {"import", "export", "part", 0}, {"library", 0}, {0}, {"typedef", 0}, 1, 0};
    static const C2G sc = {{"class", "object", "trait", "enum", 0}, {"def", 0}, {"import", 0}, {"package", 0}, {"val", "var", 0}, {"type", 0}, 0, 1};
    static const C2G gr = {{"class", "interface", "enum", "trait", 0}, {"def", 0}, {"import", 0}, {"package", 0}, {0}, {0}, 1, 1};
    switch (L) { case CL_CS: return &cs; case CL_SWIFT: return &sw; case CL_PHP: return &ph; case CL_DART: return &da; case CL_SCALA: return &sc; default: return &gr; }
}
static const char *const C2_MODS[] = {"public", "private", "protected", "internal", "static", "abstract", "sealed", "partial", "override", "virtual", "async", "readonly", "final", "open", "implicit",
    "lazy", "inline", "data", "extern", "unsafe", "required", "fileprivate", "mutating", "nonmutating", "convenience", "indirect", "dynamic", "late", "external", "factory", "covariant", "synchronized",
    "transient", "volatile", "native", "infix", "tailrec", "suspend", "operator", "new", "global", "export", "optional", "weak", "unowned", "nonisolated", "distributed", "default", "tracked", "var", NULL};
static int c2_inw(const char *const *l, const CP *c, int j) { return cp_idt(c, j) && cp_inl(c->c + c->tk[j].a, cp_tl(c, j), l); }
static int c2_tykind(const CP *c, int kw) {
    if (cp_is(c, kw, "struct")) return CK_STRUCT;
    if (cp_is(c, kw, "interface") || cp_is(c, kw, "protocol") || cp_is(c, kw, "trait")) return CK_IFACE;
    if (cp_is(c, kw, "enum")) return CK_ENUM;
    return CK_CLASS;
}
/* nome com pontos: devolve o indice depois; *last = ultimo identificador */
static int c2_dotted(CP *c, int j, int b, int *last) {
    int k = j + 1; *last = j;
    while (k + 1 < b && (cp_p(c, k, '.') || cp_p(c, k, '\\') || cp_is(c, k, "::")) && cp_idt(c, k + 1)) { *last = k + 1; k += 2; }
    return k;
}
/* a quebra de linha antes de k continua a expressao anterior? (operador, ponto, fecho, palavra de continuacao) */
static int c2_nlcont(CP *c, int k) {
    if (!(c->tk[k].fl & CF_NL)) return 1;
    if (cp_dirt(c, k)) return 1;
    const CpTok *x = &c->tk[k];
    if (x->t == CT_P) return x->b - x->a == 1 ? strchr(".?:+-*/%&|^=<>)]},", c->c[x->a]) != NULL : 1;
    if (x->t == CT_ID) return cp_inl(c->c + x->a, x->b - x->a, CP_NOBEGIN) || cp_is(c, k, "with") || cp_is(c, k, "extends") || cp_is(c, k, "throws");
    return 0;
}
/* fim de uma instrucao que comeca em j: devolve indice exclusivo; *eb = byte de fim. nl = linguagem sem ';' obrigatorio */
static int c2_stmt_end(CP *c, int j, int b, int nl, size_t *eb) {
    int k = j;
    while (k < b) {
        if (cp_p(c, k, ';')) { *eb = c->tk[k].b; return k + 1; }
        if (cp_open(c, k)) k = cp_skipgrp(c, k, b); else k++;
        if (nl && k < b && !c2_nlcont(c, k) && cp_endsexpr(c, k - 1)) break;
    }
    *eb = k > j ? c->tk[(k < b ? k : b) - 1].b : c->tk[j].b; return k;
}
/* fim do cabecalho: how 1 = '{' (corpo), 2 = ';', 3 = quebra de linha (sem corpo), 5 = '=' ou '=>' (expressao), 4 = fim do bloco */
static int c2_hdr(CP *c, int j, int b, int nl, int *how) {
    int k = j;
    if (nl && j < b && j > 0 && (c->tk[j].fl & CF_NL) && !c2_nlcont(c, j) && !cp_p(c, j, '{') && cp_endsexpr(c, j - 1)) { *how = 3; return j; }
    while (k < b) {
        if (cp_p(c, k, '{')) { *how = 1; return k; }
        if (cp_p(c, k, ';')) { *how = 2; return k; }
        if (cp_p(c, k, '=') || cp_is(c, k, "=>")) { *how = 5; return k; }
        if (cp_p(c, k, '(') || cp_p(c, k, '[')) { k = cp_skipgrp(c, k, b); if (nl && k < b && (c->tk[k].fl & CF_NL) && !c2_nlcont(c, k) && !cp_p(c, k, '{')) { *how = 3; return k; } continue; }
        if (cp_p(c, k, '<') && k > j) { k = cp_angle(c, k, b); continue; }
        k++;
        if (nl && k < b && !c2_nlcont(c, k) && !cp_p(c, k, '{') && cp_endsexpr(c, k - 1)) { *how = 3; return k; }
    }
    *how = 4; return k;
}
/* fim de um simbolo com corpo/expressao dado `how`; devolve (indice seguinte) e *eb */
static int c2_after(CP *c, int k, int how, int b, int nl, size_t *eb, int *bodytok) {
    *bodytok = -1;
    if (how == 1) { int cl_ = cp_mt(c, k); *eb = cp_end(c, cl_); *bodytok = k; return cl_ + 1 > b ? b : cl_ + 1; }
    if (how == 2) { *eb = c->tk[k].b; return k + 1; }
    if (how == 3) { *eb = c->tk[k - 1].b; return k; }
    if (how == 5) {
        int v = k + 1;
        if (v < b && cp_p(c, v, '{') && nl) { int cl_ = cp_mt(c, v); *eb = cp_end(c, cl_); *bodytok = v; return cl_ + 1 > b ? b : cl_ + 1; }
        return c2_stmt_end(c, v, b, nl, eb);
    }
    *eb = k > 0 ? c->tk[(k < c->nt ? k : c->nt) - 1].b : 0; return k;
}
static void c2_heritage(CP *c, int si, int kind, int lo, int hi) {
    int mode = 0, first = 0;
    for (int m = lo; m < hi;) {
        if (cp_p(c, m, '<')) { m = cp_angle(c, m, hi); continue; }
        if (cp_open(c, m)) { m = cp_skipgrp(c, m, hi); continue; }
        if (cp_is(c, m, "where")) break;
        if (cp_p(c, m, ':')) { mode = 3; first = 1; m++; continue; }
        if (cp_is(c, m, "extends")) { mode = 1; first = 0; m++; continue; }
        if (cp_is(c, m, "implements") || cp_is(c, m, "with") || cp_is(c, m, "on") || cp_is(c, m, "conforms")) { mode = 2; m++; continue; }
        if (mode && cp_idt(c, m)) {
            int last; int e = c2_dotted(c, m, hi, &last);
            int rk = mode == 1 ? CR_EXTENDS : mode == 2 ? CR_IMPLEMENTS : (first && kind == CK_CLASS ? CR_EXTENDS : CR_IMPLEMENTS);
            cp_reft(c, si, rk, last); first = 0; m = e; continue;
        }
        m++;
    }
}
/* nome de simbolo a partir de tokens ja conhecidos */
static int c2_symrange(CP *c, int kind, int parent, size_t b0, size_t b1, int nm, size_t sa, size_t sb) { return cp_sym(c, kind, parent, b0, b1, nm, sa, sb); }
static int c2_enumcases(CP *c, int i, int b, int parent) {   /* case a, b(1), c = 2 */
    int j = i + 1;
    for (;;) {
        if (!cp_idt(c, j)) break;
        int nm = j; j++;
        if (cp_p(c, j, '(') && c->tk[j].a == c->tk[nm].b) j = cp_skipgrp(c, j, b);
        size_t e = c->tk[j - 1].b;
        if (cp_p(c, j, '=')) { size_t eb; int x = c2_stmt_end(c, j + 1, b, 1, &eb); (void)x; int q = j + 1; while (q < b && !cp_p(c, q, ',') && !cp_p(c, q, ';') && !(c->tk[q].fl & CF_NL)) { if (cp_open(c, q)) q = cp_skipgrp(c, q, b); else q++; } e = c->tk[(q > j ? q : j + 1) - 1].b; j = q; }
        cp_sym(c, CK_ENUMERATOR, parent, c->tk[nm].a, e, nm, c->tk[nm].a, e);
        if (cp_p(c, j, ',')) { j++; continue; }
        break;
    }
    if (cp_p(c, j, ';')) j++;
    return j;
}
/* corpo de enum de C#/Dart: A, B = 2, C(1); depois do ';' (Dart) sao membros comuns */
static void c2_enumbody(CP *c, int lo, int hi, int parent, int dp) {
    int j = lo;
    while (j < hi) {
        if (cp_p(c, j, ';')) { j++; break; }
        if (cp_p(c, j, ',')) { j++; continue; }
        if (cp_p(c, j, '[') || (cp_p(c, j, '@') && cp_idt(c, j + 1))) { if (cp_p(c, j, '[')) j = cp_skipgrp(c, j, hi); else j += 2; continue; }
        if (!cp_idt(c, j)) { j++; continue; }
        int nm = j; j++;
        if (cp_p(c, j, '<')) j = cp_angle(c, j, hi);
        if (cp_p(c, j, '(')) j = cp_skipgrp(c, j, hi);
        if (cp_p(c, j, '=')) { j++; while (j < hi && !cp_p(c, j, ',') && !cp_p(c, j, ';')) { if (cp_open(c, j)) j = cp_skipgrp(c, j, hi); else j++; } }
        cp_sym(c, CK_ENUMERATOR, parent, c->tk[nm].a, c->tk[j > nm ? j - 1 : nm].b, nm, c->tk[nm].a, c->tk[j > nm ? j - 1 : nm].b);
    }
    if (j < hi) cp_scan(c, j, hi, parent, CM_CLASS, dp + 1);
}
static const char *const C2_NOFN[] = {"if", "for", "while", "switch", "catch", "using", "lock", "foreach", "return", "new", "throw", "await", "typeof", "sizeof", "nameof", "default", "when", "fixed", "checked", "unchecked", "do", "else", "try", "assert", "super", "this", "base", "yield", NULL};
static int c2_gdecl(CP *c, int i, int b, int parent, int mode, int dp) {
    const C2G *g = c2_gspec(c->lang); const int L = c->lang, nl = g->nl;
    CoderFile *f = c->f; const int pk = parent >= 0 ? f->sym[parent].kind : -1;
    const int inclass = pk == CK_CLASS || pk == CK_STRUCT || pk == CK_IFACE || pk == CK_ENUM;
    size_t b0 = c->tk[i].a;
    if (dp > 60) return 0;
    /* anotacoes e modificadores */
    int j = i;
    for (int guard = 0; j < b && guard < 30; guard++) {
        if (cp_p(c, j, '[') && L == CL_CS) { j = cp_skipgrp(c, j, b); continue; }
        if (cp_p(c, j, '@') && cp_idt(c, j + 1)) { int last; j = c2_dotted(c, j + 1, b, &last); if (cp_p(c, j, '(') && c->tk[j].a == c->tk[j - 1].b) j = cp_skipgrp(c, j, b); continue; }
        if (cp_p(c, j, '#') && cp_p(c, j + 1, '[')) { j = cp_skipgrp(c, j + 1, b); continue; }
        if (cp_is(c, j, "case") && (cp_is(c, j + 1, "class") || cp_is(c, j + 1, "object"))) { j++; continue; }
        if (cp_is(c, j, "class") && (L == CL_SWIFT) && (cp_is(c, j + 1, "func") || cp_is(c, j + 1, "var") || cp_is(c, j + 1, "let") || cp_is(c, j + 1, "final") || cp_is(c, j + 1, "override"))) { j++; continue; }
        if (c2_inw(C2_MODS, c, j) && !(L == CL_PHP && cp_is(c, j, "var") ) && !(cp_is(c, j, "static") && cp_p(c, j + 1, '(')) && !(L == CL_SWIFT && cp_is(c, j, "var")) && !(L == CL_SCALA && cp_is(c, j, "var")) && !(c->lang == CL_PHP && cp_is(c, j, "new"))) {
            j++;
            if (L == CL_SCALA && cp_p(c, j, '[') && c->tk[j].a == c->tk[j - 1].b) j = cp_skipgrp(c, j, b);
            if (L == CL_SWIFT && cp_p(c, j, '(') && c->tk[j].a == c->tk[j - 1].b) j = cp_skipgrp(c, j, b);
            continue;
        }
        break;
    }
    if (j >= b) return j > i ? j : 0;
    int kw = j, dtor = 0;
    if (cp_p(c, kw, '~') && cp_idt(c, kw + 1)) { kw++; dtor = 1; }
    if (!cp_idt(c, kw)) return 0;
    /* ---- imports ---- */
    if (c2_inw(g->im, c, kw)) {
        if (L == CL_PHP && (inclass) && cp_is(c, kw, "use")) { size_t eb; int e = c2_stmt_end(c, kw + 1, b, 0, &eb); if (cp_idt(c, kw + 1)) { int last; c2_dotted(c, kw + 1, b, &last); cp_reft(c, parent, CR_IMPLEMENTS, last); } return e; }
        size_t eb; int e = c2_stmt_end(c, kw + 1, b, nl, &eb);
        int sp = -1; for (int m = kw + 1; m + 1 < e; m++) if (cp_strpair(c, m)) { sp = m; break; }
        if (sp >= 0 && (L == CL_PHP || L == CL_DART)) cp_import_str(c, -1, sp);
        else if (L == CL_PHP && sp < 0) { /* use A\B\C [as D]; */ int a = kw + 1; while (cp_is(c, a, "function") || cp_is(c, a, "const")) a++; if (a < e) { int last = a; while (last + 1 < e && !cp_is(c, last + 1, "as") && !cp_p(c, last + 1, ';')) last++; size_t ee = c->tk[last].b; cp_ref_range(c, -1, CR_IMPORT, c->tk[a].a, ee, 1); } }
        else if (e > kw + 1) {
            int a = kw + 1; while (a < e && (cp_is(c, a, "static") || cp_is(c, a, "struct") || cp_is(c, a, "class") || cp_is(c, a, "enum") || cp_is(c, a, "func") || cp_is(c, a, "var") || cp_is(c, a, "let") || cp_is(c, a, "protocol") || cp_is(c, a, "typealias"))) a++;
            for (int m = a; m + 1 < e; m++) if (cp_p(c, m, '=') && L == CL_CS) { a = m + 1; break; }
            int last = e - 1; if (cp_p(c, last, ';')) last--;
            if (last >= a) cp_ref_range(c, -1, CR_IMPORT, c->tk[a].a, c->tk[last].b, 1);
        }
        return e;
    }
    /* ---- namespace / package / library ---- */
    if (c2_inw(g->ns, c, kw)) {
        int nm = kw + 1; if (L == CL_SCALA && cp_is(c, nm, "object")) { return 0; }
        if (!cp_idt(c, nm)) return 0;
        int last; int e = c2_dotted(c, nm, b, &last);
        size_t nb = c->tk[last].b; int plain = cp_is(c, kw, "library");
        if (cp_p(c, e, '{')) {
            int cl_ = cp_mt(c, e); int si = cp_symx(c, CK_NAMESPACE, parent, b0, cp_end(c, cl_), -1, c->t + c->tk[nm].a, nb - c->tk[nm].a, b0, c->tk[e].a, 0, 0);
            cp_scan(c, e + 1, cl_ < b ? cl_ : b, si, CM_TOP, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
        }
        if (cp_p(c, e, ';')) e++;
        int si = cp_symx(c, CK_NAMESPACE, parent, b0, plain ? c->tk[e - 1].b : cp_end(c, b - 1), -1, c->t + c->tk[nm].a, nb - c->tk[nm].a, b0, nb, 0, 0);
        if (plain) return e;
        if (e < b) cp_scan(c, e, b, si, CM_TOP, dp + 1);
        return b;
    }
    /* ---- tipos ---- */
    if (c2_inw(g->ty, c, kw) && !(L == CL_CS && cp_is(c, kw, "enum") && 0)) {
        int kind = c2_tykind(c, kw), nm = kw + 1;
        if (cp_is(c, nm, "struct") || cp_is(c, nm, "class")) nm++;                       /* record struct / record class */
        if (cp_is(c, kw, "extension") && (L == CL_DART) && (cp_is(c, nm, "on") || !cp_idt(c, nm))) {
            int e = nm + 1; int last = e; if (cp_idt(c, e)) c2_dotted(c, e, b, &last);
            char nmb[96]; size_t l = c->tk[last].b - c->tk[nm].a; if (l > 90) l = 90; memcpy(nmb, c->t + c->tk[nm].a, l); nmb[l] = 0;
            int how; int k = c2_hdr(c, nm, b, 0, &how); size_t eb; int bt; int nx = c2_after(c, k, how, b, 0, &eb, &bt);
            int si = cp_symx(c, CK_CLASS, parent, b0, eb, -1, nmb, l, b0, k < b ? c->tk[k].a : eb, 0, 0);
            if (bt >= 0) cp_scan(c, bt + 1, cp_mt(c, bt) < b ? cp_mt(c, bt) : b, si, CM_CLASS, dp + 1);
            return nx;
        }
        if (!cp_idt(c, nm)) return 0;
        int last; int e = c2_dotted(c, nm, b, &last); (void)e;
        int how; int k = c2_hdr(c, last + 1, b, nl, &how); size_t eb; int bt;
        int nx = c2_after(c, k, how == 5 && L == CL_DART ? 2 : how, b, nl, &eb, &bt);
        if (how == 5 && L != CL_DART) { eb = c->tk[k > 0 ? k - 1 : 0].b; nx = k; bt = -1; }
        char nmb[128]; size_t l = c->tk[last].b - c->tk[nm].a; if (l > 120) l = 120; memcpy(nmb, c->t + c->tk[nm].a, l); nmb[l] = 0;
        int si = cp_symx(c, kind, parent, b0, eb, -1, cp_is(c, kw, "extension") ? nmb : c->t + c->tk[last].a, cp_is(c, kw, "extension") ? l : cp_tl(c, last), b0, k < b ? c->tk[k].a : eb, 0, 0);
        c2_heritage(c, si, cp_is(c, kw, "extension") ? CK_STRUCT : kind, last + 1, k);
        if (bt >= 0) { int cl_ = cp_mt(c, bt); if (kind == CK_ENUM && (L == CL_CS || L == CL_DART)) c2_enumbody(c, bt + 1, cl_ < b ? cl_ : b, si, dp); else cp_scan(c, bt + 1, cl_ < b ? cl_ : b, si, CM_CLASS, dp + 1); }
        return nx > i ? nx : i + 1;
    }
    /* ---- case (enum Swift/PHP/Scala) ---- */
    if (cp_is(c, kw, "case") && pk == CK_ENUM && L != CL_CS && L != CL_DART) return c2_enumcases(c, kw, b, parent);
    /* ---- typedef / typealias / delegate ---- */
    if (c2_inw(g->ta, c, kw)) {
        int k = kw + 1, nm = -1;
        while (k < b && !cp_p(c, k, '=') && !cp_p(c, k, '(') && !cp_p(c, k, ';') && !(nl && k > kw + 1 && (c->tk[k].fl & CF_NL))) { if (cp_p(c, k, '<')) { k = cp_angle(c, k, b); continue; } if (cp_idt(c, k)) nm = k; k++; }
        if (cp_is(c, kw, "delegate")) { while (k < b && !cp_p(c, k, ';')) k++; }
        if (nm < 0) return 0;
        size_t eb; int nx = c2_stmt_end(c, kw + 1, b, nl, &eb);
        cp_sym(c, CK_TYPEDEF, parent, b0, eb, nm, b0, eb); return nx;
    }
    /* ---- funcoes com palavra-chave ---- */
    if (c2_inw(g->fn, c, kw)) {
        int nm = kw + 1, kind;
        if (cp_is(c, kw, "init") || cp_is(c, kw, "deinit") || cp_is(c, kw, "subscript")) { nm = kw; kind = CK_CTOR; if (cp_is(c, kw, "subscript")) kind = CK_METHOD; if (!(cp_p(c, kw + 1, '(') || cp_p(c, kw + 1, '?') || cp_p(c, kw + 1, '<') || cp_p(c, kw + 1, '{') || cp_p(c, kw + 1, '!'))) return 0; }
        else {
            if (cp_p(c, nm, '&')) nm++;
            if (!cp_idt(c, nm)) { if (L == CL_SWIFT && nm < b && c->tk[nm].t == CT_P && !cp_open(c, nm)) { /* operador */ } else return 0; }
            kind = inclass ? CK_METHOD : CK_FUNC;
            if (L == CL_PHP && cp_is(c, nm, "__construct")) kind = CK_CTOR;
            if (L == CL_SCALA && cp_is(c, nm, "this")) kind = CK_CTOR;
        }
        int how; int k = c2_hdr(c, nm + 1, b, nl, &how); size_t eb; int bt;
        int nx = c2_after(c, k, how, b, nl, &eb, &bt);
        int si = cp_sym(c, kind, parent, b0, eb, nm, b0, k < b ? c->tk[k].a : eb);
        if (how == 2 || how == 3) c->sfl[si] |= 1;
        if (cp_idt(c, kw) && cp_is(c, kw, "init") && cp_p(c, kw + 1, '?')) { }
        return nx > i ? nx : i + 1;
    }
    /* ---- variaveis com palavra-chave (let/var/val/const) ---- */
    if (c2_inw(g->vr, c, kw)) {
        int nm = kw + 1;
        if (L == CL_PHP) { if (!cp_idt(c, nm)) return 0; }
        else if (!cp_idt(c, nm)) return 0;
        int isconst = cp_is(c, kw, "let") || cp_is(c, kw, "val") || cp_is(c, kw, "const");
        int kind = isconst ? CK_CONST : (inclass ? CK_FIELD : CK_VAR);
        int how; int k = c2_hdr(c, nm + 1, b, nl || L == CL_PHP, &how); size_t eb; int bt;
        int nx = c2_after(c, k, how, b, nl, &eb, &bt);
        if (L == CL_PHP && how == 5) { size_t e2; nx = c2_stmt_end(c, k + 1, b, 0, &e2); eb = e2; }
        cp_sym(c, kind, parent, b0, eb, nm, b0, k < b ? c->tk[k].a : eb);
        return nx > i ? nx : i + 1;
    }
    /* ---- PHP: propriedade $x ---- */
    if (L == CL_PHP && inclass) {
        int pv = -1;
        for (int m = kw; m < b && m < kw + 12; m++) { if (cp_p(c, m, ';') || cp_p(c, m, '=') || cp_p(c, m, '(') || cp_p(c, m, '{')) break; if (c->c[c->tk[m].a] == '$' && cp_tl(c, m) > 1) { pv = m; break; } }
        if (pv >= 0) { size_t eb; int nx = c2_stmt_end(c, pv + 1, b, 0, &eb); cp_symx(c, CK_FIELD, parent, b0, eb, -1, c->t + c->tk[pv].a + 1, cp_tl(c, pv) - 1, b0, eb, 0, 0); return nx; }
    }
    /* ---- Groovy/Gradle: blocos de script e dependencias ---- */
    if (L == CL_GROOVY && !inclass && cp_idt(c, kw)) {
        if (pk == CK_SECTION && (cp_strpair(c, kw + 1) || (cp_p(c, kw + 1, '(') && cp_strpair(c, kw + 2)) || (cp_is(c, kw, "apply") && cp_is(c, kw + 1, "plugin")))) {
            int sp = cp_strpair(c, kw + 1) ? kw + 1 : cp_strpair(c, kw + 2) ? kw + 2 : -1;
            if (sp < 0) { for (int m = kw + 1; m < kw + 5 && m < b; m++) if (cp_strpair(c, m)) { sp = m; break; } }
            if (sp >= 0) cp_import_str(c, parent, sp);
            size_t eb; return c2_stmt_end(c, kw + 1, b, 1, &eb);
        }
        int nm = kw, k = kw + 1;
        if (cp_is(c, kw, "task") && cp_idt(c, kw + 1)) { nm = kw + 1; k = kw + 2; }
        if (cp_p(c, k, '(') && cp_is(c, kw, "task")) k = cp_skipgrp(c, k, b);
        if (cp_p(c, k, '{') && (nm != kw || dp < 6) && !cp_kwtok(c, kw, CP_NOCALL)) {
            int cl_ = cp_mt(c, k); int si = cp_sym(c, CK_SECTION, parent, b0, cp_end(c, cl_), nm, b0, c->tk[k].a);
            cp_scan(c, k + 1, cl_ < b ? cl_ : b, si, CM_TOP, dp + 1); return cl_ + 1 > b ? b : cl_ + 1;
        }
    }
    /* ---- estilo C: Tipo nome(args) { ... }, propriedades e campos ---- */
    if (g->cs && !(L == CL_GROOVY && !inclass && !cp_is(c, kw, "def") && 0)) {
        int k = kw, nm = -1, how = 0, open = -1;
        int hasconst = 0;
        for (; k < b; ) {
            if (cp_p(c, k, '{')) { how = 1; break; }
            if (cp_p(c, k, ';')) { how = 2; break; }
            if (cp_p(c, k, '=')) { how = 6; break; }
            if (cp_is(c, k, "=>")) { how = 5; break; }
            if (cp_p(c, k, '(')) { if (k > kw && (cp_idt(c, k - 1) || cp_p(c, k - 1, '>'))) { open = k; } if (open >= 0) { how = 7; break; } k = cp_skipgrp(c, k, b); continue; }
            if (cp_p(c, k, '[') ) { k = cp_skipgrp(c, k, b); continue; }
            if (cp_p(c, k, '<')) { int e2 = cp_angle(c, k, b); if (e2 > k + 1 && cp_p(c, e2, '(') && k > kw && cp_idt(c, k - 1)) { nm = k - 1; open = e2; how = 7; break; } k = e2; continue; }
            if (cp_is(c, k, "const")) hasconst = 1;
            if (nl && k > kw && (c->tk[k].fl & CF_NL) && !c2_nlcont(c, k)) { how = 8; break; }
            if (cp_p(c, k, '}') ) break;
            k++;
        }
        if (how == 7) {
            if (nm < 0) { nm = open - 1; if (cp_p(c, nm, '>')) return 0; }
            if (!cp_idt(c, nm) || cp_kwtok(c, nm, C2_NOFN)) return 0;
            if (nm == kw) { int okc = 0; if (inclass && parent >= 0) { const char *pn0 = f->sym[parent].name; okc = cp_tl(c, nm) == strlen(pn0) && !memcmp(pn0, c->t + c->tk[nm].a, cp_tl(c, nm)); } if (!okc && !(L == CL_DART && !inclass)) return 0; }
            int mc = open >= 0 ? cp_mt(c, open) : k;
            int after = mc + 1; int how2 = 0, k2 = after;
            int ininit = 0;
            while (k2 < b && !cp_p(c, k2, '{') && !cp_p(c, k2, ';') && !cp_is(c, k2, "=>") && (!cp_p(c, k2, '=') || ininit) && !(nl && (c->tk[k2].fl & CF_NL) && k2 > after && !c2_nlcont(c, k2))) { if (cp_p(c, k2, ':') && (L == CL_CS || L == CL_DART)) ininit = 1; if (cp_open(c, k2)) k2 = cp_skipgrp(c, k2, b); else k2++; }
            if (k2 >= b) how2 = 4; else if (cp_p(c, k2, '{')) how2 = 1; else if (cp_p(c, k2, ';')) how2 = 2; else if (cp_is(c, k2, "=>")) how2 = 5; else if (cp_p(c, k2, '=')) return 0; else how2 = 3;
            if (L == CL_GROOVY && how2 != 1) return 0;
            if (how2 == 4) return 0;
            const char *pn = parent >= 0 ? f->sym[parent].name : ""; size_t pnl = strlen(pn);
            int isctor = inclass && cp_tl(c, nm) == pnl && !memcmp(pn, c->t + c->tk[nm].a, pnl);
            if (!isctor && inclass && nm >= 2 && cp_p(c, nm - 1, '.') && cp_idt(c, nm - 2) && cp_tl(c, nm - 2) == pnl && !memcmp(pn, c->t + c->tk[nm - 2].a, pnl)) isctor = 1;
            if (dtor) isctor = 1;
            size_t eb; int bt; int nx = c2_after(c, k2, how2, b, nl, &eb, &bt);
            int si = cp_sym(c, isctor ? CK_CTOR : inclass ? CK_METHOD : CK_FUNC, parent, b0, eb, nm, b0, k2 < b ? c->tk[k2].a : eb);
            if (how2 == 2 || how2 == 3) c->sfl[si] |= 1;
            return nx > i ? nx : i + 1;
        }
        if ((how == 1 || how == 5) && k > kw + 0 && inclass) {            /* propriedade: Tipo Nome { get; set; }  /  Tipo Nome => expr; */
            int pn_ = k - 1;
            if (cp_idt(c, pn_) && pn_ > kw) {
                size_t eb; int bt; int nx = c2_after(c, k, how == 1 ? 1 : 5, b, nl, &eb, &bt);
                if (how == 1 && cp_p(c, nx, '=')) { size_t e2; nx = c2_stmt_end(c, nx + 1, b, 0, &e2); eb = e2; }
                cp_sym(c, CK_PROPERTY, parent, b0, eb, pn_, b0, c->tk[k].a); return nx > i ? nx : i + 1;
            }
        }
        if (how == 6 || how == 2 || how == 8) {
            int pn_ = k - 1; while (pn_ > kw && cp_p(c, pn_, ']')) pn_ = c->mt[pn_] - 1;
            if (how == 2 && !inclass && L == CL_DART && false) return 0;
            if (cp_idt(c, pn_) && pn_ > kw && !cp_kwtok(c, pn_, C2_NOFN) && (inclass || L == CL_DART || L == CL_GROOVY || L == CL_CS)) {
                size_t eb; int nx = c2_stmt_end(c, k, b, nl && how != 2 && how != 6 ? 1 : (L == CL_GROOVY ? 1 : 0), &eb);
                if (how == 8) { nx = k; eb = c->tk[k - 1].b; }
                int ncst = hasconst; for (int m = i; m < kw; m++) if (cp_is(c, m, "const") || cp_is(c, m, "readonly") && 0) ncst = 1;
                cp_sym(c, ncst ? CK_CONST : inclass ? CK_FIELD : CK_VAR, parent, b0, eb, pn_, b0, eb); return nx > i ? nx : i + 1;
            }
        }
    }
    return 0;
}

/* ---------- despacho de linguagem ---------- */
static int c2_istok(int L) { return L == CL_CS || L == CL_SWIFT || L == CL_PHP || L == CL_DART || L == CL_SCALA || L == CL_GROOVY; }
static int c2_isline(int L) { return L > CL_CONFIG && L < CL_N && !c2_istok(L); }

/* ---------- familia B: ajudantes de linha ---------- */
static int c2_ieq(const char *a, const char *b) { while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; } return *a == *b; }
static int c2_ipre(const char *a, const char *pre) { while (*pre) { if (tolower((unsigned char)*a) != tolower((unsigned char)*pre)) return 0; a++; pre++; } return 1; }
static size_t c2_lb(const CP *c, int ln) { return ln >= 1 && ln <= c->nln ? c->ln[ln - 1] : c->n; }
static size_t c2_le(const CP *c, int ln) {   /* fim da linha (sem \n nem \r) */
    size_t e = ln < c->nln ? c->ln[ln] - 1 : c->n, b = c2_lb(c, ln);
    if (e > c->n) e = c->n;
    while (e > b && c->c[e - 1] == '\r') e--;
    return e;
}
static int c2_add(CP *c, int kind, int parent, size_t b0, size_t b1, const char *nm, size_t nl, size_t sa, size_t sb) {
    if (c->f->nsym > 60000) return -1;                          /* teto: arquivo gerado gigante */
    return cp_symx(c, kind, parent, b0, b1, -1, nm, nl, sa, sb, 0, 0);
}
static size_t c2_eol(const char *s, size_t n, size_t p) { while (p < n && s[p] != '\n') p++; return p; }
static size_t c2_skipws(const char *s, size_t n, size_t p) { while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++; return p; }
static int c2_wc(int ch) { return isalnum(ch) || ch == '_' || (ch & 0x80); }
static int c2_wordeq(const char *s, size_t n, size_t p, const char *w) { size_t l = strlen(w); return p + l <= n && !memcmp(s + p, w, l) && (p + l == n || !c2_wc((unsigned char)s[p + l])) && (p == 0 || !c2_wc((unsigned char)s[p - 1])); }
/* fim do casamento de chaves a partir de s[p]=='{' (texto mascarado): indice depois do '}' (ou n) */
static size_t c2_bm(const char *s, size_t n, size_t p) {
    int d = 0;
    for (size_t i = p; i < n; i++) { if (s[i] == '{') d++; else if (s[i] == '}') { if (--d <= 0) return i + 1; } }
    return n;
}
/* idem para qualquer abertura/fecho ( [ { ) */
static size_t c2_pm(const char *s, size_t n, size_t p) {
    int d = 0;
    for (size_t i = p; i < n; i++) { char ch = s[i]; if (ch == '(' || ch == '[' || ch == '{') d++; else if (ch == ')' || ch == ']' || ch == '}') { if (--d <= 0) return i + 1; } }
    return n;
}
static int c2_lineof(const CP *c, size_t off) { return cp_line(c, off); }
/* palavra (letras, digitos, _ e os extras) a partir de p */
static size_t c2_word(const char *s, size_t n, size_t p, const char *extra) {
    while (p < n && (c2_wc((unsigned char)s[p]) || (extra && s[p] && strchr(extra, s[p])))) p++;
    return p;
}
/* referencia de texto cru de [a,b) do texto sem comentarios (aparando espacos) */
static void c2_refrange(CP *c, int from, int kind, size_t a, size_t b) { cp_ref_range(c, from, kind, a, b, 0); }
/* corpo/ body0 e visibilidade padrao das linguagens por linha */
static void c2_enrich(CP *c) {
    CoderFile *f = c->f;
    for (int s = 0; s < f->nsym; s++) {
        CoderSym *y = &f->sym[s];
        if (c->sfl[s] & 1) y->flags |= CSF_DECL;
        if (y->conf == CC_EXACT && (y->kind == CK_MACRO)) y->conf = CC_HEUR;
        if (!(y->flags & CSF_DECL) && (y->kind == CK_FUNC || y->kind == CK_METHOD || y->kind == CK_CLASS || y->kind == CK_STRUCT || y->kind == CK_IFACE || y->kind == CK_ENUM || y->kind == CK_NAMESPACE || y->kind == CK_CTOR) && y->line1 > y->line0) {
            size_t k = y->byte0, e = y->byte1 < c->n ? y->byte1 : c->n;
            while (k < e && c->c[k] != '\n' && c->c[k] != '{') k++;
            if (k < e && c->c[k] == '{') y->body0 = k; else if (k + 1 < e) y->body0 = k + 1;
        }
    }
}

/* ---------- Clojure / Common Lisp / Scheme / Racket ---------- */
typedef struct { const char *h; int kind, fl; } C2LH;
enum { LF_PRIV = 1, LF_KIDS = 2, LF_TEST = 4, LF_DEF = 8, LF_NS = 16, LF_SUPER = 32 };
static const C2LH C2_LH[] = {
    {"defn", CK_FUNC, 0}, {"defn-", CK_FUNC, LF_PRIV}, {"def", CK_VAR, 0}, {"defonce", CK_VAR, 0}, {"defmacro", CK_MACRO, 0}, {"defmulti", CK_FUNC, 0}, {"defmethod", CK_METHOD, 0},
    {"defprotocol", CK_IFACE, LF_KIDS}, {"definterface", CK_IFACE, LF_KIDS}, {"defrecord", CK_STRUCT, LF_KIDS}, {"deftype", CK_STRUCT, LF_KIDS}, {"defstruct", CK_STRUCT, LF_SUPER},
    {"deftest", CK_FUNC, LF_TEST}, {"defspec", CK_FUNC, LF_TEST}, {"deftest-", CK_FUNC, LF_TEST}, {"defn-", CK_FUNC, LF_PRIV}, {"defmacro-", CK_MACRO, LF_PRIV}, {"def-", CK_VAR, LF_PRIV},
    {"defun", CK_FUNC, 0}, {"defgeneric", CK_FUNC, 0}, {"defvar", CK_VAR, 0}, {"defparameter", CK_VAR, 0}, {"defconstant", CK_CONST, 0}, {"defclass", CK_CLASS, LF_SUPER},
    {"define-condition", CK_CLASS, LF_SUPER}, {"defpackage", CK_NAMESPACE, 0}, {"defsystem", CK_NAMESPACE, 0}, {"define-modify-macro", CK_MACRO, 0}, {"defsetf", CK_MACRO, 0},
    {"define", CK_FUNC, LF_DEF}, {"define-syntax", CK_MACRO, 0}, {"define-syntax-rule", CK_MACRO, 0}, {"define-record-type", CK_STRUCT, 0}, {"struct", CK_STRUCT, 0}, {"define-struct", CK_STRUCT, 0},
    {"library", CK_NAMESPACE, LF_NS}, {"define-library", CK_NAMESPACE, LF_NS}, {"module", CK_NAMESPACE, LF_NS}, {"ns", CK_NAMESPACE, LF_NS}, {"defproject", CK_NAMESPACE, LF_NS},
    {"define/contract", CK_FUNC, LF_DEF}, {"define-values", 0, 0}, {NULL, 0, 0}};
static const char *const C2_LIMP[] = {"require", "use", "import", "load", "include", "use-package", "ql:quickload", "asdf:load-system", "require-macros", "load-file", "refer-clojure", NULL};
static const char *const C2_LSPECIAL[] = {"if", "let", "let*", "letrec", "fn", "fn*", "do", "cond", "case", "when", "when-not", "loop", "recur", "quote", "lambda", "define", "def", "defn", "and", "or", "not",
    "try", "catch", "finally", "throw", "set!", "begin", "begin0", "unless", "ns", "->", "->>", "as->", "some->", "cond->", "doto", "binding", "for", "doseq", "dotimes", "if-let", "when-let", "if-not", "let-values",
    "progn", "setf", "setq", "defun", "defmacro", "defvar", "declare", "var", "new", "do*", "prog1", "block", "return-from", "tagbody", "go", "the", "function", "unwind-protect", "values", "multiple-value-bind",
    "cond-> ", "condp", "case-lambda", "delay", "future", "lazy-seq", "with-open", "letfn", "monitor-enter", "defrecord", "deftype", "defprotocol", "extend-type", "extend-protocol", "reify", "proxy", "import", "require",
    "use", "refer", "in-ns", "comment", "assert", "defmulti", "defmethod", "defonce", "defstruct", "defclass", "defgeneric", "defparameter", "defconstant", "define-syntax", "syntax-rules", "else", "quasiquote", "unquote", NULL};
static size_t c2_lskipws(const char *s, size_t n, size_t p) { while (p < n && (isspace((unsigned char)s[p]) || s[p] == ',')) p++; return p; }
static int c2_lsep(int ch) { return ch == 0 || isspace(ch) || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '"' || ch == ',' || ch == ';'; }
static size_t c2_ltokend(const char *s, size_t n, size_t p) { while (p < n && !c2_lsep((unsigned char)s[p])) p++; return p; }
/* pula uma forma ou token a partir de p (depois de espacos) */
static size_t c2_lskipform(const char *s, size_t n, size_t p) {
    p = c2_lskipws(s, n, p); if (p >= n) return n;
    while (p < n && (s[p] == '\'' || s[p] == '`' || s[p] == '~' || s[p] == '@' || s[p] == '^' || (s[p] == '#' && p + 1 < n && strchr("'^_(\\{[", s[p + 1])))) {
        if (s[p] == '#' && s[p + 1] == '_') { p = c2_lskipform(s, n, p + 2); p = c2_lskipws(s, n, p); continue; }
        if (s[p] == '#' && (s[p + 1] == '(' || s[p + 1] == '{' || s[p + 1] == '[')) { p++; break; }
        p++; if (s[p - 1] == '^') { p = c2_lskipform(s, n, p); p = c2_lskipws(s, n, p); }
    }
    if (p >= n) return n;
    if (s[p] == '(' || s[p] == '[' || s[p] == '{') return c2_pm(s, n, p);
    if (s[p] == '"') { size_t q = p + 1; while (q < n && s[q] != '"') q++; return q < n ? q + 1 : n; }
    size_t q = c2_ltokend(s, n, p); return q > p ? q : p + 1;
}
/* nome do simbolo a partir de p (pula metadados): *a,*b; devolve indice depois */
static size_t c2_lname(const char *s, size_t n, size_t p, size_t *a, size_t *b) {
    p = c2_lskipws(s, n, p);
    while (p < n && (s[p] == '^' || (s[p] == '#' && p + 1 < n && s[p + 1] == '^'))) { if (s[p] == '#') p++; p = c2_lskipform(s, n, p + 1); p = c2_lskipws(s, n, p); }
    if (p < n && s[p] == '(') { size_t q = c2_lskipws(s, n, p + 1); *a = q; *b = c2_ltokend(s, n, q); return c2_pm(s, n, p); }   /* (name (:include x)) / (define (f x)) */
    *a = p; *b = c2_ltokend(s, n, p); return *b;
}
static int c2_inl_(const char *w, const char *const *l);
static void c2_limports(CP *c, size_t a, size_t e, int from) {
    const char *s = c->c; size_t p = a;
    while (p < e) {
        p = c2_lskipws(s, c->n, p); if (p >= e) break;
        size_t q = p; while (q < e && (s[q] == '\'' || s[q] == '`' || s[q] == '#')) q++;
        if (s[q] == '[' || s[q] == '(') {
            size_t fe = c2_pm(s, c->n, q), r = c2_lskipws(s, c->n, q + 1);
            if (s[q] == '(') { size_t te0 = c2_ltokend(s, c->n, r), tl = te0 - r;
                if ((tl == 7 && !strncmp(s + r, "only-in", 7)) || (tl == 9 && (!strncmp(s + r, "except-in", 9) || !strncmp(s + r, "rename-in", 9))) || (tl == 10 && !strncmp(s + r, "for-syntax", 10)) || (tl == 4 && !strncmp(s + r, "only", 4)) || (tl == 6 && !strncmp(s + r, "except", 6))) r = c2_lskipws(s, c->n, te0);
                else if ((tl == 9 && !strncmp(s + r, "prefix-in", 9)) || (tl == 6 && !strncmp(s + r, "prefix", 6))) r = c2_lskipws(s, c->n, c2_ltokend(s, c->n, c2_lskipws(s, c->n, te0))); }
            size_t te = r;
            if (s[r] == '(' || s[r] == '[') {                      /* (scheme base) / [clojure [string :as s]] */
                size_t ie = c2_pm(s, c->n, r), x = c2_lskipws(s, c->n, r + 1); char buf[200]; size_t o = 0;
                while (x < ie - 1 && o < 190) { size_t y = c2_ltokend(s, c->n, x); if (y == x) break; if (o) buf[o++] = '/'; for (size_t z = x; z < y && o < 195; z++) buf[o++] = c->t[z]; x = c2_lskipws(s, c->n, y); if (s[x] == '(' || s[x] == '[') break; }
                if (o) cp_ref(c, from, CR_IMPORT, buf, o, r);
                p = fe; continue;
            }
            te = c2_ltokend(s, c->n, r);
            if (te > r && s[r] != ':') {
                size_t nx = c2_lskipws(s, c->n, te);
                if (s[nx] == '[' || s[nx] == '(') {                 /* lista com prefixo: [clojure [string :as s] [set]] */
                    char pre[160]; size_t pl = te - r < 150 ? te - r : 150; memcpy(pre, c->t + r, pl); pre[pl] = 0;
                    while (nx < fe - 1 && (s[nx] == '[' || s[nx] == '(')) {
                        size_t se = c2_pm(s, c->n, nx), x = c2_lskipws(s, c->n, nx + 1), y = c2_ltokend(s, c->n, x); char buf[260];
                        if (y > x) { int l = snprintf(buf, sizeof buf, "%s.%.*s", pre, (int)(y - x), c->t + x); if (l > 0) cp_ref(c, from, CR_IMPORT, buf, (size_t)l, x); }
                        nx = c2_lskipws(s, c->n, se);
                    }
                } else cp_ref(c, from, CR_IMPORT, c->t + r, te - r, r);
            } else if (s[r] == '"') { size_t z = r + 1; while (z < fe && c->t[z] != '"') z++; if (z > r + 1) cp_ref(c, from, CR_IMPORT, c->t + r + 1, z - r - 1, r + 1); }
            p = fe; continue;
        }
        if (s[q] == '"') { size_t z = q + 1; while (z < e && c->t[z] != '"') z++; if (z > q + 1) cp_ref(c, from, CR_IMPORT, c->t + q + 1, z - q - 1, q + 1); p = z < e ? z + 1 : e; continue; }
        size_t te = c2_ltokend(s, c->n, q);
        if (te > q && s[q] != ':' && s[q] != ')' && isalpha((unsigned char)s[q])) cp_ref(c, from, CR_IMPORT, c->t + q, te - q, q);
        p = te > p ? te : p + 1;
    }
}
static void c2_lisp_region(CP *c, size_t a, size_t e, int parent, int dp);
static void c2_lisp_kids(CP *c, size_t a, size_t e, int parent) {   /* (nome [args] ...) dentro de defprotocol/defrecord/deftype */
    const char *s = c->c; size_t p = a;
    while (p < e) {
        p = c2_lskipws(s, c->n, p); if (p >= e) break;
        if (s[p] == '(') {
            size_t fe = c2_pm(s, c->n, p), h = c2_lskipws(s, c->n, p + 1), he = c2_ltokend(s, c->n, h);
            if (he > h && s[h] != ':' && s[h] != '(' && s[h] != '[' && fe > he) {
                size_t x = c2_lskipws(s, c->n, he);
                if (s[x] == '[' || s[x] == '"' || s[x] == '(') { int si = c2_add(c, CK_METHOD, parent, p, fe, c->t + h, he - h, p, he); (void)si; }
            }
            p = fe; continue;
        }
        p = c2_lskipform(c->c, c->n, p);
    }
}
static void c2_lisp_form(CP *c, size_t p, size_t fe, int parent, int dp) {
    const char *s = c->c; size_t n = c->n;
    size_t h = c2_lskipws(s, n, p + 1), he = c2_ltokend(s, n, h);
    if (he <= h) return;
    size_t hl = he - h; char hb[48]; if (hl >= sizeof hb) return;
    memcpy(hb, s + h, hl); hb[hl] = 0;
    const char *hh = hb; { const char *sl = strrchr(hb, '/'); if (sl && sl[1]) hh = sl + 1; }
    int L = c->lang;
    if (!strcmp(hh, "do") || !strcmp(hh, "let") || !strcmp(hh, "when") || !strcmp(hh, "when-not") || !strcmp(hh, "binding") || !strcmp(hh, "with-redefs") || !strcmp(hh, "locking") || !strcmp(hh, "begin") || !strcmp(hh, "eval-when") || !strcmp(hh, "progn")) {
        if (dp < 4) c2_lisp_region(c, he, fe - 1 > he ? fe - 1 : he, parent, dp + 1); return;
    }
    if (c2_inl_(hb, C2_LIMP) && dp < 4 && !(L == CL_CLJ && (!strcmp(hb, "refer-clojure") && 0))) { c2_limports(c, he, fe > 0 ? fe - 1 : fe, -1); return; }
    const C2LH *r = NULL;
    for (int i = 0; C2_LH[i].h; i++) if (!strcmp(C2_LH[i].h, hh)) { r = &C2_LH[i]; break; }
    if (r && r->kind == 0) return;
    int heur = 0;
    if (!r) {                                                  /* defXxx desconhecido: simbolo heuristico */
        if (hl > 3 && !strncmp(hb, "def", 3) && (islower((unsigned char)hb[3]) || hb[3] == '-' || hb[3] == '/') && !strchr(hb, ':')) heur = 1;
        else if (L == CL_LISP && hl > 7 && !strncmp(hb, "define-", 7)) heur = 1;
        if (!heur) return;
    }
    size_t na, nb; size_t after = c2_lname(s, n, he, &na, &nb);
    if (nb <= na || s[na] == ':' || s[na] == '"' || s[na] == '[' || s[na] == ')' || s[na] == '{') return;
    int kind = r ? r->kind : CK_VAR, fl = r ? r->fl : 0;
    if (r && L == CL_LISP && !strcmp(r->h, "deftype")) kind = CK_TYPEDEF;
    if (fl & LF_DEF) { size_t q = c2_lskipws(s, n, he); kind = s[q] == '(' ? CK_FUNC : CK_VAR; }
    if (r && !strcmp(r->h, "def") && L == CL_CLJ) { size_t q = c2_lskipws(s, n, after); if (s[q] == '"') q = c2_lskipws(s, n, c2_lskipform(s, n, q)); }
    if (nb - na > 200) return;
    size_t eol = c2_eol(s, n, nb); size_t sb = eol < fe ? eol : fe;
    int si = c2_add(c, kind, parent, p, fe, c->t + na, nb - na, p, sb);
    if (si < 0) return;
    if (heur) c->f->sym[si].conf = CC_HEUR;
    if (fl & LF_PRIV) c->f->sym[si].vis = CV_PRIVATE;
    else if (kind == CK_FUNC || kind == CK_VAR || kind == CK_MACRO) { size_t k = h; (void)k; if (strstr(hb, "-") && hl > 4 && hb[hl - 1] == '-') c->f->sym[si].vis = CV_PRIVATE; }
    if (L == CL_CLJ && kind != CK_NAMESPACE) { for (size_t k = he; k < nb; k++) if (!strncmp(s + k, "^:private", 9)) { c->f->sym[si].vis = CV_PRIVATE; break; } }
    if (fl & LF_TEST) c->f->sym[si].flags |= CSF_TEST;
    if (kind == CK_NAMESPACE && (fl & LF_NS)) {
        size_t q = after;
        if (!strcmp(hb, "ns") || !strcmp(hb, "library") || !strcmp(hb, "define-library") || !strcmp(hb, "module")) {
            while (q < fe) {
                q = c2_lskipws(s, n, q); if (q >= fe) break;
                if (s[q] == '(') { size_t ce = c2_pm(s, n, q), hs2 = c2_lskipws(s, n, q + 1), he2 = c2_ltokend(s, n, hs2);
                    if (he2 > hs2 && (s[hs2] == ':' || !strncmp(s + hs2, "import", 6) || !strncmp(s + hs2, "export", 6) || !strncmp(s + hs2, "begin", 5))) {
                        if (!strncmp(s + hs2, ":require", 8) || !strncmp(s + hs2, ":use", 4) || !strncmp(s + hs2, ":import", 7) || !strncmp(s + hs2, ":require-macros", 15) || !strncmp(s + hs2, "import", 6)) c2_limports(c, he2, ce - 1, si);
                        else if (!strncmp(s + hs2, "begin", 5) && dp < 4) c2_lisp_region(c, he2, ce - 1, si, dp + 1);
                    }
                    q = ce; continue; }
                q = c2_lskipform(s, n, q);
            }
        } else if (!strcmp(hb, "defproject")) {
            size_t q2 = after;
            while (q2 < fe) { q2 = c2_lskipws(s, n, q2); if (q2 >= fe) break;
                if (s[q2] == ':' && !strncmp(s + q2, ":dependencies", 13)) { size_t v = c2_lskipws(s, n, q2 + 13); if (s[v] == '[') { size_t ve = c2_pm(s, n, v), x = v + 1;
                    while (x < ve) { x = c2_lskipws(s, n, x); if (s[x] == '[') { size_t ie = c2_pm(s, n, x), y = c2_lskipws(s, n, x + 1), z = c2_ltokend(s, n, y); if (z > y) cp_ref(c, si, CR_IMPORT, c->t + y, z - y, y); x = ie; } else x++; } q2 = ve; continue; } }
                q2 = c2_lskipform(s, n, q2); }
        }
    }
    if (fl & LF_KIDS) c2_lisp_kids(c, after, fe - 1 > after ? fe - 1 : after, si);
    if (fl & LF_SUPER) {                                       /* (defclass x (super...) ...) / (defstruct (x (:include y))) */
        size_t q = c2_lskipws(s, n, after);
        if (s[q] == '(') { size_t ce = c2_pm(s, n, q), x = q + 1; while (x < ce - 1) { x = c2_lskipws(s, n, x); size_t y = c2_ltokend(s, n, x); if (y == x) { x++; continue; } if (s[x] != ':' && s[x] != '(') cp_ref(c, si, CR_EXTENDS, c->t + x, y - x, x); x = y; } }
    }
    if (!strcmp(hh, "defmethod") || !strcmp(hh, "defmulti")) { }
}
static int c2_inl_(const char *w, const char *const *l) { for (int i = 0; l[i]; i++) if (!strcmp(l[i], w)) return 1; return 0; }
static void c2_lisp_region(CP *c, size_t a, size_t e, int parent, int dp) {
    const char *s = c->c; size_t n = c->n, p = a;
    if (e > n) e = n;
    while (p < e) {
        char ch = s[p];
        if (isspace((unsigned char)ch) || ch == ',') { p++; continue; }
        if (ch == '#') {
            if (p + 1 < n && s[p + 1] == '_') { size_t q = c2_lskipform(s, n, p + 2); p = q > p ? q : p + 1; continue; }
            if (p + 1 < n && s[p + 1] == '?') { size_t q = p + 2; if (q < n && s[q] == '@') q++; if (q < n && s[q] == '(') { size_t fe = c2_pm(s, n, q); if (dp < 6) c2_lisp_region(c, q + 1, fe > q + 1 ? fe - 1 : q + 1, parent, dp + 1); p = fe > p ? fe : p + 1; continue; } }
            size_t q = c2_lskipform(s, n, p); p = q > p ? q : p + 1; continue;
        }
        if (ch == '(') { size_t fe = c2_pm(s, n, p); c2_lisp_form(c, p, fe, parent, dp); p = fe > p ? fe : p + 1; continue; }
        size_t q = c2_lskipform(s, n, p); p = q > p ? q : p + 1;
    }
}
static void c2_lisp(CP *c) {
    const char *s = c->c; size_t n = c->n;
    size_t p = c2_lskipws(s, n, 0);
    if (p < n && s[p] == '{' && c->lang == CL_CLJ) {              /* .edn / deps.edn: chaves de topo do mapa */
        size_t me = c2_pm(s, n, p), x = p + 1;
        while (x < me - 1 && c->f->nsym < 4000) {
            x = c2_lskipws(s, n, x); size_t ke = c2_ltokend(s, n, x);
            if (ke <= x) { x++; continue; }
            size_t v = c2_lskipws(s, n, ke), ve = c2_lskipform(s, n, v);
            if (s[x] == ':' && ke - x < 120) c2_add(c, CK_SECTION, -1, x, ve, c->t + x + 1, ke - x - 1, x, ke);
            x = ve > x ? ve : x + 1;
        }
        return;
    }
    c2_lisp_region(c, 0, n, -1, 0);
}

/* ---------- utilidades por nivel (titulos, secoes) ---------- */
/* fecha os simbolos [s0, nsym) em ordem de criacao: cada um termina antes do proximo de nivel <= (ou no fim do arquivo); lv[i] = nivel do simbolo s0+i */
static void c2_close_levels(CP *c, int s0, const int *lv, int cnt) {
    CoderFile *f = c->f; int stk[64]; int sp = 0;
    for (int i = 0; i < cnt && s0 + i < f->nsym; i++) {
        while (sp > 0 && lv[stk[sp - 1]] >= lv[i]) { int k = stk[--sp]; size_t e = f->sym[s0 + i].byte0; while (e > f->sym[s0 + k].byte0 && isspace((unsigned char)c->c[e - 1])) e--; cp_setend(c, s0 + k, e); }
        if (sp < 63) stk[sp++] = i;
    }
    while (sp > 0) { int k = stk[--sp]; size_t e = c->n; while (e > f->sym[s0 + k].byte0 && isspace((unsigned char)c->c[e - 1])) e--; cp_setend(c, s0 + k, e); }
}
static void c2_trimname(const char *s, size_t *a, size_t *b) { while (*a < *b && isspace((unsigned char)s[*a])) (*a)++; while (*b > *a && isspace((unsigned char)s[*b - 1])) (*b)--; }

/* ---------- SQL ---------- */
static const char *const C2_SQLDDL[] = {"table", "view", "function", "procedure", "index", "trigger", "type", "schema", "sequence", "extension", "domain", "package", "database", NULL};
static const char *const C2_SQLSKIP[] = {"select", "only", "lateral", "unnest", "generate_series", "values", "with", "if", "not", "exists", "table", "set", "where", "dual", "as", "on", "using", "inner", "left", "right", "outer", "cross", "natural", "full", "the", "of", "tablesample", "json_table", NULL};
static int c2_sqlword(const char *s, size_t n, size_t p, size_t *e, char *out, size_t cap) {   /* palavra minuscula; devolve 0 se nao ha */
    while (p < n && !(c2_wc((unsigned char)s[p]) || s[p] == '(' || s[p] == ')' || s[p] == ';' || s[p] == ',' || s[p] == '.' || s[p] == '=' || s[p] == '@' || s[p] == '#')) { p++; }
    size_t q = p; while (q < n && (c2_wc((unsigned char)s[q]) || s[q] == '$' || s[q] == '#' || s[q] == '@')) q++;
    if (q == p) { *e = p < n ? p + 1 : p; if (out) out[0] = 0; return 0; }
    if (out) { size_t l = q - p; if (l >= cap) l = cap - 1; for (size_t k = 0; k < l; k++) out[k] = (char)tolower((unsigned char)s[p + k]); out[l] = 0; }
    *e = q; return 1;
}
static void c2_sqlname(CP *c, size_t p, size_t *a, size_t *b, size_t *last) {   /* nome com pontos a partir de p; *a..*b = parte final */
    const char *s = c->c; size_t n = c->n; p = c2_lskipws(s, n, p);
    size_t st = p, q = p;
    for (;;) { while (q < n && (c2_wc((unsigned char)s[q]) || s[q] == '$' || s[q] == '#' || s[q] == '@')) q++; size_t z = c2_skipws(s, n, q); if (z < n && s[z] == '.' && z + 1 < n) { q = c2_skipws(s, n, z + 1); continue; } break; }
    *last = q; size_t b0 = q; while (b0 > st && (c2_wc((unsigned char)s[b0 - 1]) || s[b0 - 1] == '$' || s[b0 - 1] == '#' || s[b0 - 1] == '@')) b0--;
    *a = b0; *b = q;
}
static void c2_sql(CP *c) {
    const char *s = c->c, *t = c->t; size_t n = c->n; CoderFile *f = c->f;
    int cur = -1; size_t curend = 0; int fnstack[64]; int fsp = 0; char prev[24] = ""; char prev2[24] = "";
    size_t p = 0; int stmtstart = 1;
    while (p < n) {
        char w[32]; size_t e;
        if (!c2_sqlword(s, n, p, &e, w, sizeof w)) {
            char ch = s[p < n ? p : n - 1];
            for (size_t z = p; z < e && z < n; z++) { ch = s[z]; if (ch == '(') { if (fsp < 64) fnstack[fsp++] = !strcmp(prev, "extract") || !strcmp(prev, "substring") || !strcmp(prev, "trim") || !strcmp(prev, "overlay") || !strcmp(prev, "position"); } else if (ch == ')') { if (fsp > 0) fsp--; } else if (ch == ';') { stmtstart = 1; cur = -1; } }
            p = e > p ? e : p + 1; if (e > p || 1) prev[0] = 0; continue;
        }
        if (cur >= 0 && p >= curend) cur = -1;
        size_t wstart = p; while (wstart < e && !c2_wc((unsigned char)s[wstart]) && s[wstart] != '$' && s[wstart] != '#' && s[wstart] != '@') wstart++;
        if (!strcmp(w, "create") || (!strcmp(w, "alter") && 0)) {
            size_t q = e; char x[32]; size_t xe; int kind = -1, matview = 0; char what[16] = "";
            for (int g = 0; g < 12; g++) {
                if (!c2_sqlword(s, n, q, &xe, x, sizeof x)) break;
                int ddl = 0; for (int k = 0; C2_SQLDDL[k]; k++) if (!strcmp(x, C2_SQLDDL[k])) ddl = 1;
                if (!strcmp(x, "materialized")) matview = 1;
                if (ddl) { snprintf(what, sizeof what, "%s", x); break; }
                if (!strcmp(x, "view")) { snprintf(what, sizeof what, "view"); break; }
                q = xe;
            }
            if (what[0]) {
                size_t k0 = q; k0 = c2_skipws(s, n, k0); char x[32]; size_t xe; c2_sqlword(s, n, k0, &xe, x, sizeof x);
                size_t nq = xe;
                if (!strcmp(what, "package")) { char y[32]; size_t ye; if (c2_sqlword(s, n, nq, &ye, y, sizeof y) && !strcmp(y, "body")) nq = ye; }
                size_t z = nq; char y[32]; size_t ye;
                if (c2_sqlword(s, n, z, &ye, y, sizeof y) && !strcmp(y, "if")) { z = ye; if (c2_sqlword(s, n, z, &ye, y, sizeof y) && !strcmp(y, "not")) { z = ye; if (c2_sqlword(s, n, z, &ye, y, sizeof y) && !strcmp(y, "exists")) z = ye; } }
                size_t na, nb, last; c2_sqlname(c, z, &na, &nb, &last);
                if (nb > na && nb - na < 120) {
                    /* fim da instrucao: ';' em profundidade 0 de begin/case e parenteses */
                    int dep = 0, par = 0; size_t endp = n, r = last; char ww[24]; size_t re;
                    while (r < n) {
                        char chh = s[r];
                        if (chh == '(') par++; else if (chh == ')') { if (par > 0) par--; }
                        else if (chh == ';' && dep == 0 && par == 0) { endp = r + 1; break; }
                        else if (c2_wc((unsigned char)chh) && (r == 0 || !c2_wc((unsigned char)s[r - 1]))) {
                            re = r; while (re < n && c2_wc((unsigned char)s[re])) re++; size_t l = re - r; if (l < sizeof ww) { for (size_t k = 0; k < l; k++) ww[k] = (char)tolower((unsigned char)s[r + k]); ww[l] = 0;
                                if (!strcmp(ww, "begin")) { size_t q2 = c2_skipws(s, n, re); if (!(q2 < n && (!strncasecmp(s + q2, "tran", 4) || !strncasecmp(s + q2, "work", 4) || s[q2] == ';')) || 1) { if (!(q2 < n && (!strncasecmp(s + q2, "tran", 4) || !strncasecmp(s + q2, "work", 4)))) dep++; } }
                                else if (!strcmp(ww, "case")) dep++;
                                else if (!strcmp(ww, "end")) { size_t q2 = c2_skipws(s, n, re); if (q2 < n && c2_wc((unsigned char)s[q2])) { char w2[12]; size_t m = q2; while (m < n && c2_wc((unsigned char)s[m])) m++; size_t l2 = m - q2; if (l2 < sizeof w2) { for (size_t k = 0; k < l2; k++) w2[k] = (char)tolower((unsigned char)s[q2 + k]); w2[l2] = 0; if (!strcmp(w2, "if") || !strcmp(w2, "loop") || !strcmp(w2, "while") || !strcmp(w2, "repeat") || !strcmp(w2, "for")) { r = re; continue; } } } if (dep > 0) dep--; }
                                else if ((!strcmp(ww, "create") || !strcmp(ww, "go")) && dep == 0 && par == 0 && r > last + 10) { size_t ls0 = r; while (ls0 > 0 && (s[ls0 - 1] == ' ' || s[ls0 - 1] == '\t')) ls0--; if (ls0 == 0 || s[ls0 - 1] == '\n') { endp = ls0; break; } } }
                            r = re; continue;
                        }
                        r++;
                    }
                    size_t b1 = endp; while (b1 > wstart && (isspace((unsigned char)s[b1 - 1]))) b1--;
                    if (b1 < wstart) b1 = wstart;
                    int kd = !strcmp(what, "table") || !strcmp(what, "view") ? CK_STRUCT : !strcmp(what, "function") || !strcmp(what, "procedure") || !strcmp(what, "trigger") ? CK_FUNC : !strcmp(what, "type") || !strcmp(what, "domain") ? CK_TYPEDEF : !strcmp(what, "schema") || !strcmp(what, "database") || !strcmp(what, "package") ? CK_NAMESPACE : CK_VAR;
                    (void)kind; (void)matview;
                    size_t eol = c2_eol(s, n, nb); size_t sb = eol < b1 ? eol : b1;
                    int si = c2_add(c, kd, -1, wstart, b1, t + na, nb - na, wstart, sb);
                    if (si >= 0) {
                        cur = si; curend = b1;
                        if (!strcmp(what, "table") || (!strcmp(what, "type") && 0)) {          /* colunas */
                            size_t o = last; while (o < b1 && s[o] != '(') { if (s[o] == ';') break; o++; }
                            if (o < b1 && s[o] == '(') {
                                size_t ce = c2_pm(s, n, o), x = o + 1; int cnt = 0;
                                while (x < ce - 1 && cnt < 300) {
                                    size_t it = c2_skipws(s, n, x); while (it < ce && (s[it] == '\n' || s[it] == '\r')) it = c2_skipws(s, n, it + 1);
                                    size_t iend = it; int dp2 = 0; while (iend < ce - 1) { if (s[iend] == '(') dp2++; else if (s[iend] == ')') dp2--; else if (s[iend] == ',' && dp2 <= 0) break; iend++; }
                                    char cw[24]; size_t cwe; if (c2_sqlword(s, n, it, &cwe, cw, sizeof cw) && cwe <= iend + 1) {
                                        size_t cs0 = it; while (cs0 < cwe && !c2_wc((unsigned char)s[cs0])) cs0++;
                                        if (strcmp(cw, "constraint") && strcmp(cw, "primary") && strcmp(cw, "foreign") && strcmp(cw, "unique") && strcmp(cw, "check") && strcmp(cw, "key") && strcmp(cw, "index") && strcmp(cw, "exclude") && strcmp(cw, "like") && strcmp(cw, "period") && strcmp(cw, "fulltext") && cwe > cs0) {
                                            size_t ie = iend; while (ie > cs0 && isspace((unsigned char)s[ie - 1])) ie--;
                                            c2_add(c, CK_FIELD, si, cs0, ie, t + cs0, cwe - cs0, cs0, ie); cnt++;
                                        }
                                    }
                                    x = iend + 1;
                                }
                            }
                        }
                    }
                    if (!strcmp(what, "index") || !strcmp(what, "trigger")) { size_t r2 = last; char y2[32]; size_t y2e; for (int g = 0; g < 6; g++) { if (!c2_sqlword(s, n, r2, &y2e, y2, sizeof y2)) break; if (!strcmp(y2, "on")) { size_t a2, b2, l2; c2_sqlname(c, y2e, &a2, &b2, &l2); if (b2 > a2) cp_ref(c, si, CR_USE, t + a2, b2 - a2, a2); break; } r2 = y2e; } }
                    p = last > p ? last : e; strcpy(prev, ""); stmtstart = 0; continue;
                }
            }
        }
        /* referencias a tabelas */
        int tabw = !strcmp(w, "from") || !strcmp(w, "join") || !strcmp(w, "into") || !strcmp(w, "update") || !strcmp(w, "references") || (!strcmp(w, "table") && (!strcmp(prev, "alter") || !strcmp(prev, "truncate") || !strcmp(prev, "drop") || !strcmp(prev, "lock") || !strcmp(prev, "exists") || !strcmp(prev, "analyze"))) || (!strcmp(w, "truncate"));
        if (tabw && !(!strcmp(w, "from") && fsp > 0 && fnstack[fsp - 1]) && !(!strcmp(w, "update") && (!strcmp(prev, "for") || !strcmp(prev, "on") || !strcmp(prev, "key") || !strcmp(prev, "no"))) && !(!strcmp(w, "from") && (!strcmp(prev, "distinct") && 0))) {
            size_t q = e; char x[32]; size_t xe;
            if (!strcmp(w, "table") || !strcmp(w, "truncate")) { if (c2_sqlword(s, n, q, &xe, x, sizeof x) && !strcmp(x, "table")) q = xe; if (c2_sqlword(s, n, q, &xe, x, sizeof x) && !strcmp(x, "if")) { q = xe; c2_sqlword(s, n, q, &xe, x, sizeof x); if (!strcmp(x, "exists")) q = xe; } if (c2_sqlword(s, n, q, &xe, x, sizeof x) && !strcmp(x, "only")) q = xe; }
            if (!strcmp(w, "from") || !strcmp(w, "join") || !strcmp(w, "update")) { if (c2_sqlword(s, n, q, &xe, x, sizeof x) && !strcmp(x, "only")) q = xe; }
            if (!strcmp(w, "into") && c2_sqlword(s, n, q, &xe, x, sizeof x) && !strcmp(x, "table")) q = xe;
            size_t z = c2_skipws(s, n, q);
            if (z < n && s[z] != '(' && (c2_wc((unsigned char)s[z]) || s[z] == '$' || s[z] == '#' || s[z] == '@')) {
                int seen = 0;
                for (;;) {
                    size_t na, nb, last; c2_sqlname(c, z, &na, &nb, &last);
                    if (nb <= na) break;
                    char nm[64]; size_t nl = nb - na; int skip = 0; if (nl < sizeof nm) { for (size_t k = 0; k < nl; k++) nm[k] = (char)tolower((unsigned char)s[na + k]); nm[nl] = 0; if (c2_inl_(nm, C2_SQLSKIP)) skip = 1; } else skip = 1;
                    if (!skip && !isdigit((unsigned char)s[na])) cp_ref(c, cur, CR_USE, t + na, nl, na);
                    seen++;
                    z = c2_skipws(s, n, last);
                    if (!strcmp(w, "from") && z < n && s[z] == ',' && seen < 6) { z = c2_skipws(s, n, z + 1); if (z < n && c2_wc((unsigned char)s[z])) { /* alias? 'a b, c' tratado abaixo */ } else break; }
                    else {
                        if (!strcmp(w, "from") && z < n) { char al[32]; size_t ale; if (c2_sqlword(s, n, z, &ale, al, sizeof al) && !c2_inl_(al, C2_SQLSKIP) && strcmp(al, "group") && strcmp(al, "order") && strcmp(al, "limit") && strcmp(al, "union") && strcmp(al, "having") && strcmp(al, "window") && strcmp(al, "for") && strcmp(al, "returning") && strcmp(al, "fetch") && strcmp(al, "offset")) { size_t z2 = c2_skipws(s, n, ale); if (z2 < n && s[z2] == ',' && seen < 6) { z = c2_skipws(s, n, z2 + 1); continue; } } }
                        break;
                    }
                }
            }
        } else if ((!strcmp(w, "call") || !strcmp(w, "exec") || !strcmp(w, "execute")) && (stmtstart || !strcmp(prev, "")) ) {
            size_t a2, b2, l2; c2_sqlname(c, e, &a2, &b2, &l2); if (b2 > a2 && b2 - a2 < 80) { char nm[8]; (void)nm; if (strncasecmp(s + a2, "immediate", 9) && strncasecmp(s + a2, "format", 6)) cp_ref(c, cur, CR_CALL, t + a2, b2 - a2, a2); }
        }
        { size_t q = c2_skipws(s, n, e); if (q < n && s[q] == '(' && (!strcmp(w, "extract") || !strcmp(w, "substring") || !strcmp(w, "trim") || !strcmp(w, "overlay") || !strcmp(w, "position"))) snprintf(prev, sizeof prev, "%s", w); else snprintf(prev, sizeof prev, "%s", w); }
        (void)prev2; stmtstart = 0;
        p = e;
        for (size_t z = wstart + 0; z < e && 0; z++) {}
        { size_t q = p; while (q < n && (s[q] == ' ' || s[q] == '\t')) q++; if (q < n && s[q] == '(') { if (fsp < 64) fnstack[fsp++] = !strcmp(w, "extract") || !strcmp(w, "substring") || !strcmp(w, "trim") || !strcmp(w, "overlay") || !strcmp(w, "position"); p = q + 1; strcpy(prev, ""); } }
        (void)f;
    }
}

/* ---------- JSON de projeto ---------- */
static size_t c2_jskip(const char *s, size_t n, size_t p) {
    while (p < n && isspace((unsigned char)s[p])) p++;
    if (p >= n) return n;
    if (s[p] == '"') { p++; while (p < n && s[p] != '"') { if (s[p] == '\\') p++; p++; } return p < n ? p + 1 : n; }
    if (s[p] == '{' || s[p] == '[') {
        int d = 0;
        while (p < n) { char ch = s[p]; if (ch == '"') { p++; while (p < n && s[p] != '"') { if (s[p] == '\\') p++; p++; } } else if (ch == '{' || ch == '[') d++; else if (ch == '}' || ch == ']') { if (--d <= 0) return p + 1; } p++; }
        return n;
    }
    while (p < n && !strchr(",}] \t\r\n", s[p])) p++;
    return p;
}
static int c2_jkey(const char *s, size_t n, size_t p, size_t *a, size_t *b, size_t *after) {   /* "chave" : em p; devolve 1 se achou */
    p = c2_lskipws(s, n, p); if (p >= n || s[p] != '"') return 0;
    size_t q = p + 1; while (q < n && s[q] != '"' && s[q] != '\n') { if (s[q] == '\\') q++; q++; }
    if (q >= n || s[q] != '"') return 0;
    *a = p + 1; *b = q; size_t z = c2_lskipws(s, n, q + 1); if (z >= n || s[z] != ':') return 0;
    *after = z + 1; return 1;
}
static void c2_json(CP *c) {
    const char *s = c->t; size_t n = c->n;
    size_t p = c2_lskipws(s, n, 0); if (p >= n || s[p] != '{') return;
    size_t me = c2_jskip(s, n, p), x = p + 1; int cnt = 0;
    while (x < me && cnt < 200) {
        size_t ka, kb, af; while (x < me && (isspace((unsigned char)s[x]) || s[x] == ',')) x++;
        if (x >= me || s[x] == '}') break;
        if (!c2_jkey(s, n, x, &ka, &kb, &af)) { x = c2_jskip(s, n, x); continue; }
        size_t v = c2_lskipws(s, n, af), ve = c2_jskip(s, n, v); if (ve > me) ve = me;
        if (kb - ka > 100) { x = ve; continue; }
        int isobj = s[v] == '{', isarr = s[v] == '[';
        int si = c2_add(c, isobj || isarr ? CK_SECTION : CK_CONST, -1, ka - 1, ve, s + ka, kb - ka, ka - 1, ve < ka + 100 ? ve : ka + 100); cnt++;
        char key[48]; size_t kl = kb - ka < 47 ? kb - ka : 47; memcpy(key, s + ka, kl); key[kl] = 0;
        if (si >= 0 && !strcmp(key, "extends") && s[v] == '"') { size_t e2 = v + 1; while (e2 < ve && s[e2] != '"') e2++; if (e2 > v + 1) cp_ref(c, si, CR_IMPORT, s + v + 1, e2 - v - 1, v + 1); }
        if (si >= 0 && isobj && ve - v < 200000) {
            int dep = strstr(key, "ependencies") != NULL || !strcmp(key, "require") || !strcmp(key, "require-dev") || !strcmp(key, "peerDependencies");
            size_t y = v + 1; int cc = 0;
            while (y < ve && cc < 150) {
                size_t k2a, k2b, af2; while (y < ve && (isspace((unsigned char)s[y]) || s[y] == ',')) y++;
                if (y >= ve || s[y] == '}') break;
                if (!c2_jkey(s, n, y, &k2a, &k2b, &af2)) { y = c2_jskip(s, n, y); continue; }
                size_t v2 = c2_lskipws(s, n, af2), v2e = c2_jskip(s, n, v2); if (v2e > ve) v2e = ve;
                if (k2b - k2a < 120) { c2_add(c, CK_CONST, si, k2a - 1, v2e, s + k2a, k2b - k2a, k2a - 1, v2e < k2a + 160 ? v2e : k2a + 160); cc++; if (dep) cp_ref(c, si, CR_IMPORT, s + k2a, k2b - k2a, k2a); }
                y = v2e > y ? v2e : y + 1;
            }
        } else if (si >= 0 && isarr && !strcmp(key, "references")) {
            size_t y = v; while (y < ve) { const char *pp = strstr(s + y, "\"path\""); if (!pp || (size_t)(pp - s) >= ve) break; size_t z = c2_lskipws(s, n, (size_t)(pp - s) + 6); if (s[z] == ':') { z = c2_lskipws(s, n, z + 1); if (s[z] == '"') { size_t e2 = z + 1; while (e2 < ve && s[e2] != '"') e2++; cp_ref(c, si, CR_IMPORT, s + z + 1, e2 - z - 1, z + 1); } } y = (size_t)(pp - s) + 6; }
        }
        x = ve > x ? ve : x + 1;
    }
}

/* ---------- YAML ---------- */
static int c2_indent(const char *s, size_t a, size_t e) { int k = 0; while (a < e && (s[a] == ' ' || s[a] == '\t')) { k += s[a] == '\t' ? 4 : 1; a++; } return a >= e ? -1 : k; }
static int c2_ykey(const char *s, size_t a, size_t e, size_t *ka, size_t *kb, size_t *vs) {   /* chave: valor numa linha */
    size_t p = a; while (p < e && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= e || s[p] == '#' || s[p] == '-' || s[p] == '[' || s[p] == '{' || s[p] == '?' || s[p] == '|' || s[p] == '>') { if (!(s[p] == '-' && p + 1 < e && s[p + 1] != ' ')) return 0; }
    size_t k0 = p, k1;
    if (s[p] == '"' || s[p] == '\'') { char q = s[p]; k0 = p + 1; k1 = k0; while (k1 < e && s[k1] != q) k1++; if (k1 >= e) return 0; p = k1 + 1; }
    else { k1 = p; while (k1 < e && !(s[k1] == ':' && (k1 + 1 >= e || s[k1 + 1] == ' ' || s[k1 + 1] == '\t' || s[k1 + 1] == '\r'))) { if (s[k1] == '#' && k1 > p && (s[k1 - 1] == ' ')) return 0; k1++; } if (k1 >= e) return 0; p = k1; }
    if (p >= e || s[p] != ':') return 0;
    size_t kk = k1; while (kk > k0 && (s[kk - 1] == ' ' || s[kk - 1] == '\t')) kk--;
    if (kk <= k0 || kk - k0 > 120) return 0;
    *ka = k0; *kb = kk; *vs = p + 1; return 1;
}
static void c2_yaml(CP *c) {
    const char *s = c->t; size_t n = c->n; int cur = -1, curchild = -1; int curind = -1, childind = -1; int cnt = 0, blk = -1;
    int sec = -1; (void)sec;
    for (int ln = 1; ln <= c->nln && cnt < 1500; ln++) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln); int ind = c2_indent(s, ls, le);
        if (ind < 0) continue;
        size_t fs = ls; while (fs < le && (s[fs] == ' ' || s[fs] == '\t')) fs++;
        if (blk >= 0) { if (ind > blk) continue; blk = -1; }
        if (s[fs] == '#') continue;
        if (!strncmp(s + fs, "---", 3) || !strncmp(s + fs, "...", 3)) { if (cur >= 0) cp_setend(c, cur, ls > 0 ? ls - 1 : 0); cur = -1; curchild = -1; curind = -1; childind = -1; continue; }
        if (s[fs] == '-' && fs + 1 < le && s[fs + 1] == ' ') {
            const char *u = NULL; for (size_t z = fs; z + 5 < le; z++) if (!strncmp(s + z, "uses:", 5) && (s[z - 1] == ' ' || s[z - 1] == '-')) { u = s + z + 5; break; }
            if (u) { size_t v2 = (size_t)(u - s); while (v2 < le && s[v2] == ' ') v2++; size_t e2 = le; while (e2 > v2 && (s[e2 - 1] == ' ' || s[e2 - 1] == '\r')) e2--; if (e2 > v2) cp_ref(c, cur, CR_IMPORT, s + v2, e2 - v2, v2); }
            if (cur >= 0) cp_setend(c, cur, le);
            continue;
        }
        size_t ka, kb, vs;
        if (!c2_ykey(s, ls, le, &ka, &kb, &vs)) continue;
        size_t v = vs; while (v < le && (s[v] == ' ' || s[v] == '\t')) v++;
        int block = v < le && (s[v] == '|' || s[v] == '>') && (v + 1 >= le || s[v + 1] == '-' || s[v + 1] == '+' || s[v + 1] == ' ' || s[v + 1] == '\r' || isdigit((unsigned char)s[v + 1]));
        if (v < le && !strncmp(s + ka, "uses", 4) && kb - ka == 4) { size_t e2 = le; while (e2 > v && (s[e2 - 1] == ' ' || s[e2 - 1] == '\r')) e2--; if (e2 > v) cp_ref(c, cur, CR_IMPORT, s + v, e2 - v, v); }
        if (block) blk = ind;
        if (ind == 0) {
            int hasval = v < le && s[v] != '#' && !block;
            cur = c2_add(c, hasval ? CK_CONST : CK_SECTION, -1, ls, le, s + ka, kb - ka, ls, le); cnt++; curind = 0; curchild = -1; childind = -1;
        } else if (cur >= 0 && ind > curind) {
            if (childind < 0) childind = ind;
            if (ind == childind) { curchild = c2_add(c, CK_VAR, cur, fs, le, s + ka, kb - ka, fs, le); cnt++; }
        }
        if (cur >= 0 && !(ind == 0) ) { cp_setend(c, cur, le); if (curchild >= 0 && ind > childind) cp_setend(c, curchild, le); else if (curchild >= 0 && ind == childind) cp_setend(c, curchild, le); }
    }
    for (int s0 = 0; s0 < c->f->nsym; s0++) { CoderSym *y = &c->f->sym[s0]; if (y->parent < 0 && y->kind == CK_SECTION) { /* intervalo ja crescido linha a linha */ } }
}

/* ---------- TOML ---------- */
static void c2_toml(CP *c) {
    const char *s = c->t; size_t n = c->n; int cur = -1, cnt = 0; (void)n;
    int dep = 0; int mlq = 0; char mlc = 0;
    for (int ln = 1; ln <= c->nln && cnt < 1500; ln++) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln), fs = c2_skipws(s, le, ls);
        if (mlq) { for (size_t k = fs; k + 2 < le + 1 && k + 2 <= le; k++) if (s[k] == mlc && k + 2 < le + 0 + 1 && s[k + 1] == mlc && s[k + 2] == mlc) { mlq = 0; break; } if (cur >= 0) cp_setend(c, cur, le); continue; }
        if (dep > 0) { for (size_t k = fs; k < le; k++) { if (s[k] == '"' || s[k] == '\'') { char q = s[k]; k++; while (k < le && s[k] != q) { if (s[k] == '\\' && q == '"') k++; k++; } } else if (s[k] == '#') break; else if (s[k] == '[' || s[k] == '{') dep++; else if (s[k] == ']' || s[k] == '}') dep--; } if (cur >= 0) cp_setend(c, cur, le); continue; }
        if (fs >= le || s[fs] == '#') continue;
        if (s[fs] == '[') {
            int dbl = fs + 1 < le && s[fs + 1] == '['; size_t a = fs + (dbl ? 2 : 1), b = a; while (b < le && s[b] != ']') b++;
            size_t aa = a, bb = b; c2_trimname(s, &aa, &bb);
            if (bb > aa) { if (cur >= 0) { size_t e = ls; while (e > c->f->sym[cur].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, cur, e); }
                cur = c2_add(c, CK_SECTION, -1, ls, le, s + aa, bb - aa, ls, le); cnt++; }
            continue;
        }
        size_t k = fs; while (k < le && s[k] != '=' && s[k] != '#') k++;
        if (k >= le || s[k] != '=') continue;
        size_t ka = fs, kb = k; c2_trimname(s, &ka, &kb);
        if (kb > ka && (s[ka] == '"' || s[ka] == '\'') && kb - ka >= 2) { ka++; kb--; }
        if (kb <= ka || kb - ka > 120) continue;
        size_t v = k + 1; while (v < le && (s[v] == ' ' || s[v] == '\t')) v++;
        int isdep = 0; if (cur >= 0) { const char *cn = c->f->sym[cur].name; size_t cl2 = strlen(cn); isdep = (cl2 >= 12 && strstr(cn, "dependencies") != NULL) || !strcmp(cn, "dependencies"); }
        if (cnt < 1500) { c2_add(c, CK_CONST, cur, ls, le, s + ka, kb - ka, ls, le < ls + 160 ? le : ls + 160); cnt++; if (isdep) cp_ref(c, cur, CR_IMPORT, s + ka, kb - ka, ka); }
        if (cur >= 0) cp_setend(c, cur, le);
        if (v + 2 < le + 0 && (s[v] == '"' || s[v] == '\'') && s[v + 1] == s[v] && s[v + 2] == s[v]) { int closed = 0; for (size_t z = v + 3; z + 2 < le + 1 && z + 2 <= le; z++) if (s[z] == s[v] && s[z + 1] == s[v] && s[z + 2] == s[v]) { closed = 1; break; } if (!closed) { mlq = 1; mlc = s[v]; } continue; }
        for (size_t z = v; z < le; z++) { if (s[z] == '"' || s[z] == '\'') { char q = s[z]; z++; while (z < le && s[z] != q) { if (s[z] == '\\' && q == '"') z++; z++; } } else if (s[z] == '#') break; else if (s[z] == '[' || s[z] == '{') dep++; else if (s[z] == ']' || s[z] == '}') dep--; }
        if (dep < 0) dep = 0;
    }
    if (cur >= 0) { size_t e = c->n; while (e > c->f->sym[cur].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, cur, e); }
}

/* ---------- Dockerfile ---------- */
static void c2_docker(CP *c) {
    const char *s = c->t; size_t n = c->n; int stage = -1, nst = 0; char stages[64][64]; int ns = 0; (void)n;
    int ln = 1;
    while (ln <= c->nln) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln), fs = c2_skipws(s, le, ls); int l0 = ln;
        if (fs >= le || s[fs] == '#') { ln++; continue; }
        size_t lend = le;                                               /* linha logica (continuacao por \) */
        while (lend > fs && (s[lend - 1] == ' ' || s[lend - 1] == '\t')) lend--;
        while (lend > fs && s[lend - 1] == '\\' && ln < c->nln) { ln++; size_t l2 = c2_lb(c, ln); size_t e2 = c2_le(c, ln); while (l2 < e2 && (s[l2] == ' ' || s[l2] == '\t')) l2++; if (l2 < e2 && s[l2] == '#') { lend = e2; while (lend > l2 && s[lend - 1] == ' ') lend--; if (!(lend > l2 && s[lend - 1] == '\\')) { lend = l2; break; } continue; } lend = e2; while (lend > l2 && (s[lend - 1] == ' ' || s[lend - 1] == '\t')) lend--; }
        ln++;
        size_t wend = fs; while (wend < lend && isalpha((unsigned char)s[wend])) wend++;
        char w[16]; size_t wl = wend - fs; if (wl == 0 || wl >= sizeof w) continue; for (size_t k = 0; k < wl; k++) w[k] = (char)toupper((unsigned char)s[fs + k]); w[wl] = 0;
        size_t a = c2_skipws(s, lend, wend);
        if (!strcmp(w, "FROM")) {
            while (a < lend && s[a] == '-' ) { while (a < lend && !isspace((unsigned char)s[a])) a++; a = c2_skipws(s, lend, a); }
            size_t ie = a; while (ie < lend && !isspace((unsigned char)s[ie])) ie++;
            size_t q = c2_skipws(s, lend, ie); char as[64] = ""; if (q + 2 <= lend && !strncasecmp(s + q, "as", 2) && (q + 2 >= lend || isspace((unsigned char)s[q + 2]))) { size_t z = c2_skipws(s, lend, q + 2), ze = z; while (ze < lend && !isspace((unsigned char)s[ze])) ze++; size_t l2 = ze - z < 63 ? ze - z : 63; memcpy(as, s + z, l2); as[l2] = 0; }
            if (stage >= 0) { size_t e = ls; while (e > c->f->sym[stage].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, stage, e); }
            char nm[80]; if (as[0]) snprintf(nm, sizeof nm, "%s", as); else snprintf(nm, sizeof nm, "stage-%d", nst);
            nst++;
            stage = c2_add(c, CK_NAMESPACE, -1, ls, lend, nm, strlen(nm), ls, lend);
            if (as[0] && ns < 64) { snprintf(stages[ns], 64, "%s", as); ns++; }
            if (ie > a && stage >= 0) { int isst = 0; for (int k = 0; k < ns; k++) if (strlen(stages[k]) == ie - a && !memcmp(stages[k], s + a, ie - a)) isst = 1; if (isst) cp_ref(c, stage, CR_USE, s + a, ie - a, a); else cp_ref(c, stage, CR_IMPORT, s + a, ie - a, a); }
        } else if (!strcmp(w, "ARG") || !strcmp(w, "ENV")) {
            size_t q = a;
            while (q < lend) {
                size_t ke = q; while (ke < lend && s[ke] != '=' && !isspace((unsigned char)s[ke])) ke++;
                if (ke > q) c2_add(c, CK_VAR, stage, fs, lend, s + q, ke - q, fs, lend);
                if (!strcmp(w, "ENV") && ke < lend && s[ke] != '=') break;                    /* ENV chave valor */
                if (ke < lend && s[ke] == '=') { size_t z = ke + 1; if (z < lend && (s[z] == '"' || s[z] == '\'')) { char qq = s[z]; z++; while (z < lend && s[z] != qq) { if (s[z] == '\\') z++; z++; } z++; } else while (z < lend && !isspace((unsigned char)s[z])) z++; ke = z; }
                q = c2_skipws(s, lend, ke); if (!strcmp(w, "ARG")) break;
            }
        } else if (!strcmp(w, "EXPOSE") || !strcmp(w, "ENTRYPOINT") || !strcmp(w, "CMD") || !strcmp(w, "HEALTHCHECK") || !strcmp(w, "WORKDIR") || !strcmp(w, "USER") || !strcmp(w, "VOLUME")) {
            c2_add(c, CK_CONST, stage, fs, lend, !strcmp(w, "EXPOSE") ? s + a : s + fs, !strcmp(w, "EXPOSE") ? (lend - a < 60 ? lend - a : 60) : wl, fs, lend);
        } else if (!strcmp(w, "COPY") || !strcmp(w, "ADD")) {
            for (size_t q = a; q + 7 < lend; q++) if (!strncasecmp(s + q, "--from=", 7)) { size_t z = q + 7, ze = z; while (ze < lend && !isspace((unsigned char)s[ze])) ze++; if (ze > z) cp_ref(c, stage, CR_USE, s + z, ze - z, z); break; }
        }
        if (!strcmp(w, "RUN") || !strcmp(w, "COPY") || !strcmp(w, "ADD") || !strcmp(w, "CMD")) {                     /* heredoc <<EOF */
            for (size_t q = a; q + 2 < lend; q++) if (s[q] == '<' && s[q + 1] == '<') { size_t z = q + 2; if (z < lend && s[z] == '-') z++; if (z < lend && (s[z] == '"' || s[z] == '\'')) z++; size_t ze = z; while (ze < lend && (c2_wc((unsigned char)s[ze]))) ze++; if (ze > z && ze - z < 48) { char tag[48]; memcpy(tag, s + z, ze - z); tag[ze - z] = 0; while (ln <= c->nln) { size_t l3 = c2_lb(c, ln), e3 = c2_le(c, ln); size_t f3 = c2_skipws(s, e3, l3); ln++; if (e3 - f3 >= ze - z && !strncmp(s + f3, tag, ze - z)) break; } } break; }
        }
        if (stage >= 0 && ln - 1 >= l0) cp_setend(c, stage, c2_le(c, ln - 1));
    }
    if (stage >= 0) { size_t e = c->n; while (e > c->f->sym[stage].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, stage, e); }
}

/* ---------- Makefile ---------- */
static void c2_make(CP *c) {
    const char *s = c->t; size_t n = c->n; int cnt = 0; (void)n;
    int ln = 1, indefine = -1;
    while (ln <= c->nln && cnt < 3000) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln);
        if (indefine >= 0) { size_t fs = c2_skipws(s, le, ls); if (c2_wordeq(s, le, fs, "endef")) { cp_setend(c, indefine, le); indefine = -1; } ln++; continue; }
        if (ls < le && s[ls] == '\t') { ln++; continue; }
        size_t fs = c2_skipws(s, le, ls);
        if (fs >= le || s[fs] == '#') { ln++; continue; }
        int l0 = ln; size_t lend = le;
        while (lend > fs && s[lend - 1] == '\\' && ln < c->nln) { ln++; lend = c2_le(c, ln); }
        ln++;
        if (c2_wordeq(s, lend, fs, "define")) { size_t q = c2_skipws(s, lend, fs + 6), qe = q; while (qe < lend && !isspace((unsigned char)s[qe]) && s[qe] != ':' && s[qe] != '=') qe++; if (qe > q) indefine = c2_add(c, CK_MACRO, -1, fs, lend, s + q, qe - q, fs, lend); continue; }
        if (c2_wordeq(s, lend, fs, "include") || (s[fs] == '-' && c2_wordeq(s, lend, fs + 1, "include")) || c2_wordeq(s, lend, fs, "sinclude")) {
            size_t q = c2_skipws(s, lend, c2_word(s, lend, fs[s] == '-' ? fs + 1 : fs, NULL));
            while (q < lend) { size_t qe = q; while (qe < lend && !isspace((unsigned char)s[qe])) qe++; if (qe > q) cp_ref(c, -1, CR_IMPORT, s + q, qe - q, q); q = c2_skipws(s, lend, qe); }
            continue;
        }
        if (c2_wordeq(s, lend, fs, "ifeq") || c2_wordeq(s, lend, fs, "ifneq") || c2_wordeq(s, lend, fs, "ifdef") || c2_wordeq(s, lend, fs, "ifndef") || c2_wordeq(s, lend, fs, "else") || c2_wordeq(s, lend, fs, "endif") || c2_wordeq(s, lend, fs, "export") || c2_wordeq(s, lend, fs, "unexport") || c2_wordeq(s, lend, fs, "vpath") || c2_wordeq(s, lend, fs, "override") || c2_wordeq(s, lend, fs, "private")) {
            if (!(c2_wordeq(s, lend, fs, "export") || c2_wordeq(s, lend, fs, "override") || c2_wordeq(s, lend, fs, "private"))) continue;
            fs = c2_skipws(s, lend, c2_word(s, lend, fs, NULL));
        }
        /* atribuicao ou regra: primeiro ':' ou '=' fora de $( ) */
        size_t q = fs; int par = 0; size_t op = (size_t)-1; int kind = 0;
        for (; q < lend; q++) { char ch = s[q]; if (ch == '$' && q + 1 < lend && (s[q + 1] == '(' || s[q + 1] == '{')) { par++; q++; continue; } if ((ch == ')' || ch == '}') && par > 0) { par--; continue; } if (par > 0) continue;
            if (ch == '=') { op = q; kind = 1; break; } if (ch == ':') { if (q + 1 < lend && s[q + 1] == '=') { op = q; kind = 1; break; } if (q + 2 < lend && s[q + 1] == ':' && s[q + 2] == '=') { op = q; kind = 1; break; } op = q; kind = 2; break; }
            if (ch == '?' || ch == '+' || ch == '!') { if (q + 1 < lend && s[q + 1] == '=') { op = q; kind = 1; break; } } }
        if (kind == 1) {
            size_t ke = op; while (ke > fs && (isspace((unsigned char)s[ke - 1]) || strchr("?+!:", s[ke - 1]))) ke--;
            if (ke > fs && ke - fs < 100) { c2_add(c, CK_VAR, -1, fs, lend, s + fs, ke - fs, fs, lend < fs + 160 ? lend : fs + 160); cnt++; }
        } else if (kind == 2 && op > fs) {
            size_t after = op + 1; if (after < lend && s[after] == ':') after++;
            size_t z = c2_skipws(s, lend, after);
            /* "alvo: VAR = valor" (variavel especifica de alvo) */
            { size_t t2 = z; while (t2 < lend && c2_wc((unsigned char)s[t2])) t2++; size_t t3 = c2_skipws(s, lend, t2); if (t2 > z && t3 < lend && (s[t3] == '=' || (strchr("?+:", s[t3]) && t3 + 1 < lend && s[t3 + 1] == '='))) continue; }
            /* recipe */
            int last = l0; int k2 = ln;
            if (z < lend && s[z] == ';') { }
            while (k2 <= c->nln) { size_t a2 = c2_lb(c, k2), e2 = c2_le(c, k2); if (a2 < e2 && s[a2] == '\t') { last = k2; k2++; } else { size_t f2 = c2_skipws(s, e2, a2); if (f2 >= e2 || s[f2] == '#') { int look = k2 + 1; while (look <= c->nln) { size_t a3 = c2_lb(c, look), e3 = c2_le(c, look); size_t f3 = c2_skipws(s, e3, a3); if (f3 < e3 && s[f3] != '#') break; look++; } if (look <= c->nln && c2_lb(c, look) < c2_le(c, look) && s[c2_lb(c, look)] == '\t') { k2 = look; continue; } } break; } }
            size_t rend = c2_le(c, last > ln - 1 ? last : ln - 1);
            if (last >= ln) ln = last + 1;
            int firstsym = -1; size_t tp = fs;
            while (tp < op) {
                tp = c2_skipws(s, op, tp); size_t te = tp; while (te < op && !isspace((unsigned char)s[te])) te++;
                if (te > tp && te - tp < 100 && !(s[tp] == '.' && isupper((unsigned char)s[tp + 1 < te ? tp + 1 : tp]))) { int si = c2_add(c, CK_FUNC, -1, fs, rend, s + tp, te - tp, fs, lend); if (si >= 0) { cnt++; if (firstsym < 0) firstsym = si;
                    size_t d = z; while (d < lend) { d = c2_skipws(s, lend, d); size_t de = d; while (de < lend && !isspace((unsigned char)s[de]) && s[de] != '|' && s[de] != ';') de++; if (s[d] == '|') { d++; continue; } if (s[d] == ';') break; if (de > d && de - d < 100 && s[d] != '$' && s[d] != '#') cp_ref(c, si, CR_CALL, s + d, de - d, d); d = de > d ? de : d + 1; } } }
                tp = te > tp ? te : tp + 1;
            }
        }
    }
    if (indefine >= 0) cp_setend(c, indefine, c->n);
}

/* ---------- CMake ---------- */
typedef struct { size_t a, e; } C2Cmd;
static void c2_cmake(CP *c) {
    const char *s = c->c, *t = c->t; size_t n = c->n, p = 0; CoderFile *f = c->f; int open_def[32]; int nd = 0;
    C2Cmd *cmds = NULL; int nc = 0, cap = 0; int cnt = 0;
    while (p < n && cnt < 6000) {
        while (p < n && !(isalpha((unsigned char)s[p]) || s[p] == '_')) { if (s[p] == '"') { p++; while (p < n && s[p] != '"') p++; } p++; }
        if (p >= n) break;
        size_t ne = p; while (ne < n && (c2_wc((unsigned char)s[ne]))) ne++;
        size_t q = c2_skipws(s, n, ne); while (q < n && s[q] == '\n') q = c2_skipws(s, n, q + 1);
        if (q >= n || s[q] != '(' || (p > 0 && !(s[p - 1] == '\n' || s[p - 1] == ' ' || s[p - 1] == '\t' || s[p - 1] == '\r' || s[p - 1] == '(' || s[p - 1] == ')'))) { p = ne; continue; }
        size_t ce = c2_pm(s, n, q); char cmd[40]; size_t cl = ne - p; if (cl >= sizeof cmd) { p = ce; continue; }
        for (size_t k = 0; k < cl; k++) cmd[k] = (char)tolower((unsigned char)s[p + k]); cmd[cl] = 0;
        size_t a1 = c2_lskipws(s, n, q + 1), a1e = a1; while (a1e < ce && !isspace((unsigned char)s[a1e]) && s[a1e] != ')' && s[a1e] != '"') a1e++;
        if (nc >= cap) { cap = cap * 2 + 64; cmds = (C2Cmd *)realloc(cmds, (size_t)cap * sizeof(C2Cmd)); } cmds[nc].a = p; cmds[nc].e = ne; nc++;
        int kind = -1;
        if (!strcmp(cmd, "project")) kind = CK_NAMESPACE;
        else if (!strcmp(cmd, "add_executable") || !strcmp(cmd, "add_library") || !strcmp(cmd, "add_custom_target") || !strcmp(cmd, "add_test")) kind = CK_CLASS;
        else if (!strcmp(cmd, "set") || !strcmp(cmd, "option") || !strcmp(cmd, "set_property") * 0 + 1 * 0) kind = !strcmp(cmd, "set") || !strcmp(cmd, "option") ? CK_VAR : -1;
        if (!strcmp(cmd, "function") || !strcmp(cmd, "macro")) {
            if (a1e > a1 && a1e - a1 < 100) { int si = c2_add(c, !strcmp(cmd, "function") ? CK_FUNC : CK_MACRO, -1, p, ce, t + a1, a1e - a1, p, ce); if (si >= 0 && nd < 32) open_def[nd++] = si; cnt++; }
        } else if (!strcmp(cmd, "endfunction") || !strcmp(cmd, "endmacro")) { if (nd > 0) { int si = open_def[--nd]; cp_setend(c, si, ce); } }
        else if (kind >= 0 && a1e > a1 && a1e - a1 < 100 && s[a1] != '$') { if (!(kind == CK_VAR && strncmp(s + a1, "CMAKE_", 6) == 0 && 0)) { c2_add(c, kind, nd > 0 ? open_def[nd - 1] : -1, p, ce, t + a1, a1e - a1, p, ce); cnt++; } }
        else if (!strcmp(cmd, "add_subdirectory") || !strcmp(cmd, "include") || !strcmp(cmd, "find_package") || !strcmp(cmd, "find_library") * 0 || !strcmp(cmd, "fetchcontent_declare") || !strcmp(cmd, "find_dependency") || !strcmp(cmd, "include_guard") * 0) {
            if (a1e > a1 && s[a1] != '$') cp_ref(c, nd > 0 ? open_def[nd - 1] : -1, CR_IMPORT, t + a1, a1e - a1, a1);
            else if (a1e > a1) cp_ref(c, nd > 0 ? open_def[nd - 1] : -1, CR_IMPORT, t + a1, a1e - a1, a1);
        } else if (!strcmp(cmd, "target_link_libraries") || !strcmp(cmd, "add_dependencies") || !strcmp(cmd, "target_include_directories") * 0) {
            int from = -1; for (int k = 0; k < f->nsym; k++) if (f->sym[k].kind == CK_CLASS && strlen(f->sym[k].name) == a1e - a1 && !memcmp(f->sym[k].name, t + a1, a1e - a1)) { from = k; break; }
            size_t x = a1e;
            while (x < ce - 1) { x = c2_lskipws(s, n, x); size_t xe = x; while (xe < ce - 1 && !isspace((unsigned char)s[xe]) && s[xe] != ')' && s[xe] != '"') xe++; if (xe == x) { x++; continue; }
                char kw[16]; size_t kl2 = xe - x; int isk = 0; if (kl2 < sizeof kw) { memcpy(kw, s + x, kl2); kw[kl2] = 0; isk = !strcmp(kw, "PUBLIC") || !strcmp(kw, "PRIVATE") || !strcmp(kw, "INTERFACE") || !strcmp(kw, "debug") || !strcmp(kw, "optimized") || !strcmp(kw, "general") || !strcmp(kw, "LINK_PUBLIC") || !strcmp(kw, "LINK_PRIVATE"); }
                if (!isk && s[x] != '$' && s[x] != '-' && s[x] != '<' && xe - x < 100) cp_ref(c, from, CR_USE, t + x, xe - x, x); x = xe; }
        }
        p = ce > p ? ce : p + 1;
    }
    /* chamadas as funcoes/macros definidas no arquivo */
    for (int k = 0; k < nc; k++) {
        size_t l = cmds[k].e - cmds[k].a; if (l >= 40) continue; char nm[40]; for (size_t z = 0; z < l; z++) nm[z] = (char)tolower((unsigned char)s[cmds[k].a + z]); nm[l] = 0;
        for (int y = 0; y < f->nsym; y++) if ((f->sym[y].kind == CK_FUNC || f->sym[y].kind == CK_MACRO) && strlen(f->sym[y].name) == l) { int eq = 1; for (size_t z = 0; z < l; z++) if (tolower((unsigned char)f->sym[y].name[z]) != nm[z]) { eq = 0; break; } if (eq && cmds[k].a >= f->sym[y].byte1) { int from = -1; for (int w2 = 0; w2 < f->nsym; w2++) if ((f->sym[w2].kind == CK_FUNC || f->sym[w2].kind == CK_MACRO) && cmds[k].a >= f->sym[w2].byte0 && cmds[k].a < f->sym[w2].byte1) from = w2; cp_ref(c, from, CR_CALL, f->sym[y].name, l, cmds[k].a); } if (eq && cmds[k].a < f->sym[y].byte0) { cp_ref(c, -1, CR_CALL, f->sym[y].name, l, cmds[k].a); } }
        if (k > 3000) break;
    }
    free(cmds);
}

/* ---------- .properties / .ini / .env / .gitignore ---------- */
static void c2_props(CP *c) {
    const char *s = c->t; int cur = -1, cnt = 0;
    const char *base = c->path ? c->path : ""; { const char *b2 = base; for (const char *q = base; *q; q++) if (*q == '/' || *q == '\\') b2 = q + 1; base = b2; }
    int ign = c2_ipre(base, ".gitignore") || c2_ipre(base, ".dockerignore") || c2_ipre(base, ".npmignore") || c2_ipre(base, ".gitattributes");
    int ln = 1;
    while (ln <= c->nln && cnt < 400) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln), fs = c2_skipws(s, le, ls); int l0 = ln; (void)l0;
        ln++;
        if (fs >= le || s[fs] == '#' || s[fs] == ';' || (s[fs] == '!' && !ign)) continue;
        size_t lend = le; while (lend > fs && s[lend - 1] == '\\' && ln <= c->nln) { lend = c2_le(c, ln); ln++; }
        if (s[fs] == '[' && !ign) { size_t b = fs + 1; while (b < lend && s[b] != ']') b++; if (b > fs + 1) { if (cur >= 0) { size_t e = ls; while (e > c->f->sym[cur].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, cur, e); } cur = c2_add(c, CK_SECTION, -1, ls, lend, s + fs + 1, b - fs - 1, ls, lend); cnt++; } continue; }
        if (ign) { size_t e = lend; while (e > fs && isspace((unsigned char)s[e - 1])) e--; if (e > fs && e - fs < 150) { c2_add(c, CK_CONST, -1, fs, e, s + fs, e - fs, fs, e); cnt++; } continue; }
        size_t k = fs; if (c2_wordeq(s, lend, fs, "export") ) k = c2_skipws(s, lend, fs + 6);
        size_t ks = k; while (k < lend && s[k] != '=' && s[k] != ':' && !isspace((unsigned char)s[k])) k++;
        size_t ke = k; k = c2_skipws(s, lend, k);
        if (ke > ks && ke - ks < 150 && (k >= lend || s[k] == '=' || s[k] == ':' || k > ke)) { c2_add(c, CK_CONST, cur, ls, lend, s + ks, ke - ks, ls, lend < ls + 160 ? lend : ls + 160); cnt++; if (cur >= 0) cp_setend(c, cur, lend); }
    }
    if (cur >= 0) { size_t e = c->n; while (e > c->f->sym[cur].byte0 && isspace((unsigned char)s[e - 1])) e--; cp_setend(c, cur, e); }
}

/* ---------- Ruby, Lua, Elixir, Julia: blocos terminados por `end` ---------- */
typedef struct { int sym, priv; } C2Fr;
typedef struct { int active, kind, vis; size_t na, nb, kw, sa, sb; int oneline; } C2Pend;
/* texto entre aspas do texto sem comentarios, a partir de q (aspa de abertura) */
static void c2_strref(CP *c, size_t q, size_t le, int from) {
    const char *t = c->t; if (q >= le || (t[q] != '"' && t[q] != '\'')) return;
    size_t e = q + 1; while (e < le && t[e] != t[q]) e++;
    if (e > q + 1 && e - q < 200) cp_ref(c, from, CR_IMPORT, t + q + 1, e - q - 1, q + 1);
}
static size_t c2_cpath(const char *s, size_t le, size_t p) { while (p < le && (c2_wc((unsigned char)s[p]) || s[p] == ':' || s[p] == '.')) p++; return p; }
static int c2_curparent(const C2Fr *st, int sp) { for (int k = sp - 1; k >= 0; k--) if (st[k].sym >= 0) return st[k].sym; return -1; }
static void c2_endlang(CP *c) {
    const int L = c->lang; const char *s = c->c, *t = c->t; CoderFile *f = c->f;
    C2Fr st[256]; int sp = 0, over = 0, bd = 0;
    for (int ln = 1; ln <= c->nln; ln++) {
        size_t ls = c2_lb(c, ln), le = c2_le(c, ln), fs = c2_skipws(s, le, ls);
        if (fs >= le) continue;
        C2Pend pd; memset(&pd, 0, sizeof pd);
        int par = c2_curparent(st, sp); const int pk = par >= 0 ? f->sym[par].kind : -1;
        const int incls = pk == CK_CLASS || pk == CK_NAMESPACE || pk == CK_STRUCT || pk == CK_IFACE;
        size_t w0 = fs, w0e = c2_word(s, le, fs, "?!");
        int ownsym = -1;
        /* ---------- reconhecimento da declaracao da linha ---------- */
        if (L == CL_RB) {
            size_t p = fs; int vis = 0;
            if (c2_wordeq(s, le, p, "private") || c2_wordeq(s, le, p, "public") || c2_wordeq(s, le, p, "protected") || c2_wordeq(s, le, p, "module_function")) {
                size_t q = c2_skipws(s, le, w0e);
                if (q >= le && sp > 0) { int k = sp - 1; while (k >= 0 && st[k].sym < 0) k--; if (k >= 0) st[k].priv = c2_wordeq(s, le, p, "private") ? 1 : c2_wordeq(s, le, p, "protected") ? 2 : 0; }
                else if (q < le && c2_wordeq(s, le, q, "def")) { vis = c2_wordeq(s, le, p, "private") ? CV_PRIVATE : c2_wordeq(s, le, p, "protected") ? CV_PROTECTED : 0; p = q; }
            }
            size_t we = c2_word(s, le, p, "?!");
            if (we > p) {
                if (c2_wordeq(s, le, p, "def")) {
                    size_t q = c2_skipws(s, le, we), na = q, nb;
                    if (c2_wc((unsigned char)s[q])) { nb = c2_word(s, le, q, "?!"); if (nb < le && s[nb] == '.') { na = nb + 1; nb = c2_word(s, le, na, "?!"); } if (nb < le && s[nb] == '=' && nb + 1 < le && (s[nb + 1] == '(' || s[nb + 1] == ' ') && nb + 1 < le && s[nb + 1] == '(') nb++; }
                    else { nb = q; while (nb < le && !isspace((unsigned char)s[nb]) && s[nb] != '(' && s[nb] != ';') nb++; }
                    if (nb > na && nb - na < 100) {
                        pd.active = 1; pd.kind = incls ? CK_METHOD : CK_FUNC; pd.na = na; pd.nb = nb; pd.kw = p; pd.sa = ls + (fs - ls); pd.sb = le; pd.vis = vis;
                        if (nb - na == 10 && !memcmp(s + na, "initialize", 10)) pd.kind = CK_CTOR;
                        size_t z = nb; if (z < le && s[z] == '(') z = c2_pm(s, c->n, z); z = c2_skipws(s, le, z); if (z < le && s[z] == '=' && (z + 1 >= le || (s[z + 1] != '=' && s[z + 1] != '~' && s[z + 1] != '>'))) pd.oneline = 1;
                    }
                } else if (c2_wordeq(s, le, p, "class") || c2_wordeq(s, le, p, "module")) {
                    size_t q = c2_skipws(s, le, we);
                    if (q < le && isupper((unsigned char)s[q])) { size_t nb = c2_cpath(s, le, q); pd.active = 1; pd.kind = c2_wordeq(s, le, p, "class") ? CK_CLASS : CK_NAMESPACE; pd.na = q; pd.nb = nb; pd.kw = p; pd.sa = fs; pd.sb = le;
                        size_t z = c2_skipws(s, le, nb); if (z < le && s[z] == '<') { z = c2_skipws(s, le, z + 1); size_t ze = c2_cpath(s, le, z); if (ze > z) { pd.oneline = 0; ownsym = -2; pd.vis = (int)z; pd.sa = fs; pd.sb = ze; } } }
                } else if (c2_wordeq(s, le, p, "attr_reader") || c2_wordeq(s, le, p, "attr_writer") || c2_wordeq(s, le, p, "attr_accessor")) {
                    size_t q = we;
                    while (q < le) { while (q < le && (s[q] == ' ' || s[q] == ',' || s[q] == '\t')) q++; if (q < le && s[q] == ':') { size_t na = q + 1, nb = c2_word(s, le, na, "?!"); if (nb > na) c2_add(c, CK_PROPERTY, par, ls + (fs - ls), le, t + na, nb - na, fs, le); q = nb > q ? nb : q + 1; } else break; }
                } else if (c2_wordeq(s, le, p, "require") || c2_wordeq(s, le, p, "require_relative") || c2_wordeq(s, le, p, "load")) {
                    size_t q = c2_skipws(s, le, we); if (q < le && s[q] == '(') q = c2_skipws(s, le, q + 1); c2_strref(c, q, le, -1);
                } else if (c2_wordeq(s, le, p, "autoload")) {
                    size_t q = we; while (q < le && s[q] != ',') q++; if (q < le) c2_strref(c, c2_skipws(s, le, q + 1), le, -1);
                } else if (par >= 0 && (c2_wordeq(s, le, p, "include") || c2_wordeq(s, le, p, "extend") || c2_wordeq(s, le, p, "prepend"))) {
                    size_t q = c2_skipws(s, le, we); if (q < le && isupper((unsigned char)s[q])) { size_t qe = c2_cpath(s, le, q); cp_ref(c, par, CR_IMPLEMENTS, t + q, qe - q, q); }
                } else if (isupper((unsigned char)s[p])) {
                    size_t q = c2_skipws(s, le, we);
                    if (we > p && q < le && s[q] == '=' && (q + 1 >= le || (s[q + 1] != '=' && s[q + 1] != '~' && s[q + 1] != '>')) && (sp == 0 || incls)) { int cur = c2_add(c, CK_CONST, par, ls + (fs - ls), le, t + p, we - p, fs, le); (void)cur; }
                }
            }
        } else if (L == CL_LUA) {
            size_t p = fs; if (c2_wordeq(s, le, p, "local")) p = c2_skipws(s, le, c2_word(s, le, p, NULL));
            if (c2_wordeq(s, le, p, "function")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)), na = q, nb = c2_word(s, le, q, ".:");
                if (nb > na) { size_t fin = nb; while (fin > na && s[fin - 1] != '.' && s[fin - 1] != ':') fin--; size_t nm0 = fin > na ? fin : na; int meth = fin > na;
                    pd.active = 1; pd.kind = meth ? CK_METHOD : CK_FUNC; pd.na = nm0; pd.nb = nb; pd.kw = p; pd.sa = fs; pd.sb = le; }
            } else if (c2_wc((unsigned char)s[p])) {
                size_t nb = c2_word(s, le, p, ".:"), q = c2_skipws(s, le, nb);
                if (nb > p && q < le && s[q] == '=' && q + 1 < le && s[q + 1] != '=') { size_t z = c2_skipws(s, le, q + 1); if (c2_wordeq(s, le, z, "function")) { size_t fin = nb; while (fin > p && s[fin - 1] != '.' && s[fin - 1] != ':') fin--; pd.active = 1; pd.kind = fin > p ? CK_METHOD : CK_FUNC; pd.na = fin > p ? fin : p; pd.nb = nb; pd.kw = z; pd.sa = fs; pd.sb = le; } }
                if (c2_wordeq(s, le, fs, "local") || 1) {
                    for (size_t z = fs; z + 7 < le; z++) if (!strncmp(s + z, "require", 7) && (z == 0 || !c2_wc((unsigned char)s[z - 1]))) { size_t y = c2_skipws(s, le, z + 7); if (y < le && s[y] == '(') y = c2_skipws(s, le, y + 1); if (y < le && (s[y] == '"' || s[y] == '\'')) c2_strref(c, y, le, -1); break; }
                }
            }
        } else if (L == CL_EX) {
            if (c2_wordeq(s, le, fs, "defmodule") || c2_wordeq(s, le, fs, "defprotocol") || c2_wordeq(s, le, fs, "defimpl")) {
                size_t q = c2_skipws(s, le, w0e), nb = c2_cpath(s, le, q);
                if (nb > q) { pd.active = 1; pd.kind = c2_wordeq(s, le, fs, "defprotocol") ? CK_IFACE : CK_CLASS; pd.na = q; pd.nb = nb; pd.kw = (size_t)-1; pd.sa = fs; pd.sb = le;
                    if (c2_wordeq(s, le, fs, "defimpl")) { ownsym = -3; } }
            } else if (c2_wordeq(s, le, fs, "def") || c2_wordeq(s, le, fs, "defp") || c2_wordeq(s, le, fs, "defmacro") || c2_wordeq(s, le, fs, "defmacrop") || c2_wordeq(s, le, fs, "defguard") || c2_wordeq(s, le, fs, "defguardp") || c2_wordeq(s, le, fs, "defdelegate")) {
                size_t q = c2_skipws(s, le, w0e), nb = c2_word(s, le, q, "?!");
                if (nb > q && nb - q < 100 && !(s[q] == '(')) {
                    pd.active = 1; pd.kind = incls ? CK_METHOD : CK_FUNC; pd.na = q; pd.nb = nb; pd.kw = (size_t)-1; pd.sa = fs; pd.sb = le;
                    if (w0e - fs >= 4 && (s[w0e - 1] == 'p' || (s[w0e - 1] == 'e' && 0))) pd.vis = CV_PRIVATE;
                    if (c2_wordeq(s, le, fs, "defmacrop") || c2_wordeq(s, le, fs, "defguardp")) pd.vis = CV_PRIVATE;
                    pd.kind = (c2_wordeq(s, le, fs, "defmacro") || c2_wordeq(s, le, fs, "defmacrop")) ? CK_MACRO : pd.kind;
                    int hasdo = 0; for (size_t z = nb; z + 1 < le; z++) if (s[z] == 'd' && s[z + 1] == 'o' && (z + 2 >= le || !c2_wc((unsigned char)s[z + 2])) && (s[z - 1] == ' ' || s[z - 1] == ')')) { hasdo = (z + 2 < le && s[z + 2] == ':') ? 0 : 1; if (hasdo) break; }
                    if (!hasdo) pd.oneline = 1;
                    for (size_t z = nb; z + 3 < le; z++) if (s[z] == 'd' && s[z + 1] == 'o' && s[z + 2] == ':') { pd.oneline = 1; break; }
                }
            } else if (c2_wordeq(s, le, fs, "use") || c2_wordeq(s, le, fs, "import") || c2_wordeq(s, le, fs, "alias") || c2_wordeq(s, le, fs, "require")) {
                size_t q = c2_skipws(s, le, w0e), qe = c2_cpath(s, le, q); if (qe > q && isupper((unsigned char)s[q])) cp_ref(c, par, CR_IMPORT, t + q, qe - q, q);
            } else if (s[fs] == '@' && !strncmp(s + fs, "@behaviour", 10)) { size_t q = c2_skipws(s, le, fs + 10), qe = c2_cpath(s, le, q); if (qe > q) cp_ref(c, par, CR_IMPLEMENTS, t + q, qe - q, q); }
            else if (c2_wordeq(s, le, fs, "test") || c2_wordeq(s, le, fs, "describe")) {
                size_t q = c2_skipws(s, le, w0e); if (q < le && s[q] == '"') { size_t e2 = q + 1; while (e2 < le && t[e2] != '"') e2++; if (e2 > q + 1 && e2 - q < 150) { pd.active = 1; pd.kind = c2_wordeq(s, le, fs, "test") ? CK_FUNC : CK_SECTION; pd.na = q + 1; pd.nb = e2; pd.kw = (size_t)-1; pd.sa = fs; pd.sb = le; pd.vis = -1; } }
            }
        } else if (L == CL_JL) {
            size_t p = fs; if (c2_wordeq(s, le, p, "mutable")) p = c2_skipws(s, le, c2_word(s, le, p, NULL));
            if (c2_wordeq(s, le, p, "function") || c2_wordeq(s, le, p, "macro")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)), nb = c2_word(s, le, q, ".!");
                if (nb > q) { size_t fin = nb; while (fin > q && s[fin - 1] != '.') fin--; size_t na = fin > q ? fin : q; pd.active = 1; pd.kind = c2_wordeq(s, le, p, "macro") ? CK_MACRO : (incls ? CK_METHOD : CK_FUNC); pd.na = na; pd.nb = nb; pd.kw = p; pd.sa = fs; pd.sb = le; }
            } else if (c2_wordeq(s, le, p, "struct") || c2_wordeq(s, le, p, "module") || c2_wordeq(s, le, p, "baremodule")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)), nb = c2_word(s, le, q, "!");
                if (nb > q) { pd.active = 1; pd.kind = c2_wordeq(s, le, p, "struct") ? CK_STRUCT : CK_NAMESPACE; pd.na = q; pd.nb = nb; pd.kw = p; pd.sa = fs; pd.sb = le; }
            } else if (c2_wordeq(s, le, p, "abstract")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)); if (c2_wordeq(s, le, q, "type")) { q = c2_skipws(s, le, c2_word(s, le, q, NULL)); size_t nb = c2_word(s, le, q, NULL); if (nb > q) { pd.active = 1; pd.kind = CK_IFACE; pd.na = q; pd.nb = nb; pd.kw = c2_skipws(s, le, c2_word(s, le, p, NULL)); pd.oneline = 0; pd.sa = fs; pd.sb = le; } }
            } else if (c2_wordeq(s, le, p, "const")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)), nb = c2_word(s, le, q, NULL); if (nb > q) { pd.active = 1; pd.kind = CK_CONST; pd.na = q; pd.nb = nb; pd.kw = 0; pd.sa = fs; pd.sb = le; pd.oneline = 1; }
            } else if (c2_wordeq(s, le, p, "using") || c2_wordeq(s, le, p, "import")) {
                size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL));
                while (q < le) { size_t qe = c2_word(s, le, q, "."); if (qe == q) break; cp_ref(c, par, CR_IMPORT, t + q, qe - q, q); q = c2_skipws(s, le, qe); if (q < le && s[q] == ',') q = c2_skipws(s, le, q + 1); else break; }
            } else if (c2_wordeq(s, le, p, "include")) { size_t q = c2_skipws(s, le, c2_word(s, le, p, NULL)); if (q < le && s[q] == '(') c2_strref(c, c2_skipws(s, le, q + 1), le, -1); }
            else if (c2_wc((unsigned char)s[p]) && (sp == 0 || incls)) {
                size_t nb = c2_word(s, le, p, "!"); if (nb < le && s[nb] == '(') { size_t pe = c2_pm(s, le, nb), q = c2_skipws(s, le, pe); if (q < le && s[q] == '=' && (q + 1 >= le || s[q + 1] != '=')) { char wb[16]; size_t wl = nb - p; if (wl < 16) { memcpy(wb, s + p, wl); wb[wl] = 0; if (strcmp(wb, "if") && strcmp(wb, "for") && strcmp(wb, "while") && strcmp(wb, "elseif")) { pd.active = 1; pd.kind = incls ? CK_METHOD : CK_FUNC; pd.na = p; pd.nb = nb; pd.kw = 0; pd.sa = fs; pd.sb = le; pd.oneline = 1; } } } }
            }
        }
        /* ---------- varredura de palavras: abre/fecha blocos ---------- */
        int first = 1, loopline = 0, madesym = 0;
        size_t p = fs;
        if (L == CL_RB || L == CL_EX || L == CL_JL) { size_t fw = fs; if (c2_wordeq(s, le, fw, "private") || c2_wordeq(s, le, fw, "public")) fw = c2_skipws(s, le, c2_word(s, le, fw, NULL)); if (c2_wordeq(s, le, fw, "while") || c2_wordeq(s, le, fw, "until") || c2_wordeq(s, le, fw, "for")) loopline = 1; }
        int symdone = 0;
        for (; p < le; ) {
            unsigned char ch = (unsigned char)s[p];
            if (ch == '(' || ch == '[' || ch == '{') { if (L == CL_JL) bd++; p++; continue; }
            if (ch == ')' || ch == ']' || ch == '}') { if (L == CL_JL && bd > 0) bd--; p++; continue; }
            if (!(isalpha(ch) || ch == '_' || (ch & 0x80))) { p++; if (!(ch == ' ' || ch == '\t')) first = 0; continue; }
            size_t we = c2_word(s, le, p, NULL);
            char wb[16]; size_t wl = we - p; int iskw = 0;
            if (wl < sizeof wb) { memcpy(wb, s + p, wl); wb[wl] = 0; iskw = 1; }
            char pc = p > 0 ? s[p - 1] : 0; size_t nx = we;
            int skip = (pc == '.' || pc == ':' || pc == '@' || pc == '$' || pc == '&') && !(pc == ':' && p > 1 && s[p - 2] == ':' && 0);
            if (we < le && (s[we] == ':' && (we + 1 >= le || s[we + 1] != ':')) && L != CL_LUA) skip = 1;     /* chave de hash / do: */
            if (we < le && (s[we] == '?' || s[we] == '!') && L == CL_RB && we + 1 < le && !isspace((unsigned char)s[we + 1]) && s[we + 1] != '(' && 0) skip = 1;
            if (L == CL_LUA && pc == ':' ) skip = 1;
            if (L == CL_JL && bd > 0) skip = 1;
            int opener = 0, closer = 0;
            if (iskw && !skip) {
                if (L == CL_RB) {
                    if (!strcmp(wb, "def") || !strcmp(wb, "class") || !strcmp(wb, "module") || !strcmp(wb, "begin") || !strcmp(wb, "case")) opener = 1;
                    else if (!strcmp(wb, "if") || !strcmp(wb, "unless") || !strcmp(wb, "while") || !strcmp(wb, "until")) { size_t b = p; while (b > fs && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--; opener = first || (b > fs && strchr("=(,|&", s[b - 1]) != NULL) || (b >= fs + 7 && !strncmp(s + b - 7, "private", 7)); }
                    else if (!strcmp(wb, "for")) opener = first;
                    else if (!strcmp(wb, "do")) { if (loopline) loopline = 0; else opener = 1; }
                    else if (!strcmp(wb, "end")) closer = 1;
                    if (opener && !strcmp(wb, "def") && pd.active && pd.oneline && p == pd.kw) opener = 0;
                } else if (L == CL_LUA) {
                    if (!strcmp(wb, "function") || !strcmp(wb, "if") || !strcmp(wb, "do") || !strcmp(wb, "repeat")) opener = 1;
                    else if (!strcmp(wb, "end") || !strcmp(wb, "until")) closer = 1;
                } else if (L == CL_EX) {
                    if (!strcmp(wb, "do") || !strcmp(wb, "fn")) opener = !(we < le && s[we] == ':');
                    else if (!strcmp(wb, "end")) closer = 1;
                    if (opener && !strcmp(wb, "do") && pd.active && pd.oneline) opener = 0;
                } else if (L == CL_JL) {
                    if (!strcmp(wb, "function") || !strcmp(wb, "macro") || !strcmp(wb, "if") || !strcmp(wb, "for") || !strcmp(wb, "while") || !strcmp(wb, "begin") || !strcmp(wb, "let") || !strcmp(wb, "struct") || !strcmp(wb, "module") || !strcmp(wb, "baremodule") || !strcmp(wb, "try") || !strcmp(wb, "quote") || !strcmp(wb, "do")) opener = 1;
                    else if (!strcmp(wb, "type")) { size_t b = p; while (b > fs && s[b - 1] == ' ') b--; opener = (b >= fs + 8 && !strncmp(s + b - 8, "abstract", 8)) || (b >= fs + 9 && !strncmp(s + b - 9, "primitive", 9)); }
                    else if (!strcmp(wb, "end")) closer = 1;
                    if (opener && pd.active && pd.oneline && p == pd.kw) opener = 0;
                }
            }
            if (opener) {
                int sym = -1;
                if (pd.active && !symdone && !pd.oneline) { if (pd.kw == (size_t)-1) { if (!strcmp(wb, "do")) sym = -9; } else if (p == pd.kw) sym = -9; }
                if (sym == -9) {
                    const char *nm = t + pd.na; size_t nl = pd.nb - pd.na;
                    int vis = pd.vis; int sk = pd.kind;
                    int si = c2_add(c, sk, par, fs, le, nm, nl, fs, pd.sb < le ? pd.sb : le);
                    if (si >= 0) {
                        if (L == CL_RB && pd.vis >= 0 && !(vis)) { int k = sp - 1; while (k >= 0 && st[k].sym < 0) k--; if (k >= 0 && st[k].priv && (sk == CK_METHOD || sk == CK_FUNC)) vis = st[k].priv == 1 ? CV_PRIVATE : CV_PROTECTED; }
                        if (vis > 0 && L != CL_RB) f->sym[si].vis = vis; else if (vis > 0 && vis <= CV_PROTECTED) f->sym[si].vis = vis;
                        if (L == CL_RB && ownsym == -2 && (sk == CK_CLASS)) { size_t z = (size_t)pd.vis; size_t ze = c2_cpath(s, le, z); cp_ref(c, si, CR_EXTENDS, t + z, ze - z, z); }
                        if (L == CL_EX && ownsym == -3) { size_t q = c2_skipws(s, le, w0e), qe = c2_cpath(s, le, q); cp_ref(c, si, CR_IMPLEMENTS, t + q, qe - q, q); }
                        if (L == CL_EX && pd.vis == -1) f->sym[si].flags |= CSF_TEST;
                        if (pd.vis == -1) f->sym[si].vis = CV_UNKNOWN;
                    }
                    sym = si; symdone = 1; madesym = 1;
                } else sym = -1;
                if (sp < 255) { st[sp].sym = sym; st[sp].priv = 0; sp++; } else over++;
            } else if (closer) {
                if (over > 0) over--;
                else if (sp > 0) { sp--; if (st[sp].sym >= 0) cp_setend(c, st[sp].sym, le); }
            }
            first = 0; p = we > p ? we : p + 1; (void)nx;
        }
        if (!madesym && pd.active) {                              /* declaracao de uma linha so (def x = ..., def a, do: ..., const X = ..., f(x) = ...) */
            int vis = pd.vis;
            int si = c2_add(c, pd.kind, par, fs, le, t + pd.na, pd.nb - pd.na, fs, le);
            if (si >= 0) {
                if (L == CL_RB && !vis) { int k = sp - 1; while (k >= 0 && st[k].sym < 0) k--; if (k >= 0 && st[k].priv && (pd.kind == CK_METHOD || pd.kind == CK_FUNC)) vis = st[k].priv == 1 ? CV_PRIVATE : CV_PROTECTED; }
                if (vis > 0 && vis <= CV_PROTECTED) f->sym[si].vis = vis;
                if (pd.vis == -1) { f->sym[si].flags |= CSF_TEST; f->sym[si].vis = CV_UNKNOWN; }
            }
        }
        (void)w0;
    }
    for (int k = 0; k < sp; k++) if (st[k].sym >= 0) cp_setend(c, st[k].sym, c->n);     /* bloco sem `end`: vai ate o fim */
}

/* ---------- chamadas (CALL) das linguagens por linha ---------- */
static const char *const C2_CALLNO[] = {"if", "while", "for", "foreach", "switch", "case", "catch", "return", "function", "def", "defp", "fn", "fun", "func", "proc", "sub", "elsif", "elseif", "unless", "until", "and", "or", "not",
    "when", "with", "in", "do", "end", "begin", "rescue", "ensure", "then", "else", "class", "module", "lambda", "assert", "raise", "throw", "try", "local", "print", "puts", "using", "import", "require", "include", "defmodule",
    "defmacro", "defmacrop", "defdelegate", "defstruct", "defguard", "mutable", "struct", "macro", "iterator", "template", "method", "converter", "spec", "callback", "type", "typep", "opaque", "is", "as", "of", "like", "var", "let",
    "const", "val", "new", "call", "select", "from", "where", "values", "set", "unquote", "super", "yield", "defined", "isa", "ref", "ptr", "cast", "sizeof", "typeof", "alignof", "export", "pub", "extern", "inline", "test", "comptime", NULL};
/* varre o texto mascarado procurando `nome(` e atribui ao simbolo mais interno; so para linguagens com chamadas por parenteses */
static void c2_sweep_calls(CP *c, int sp_ok) {
    CoderFile *f = c->f; const char *s = c->c; size_t n = c->n; int nsym = f->nsym;
    int *stk = (int *)malloc(sizeof(int) * (size_t)(nsym + 2)); int sp = 0, ni = 0;
    const int L = c->lang; (void)sp_ok;
    const int lisp = L == CL_CLJ || L == CL_LISP, bang = L == CL_RB || L == CL_EX || L == CL_JL, dollar = L == CL_PHP || L == CL_PERL || L == CL_R;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (lisp) {
            if (ch != '(') continue;
            size_t h = c2_lskipws(s, n, i + 1), he = c2_ltokend(s, n, h);
            if (he <= h || he - h > 60 || (i > 0 && (s[i - 1] == '\'' || s[i - 1] == '`')) || s[h] == ':' || !(isalpha((unsigned char)s[h]) || strchr("_.*+!-<>=/&%", s[h])) ) continue;
            char nb[64]; size_t nl = he - h; memcpy(nb, s + h, nl); nb[nl] = 0;
            const char *nm = nb; const char *sl = strrchr(nb, '/'); if (sl && sl[1] && sl != nb) { nm = sl + 1; nl = strlen(nm); }
            if (nm[0] == '.' && nl > 1) { nm++; nl--; }
            if (nl < 2 && !strchr("_", nm[0])) { if (!isalpha((unsigned char)nm[0])) continue; }
            if (c2_inl_(nm, C2_LSPECIAL) || !strncmp(nm, "def", 3) && nl > 3 && 0) continue;
            while (sp > 0 && i >= f->sym[stk[sp - 1]].byte1) sp--;
            while (ni < nsym && f->sym[ni].byte0 <= i) { if (i < f->sym[ni].byte1) stk[sp++] = ni; ni++; }
            int cur = sp > 0 ? stk[sp - 1] : -1;
            if (cur >= 0 && f->sym[cur].byte0 == i) continue;       /* o proprio cabecalho (defn ...) */
            if (cur >= 0 && f->sym[cur].kind == CK_NAMESPACE && L == CL_CLJ) continue;
            if (i >= 2 && s[i - 2] == '#' && s[i - 1] == '_') continue;
            if (nm[0] == '-' && nl < 3) continue;
            cp_ref(c, cur, CR_CALL, nm, nl, h);
            continue;
        }
        if (!(isalpha(ch) || ch == '_' || (ch & 0x80)) ) continue;
        if (i > 0 && (c2_wc((unsigned char)s[i - 1]) || (dollar && s[i - 1] == '$') || s[i - 1] == '@' || (s[i - 1] == ':' && L != CL_ERL && L != CL_PERL))) { while (i < n && c2_wc((unsigned char)s[i])) i++; i--; continue; }
        size_t e = i; while (e < n && (c2_wc((unsigned char)s[e]))) e++;
        size_t nend = e; if (bang && e < n && (s[e] == '?' || s[e] == '!')) nend = e + 1;
        size_t q = nend; if (L == CL_FORTRAN || L == CL_ASM) { while (q < n && s[q] == ' ') q++; }
        if (L == CL_FORTRAN) { if (!(i >= 5 && !strncasecmp(s + (i >= 5 ? i - 5 : 0), "call ", 5))) { i = e - 1; continue; } }
        else if (q >= n || s[q] != '(') { i = e - 1; continue; }
        size_t nl = nend - i; char nb[64];
        if (nl >= sizeof nb || nl < 2) { i = e - 1; continue; }
        memcpy(nb, s + i, nl); nb[nl] = 0;
        if (c2_inl_(nb, C2_CALLNO)) { i = e - 1; continue; }
        /* palavra anterior na linha: definicao, nao chamada */
        size_t pw = i; while (pw > 0 && (s[pw - 1] == ' ' || s[pw - 1] == '\t')) pw--;
        if (pw < i && pw > 0) { size_t ws = pw; while (ws > 0 && (c2_wc((unsigned char)s[ws - 1]) || s[ws - 1] == '?' || s[ws - 1] == '!')) ws--; char pb[24]; size_t pl = pw - ws; if (pl && pl < sizeof pb) { memcpy(pb, s + ws, pl); pb[pl] = 0; if (c2_inl_(pb, C2_CALLNO) && strcmp(pb, "not") && strcmp(pb, "and") && strcmp(pb, "or") && strcmp(pb, "return") && strcmp(pb, "in") && strcmp(pb, "if") && strcmp(pb, "unless") && strcmp(pb, "while") && strcmp(pb, "until") && strcmp(pb, "elsif") && strcmp(pb, "when") && strcmp(pb, "yield") && strcmp(pb, "puts") && strcmp(pb, "print") && strcmp(pb, "raise") && strcmp(pb, "throw") && strcmp(pb, "assert") && strcmp(pb, "do") && strcmp(pb, "then") && strcmp(pb, "else") && strcmp(pb, "case") && strcmp(pb, "with") && strcmp(pb, "begin") && strcmp(pb, "rescue") && strcmp(pb, "end") && strcmp(pb, "from") && strcmp(pb, "where") && strcmp(pb, "select") && strcmp(pb, "values") && strcmp(pb, "set") && strcmp(pb, "call") && strcmp(pb, "export") && strcmp(pb, "new") && strcmp(pb, "of") && strcmp(pb, "is") && strcmp(pb, "as")) { i = e - 1; continue; } } }
        while (sp > 0 && i >= f->sym[stk[sp - 1]].byte1) sp--;
        while (ni < nsym && f->sym[ni].byte0 <= i) { if (i < f->sym[ni].byte1) stk[sp++] = ni; ni++; }
        int cur = sp > 0 ? stk[sp - 1] : -1;
        if (cur >= 0) {                                          /* nome do proprio simbolo na 1a linha = definicao (clausula Erlang, def sem palavra) */
            const CoderSym *y = &f->sym[cur]; size_t yl = strlen(y->name);
            if (yl == nl && !memcmp(y->name, nb, nl) && i < y->byte0 + 400 && c2_lineof(c, i) == y->line0 && (L == CL_ERL || L == CL_HS || L == CL_JL || L == CL_NIM || L == CL_ZIG || L == CL_VLANG || L == CL_R || L == CL_LUA || L == CL_OBJC || L == CL_FORTRAN || L == CL_PERL || L == CL_EX || L == CL_RB)) { i = e - 1; continue; }
        }
        cp_ref(c, cur, CR_CALL, nb, nl, i);
        i = e - 1;
    }
    free(stk);
}
static void c2_sweep_line(CP *c) {
    switch (c->lang) {
    case CL_ERL: case CL_EX: case CL_RB: case CL_LUA: case CL_JL: case CL_R: case CL_PERL: case CL_NIM: case CL_VLANG: case CL_ZIG: case CL_OBJC: case CL_FORTRAN: case CL_CLJ: case CL_LISP:
        c2_sweep_calls(c, 0); break;
    default: break;
    }
}

/* ---------- despacho da analise por linha ---------- */
static void c2_parse_line(CP *c) {
    switch (c->lang) {
    case CL_CLJ: case CL_LISP: c2_lisp(c); break;
    case CL_RB: case CL_LUA: case CL_EX: case CL_JL: c2_endlang(c); break;
    case CL_SQL: c2_sql(c); break;
    case CL_JSON: c2_json(c); break;
    case CL_YAML: c2_yaml(c); break;
    case CL_TOML: c2_toml(c); break;
    case CL_DOCKER: c2_docker(c); break;
    case CL_MAKE: c2_make(c); break;
    case CL_CMAKE: c2_cmake(c); break;
    case CL_PROPS: c2_props(c); break;
    /*ENGINES*/
    default: break;
    }
}
/* nome do arquivo / extensao -> linguagem (so as novas; o resto fica com a tabela de parse.c) */
static int c2_vlang_ok(const char *path_unused) { (void)path_unused; return 1; }
static int c2_lang_of(const char *base, const char *ext) {
    static const struct { const char *e; int l; } T[] = {
        {"cs", CL_CS}, {"swift", CL_SWIFT}, {"php", CL_PHP}, {"phtml", CL_PHP}, {"rb", CL_RB}, {"rake", CL_RB}, {"gemspec", CL_RB}, {"ru", CL_RB}, {"lua", CL_LUA}, {"dart", CL_DART},
        {"scala", CL_SCALA}, {"sc", CL_SCALA}, {"sql", CL_SQL}, {"yaml", CL_YAML}, {"yml", CL_YAML}, {"toml", CL_TOML}, {"mk", CL_MAKE}, {"mak", CL_MAKE}, {"cmake", CL_CMAKE},
        {"html", CL_HTML}, {"htm", CL_HTML}, {"xhtml", CL_HTML}, {"css", CL_CSS}, {"scss", CL_CSS}, {"less", CL_CSS}, {"clj", CL_CLJ}, {"cljs", CL_CLJ}, {"cljc", CL_CLJ}, {"edn", CL_CLJ},
        {"lisp", CL_LISP}, {"lsp", CL_LISP}, {"el", CL_LISP}, {"scm", CL_LISP}, {"rkt", CL_LISP}, {"sld", CL_LISP}, {"asd", CL_LISP},
        {"ex", CL_EX}, {"exs", CL_EX}, {"erl", CL_ERL}, {"hrl", CL_ERL}, {"hs", CL_HS}, {"lhs", CL_HS}, {"ml", CL_ML}, {"mli", CL_ML}, {"fs", CL_ML}, {"fsi", CL_ML}, {"fsx", CL_ML},
        {"r", CL_R}, {"jl", CL_JL}, {"pl", CL_PERL}, {"pm", CL_PERL}, {"groovy", CL_GROOVY}, {"gradle", CL_GROOVY}, {"gvy", CL_GROOVY}, {"m", CL_OBJC}, {"mm", CL_OBJC}, {"zig", CL_ZIG},
        {"nim", CL_NIM}, {"nims", CL_NIM}, {"v", CL_VLANG}, {"f90", CL_FORTRAN}, {"f95", CL_FORTRAN}, {"f03", CL_FORTRAN}, {"f08", CL_FORTRAN}, {"f", CL_FORTRAN}, {"for", CL_FORTRAN},
        {"cob", CL_COBOL}, {"cbl", CL_COBOL}, {"cobol", CL_COBOL}, {"cpy", CL_COBOL}, {"s", CL_ASM}, {"asm", CL_ASM}, {"nasm", CL_ASM}, {"proto", CL_PROTO}, {"thrift", CL_PROTO},
        {"graphql", CL_GQL}, {"gql", CL_GQL}, {"graphqls", CL_GQL}, {"tf", CL_TF}, {"tfvars", CL_TF}, {"vue", CL_VUE}, {"svelte", CL_VUE}, {"tex", CL_TEX}, {"sty", CL_TEX}, {"cls", CL_TEX},
        {"rst", CL_DOC}, {"adoc", CL_DOC}, {"asciidoc", CL_DOC}, {"org", CL_DOC}, {"xml", CL_XML}, {"xaml", CL_XML}, {"xsd", CL_XML}, {"xsl", CL_XML}, {"xslt", CL_XML}, {"csproj", CL_XML},
        {"vbproj", CL_XML}, {"fsproj", CL_XML}, {"targets", CL_XML}, {"plist", CL_XML}, {"properties", CL_PROPS}, {"ini", CL_PROPS}, {"cfg", CL_PROPS}, {"env", CL_PROPS}, {NULL, 0}};
    if (base) {
        if (c2_ieq(base, "dockerfile") || c2_ieq(base, "containerfile") || c2_ipre(base, "dockerfile.") || c2_ipre(base, "containerfile.")) return CL_DOCKER;
        if (c2_ieq(base, "makefile") || c2_ieq(base, "gnumakefile") || c2_ieq(base, "bsdmakefile")) return CL_MAKE;
        if (c2_ieq(base, "cmakelists.txt")) return CL_CMAKE;
        if (c2_ieq(base, "rakefile") || c2_ieq(base, "gemfile") || c2_ieq(base, "podfile") || c2_ieq(base, "vagrantfile") || c2_ieq(base, "guardfile") || c2_ieq(base, "capfile") || c2_ieq(base, "brewfile")) return CL_RB;
        if (c2_ieq(base, "jenkinsfile")) return CL_GROOVY;
        if (c2_ieq(base, "project.clj") || c2_ieq(base, "deps.edn") || c2_ieq(base, "bb.edn")) return CL_CLJ;
        if (c2_ieq(base, ".env") || c2_ipre(base, ".env.") || c2_ieq(base, ".gitignore") || c2_ieq(base, ".dockerignore") || c2_ieq(base, ".gitattributes") || c2_ieq(base, ".npmrc") || c2_ieq(base, ".editorconfig") || c2_ieq(base, ".npmignore")) return CL_PROPS;
        size_t bl = strlen(base);
        if (bl > 5 && c2_ieq(base + bl - 5, ".json")) {
            if (c2_ieq(base, "package.json") || c2_ieq(base, "composer.json") || c2_ipre(base, "tsconfig") || c2_ipre(base, "jsconfig") || c2_ieq(base, "deno.json") || c2_ieq(base, "deno.jsonc") ||
                c2_ipre(base, "appsettings") || c2_ieq(base, "manifest.json") || c2_ieq(base, "lerna.json") || c2_ieq(base, "nx.json") || c2_ieq(base, "angular.json") || c2_ieq(base, "renovate.json") || c2_ieq(base, "vercel.json") || c2_ieq(base, "firebase.json")) return CL_JSON;
        }
    }
    if (!ext) return CL_UNKNOWN;
    char e[12]; size_t l = strlen(ext); if (l > 10) return CL_UNKNOWN;
    for (size_t k = 0; k <= l; k++) e[k] = (char)tolower((unsigned char)ext[k]);
    for (int k = 0; T[k].e; k++) if (!strcmp(T[k].e, e)) return T[k].l;
    return CL_UNKNOWN;
}
