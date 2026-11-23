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
        if (L == CL_SQL && c == '`') { cl[i] = ' '; i++; continue; }
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
static void c2_parse_line(CP *c) { (void)c; }
static void c2_sweep_line(CP *c) { (void)c; }
static void c2_enrich(CP *c) { (void)c; }
static int c2_lang_of(const char *base, const char *ext) {
    if (!ext) return CL_UNKNOWN; char e[12]; size_t l = strlen(ext); if (l > 10) return CL_UNKNOWN;
    for (size_t k = 0; k <= l; k++) e[k] = (char)tolower((unsigned char)ext[k]);
    static const struct { const char *e; int l; } T[] = {{"cs", CL_CS}, {"swift", CL_SWIFT}, {"php", CL_PHP}, {"dart", CL_DART}, {"scala", CL_SCALA}, {"sc", CL_SCALA}, {"groovy", CL_GROOVY}, {"gradle", CL_GROOVY}, {NULL, 0}};
    for (int k = 0; T[k].e; k++) if (!strcmp(T[k].e, e)) return T[k].l;
    return CL_UNKNOWN;
}
