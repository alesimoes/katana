#!/bin/sh
# Testa o motor de expressoes regulares do Grep (src/tools/regex.c) direto, sem andar por pastas, pelo gancho
# KATANA_REGEX_TEST=1: Grep sem path e com subject mostra o trecho casado e o de cada grupo ("0:3-7 1:4-6 nome=4-6"),
# "no match" ou o erro de compilacao. Uso: sh tools/tooltest/regex.sh ./katana
# Tabela: padrao @@ texto @@ esperado [@@ argumentos extras]; padrao e texto ja escritos como texto JSON (\\ = barra).
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export KATANA_LOG=${KATANA_LOG:-$(mktemp)}     # o log das chamadas destes testes nao vai para o ~/.katana/agente.log do dono
[ -x "$K" ] || { echo "uso: $0 <binario do katana>"; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
N=0; F=0
chk() {   # nome esperado obtido
    N=$((N+1))
    if [ "$2" = "$3" ]; then printf 'ok   %s\n' "$1"; else F=$((F+1)); printf 'FAIL %s\n' "$1"; printf '  esperado: %s\n  obtido:   %s\n' "$2" "$3"; fi
}
rx() { KATANA_REGEX_TEST=1 "$K" tool Grep "$1" 2>&1; }

while IFS= read -r line; do
    case "$line" in ''|'#'*) continue;; esac
    p=${line%% @@ *}; r=${line#* @@ }
    s=${r%% @@ *}; r=${r#* @@ }
    case "$r" in *' @@ '*) e=${r%% @@ *}; x=",${r#* @@ }";; *) e=$r; x="";; esac
    chk "/$p/ em \"$s\"$x" "$e" "$(rx "{\"pattern\":\"$p\",\"subject\":\"$s\"$x}")"
done <<'EOF'
# ---- literais e ponto ----
abc @@ xxabcxx @@ 0:2-5
abc @@ ab @@ no match
abc @@ abd abc @@ 0:4-7
a.c @@ abc @@ 0:0-3
a.c @@ a\nc @@ no match
(?s)a.c @@ a\nc @@ 0:0-3
a.c @@ a\nc @@ 0:0-3 @@ "multiline":true
. @@ é @@ 0:0-2
^.$ @@ é @@ 0:0-2
^..$ @@ é @@ no match
x* @@ abc @@ 0:0-0
ção @@ informação @@ 0:7-12
😀 @@ a😀b @@ 0:1-5
# ---- ancoras ----
^abc @@ abc @@ 0:0-3
^abc @@ xabc @@ no match
abc$ @@ xabc @@ 0:1-4
abc$ @@ abcx @@ no match
abc$ @@ abc\n @@ 0:0-3
abc\\z @@ abc\n @@ no match
abc\\z @@ xabc @@ 0:1-4
abc\\Z @@ abc\n @@ 0:0-3
abc\\Z @@ abc\nx @@ no match
\\Aabc @@ xabc @@ no match
\\Aabc @@ abc @@ 0:0-3
^b @@ a\nb @@ no match
(?m)^b @@ a\nb @@ 0:2-3
(?m)a$ @@ a\nb @@ 0:0-1
a$ @@ a\nb @@ no match
(?m)\\Ab @@ a\nb @@ no match
(?m)a\\z @@ a\nb @@ no match
^b @@ ab @@ no match @@ "start":1
b @@ ab @@ 0:1-2 @@ "start":1
a @@ a @@ no match @@ "start":5
$ @@ abc @@ 0:3-3
^$ @@  @@ 0:0-0
(?m)^$ @@ a\n\nb @@ 0:2-2
(?m)^\\w+$ @@ ab\ncd @@ 0:3-5 @@ "start":1
(?m)^\\w+$ @@ ab\r\ncd @@ 0:0-2
# ---- limites de palavra ----
\\bcat\\b @@ a cat. @@ 0:2-5
\\bcat\\b @@ concat @@ no match
\\Bcat @@ concat @@ 0:3-6
\\Bcat @@ cat @@ no match
\\<cat @@ the cat @@ 0:4-7
\\<at @@ cat @@ no match
cat\\> @@ cat s @@ 0:0-3
ca\\> @@ cat @@ no match
\\bação\\b @@ a ação! @@ 0:2-8
\\bçã @@ ação @@ no match
\\b @@  @@ no match
\\B @@  @@ 0:0-0
\\b\\w @@ ..x @@ 0:2-3
\\w\\b @@ ab cd @@ 0:1-2
# ---- classes ----
[abc]+ @@ xxbcax @@ 0:2-5
[abc] @@ xyz @@ no match
[^abc]+ @@ abxyc @@ 0:2-4
[^abc] @@ abc @@ no match
[a-z]+ @@ ABcdE @@ 0:2-4
[a-z]+ @@ ABcdE @@ 0:0-5 @@ "-i":true
[]a]+ @@ x]a] @@ 0:1-4
[^]a] @@ ]ab @@ 0:2-3
[a-]+ @@ x-a- @@ 0:1-4
[-a]+ @@ x-a- @@ 0:1-4
[\\]]+ @@ a]] @@ 0:1-3
[\\[\\]]+ @@ a[]b @@ 0:1-3
[.*+?] @@ ab?c @@ 0:2-3
[\\^x] @@ a^ @@ 0:1-2
[a^]+ @@ ^a^ @@ 0:0-3
[[:alpha:]]+ @@ 12abç3 @@ 0:2-6
[[:digit:]]+ @@ ab123 @@ 0:2-5
[[:space:]] @@ a\tb @@ 0:1-2
[[:blank:]] @@ a\nb c @@ 0:3-4
[[:upper:]]+ @@ abCDe @@ 0:2-4
[[:lower:]]+ @@ ABcdE @@ 0:2-4
[[:punct:]]+ @@ ab,.;c @@ 0:2-5
[[:xdigit:]]+ @@ zz0aFg @@ 0:2-5
[[:alnum:]_]+ @@ -a_1- @@ 0:1-4
[[:^digit:]]+ @@ 12ab3 @@ 0:2-4
[[:word:]]+ @@ ..a_1.. @@ 0:2-5
[[:cntrl:]] @@ a\u0001 @@ 0:1-2
[[:graph:]]+ @@  ab  @@ 0:1-3
[[:print:]]+ @@ \tab c\t @@ 0:1-5
[[:bogus:]] @@ x @@ error: unknown class name in [:name:] (known: alpha digit alnum upper lower space blank punct print graph cntrl xdigit word) at position 2
[z-a] @@ x @@ error: invalid range in class (the first character comes after the second) at position 2
[abc @@ x @@ error: missing ] to close the class [ opened at position 1
[ @@ x @@ error: missing ] to close the class [ opened at position 1
[\\d.]+ @@ v1.25x @@ 0:1-5
[^\\s]+ @@   ab c @@ 0:2-4
[\\w-]+ @@ @foo-bar! @@ 0:1-8
[\\d-z]+ @@ a1-z @@ 0:1-4
[α-ω]+ @@ xαβγx @@ 0:1-7
[à-ÿ] @@ aé @@ 0:1-3
[À-Ö] @@ é @@ 0:0-2 @@ "-i":true
[À-Ö] @@ é @@ no match
[\\x41-\\x43]+ @@ zABCD @@ 0:1-4
[\\u00e0-\\u00e9]+ @@ xàé @@ 0:1-5
[\\t ]+ @@ a \t b @@ 0:1-4
[\\b] @@ a\bb @@ 0:1-2
[^\\n]+ @@ \nab @@ 0:1-3
# ---- escapes ----
\\d+ @@ ab123c @@ 0:2-5
\\d @@ abc @@ no match
\\D+ @@ 12ab3 @@ 0:2-4
\\d @@ ٣ @@ no match
\\w+ @@ --olá-- @@ 0:2-6
\\w+ @@ --Привет-- @@ 0:2-14
\\w @@ -- @@ no match
\\W+ @@ ab, cd @@ 0:2-4
\\s+ @@ a \t b @@ 0:1-4
\\s @@ a b @@ 0:1-3
\\S+ @@   xy  @@ 0:2-4
\\h+ @@ a \t\nb @@ 0:1-3
\\v @@ a \nb @@ 0:2-3
\\H @@  \tx @@ 0:2-3
\\V+ @@ \nab\n @@ 0:1-3
\\t @@ a\tb @@ 0:1-2
\\n @@ a\nb @@ 0:1-2
\\r @@ a\rb @@ 0:1-2
\\f @@ a\fb @@ 0:1-2
\\e @@ a\u001bb @@ 0:1-2
\\a @@ a\u0007b @@ 0:1-2
\\0 @@ a\u0000b @@ 0:1-2
\\x41 @@ zA @@ 0:1-2
\\x4 @@ z\u0004 @@ 0:1-2
\\x{e9} @@ café @@ 0:3-5
\\x{1F600} @@ a😀 @@ 0:1-5
\\u00e9 @@ café @@ 0:3-5
\\u00E9+ @@ éé @@ 0:0-4
\\. @@ a.b @@ 0:1-2
\\. @@ ab @@ no match
\\/ @@ a/b @@ 0:1-2
\\$\\d @@ x$5 @@ 0:1-3
\\(\\) @@ f() @@ 0:1-3
\\{\\} @@ {} @@ 0:0-2
\\\\ @@ a\\b @@ 0:1-2
\\* @@ a*b @@ 0:1-2
\\x @@ x @@ error: bad \x escape: use \xHH or \x{HHHH} at position 1
\\u12 @@ x @@ error: bad \u escape: use \uHHHH at position 1
\\x{110000} @@ x @@ error: bad \x{...} escape: use 1 to 6 hex digits up to 10FFFF at position 1
\\x{} @@ x @@ error: bad \x{...} escape: use 1 to 6 hex digits up to 10FFFF at position 1
\\y @@ x @@ error: unknown escape \y (write \\y for a backslash followed by y) at position 1
a\\ @@ x @@ error: trailing backslash (write \\ for a literal backslash) at position 2
# ---- propriedades Unicode ----
\\p{L}+ @@ 12Ωmega3 @@ 0:2-8
\\p{L} @@ 123 @@ no match
\\p{Lu} @@ abcÉ @@ 0:3-5
\\p{Lu} @@ abc @@ no match
\\p{Ll}+ @@ ÉCOle @@ 0:4-6
\\p{Ll} @@ ÀÉ @@ no match
\\p{N}+ @@ x²3 @@ 0:1-4
\\p{Nd}+ @@ x²3 @@ 0:3-4
\\p{P}+ @@ a«b» @@ 0:1-3
\\p{P} @@ a+b @@ no match
\\p{Z} @@ a b @@ 0:1-3
\\p{Z} @@ a\tb @@ no match
\\P{L}+ @@ ab12cd @@ 0:2-4
\\P{N} @@ 12 @@ no match
\\pL+ @@ 12ab @@ 0:2-4
\\p{Greek}+ @@ abγδ @@ 0:2-6
\\p{Cyrillic}+ @@ abжи @@ 0:2-6
\\p{Latin}+ @@ γåb @@ 0:2-5
\\p{^L}+ @@ ab12 @@ 0:2-4
\\p{Letter}+ @@ 1ab @@ 0:1-3
[\\p{L}\\d]+ @@ -é1- @@ 0:1-4
[^\\p{L}]+ @@ ab12 @@ 0:2-4
\\p{L}+ @@ 1漢字2 @@ 0:1-7
\\p{L}+ @@ 1한국2 @@ 0:1-7
\\p{L}+ @@ 1שלום2 @@ 0:1-9
\\p{Foo} @@ x @@ error: unknown Unicode property (known: L Lu Ll N Nd P Z Latin Greek Cyrillic) at position 1
\\p{L @@ x @@ error: missing } in \p{...} at position 1
# ---- grupos ----
(a)(b)? @@ a @@ 0:0-1 1:0-1 2:-
(?:ab)+ @@ ababx @@ 0:0-4
(?:ab)+(c) @@ ababc @@ 0:0-5 1:4-5
(?<word>\\w+)@ @@ hi joe@x @@ 0:3-7 1:3-6 word=3-6
(?P<n>\\d) @@ a5 @@ 0:1-2 1:1-2 n=1-2
(?'q'x) @@ ax @@ 0:1-2 1:1-2 q=1-2
(?<a>x)|(?<b>y) @@ y @@ 0:0-1 1:- 2:0-1 a=- b=0-1
(a(b(c))) @@ abc @@ 0:0-3 1:0-3 2:1-3 3:2-3
(a|b)* @@ abab @@ 0:0-4 1:3-4
(a)|b @@ b @@ 0:0-1 1:-
() @@ a @@ 0:0-0 1:0-0
(?#comment)ab @@ xab @@ 0:1-3
a(?#x)b @@ ab @@ 0:0-2
(abc @@ x @@ error: missing ) to close the group opened at position 1
((a) @@ x @@ error: missing ) to close the group opened at position 1
abc) @@ x @@ error: unmatched ) (escape it as \) to match a parenthesis) at position 4
(?<1x>a) @@ x @@ error: bad group name: use letters, digits and _, starting with a letter at position 1
(?<n>a)(?<n>b) @@ x @@ error: duplicate group name at position 8
(?Z) @@ x @@ error: unknown group syntax after (? (known: (?: (?> (?= (?! (?<= (?<! (?<name> (?i) at position 1
(?#x @@ x @@ error: missing ) to close the comment (?# at position 1
# ---- grupo atomico ----
(?>a+)b @@ aaab @@ 0:0-4
(?>a+)a @@ aaaa @@ no match
a+a @@ aaaa @@ 0:0-4
(?>ab|a)c @@ abc @@ 0:0-3
(?>a|ab)c @@ abc @@ no match
(?:a|ab)c @@ abc @@ 0:0-3
(?>(a+))b @@ aab @@ 0:0-3 1:0-2
(?>x*)y @@ xxy @@ 0:0-3
# ---- alternativa (a primeira que casa, nao a mais longa) ----
cat|dog @@ hotdog @@ 0:3-6
cat|dog @@ bird @@ no match
a|ab @@ ab @@ 0:0-1
ab|a @@ ab @@ 0:0-2
(a|ab)(c|bcd) @@ abcd @@ 0:0-4 1:0-1 2:1-4
(a|ab)(c|bcd)$ @@ abcd @@ 0:0-4 1:0-1 2:1-4
(ab|a)(c|bcd)$ @@ abcd @@ 0:0-4 1:0-1 2:1-4
(ab|a)(c|bcd) @@ abcd @@ 0:0-3 1:0-2 2:2-3
|a @@ a @@ 0:0-0
a| @@ b @@ 0:0-0
x(?:a|b|c)y @@ xby @@ 0:0-3
x(?:a|b|c)y @@ xdy @@ no match
(?:a|b)+?c @@ ababc @@ 0:0-5
foo|foobar @@ foobar @@ 0:0-3
(?:foo|foobar)$ @@ foobar @@ 0:0-6
# ---- quantificadores: guloso, preguicoso, possessivo ----
a* @@ aaa @@ 0:0-3
a*? @@ aaa @@ 0:0-0
a+ @@ baaa @@ 0:1-4
a+? @@ aaa @@ 0:0-1
a+ @@ bbb @@ no match
a? @@ a @@ 0:0-1
a?? @@ a @@ 0:0-0
a{2} @@ aaa @@ 0:0-2
a{2,} @@ aaaa @@ 0:0-4
a{2,3} @@ aaaa @@ 0:0-3
a{2,3}? @@ aaaa @@ 0:0-2
a{2,}? @@ aaaa @@ 0:0-2
a{,2} @@ aaa @@ 0:0-2
a{3} @@ aa @@ no match
a{2}+ @@ aaa @@ 0:0-2
x{0} @@ x @@ 0:0-0
<.+> @@ <a><b> @@ 0:0-6
<.+?> @@ <a><b> @@ 0:0-3
<[^>]+> @@ <a><b> @@ 0:0-3
a*+a @@ aaa @@ no match
a++b @@ aab @@ 0:0-3
a?+a @@ a @@ no match
a{1,3}+a @@ aaaa @@ 0:0-4
a{1,3}+a @@ aaa @@ no match
a{1,3}a @@ aaa @@ 0:0-3
\".*\" @@ say \"a\" and \"b\" @@ 0:4-15
\".*?\" @@ say \"a\" and \"b\" @@ 0:4-7
\".*+\" @@ say \"a\" and \"b\" @@ no match
\"[^\"]*+\" @@ say \"a\" and \"b\" @@ 0:4-7
\\d{1,3}(?:,\\d{3})* @@ 1,234,567 @@ 0:0-9
(\\d+)(\\d) @@ 1234 @@ 0:0-4 1:0-3 2:3-4
(\\d+?)(\\d) @@ 1234 @@ 0:0-2 1:0-1 2:1-2
(\\d++)(\\d) @@ 1234 @@ no match
(a{2})* @@ aaaaa @@ 0:0-4 1:2-4
a{10001} @@ x @@ error: repetition count above 10000 at position 2
a{3,2} @@ x @@ error: repetition {n,m} with m smaller than n at position 2
*a @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 1
+ @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 1
a** @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 3
a{2}{3} @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 5
{2} @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 1
(|*) @@ x @@ error: quantifier with nothing to repeat (escape it with a backslash to match it literally) at position 3
a{ @@ a{ @@ 0:0-2
{} @@ {} @@ 0:0-2
x{a} @@ x{a} @@ 0:0-4
f{1,2 @@ f{1,2 @@ 0:0-5
main() { @@ int main() { @@ no match
main\\(\\) { @@ int main() { @@ 0:4-12
(?:a{1000}){20} @@ aaa @@ no match
(?:a{10000}){100} @@ x @@ error: pattern too large (repetition counts too high; at most 200000 instructions)
# ---- repeticao que casa vazio ----
(a*)* @@ b @@ 0:0-0 1:0-0
(a*)* @@ aab @@ 0:0-2 1:2-2
(a*)+ @@ b @@ 0:0-0 1:0-0
()* @@ a @@ 0:0-0 1:0-0
()+x @@ x @@ 0:0-1 1:0-0
(a|)+ @@ aa @@ 0:0-2 1:2-2
(?:)+x @@ x @@ 0:0-1
(\\b)*a @@ a @@ 0:0-1 1:0-0
(a?)*?b @@ aab @@ 0:0-3 1:1-2
(?:a*)*b @@ aaab @@ 0:0-4
(?:a*)*b @@ aaa @@ no match
(?:x?)*y @@ xxy @@ 0:0-3
(a*?)* @@ aa @@ 0:0-0 1:0-0
x* @@ ab @@ 0:1-1 @@ "start":1
x* @@  @@ 0:0-0
(?:^)* @@ a @@ 0:0-0
(?=a)* @@ a @@ 0:0-0
# ---- lookahead e lookbehind ----
foo(?=bar) @@ foobaz foobar @@ 0:7-10
foo(?=bar) @@ foobaz @@ no match
foo(?!bar) @@ foobar foobaz @@ 0:7-10
foo(?!bar) @@ foobar @@ no match
(?<=\\$)\\d+ @@ cost $42 @@ 0:6-8
(?<=\\$)\\d+ @@ cost 42 @@ no match
(?<!\\$)\\b\\d+ @@ $42 17 @@ 0:4-6
(?<=ab|xyz)c @@ xyzc @@ 0:3-4
(?<=ab|xyz)c @@ yzc @@ no match
(?<=a{2,3})b @@ aab @@ 0:2-3
(?<=a{2,3})b @@ ab @@ no match
(?<=é)x @@ éx @@ 0:2-3
(?<=\\p{L}{2})x @@ éax @@ 0:3-4
(?<=^)a @@ a @@ 0:0-1
(?<=a) @@ ba @@ 0:2-2
(?<=.{0,255})b @@ ab @@ 0:1-2
(?<=a+)b @@ x @@ error: lookbehind must have a bounded length (at most 255 characters; no *, +, {n,} or backreference inside) at position 1
(?<=a*)b @@ x @@ error: lookbehind must have a bounded length (at most 255 characters; no *, +, {n,} or backreference inside) at position 1
(?<=a{300})b @@ x @@ error: lookbehind must have a bounded length (at most 255 characters; no *, +, {n,} or backreference inside) at position 1
(a)(?<=\\1)b @@ x @@ error: lookbehind must have a bounded length (at most 255 characters; no *, +, {n,} or backreference inside) at position 4
(?<!a{1,2}x)y @@ aaxy bxy @@ 0:7-8
(?<=(\\d))x @@ 5x @@ 0:1-2 1:0-1
(?=(\\w+))\\w @@ ab @@ 0:0-1 1:0-2
(?!(a))b @@ b @@ 0:0-1 1:-
\\b(?=\\w*\\d)\\w+ @@ abc x1y @@ 0:4-7
q(?=u) @@ qatar quit @@ 0:6-7
(?<!\\w)cat @@ concat cat @@ 0:7-10
(?<=a)b @@ ab @@ 0:1-2 @@ "start":1
(?=.*\\d)(?=.*[a-z])\\w{4,} @@ abc1 @@ 0:0-4
(?=.*\\d)(?=.*[a-z])\\w{4,} @@ abcd @@ no match
(?<=(?<=a)b)c @@ abc @@ 0:2-3
(?<=(?=b)b)c @@ bc @@ 0:1-2
# ---- referencias ----
(\\w)\\1 @@ abccd @@ 0:2-4 1:2-3
(\\w)\\1 @@ abcd @@ no match
(\\w+) \\1 @@ the the cat @@ 0:0-7 1:0-3
(?<x>a)\\k<x> @@ aa @@ 0:0-2 1:0-1 x=0-1
(?<x>a)\\k'x' @@ aa @@ 0:0-2 1:0-1 x=0-1
(?<x>a)\\k{x} @@ aa @@ 0:0-2 1:0-1 x=0-1
(?P<x>a)(?P=x) @@ aa @@ 0:0-2 1:0-1 x=0-1
(a)|\\1b @@ b @@ no match
(?i)(a)\\1 @@ aA @@ 0:0-2 1:0-1
(a)\\1 @@ aA @@ no match
(é)\\1 @@ éÉ @@ 0:0-4 1:0-2 @@ "-i":true
([\"'])\\w+\\1 @@ say 'hi' @@ 0:4-8 1:4-5
\\2(a) @@ x @@ error: backreference \2 to a group that does not exist (the pattern has 1 group)
\\k<nope> @@ x @@ error: reference to an unknown group name at position 1
(a)\\10 @@ aa0 @@ 0:0-3 1:0-1
(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10 @@ abcdefghijj @@ 0:0-11 1:0-1 2:1-2 3:2-3 4:3-4 5:4-5 6:5-6 7:6-7 8:7-8 9:8-9 10:9-10
(a*)b\\1 @@ aabaa @@ 0:0-5 1:0-2
(a*)b\\1$ @@ aaba @@ 0:1-4 1:1-2
# ---- flags ----
(?i)hello @@ HeLLo @@ 0:0-5
hello @@ HELLO @@ no match
hello @@ HELLO @@ 0:0-5 @@ "-i":true
(?i:he)llo @@ HEllo @@ 0:0-5
(?i:he)llo @@ HELLO @@ no match
(?i)he(?-i)llo @@ HEllo @@ 0:0-5
(?i)he(?-i)llo @@ HELLO @@ no match
a(?i)b @@ aB @@ 0:0-2
(?:a(?i)b)c @@ aBC @@ no match
(?:a(?i)b)c @@ aBc @@ 0:0-3
(?x) a b  c # comment @@ abc @@ 0:0-3
(?x)a\\ b @@ a b @@ 0:0-3
(?x)[ ] @@ a b @@ 0:1-2
(?x)a b @@ a b @@ no match
(?s).+ @@ a\nb @@ 0:0-3
.+ @@ a\nb @@ 0:0-1
(?is)A.B @@ a\nb @@ 0:0-3
(?m-s)^.+$ @@ ab\ncd @@ 0:0-2
(?q) @@ x @@ error: unknown group syntax after (? (known: (?: (?> (?= (?! (?<= (?<! (?<name> (?i) at position 1
(?i @@ x @@ error: bad inline flags: use (?i) (?m) (?s) (?x), (?-i) or (?i:...) at position 1
# ---- caixa com acentos, grego e cirilico ----
(?i)é @@ É @@ 0:0-2
é @@ É @@ no match
(?i)ÇÃO @@ ação @@ 0:1-6
(?i)ÉCOLE @@ une école @@ 0:4-10
(?i)straße @@ STRASSE @@ no match
(?i)σ @@ Σ @@ 0:0-2
(?i)σ @@ ς @@ 0:0-2
(?i)ς @@ Σ @@ 0:0-2
(?i)ά @@ Ά @@ 0:0-2
(?i)έ @@ Έ @@ 0:0-2
(?i)ω @@ Ω @@ 0:0-2
(?i)привет @@ ПРИВЕТ @@ 0:0-12
(?i)ё @@ Ё @@ 0:0-2
(?i)ї @@ Ї @@ 0:0-2
(?i)ÿ @@ Ÿ @@ 0:0-2
(?i)ő @@ Ő @@ 0:0-2
(?i)ł @@ Ł @@ 0:0-2
(?i)ž @@ Ž @@ 0:0-2
(?i)ệ @@ Ệ @@ 0:0-3
(?i)ա @@ Ա @@ 0:0-2
(?i)Ａ @@ ａ @@ 0:0-3
(?i)[é] @@ É @@ 0:0-2
(?i)[à-ö]+ @@ ÀÉÖ @@ 0:0-6
(?i)[a-z]+ @@ ÀB @@ 0:2-3
(?i)\\p{Lu} @@ a @@ 0:0-1
(?i)k @@ K @@ 0:0-1
(?i)[^é] @@ É @@ no match
(?i)Ä+ @@ xäÄä @@ 0:1-7
# ---- \Q...\E ----
\\Qa.b*\\E @@ xa.b* @@ 0:1-5
\\Qa.b*\\E @@ aXbb @@ no match
\\Q(x) @@ (x) @@ 0:0-3
\\Qa\\E+ @@ aaa @@ 0:0-3
\\Q[\\E\\d @@ [5 @@ 0:0-2
x\\Q\\Ey @@ xy @@ 0:0-2
# ---- busca a partir de um deslocamento ----
a @@ aaa @@ 0:2-3 @@ "start":2
\\ba @@ aa a @@ 0:3-4 @@ "start":1
\\Ba @@ aa @@ 0:1-2 @@ "start":1
# ---- texto simples (literal) ----
a.c @@ abc a.c @@ 0:4-7 @@ "literal":true
(x @@ f(x) @@ 0:1-3 @@ "literal":true
É @@ é @@ 0:0-2 @@ "literal":true,"-i":true
EOF

# ---- UTF-8 invalido (bytes crus no padrao e no texto) ----
bad() {   # nome padrao(printf) texto(printf) esperado
    chk "$1" "$4" "$(KATANA_REGEX_TEST=1 "$K" tool Grep "$(printf "{\"pattern\":\"$2\",\"subject\":\"$3\"}")" 2>&1)"
}
bad "byte invalido no texto e ." 'a.b' 'a\377b' "0:0-3"
bad "byte invalido no padrao e no texto" '\377' 'x\377y' "0:1-2"
bad "classe negada casa byte invalido" '[^a]' '\377' "0:0-1"
bad "sequencia cortada no fim" '^.$' '\303' "0:0-1"
bad "sequencia cortada no meio" 'a..b' 'a\303xb' "0:0-4"
bad "byte solto no padrao nao casa UTF-8 valido" 'caf\351' 'café' "no match"
bad "byte solto no padrao casa o mesmo byte" 'caf\351' 'caf\351' "0:0-4"
bad "\\\\w para no byte invalido" '\\\\w+' 'ab\377cd' "0:0-2"
bad "lookbehind sobre byte invalido" '(?<=\377)c' '\377c' "0:1-2"
bad "caixa com byte invalido" '(?i)\377' '\377' "0:0-1"
bad "continuacao solta" '\\\\x{80}' '\200' "no match"
bad "sobrelongo (C0 80) vale como dois bytes" '^..$' '\300\200' "0:0-2"
bad "substituto codificado (ED A0 80) vale como tres bytes" '^...$' '\355\240\200' "0:0-3"
bad "classe com byte invalido no padrao" '[\377]' 'a\377' "0:1-2"
bad "limite de palavra junto a byte invalido" '\\\\bab\\\\b' '\377ab\377' "0:1-3"

# ---- limite de passos ----
A40=$(printf 'a%.0s' $(seq 1 40))
t0=$(date +%s%N)
chk "(a+)+\$ em 40 a + b: limite de passos" "error: pattern too slow on this input; simplify it" "$(rx "{\"pattern\":\"(a+)+\$\",\"subject\":\"${A40}b\"}")"
t1=$(date +%s%N); ms=$(( (t1 - t0) / 1000000 )); echo "     (levou $ms ms)"
chk "limite de passos em menos de 2 s" "1" "$([ $ms -lt 2000 ] && echo 1 || echo 0)"
chk "(x+x+)+y em y + 30 x: limite de passos" "error: pattern too slow on this input; simplify it" "$(rx "{\"pattern\":\"(x+x+)+y\",\"subject\":\"y$(printf 'x%.0s' $(seq 1 30))\"}")"
chk "(a|aa)*b\$ em b + 60 a: limite de passos" "error: pattern too slow on this input; simplify it" "$(rx "{\"pattern\":\"(a|aa)*b\$\",\"subject\":\"b$(printf 'a%.0s' $(seq 1 60))\"}")"
chk "(a+)+\$ que casa e rapido" "0:0-40 1:0-40" "$(rx "{\"pattern\":\"(a+)+\$\",\"subject\":\"${A40}\"}")"
chk "(?>a+)+b atomico nao explode" "no match" "$(rx "{\"pattern\":\"(?>a+)+b\",\"subject\":\"${A40}c\"}")"
L2000=$(printf 'k=v; %.0s' $(seq 1 400))
chk ".*=.*; numa linha de 2000 bytes" "0:0-1999" "$(rx "{\"pattern\":\".*=.*;\",\"subject\":\"$L2000\"}")"
chk "palavra no fim de linha longa" "0:2000-2003" "$(rx "{\"pattern\":\"end\",\"subject\":\"${L2000}end\"}")"
chk "lookbehind de 255 casa" "0:255-256" "$(rx "{\"pattern\":\"(?<=a{255})b\",\"subject\":\"$(printf 'a%.0s' $(seq 1 255))b\"}")"
chk "lookbehind de 255 com 254 nao casa" "no match" "$(rx "{\"pattern\":\"(?<=a{255})b\",\"subject\":\"$(printf 'a%.0s' $(seq 1 254))b\"}")"
chk "grupos aninhados demais" "error: groups nested too deeply at position 201" "$(rx "{\"pattern\":\"$(printf '(%.0s' $(seq 1 201))a$(printf ')%.0s' $(seq 1 201))\",\"subject\":\"a\"}")"
chk "200 grupos aninhados ainda valem" "0:0-1" "$(rx "{\"pattern\":\"$(printf '(?:%.0s' $(seq 1 200))a$(printf ')%.0s' $(seq 1 200))\",\"subject\":\"a\"}")"
chk "sem o gancho, subject e ignorado" "1" "$(cd "$T" && "$K" tool Grep '{"pattern":"zzqq_nada","subject":"zzqq_nada"}' 2>&1 | grep -c 'No ')"

echo
if [ $F -eq 0 ]; then echo "OK $N checks"; else echo "FAILED $F"; exit 1; fi
