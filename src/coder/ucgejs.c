/* ---------- coder/ucgejs.c: UCGE, backends JavaScript e TypeScript (docs/ucge.md, "Backends JS e TS") ----------
 * Um gerador so serve as duas linguagens: percorre a AST ja validada e monta um documento (ucgedoc.c); `p->ts` liga as anotacoes de tipo (derivadas
 * dos tipos da IR pela tabela de ucgetype.c) e os poucos pontos onde o TypeScript pede mais (genericos nos ajudantes, `Map<string, T>`).
 * Semantica IDENTICA a do Python onde a especificacao a fixa; onde o JavaScript diverge o gerador emite um AJUDANTE de execucao (`_fdiv`, `_mod`,
 * `_at`, `_len`, ...), copiado so quando algum no o usa (arvore de dependencias entre ajudantes), nunca uma biblioteca inteira:
 *   - `//` e `%` por piso (`_fdiv`, `_mod`; troca por `Math.floor(a / 2)` ou `a % n` quando o operando e conhecido), `/` e divisao por zero levantam erro;
 *   - mapas sao `Map` (ordem de insercao, qualquer chave str, `in` e `len` exatos); listas sao arrays; leitura e escrita por indice conferem limite e
 *     aceitam indice negativo (como o Python); strings contam por ponto de codigo (len, indice, fatia, reverse, indexof), nao por unidade UTF-16;
 *   - sem truthiness: so o tipo `any` passa por `_truth`; `==` de lista, mapa ou any e profundo (`_eq`); `<` de str e por ponto de codigo (`_scmp`);
 *   - str() de float e bool imita o Python (`2.0`, `True`), int()/float() de str idem, erros de execucao levam o texto do CPython;
 *   - funcoes sao DECLARACOES (`function`, hoisted: recursao e chamadas para frente como no Python), `const` quando o nome nao e reatribuido, `let` senao,
 *     laco `for (let i = a; i < b; i++)` com limite avaliado uma vez (como `range`), `try/catch` com `catch {}` quando a variavel nao e usada.
 * Nomes que comecam com `_` sao reservados para os ajudantes e temporarios. Divergencias que nao da para remover estao em docs/ucge.md. */
typedef struct { Cgx *g; const CgxGenOpts *o; CgxDocs D; CgxTypes T; int ts, ntmp; unsigned char need[96]; unsigned char *asg; int nasg; int has_raw; } CgxJs;
#define JD (&p->D)

/* ---- ajudantes de execucao: o texto e escrito UMA vez, em TypeScript; o que esta entre TS( ) some na saida JavaScript ---- */
#define TS(x) "\x01" x "\x02"
enum { H_SUR, H_ISWS, H_TRIM, H_WORDS, H_FSTR, H_PYQ, H_REPR, H_STR, H_EQ, H_HAS, H_NORM, H_AT, H_PUT, H_SLEN, H_SAT, H_MGET, H_IX, H_IXPUT, H_LEN, H_SSLICE, H_SLICE, H_FIND,
       H_REV, H_POP, H_SUM, H_SCMP, H_CMP, H_SORT, H_DIV, H_FDIV, H_MOD, H_INT, H_FLOAT, H_SQRT, H_POW, H_SPLIT, H_REPLACE, H_ERRMSG, H_EXC, H_TRUTH, H_ITER, H_ITEMS, H_KEYS, H_IN, H_JOIN, H_MAPIT, H_N };
typedef struct { const char *text; int dep[5]; } CgxJsHelper;
static const CgxJsHelper CGX_JS_H[H_N] = {
    /* H_SUR */ {"const _SUR = /[\\ud800-\\udfff]/;\n", {-1}},
    /* H_ISWS */ {"function _isws(c" TS(": number") ")" TS(": boolean") " {\n"
        "    return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c === 133 || c === 160 || c === 5760 || (c >= 8192 && c <= 8202) || c === 8232 || c === 8233 || c === 8239 || c === 8287 || c === 12288;\n}\n", {-1}},
    /* H_TRIM */ {"function _trim(s" TS(": string") ")" TS(": string") " {\n"
        "    let a = 0, b = s.length;\n    while (a < b && _isws(s.charCodeAt(a))) a++;\n    while (b > a && _isws(s.charCodeAt(b - 1))) b--;\n    return s.slice(a, b);\n}\n", {H_ISWS, -1}},
    /* H_WORDS */ {"function _words(s" TS(": string") ")" TS(": string[]") " {\n"
        "    const r" TS(": string[]") " = [];\n    let a = -1;\n    for (let i = 0; i < s.length; i++) {\n"
        "        if (_isws(s.charCodeAt(i))) {\n            if (a >= 0) { r.push(s.slice(a, i)); a = -1; }\n        } else if (a < 0) a = i;\n    }\n"
        "    if (a >= 0) r.push(s.slice(a));\n    return r;\n}\n", {H_ISWS, -1}},
    /* H_FSTR */ {"function _fstr(x" TS(": number") ")" TS(": string") " {\n"
        "    if (Number.isNaN(x)) return \"nan\";\n    if (x === Infinity) return \"inf\";\n    if (x === -Infinity) return \"-inf\";\n"
        "    if (x === 0) return Object.is(x, -0) ? \"-0.0\" : \"0.0\";\n"
        "    const [m, ex] = x.toExponential().split(\"e\");\n    const e = Number(ex);\n    const digits = m.replace(\"-\", \"\").replace(\".\", \"\");\n    let s" TS(": string") ";\n"
        "    if (e >= -4 && e < 16) {\n"
        "        if (e < 0) s = \"0.\" + \"0\".repeat(-e - 1) + digits;\n"
        "        else if (digits.length <= e + 1) s = digits + \"0\".repeat(e + 1 - digits.length) + \".0\";\n"
        "        else s = digits.slice(0, e + 1) + \".\" + digits.slice(e + 1);\n"
        "    } else {\n"
        "        s = digits[0] + (digits.length > 1 ? \".\" + digits.slice(1) : \"\") + \"e\" + (e < 0 ? \"-\" : \"+\") + String(Math.abs(e)).padStart(2, \"0\");\n    }\n"
        "    return (x < 0 ? \"-\" : \"\") + s;\n}\n", {-1}},
    /* H_PYQ */ {"function _pyq(s" TS(": string") ")" TS(": string") " {\n"
        "    const q = s.includes(\"'\") && !s.includes('\"') ? '\"' : \"'\";\n    let r = \"\";\n    for (const c of s) {\n"
        "        const k = c.codePointAt(0)" TS(" as number") ";\n"
        "        if (c === q || c === \"\\\\\") r += \"\\\\\" + c;\n"
        "        else if (c === \"\\n\") r += \"\\\\n\";\n        else if (c === \"\\t\") r += \"\\\\t\";\n        else if (c === \"\\r\") r += \"\\\\r\";\n"
        "        else if (k < 32 || (k >= 127 && k < 161) || k === 173) r += \"\\\\x\" + k.toString(16).padStart(2, \"0\");\n"
        "        else r += c;\n    }\n    return q + r + q;\n}\n", {-1}},
    /* H_REPR */ {"function _repr(x" TS(": unknown") ", fl" TS("?: boolean") ")" TS(": string") " {\n"
        "    if (typeof x === \"string\") return _pyq(x);\n"
        "    if (typeof x === \"number\") return Number.isInteger(x) && !fl ? String(x) : _fstr(x);\n"
        "    if (typeof x === \"boolean\") return x ? \"True\" : \"False\";\n"
        "    if (x === null || x === undefined) return \"None\";\n"
        "    if (Array.isArray(x)) return \"[\" + x.map((y) => _repr(y, fl)).join(\", \") + \"]\";\n"
        "    if (x instanceof Map) return \"{\" + Array.from(x, ([k, v]) => _repr(k) + \": \" + _repr(v, fl)).join(\", \") + \"}\";\n"
        "    const o = x" TS(" as any") ";\n"
        "    if (o.constructor !== Object) return \"<\" + o.constructor.name + \" object>\";\n"
        "    return \"{\" + Object.entries(o).map(([k, v]) => _repr(k) + \": \" + _repr(v, fl)).join(\", \") + \"}\";\n}\n", {H_FSTR, H_PYQ, -1}},
    /* H_STR */ {"function _str(x" TS(": unknown") ", fl" TS("?: boolean") ")" TS(": string") " {\n    return typeof x === \"string\" ? x : _repr(x, fl);\n}\n", {H_REPR, -1}},
    /* H_EQ */ {"function _eq(a" TS(": unknown") ", b" TS(": unknown") ")" TS(": boolean") " {\n"
        "    if (a === b) return true;\n"
        "    if (Array.isArray(a) && Array.isArray(b)) {\n        if (a.length !== b.length) return false;\n"
        "        for (let i = 0; i < a.length; i++) if (!_eq(a[i], b[i])) return false;\n        return true;\n    }\n"
        "    if (a instanceof Map && b instanceof Map) {\n        if (a.size !== b.size) return false;\n"
        "        for (const [k, v] of a) if (!b.has(k) || !_eq(v, b.get(k))) return false;\n        return true;\n    }\n    return false;\n}\n", {-1}},
    /* H_HAS */ {"function _has(c" TS(": any") ", x" TS(": unknown") ")" TS(": boolean") " {\n"
        "    if (typeof c === \"string\") return c.includes(x" TS(" as string") ");\n"
        "    if (Array.isArray(c)) return c.some((y) => _eq(y, x));\n    if (c instanceof Map) return c.has(x);\n"
        "    return typeof c === \"object\" && c !== null && Object.prototype.hasOwnProperty.call(c, x" TS(" as string") ");\n}\n", {H_EQ, -1}},
    /* H_NORM */ {"function _norm(i" TS(": number") ", n" TS(": number") ", msg" TS(": string") ")" TS(": number") " {\n"
        "    const j = i < 0 ? i + n : i;\n    if (!Number.isInteger(j) || j < 0 || j >= n) throw new Error(msg);\n    return j;\n}\n", {-1}},
    /* H_AT */ {"function _at" TS("<T>") "(xs" TS(": T[]") ", i" TS(": number") ")" TS(": T") " {\n    return xs[_norm(i, xs.length, \"list index out of range\")];\n}\n", {H_NORM, -1}},
    /* H_PUT */ {"function _put" TS("<T>") "(xs" TS(": T[]") ", i" TS(": number") ", v" TS(": T") ")" TS(": void") " {\n    xs[_norm(i, xs.length, \"list assignment index out of range\")] = v;\n}\n", {H_NORM, -1}},
    /* H_SLEN */ {"function _slen(s" TS(": string") ")" TS(": number") " {\n    return _SUR.test(s) ? Array.from(s).length : s.length;\n}\n", {H_SUR, -1}},
    /* H_SAT */ {"function _sat(s" TS(": string") ", i" TS(": number") ")" TS(": string") " {\n"
        "    if (!_SUR.test(s)) return s[_norm(i, s.length, \"string index out of range\")];\n"
        "    const cs = Array.from(s);\n    return cs[_norm(i, cs.length, \"string index out of range\")];\n}\n", {H_NORM, H_SUR, -1}},
    /* H_MGET */ {"function _mget" TS("<V>") "(m" TS(": Map<string, V>") ", k" TS(": string") ")" TS(": V") " {\n"
        "    if (!m.has(k)) throw new Error(_repr(k));\n    return m.get(k)" TS(" as V") ";\n}\n", {H_REPR, -1}},
    /* H_IX */ {"function _ix(c" TS(": any") ", k" TS(": any") ")" TS(": any") " {\n"
        "    if (Array.isArray(c)) return _at(c, k);\n    if (typeof c === \"string\") return _sat(c, k);\n    if (c instanceof Map) return _mget(c, k);\n"
        "    if (c !== null && typeof c === \"object\" && Object.prototype.hasOwnProperty.call(c, k)) return c[k];\n    throw new Error(_repr(k));\n}\n", {H_AT, H_SAT, H_MGET, H_REPR, -1}},
    /* H_IXPUT */ {"function _ixput(c" TS(": any") ", k" TS(": any") ", v" TS(": any") ")" TS(": void") " {\n"
        "    if (Array.isArray(c)) _put(c, k, v);\n    else if (c instanceof Map) c.set(k, v);\n    else c[k] = v;\n}\n", {H_PUT, -1}},
    /* H_LEN */ {"function _len(x" TS(": any") ")" TS(": number") " {\n"
        "    if (typeof x === \"string\") return _slen(x);\n    if (Array.isArray(x)) return x.length;\n    if (x instanceof Map) return x.size;\n    return Object.keys(x).length;\n}\n", {H_SLEN, -1}},
    /* H_SSLICE */ {"function _sslice(s" TS(": string") ", a" TS(": number") ", b" TS("?: number") ")" TS(": string") " {\n"
        "    return _SUR.test(s) ? Array.from(s).slice(a, b).join(\"\") : s.slice(a, b);\n}\n", {H_SUR, -1}},
    /* H_SLICE */ {"function _slice(x" TS(": any") ", a" TS(": number") ", b" TS("?: number") ")" TS(": any") " {\n    return typeof x === \"string\" ? _sslice(x, a, b) : x.slice(a, b);\n}\n", {H_SSLICE, -1}},
    /* H_FIND */ {"function _find(s" TS(": string") ", sub" TS(": string") ")" TS(": number") " {\n    const i = s.indexOf(sub);\n    return i < 0 ? -1 : _slen(s.slice(0, i));\n}\n", {H_SLEN, -1}},
    /* H_REV */ {"function _rev(x" TS(": any") ")" TS(": any") " {\n    return typeof x === \"string\" ? Array.from(x).reverse().join(\"\") : [...x].reverse();\n}\n", {-1}},
    /* H_POP */ {"function _pop" TS("<T>") "(xs" TS(": T[]") ")" TS(": T") " {\n    if (xs.length === 0) throw new Error(\"pop from empty list\");\n    return xs.pop()" TS(" as T") ";\n}\n", {-1}},
    /* H_SUM */ {"function _sum(xs" TS(": number[]") ")" TS(": number") " {\n"
        "    let s = 0, c = 0;\n    for (const x of xs) {\n        const t = s + x;\n        if (Math.abs(s) >= Math.abs(x)) c += (s - t) + x;\n        else c += (x - t) + s;\n        s = t;\n    }\n"
        "    if (c !== 0 && Number.isFinite(c)) s += c;\n    return s;\n}\n", {-1}},
    /* H_SCMP */ {"function _scmp(a" TS(": string") ", b" TS(": string") ")" TS(": number") " {\n"
        "    const n = Math.min(a.length, b.length);\n    for (let i = 0; i < n; i++) {\n        let x = a.charCodeAt(i), y = b.charCodeAt(i);\n"
        "        if (x !== y) {\n            if (x >= 0xd800 && y >= 0xd800) {\n                x += x >= 0xe000 ? -0x800 : 0x2000;\n                y += y >= 0xe000 ? -0x800 : 0x2000;\n            }\n            return x < y ? -1 : 1;\n        }\n    }\n"
        "    return a.length === b.length ? 0 : a.length < b.length ? -1 : 1;\n}\n", {-1}},
    /* H_CMP */ {"function _cmp(a" TS(": any") ", b" TS(": any") ")" TS(": number") " {\n"
        "    if (typeof a === \"string\" && typeof b === \"string\") return _scmp(a, b);\n    return a < b ? -1 : a > b ? 1 : 0;\n}\n", {H_SCMP, -1}},
    /* H_SORT */ {"function _sort" TS("<T>") "(xs" TS(": T[]") ")" TS(": T[]") " {\n    return [...xs].sort(_cmp);\n}\n", {H_CMP, -1}},
    /* H_DIV */ {"function _div(a" TS(": number") ", b" TS(": number") ")" TS(": number") " {\n    if (b === 0) throw new Error(\"division by zero\");\n    return a / b;\n}\n", {-1}},
    /* H_FDIV */ {"function _fdiv(a" TS(": number") ", b" TS(": number") ")" TS(": number") " {\n"
        "    if (b === 0) throw new Error(\"division by zero\");\n    const r = a % b;\n    return (a - r) / b - (r !== 0 && (r < 0) !== (b < 0) ? 1 : 0) + 0;\n}\n", {-1}},
    /* H_MOD */ {"function _mod(a" TS(": number") ", b" TS(": number") ")" TS(": number") " {\n"
        "    if (b === 0) throw new Error(\"division by zero\");\n    const r = a % b;\n    return (r !== 0 && (r < 0) !== (b < 0) ? r + b : r) + 0;\n}\n", {-1}},
    /* H_INT */ {"function _int(x" TS(": any") ")" TS(": number") " {\n"
        "    if (typeof x === \"number\") {\n        if (Number.isNaN(x)) throw new Error(\"cannot convert float NaN to integer\");\n"
        "        if (!Number.isFinite(x)) throw new Error(\"cannot convert float infinity to integer\");\n        return Math.trunc(x) + 0;\n    }\n"
        "    if (typeof x === \"boolean\") return x ? 1 : 0;\n"
        "    if (typeof x === \"string\") {\n        const t = _trim(x);\n        if (/^[+-]?\\d+(_\\d+)*$/.test(t)) return Number(t.replace(/_/g, \"\")) + 0;\n"
        "        throw new Error(\"invalid literal for int() with base 10: \" + _repr(x));\n    }\n    throw new Error(\"int() argument must be a string or a real number\");\n}\n", {H_TRIM, H_REPR, -1}},
    /* H_FLOAT */ {"function _float(x" TS(": any") ")" TS(": number") " {\n"
        "    if (typeof x === \"number\") return x;\n    if (typeof x === \"boolean\") return x ? 1 : 0;\n"
        "    if (typeof x === \"string\") {\n        const t = _trim(x);\n"
        "        if (/^[+-]?(\\d+(_\\d+)*(\\.(\\d+(_\\d+)*)?)?|\\.\\d+(_\\d+)*)([eE][+-]?\\d+(_\\d+)*)?$/.test(t)) return Number(t.replace(/_/g, \"\"));\n"
        "        if (/^[+-]?(inf|infinity)$/i.test(t)) return t[0] === \"-\" ? -Infinity : Infinity;\n        if (/^[+-]?nan$/i.test(t)) return NaN;\n"
        "        throw new Error(\"could not convert string to float: \" + _repr(x));\n    }\n    throw new Error(\"float() argument must be a string or a real number\");\n}\n", {H_TRIM, H_REPR, -1}},
    /* H_SQRT */ {"function _sqrt(x" TS(": number") ")" TS(": number") " {\n    if (x < 0) throw new Error(\"expected a nonnegative input, got \" + _fstr(x));\n    return Math.sqrt(x);\n}\n", {H_FSTR, -1}},
    /* H_POW */ {"function _pow(a" TS(": number") ", b" TS(": number") ")" TS(": number") " {\n    const r = Math.pow(a, b);\n"
        "    if (!Number.isFinite(r) && Number.isFinite(a) && Number.isFinite(b)) throw new Error(Number.isNaN(r) || a === 0 ? \"math domain error\" : \"math range error\");\n    return r;\n}\n", {-1}},
    /* H_SPLIT */ {"function _split(s" TS(": string") ", sep" TS(": string") ")" TS(": string[]") " {\n    if (sep === \"\") throw new Error(\"empty separator\");\n    return s.split(sep);\n}\n", {-1}},
    /* H_REPLACE */ {"function _replace(s" TS(": string") ", a" TS(": string") ", b" TS(": string") ")" TS(": string") " {\n"
        "    if (a !== \"\") return s.split(a).join(b);\n    const cs = Array.from(s);\n    return cs.length > 0 ? b + cs.join(b) + b : b;\n}\n", {-1}},
    /* H_ERRMSG */ {"function _errmsg(e" TS(": unknown") ")" TS(": string") " {\n    return e instanceof Error ? e.message : String(e);\n}\n", {-1}},
    /* H_EXC */ {"function _exc(x" TS(": unknown") ")" TS(": Error") " {\n    return x instanceof Error ? x : new Error(_str(x));\n}\n", {H_STR, -1}},
    /* H_TRUTH */ {"function _truth(x" TS(": unknown") ")" TS(": boolean") " {\n"
        "    if (x === null || x === undefined || x === false || x === 0 || x === \"\") return false;\n"
        "    if (Array.isArray(x)) return x.length > 0;\n    if (x instanceof Map) return x.size > 0;\n    return true;\n}\n", {-1}},
    /* H_ITER */ {"function _iter(x" TS(": any") ")" TS(": Iterable<any>") " {\n"
        "    if (x instanceof Map) return x.keys();\n    if (typeof x === \"object\" && x !== null && !Array.isArray(x)) return Object.keys(x);\n    return x;\n}\n", {-1}},
    /* H_ITEMS */ {"function _items(x" TS(": any") ")" TS(": Iterable<[any, any]>") " {\n    return x instanceof Map ? x : Object.entries(x)" TS(" as [any, any][]") ";\n}\n", {-1}},
    /* H_KEYS */ {"function _keys(x" TS(": any") ")" TS(": string[]") " {\n    return x instanceof Map ? [...x.keys()] : Object.keys(x);\n}\n", {-1}},
    /* H_IN */ {"function _in(x" TS(": unknown") ", c" TS(": any") ")" TS(": boolean") " {\n    return _has(c, x);\n}\n", {H_HAS, -1}},
    /* H_JOIN */ {"function _join(sep" TS(": string") ", xs" TS(": string[]") ")" TS(": string") " {\n    return xs.join(sep);\n}\n", {-1}},
    /* H_MAPIT */ {"function* _mapit(m" TS(": Map<string, any>") ", kv" TS(": boolean") ")" TS(": IterableIterator<any>") " {\n"
        "    const n = m.size;\n    for (const e of kv ? m : m.keys()) {\n        if (m.size !== n) throw new Error(\"dictionary changed size during iteration\");\n        yield e;\n    }\n"
        "    if (m.size !== n) throw new Error(\"dictionary changed size during iteration\");\n}\n", {-1}},
};
static void cjs_need(CgxJs *p, int h) {
    if (p->need[h]) return;
    p->need[h] = 1;
    for (int i = 0; CGX_JS_H[h].dep[i] >= 0; i++) cjs_need(p, CGX_JS_H[h].dep[i]);
}
/* copia o texto de um ajudante: JS tira o que esta entre \x01 e \x02, TS so tira as marcas */
static void cjs_emit_text(const char *s, int ts, CgxB *o) {
    int skip = 0;
    for (; *s; s++) {
        if (*s == '\x01') { if (!ts) skip = 1; continue; }
        if (*s == '\x02') { skip = 0; continue; }
        if (!skip) cgx_bn(o, s, 1);
    }
}

/* ---- nomes ---- */
static const char *const CGX_JS_RESERVED[] = {"break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for",
    "function", "if", "import", "in", "instanceof", "new", "null", "return", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield", "let", "static",
    "implements", "interface", "package", "private", "protected", "public", "await", "async", "arguments", "eval", "undefined", "NaN", "Infinity", "of", NULL};
static const char *const CGX_JS_SHADOW[] = {"Math", "Map", "Set", "Array", "Object", "String", "Number", "Boolean", "Error", "RegExp", "JSON", "Symbol", "Date", "console", "process", "require", "module",
    "exports", "global", "globalThis", "Iterable", "IterableIterator", "any", "unknown", "never", NULL};
static void cjs_name(CgxJs *p, int line, const char *n, int attr) {
    if (attr) { if (!strcmp(n, "constructor") || !strcmp(n, "__proto__") || !strcmp(n, "prototype")) cgx_err(p->g, line, "javascript: `%s` cannot be a field or method name; rename it", n); return; }
    for (int i = 0; CGX_JS_RESERVED[i]; i++) if (!strcmp(n, CGX_JS_RESERVED[i])) { cgx_err(p->g, line, "javascript: `%s` is a JavaScript keyword; rename it", n); return; }
    for (int i = 0; CGX_JS_SHADOW[i]; i++) if (!strcmp(n, CGX_JS_SHADOW[i])) { cgx_err(p->g, line, "javascript: `%s` would shadow a JavaScript global that the generated code uses; rename it", n); return; }
    if (n[0] == '_') cgx_err(p->g, line, "javascript: names that start with `_` are reserved for the generated helpers (`%s`); rename it", n);
}

/* ---- literais ---- */
static void cjs_quote(const char *s, CgxB *o, int tpl) {
    cgx_bs(o, tpl ? "`" : "\"");
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '\\') cgx_bs(o, "\\\\");
        else if (!tpl && c == '"') cgx_bs(o, "\\\"");
        else if (tpl && c == '`') cgx_bs(o, "\\`");
        else if (tpl && c == '$' && s[1] == '{') cgx_bs(o, "\\$");
        else if (c == '\n') cgx_bs(o, "\\n"); else if (c == '\t') cgx_bs(o, "\\t"); else if (c == '\r') cgx_bs(o, "\\r");
        else if (c < 32 || c == 127) cgx_bf(o, "\\x%02x", c);
        else if (c == 0xE2 && (unsigned char)s[1] == 0x80 && ((unsigned char)s[2] == 0xA8 || (unsigned char)s[2] == 0xA9)) { cgx_bs(o, (unsigned char)s[2] == 0xA8 ? "\\u2028" : "\\u2029"); s += 2; }
        else cgx_bn(o, s, 1);
    }
    cgx_bs(o, tpl ? "`" : "\"");
}
static CgxDoc *cjs_t(CgxJs *p, const char *s) { return cgx_dtext(JD, s); }
static CgxDoc *cjs_tb(CgxJs *p, CgxB *b) { CgxDoc *d = cgx_dtext(JD, b->p ? b->p : ""); free(b->p); b->p = NULL; return d; }

/* ---- tipos: da IR (CgxTy) para a tabela de ucgetype.c e dela para o texto TypeScript/JSDoc ---- */
static CgxTid cjs_elem_tid(CgxJs *p, int e) {
    switch (e) { case CGT_INT: return cgx_ty_prim(&p->T, TYK_INT); case CGT_FLOAT: return cgx_ty_prim(&p->T, TYK_FLOAT); case CGT_STR: return cgx_ty_prim(&p->T, TYK_STR); case CGT_BOOL: return cgx_ty_prim(&p->T, TYK_BOOL); default: return cgx_ty_prim(&p->T, TYK_ANY); }
}
static CgxTid cjs_tid(CgxJs *p, const CgxTy *t) {
    CgxTypes *T = &p->T;
    switch (t->k) {
        case CGT_INT: return cgx_ty_prim(T, TYK_INT);
        case CGT_FLOAT: return cgx_ty_prim(T, TYK_FLOAT);
        case CGT_STR: return cgx_ty_prim(T, TYK_STR);
        case CGT_BOOL: return cgx_ty_prim(T, TYK_BOOL);
        case CGT_VOID: return cgx_ty_prim(T, TYK_VOID);
        case CGT_LIST: return cgx_ty_list(T, cjs_elem_tid(p, t->e));
        case CGT_MAP: return cgx_ty_map(T, cjs_elem_tid(p, t->e));
        case CGT_CLASS: return cgx_ty_class(T, cgx_intern(p->g, t->cls));
        default: return cgx_ty_prim(T, TYK_ANY);
    }
}
static void cjs_tystr(CgxJs *p, CgxTid t, CgxB *o) {
    CgxTypes *T = &p->T;
    t = cgx_ty_find(T, t);
    const CgxTyNode *x = &T->t[t];
    switch (x->k) {
        case TYK_INT: case TYK_FLOAT: cgx_bs(o, "number"); break;
        case TYK_STR: cgx_bs(o, "string"); break;
        case TYK_BOOL: cgx_bs(o, "boolean"); break;
        case TYK_VOID: cgx_bs(o, "void"); break;
        case TYK_NEVER: cgx_bs(o, "never"); break;
        case TYK_LIST: { const int par = T->t[cgx_ty_find(T, x->a[0])].k == TYK_UNION || T->t[cgx_ty_find(T, x->a[0])].k == TYK_OPT || T->t[cgx_ty_find(T, x->a[0])].k == TYK_FN;
            if (par) { cgx_bs(o, "("); }
            cjs_tystr(p, x->a[0], o);
            if (par) { cgx_bs(o, ")"); }
            cgx_bs(o, "[]"); break; }
        case TYK_MAP: cgx_bs(o, "Map<string, "); cjs_tystr(p, x->a[0], o); cgx_bs(o, ">"); break;
        case TYK_OPT: cjs_tystr(p, x->a[0], o); cgx_bs(o, " | null"); break;
        case TYK_UNION: for (int i = 0; i < x->n; i++) { if (i) cgx_bs(o, " | "); cjs_tystr(p, x->a[i], o); } break;
        case TYK_TUPLE: cgx_bs(o, "["); for (int i = 0; i < x->n; i++) { if (i) cgx_bs(o, ", "); cjs_tystr(p, x->a[i], o); } cgx_bs(o, "]"); break;
        case TYK_FN: cgx_bs(o, "("); for (int i = 0; i + 1 < x->n; i++) { if (i) cgx_bs(o, ", "); cgx_bf(o, "a%d: ", i); cjs_tystr(p, x->a[i], o); } cgx_bs(o, ") => "); cjs_tystr(p, x->a[x->n - 1], o); break;
        case TYK_CLASS: { const char *nm = cgx_symname(p->g, x->sym); cgx_bs(o, !strcmp(nm, "Error") ? "unknown" : nm); break; }
        default: cgx_bs(o, "any"); break;
    }
}
static void cjs_type(CgxJs *p, const CgxTy *t, CgxB *o) { cjs_tystr(p, cjs_tid(p, t), o); }

/* ---- expressoes ---- */
enum { JP_TERN = 1, JP_OR = 2, JP_AND = 3, JP_EQ = 4, JP_REL = 5, JP_ADD = 6, JP_MUL = 7, JP_UN = 8, JP_POST = 9, JP_PRIM = 10 };
static CgxDoc *cjs_ex(CgxJs *p, const CgxNode *n, int minprec);
static CgxDoc *cjs_wrap(CgxJs *p, CgxDoc *d, int prec, int minprec) { return prec < minprec ? cgx_dcatn(JD, cjs_t(p, "("), d, cjs_t(p, ")"), NULL) : d; }
/* itens entre delimitadores: numa linha se couber, senao um por linha */
static CgxDoc *cjs_seq(CgxJs *p, const char *open, CgxDoc *const *items, int n, const char *close) {
    if (!n) return cgx_dtextf(JD, "%s%s", open, close);
    CgxDoc *in = NULL;
    for (int i = 0; i < n; i++) { CgxDoc *x = i ? cgx_dcatn(JD, cjs_t(p, ","), cgx_dline(JD), items[i], NULL) : items[i]; in = in ? cgx_dcat(JD, in, x) : x; }
    return cgx_dgroup(JD, cgx_dcatn(JD, cjs_t(p, open), cgx_dnest(JD, p->o->indent, cgx_dcat(JD, cgx_dsoft(JD), in)), cgx_dsoft(JD), cjs_t(p, close), NULL));
}
static CgxDoc *cjs_app(CgxJs *p, const char *head, CgxDoc *const *items, int n) { return cgx_dcat(JD, cjs_t(p, head), cjs_seq(p, "(", items, n, ")")); }
static CgxDoc *cjs_app1(CgxJs *p, const char *head, CgxDoc *a) { CgxDoc *it[1] = {a}; return cjs_app(p, head, it, 1); }
static CgxDoc *cjs_app2(CgxJs *p, const char *head, CgxDoc *a, CgxDoc *b) { CgxDoc *it[2] = {a, b}; return cjs_app(p, head, it, 2); }
static CgxDoc *cjs_app3(CgxJs *p, const char *head, CgxDoc *a, CgxDoc *b, CgxDoc *c) { CgxDoc *it[3] = {a, b, c}; return cjs_app(p, head, it, 3); }
static CgxDoc *cjs_h1(CgxJs *p, int h, const char *name, CgxDoc *a) { cjs_need(p, h); return cjs_app1(p, name, a); }
static CgxDoc *cjs_h2(CgxJs *p, int h, const char *name, CgxDoc *a, CgxDoc *b) { cjs_need(p, h); return cjs_app2(p, name, a, b); }
static CgxDoc *cjs_h3(CgxJs *p, int h, const char *name, CgxDoc *a, CgxDoc *b, CgxDoc *c) { cjs_need(p, h); return cjs_app3(p, name, a, b, c); }
static CgxDoc *cjs_meth(CgxJs *p, CgxDoc *recv, const char *m, CgxDoc *const *items, int n) { return cgx_dcatn(JD, recv, cgx_dtextf(JD, ".%s", m), cjs_seq(p, "(", items, n, ")"), NULL); }
static CgxDoc *cjs_meth0(CgxJs *p, CgxDoc *recv, const char *m) { return cgx_dcatn(JD, recv, cgx_dtextf(JD, ".%s()", m), NULL); }
static CgxDoc *cjs_meth1(CgxJs *p, CgxDoc *recv, const char *m, CgxDoc *a) { CgxDoc *it[1] = {a}; return cjs_meth(p, recv, m, it, 1); }

static int cjs_is_lit_int(const CgxNode *n, long long *v) {
    if (n->k == CGN_INT) { *v = n->iv; return 1; }
    if (n->k == CGN_UN && n->op == CGO_NEG && n->a->k == CGN_INT) { *v = -n->a->iv; return 1; }
    return 0;
}
static int cjs_is_lit_num(const CgxNode *n, double *v) {
    long long i; if (cjs_is_lit_int(n, &i)) { *v = (double)i; return 1; }
    if (n->k == CGN_FLT) { *v = n->dv; return 1; }
    if (n->k == CGN_UN && n->op == CGO_NEG && n->a->k == CGN_FLT) { *v = -n->a->dv; return 1; }
    return 0;
}
/* o valor e sempre >= 0? (literais, len, variavel de laco `for` que sobe a partir de um literal e nao e reatribuida) */
static int cjs_asg(const CgxJs *p, const CgxNode *d) { return !d || d->id <= 0 || d->id >= p->nasg || p->asg[d->id]; }
static int cjs_nonneg(const CgxJs *p, const CgxNode *n) {
    long long v;
    if (cjs_is_lit_int(n, &v)) return v >= 0;
    switch (n->k) {
        case CGN_CALL: return n->bi == CGB_LEN;
        case CGN_VAR: return n->ref && n->ref->k == CGN_FOR && !cjs_asg(p, n->ref) && cjs_is_lit_int(n->ref->a, &v) && v >= 0 && (!n->ref->c || n->ref->c->iv > 0);
        case CGN_BIN: return (n->op == CGO_ADD || n->op == CGO_MUL) && cjs_nonneg(p, n->a) && cjs_nonneg(p, n->b);
    }
    return 0;
}
static int cjs_pure(const CgxNode *n) {
    if (!n) return 1;
    if (n->k == CGN_CALL || n->k == CGN_MCALL) return 0;
    if (!cjs_pure(n->a) || !cjs_pure(n->b) || !cjs_pure(n->c)) return 0;
    for (int i = 0; i < n->b1.n; i++) if (!cjs_pure(n->b1.p[i])) return 0;
    return 1;
}
/* expressao que da o mesmo valor ao ser reavaliada mais tarde (limite de `for`, que o Python avalia uma vez) */
static int cjs_stable(const CgxJs *p, const CgxNode *n) {
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: return 1;
        case CGN_VAR: return n->ref && n->ref->k != CGN_RAW && !cjs_asg(p, n->ref) && (n->ty.k == CGT_INT || n->ty.k == CGT_FLOAT);
        case CGN_UN: return cjs_stable(p, n->a);
        case CGN_BIN: return cjs_stable(p, n->a) && cjs_stable(p, n->b);
    }
    return 0;
}
static int cjs_elem_scalar(int e) { return e == CGT_INT || e == CGT_FLOAT || e == CGT_STR || e == CGT_BOOL; }
/* condicao de tipo any: verdade do Python */
static CgxDoc *cjs_cond(CgxJs *p, const CgxNode *n, int minprec) {
    if (n->ty.k == CGT_ANY) return cjs_h1(p, H_TRUTH, "_truth", cjs_ex(p, n, 0));
    return cjs_ex(p, n, minprec);
}
static int cjs_cond_prec(const CgxNode *n, int prec) { return n->ty.k == CGT_ANY ? JP_POST : prec; }

/* leitura e escrita por indice, segundo o tipo da colecao */
static CgxDoc *cjs_idx_read(CgxJs *p, int ck, CgxDoc *c, CgxDoc *k) {
    switch (ck) {
        case CGT_LIST: return cjs_h2(p, H_AT, "_at", c, k);
        case CGT_STR: return cjs_h2(p, H_SAT, "_sat", c, k);
        case CGT_MAP: return cjs_h2(p, H_MGET, "_mget", c, k);
        default: return cjs_h2(p, H_IX, "_ix", c, k);
    }
}
static CgxDoc *cjs_idx_write(CgxJs *p, int ck, CgxDoc *c, CgxDoc *k, CgxDoc *v) {
    switch (ck) {
        case CGT_LIST: return cjs_h3(p, H_PUT, "_put", c, k, v);
        case CGT_MAP: { CgxDoc *it[2] = {k, v}; return cjs_meth(p, c, "set", it, 2); }
        default: return cjs_h3(p, H_IXPUT, "_ixput", c, k, v);
    }
}

static int cjs_triv(const CgxNode *n);
static int cjs_is_str_call(const CgxNode *n) { return n->k == CGN_CALL && n->bi == CGB_STR && n->b1.n == 1; }
/* `str(x)` como texto: sem o `String(...)` quando vai dentro de um modelo de texto */
static CgxDoc *cjs_str_of(CgxJs *p, const CgxNode *x, int in_tpl) {
    switch (x->ty.k) {
        case CGT_STR: return cjs_ex(p, x, in_tpl ? 0 : JP_PRIM);
        case CGT_INT: return in_tpl ? cjs_ex(p, x, 0) : cjs_app1(p, "String", cjs_ex(p, x, 0));
        case CGT_FLOAT: return cjs_h1(p, H_FSTR, "_fstr", cjs_ex(p, x, 0));
        case CGT_BOOL: return cgx_dcatn(JD, cjs_ex(p, x, JP_TERN + 1), cjs_t(p, " ? \"True\" : \"False\""), NULL);
        case CGT_LIST: case CGT_MAP: if (x->ty.e == CGT_FLOAT) return cjs_h2(p, H_STR, "_str", cjs_ex(p, x, 0), cjs_t(p, "true")); /* fallthrough */
        default: return cjs_h1(p, H_STR, "_str", cjs_ex(p, x, 0));
    }
}
/* cadeia de + entre str com pelo menos um literal e um str(...): modelo de texto (`a${x}b`) */
static void cjs_flat_add(const CgxNode *n, const CgxNode **v, int *k, int cap) {
    if (n->k == CGN_BIN && n->op == CGO_ADD && n->ty.k == CGT_STR && n->a->ty.k == CGT_STR && n->b->ty.k == CGT_STR) { cjs_flat_add(n->a, v, k, cap); cjs_flat_add(n->b, v, k, cap); }
    else if (*k < cap) v[(*k)++] = n;
}
static CgxDoc *cjs_template(CgxJs *p, const CgxNode *n) {
    const CgxNode *v[64]; int k = 0, lit = 0, conv = 0;
    cjs_flat_add(n, v, &k, 64);
    if (k < 2 || k >= 64) return NULL;
    for (int i = 0; i < k; i++) { if (v[i]->k == CGN_STR) lit++; else if (cjs_is_str_call(v[i])) conv++; }
    if (!lit || !conv) return NULL;
    CgxDoc *d = NULL; CgxB run = {0};
    for (int i = 0; i < k; i++) {
        if (v[i]->k == CGN_STR) { CgxB q = {0}; cjs_quote(v[i]->s, &q, 1); if (q.n >= 2) cgx_bn(&run, q.p + 1, q.n - 2); free(q.p); continue; }
        CgxDoc *e = cjs_is_str_call(v[i]) ? cjs_str_of(p, v[i]->b1.p[0], 1) : cjs_ex(p, v[i], 0);
        CgxB pb = {0}; cgx_bs(&pb, run.p ? run.p : ""); cgx_bs(&pb, "${"); cgx_render(e, 1000000, &pb); cgx_bs(&pb, "}");      /* a expressao dentro do modelo nao quebra */
        CgxDoc *piece = cjs_tb(p, &pb);
        if (run.p) { run.n = 0; run.p[0] = 0; }
        d = d ? cgx_dcat(JD, d, piece) : piece;
    }
    CgxB tb = {0}; cgx_bs(&tb, run.p ? run.p : ""); cgx_bs(&tb, "`"); free(run.p); CgxDoc *tail = cjs_tb(p, &tb);
    d = cgx_dcatn(JD, cjs_t(p, "`"), d ? d : cgx_dnil(JD), tail, NULL);
    return d;
}

/* expressao cujo tipo TypeScript e um literal (`"a"`, `3`, `c ? "a" : "b"`): comparar dois assim, ou guardar um num `const`, faz o tsc reclamar de tipos sem sobreposicao */
static int cjs_litish(const CgxNode *n) {
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: return 1;
        case CGN_UN: return n->op == CGO_NEG && cjs_litish(n->a);
        case CGN_TERN: return cjs_litish(n->b) && cjs_litish(n->c);
        case CGN_CALL:                                               /* `float(3)`, `int(3)`, `str("a")` e `str(flag)` saem sem conversao ou como `x ? "True" : "False"` */
            if (n->bi == CGB_FLOAT || n->bi == CGB_INT) return n->b1.n == 1 && (n->b1.p[0]->ty.k == CGT_INT || n->b1.p[0]->ty.k == CGT_FLOAT) && cjs_litish(n->b1.p[0]);
            if (n->bi == CGB_STR) return n->b1.n == 1 && (n->b1.p[0]->ty.k == CGT_BOOL || (n->b1.p[0]->ty.k == CGT_STR && cjs_litish(n->b1.p[0])));
            return 0;
    }
    return 0;
}
static const char *cjs_tsbase(int k) { return k == CGT_STR ? "string" : k == CGT_BOOL ? "boolean" : "number"; }
static CgxDoc *cjs_call(CgxJs *p, const CgxNode *n, int *prec);
static CgxDoc *cjs_ex0(CgxJs *p, const CgxNode *n, int *prec);
/* um inteiro que vira float (float(n), n / m, n * x): o JS pode ter -0 (0 * -3) onde o Python tem 0; `n + 0` o normaliza */
static CgxDoc *cjs_ex_z(CgxJs *p, const CgxNode *n, int minprec) {
    long long v;
    if (n->ty.k != CGT_INT || cjs_is_lit_int(n, &v) || cjs_nonneg(p, n)) return cjs_ex(p, n, minprec);
    return cjs_wrap(p, cgx_dcatn(JD, cjs_ex(p, n, JP_ADD), cjs_t(p, " + 0"), NULL), JP_ADD, minprec);
}
/* aritmetica (e compostos `x op= v`): L e opcional (leitura do alvo no composto) */
static CgxDoc *cjs_arith(CgxJs *p, int op, const CgxNode *ln, const CgxNode *rn, CgxDoc *Lover, int *prec) {
    long long lv; double dv;
    static const char *const sym[] = {"+", "-", "*", "/"};
    if (op == CGO_ADD || op == CGO_SUB || op == CGO_MUL) {
        const int pr = op == CGO_MUL ? JP_MUL : JP_ADD; *prec = pr;
        const int mx = op == CGO_MUL && ((ln->ty.k == CGT_INT && rn->ty.k == CGT_FLOAT) || (ln->ty.k == CGT_FLOAT && rn->ty.k == CGT_INT));
        CgxDoc *L = Lover ? Lover : mx && ln->ty.k == CGT_INT ? cjs_ex_z(p, ln, pr) : cjs_ex(p, ln, pr), *R = mx && rn->ty.k == CGT_INT ? cjs_ex_z(p, rn, pr + 1) : cjs_ex(p, rn, pr + 1);
        return cgx_dgroup(JD, cgx_dcatn(JD, L, cgx_dtextf(JD, " %s", sym[op]), cgx_dnest(JD, p->o->indent, cgx_dcatn(JD, cgx_dline(JD), R, NULL)), NULL));
    }
    const int mixed = (op == CGO_MUL || op == CGO_DIV) && ((ln->ty.k == CGT_INT && rn->ty.k == CGT_FLOAT) || (ln->ty.k == CGT_FLOAT && rn->ty.k == CGT_INT));
    const int zl = !Lover && ((op == CGO_DIV && ln->ty.k == CGT_INT && rn->ty.k == CGT_INT) || (mixed && ln->ty.k == CGT_INT)), zr = mixed && rn->ty.k == CGT_INT;
    if (op == CGO_DIV) {
        if (cjs_is_lit_num(rn, &dv) && dv != 0) { *prec = JP_MUL; return cgx_dcatn(JD, Lover ? Lover : zl ? cjs_ex_z(p, ln, JP_MUL) : cjs_ex(p, ln, JP_MUL), cjs_t(p, " / "), cjs_ex(p, rn, JP_MUL + 1), NULL); }
        *prec = JP_POST; return cjs_h2(p, H_DIV, "_div", Lover ? Lover : zl ? cjs_ex_z(p, ln, 0) : cjs_ex(p, ln, 0), zr ? cjs_ex_z(p, rn, 0) : cjs_ex(p, rn, 0));
    }
    if (op == CGO_IDIV) {
        *prec = JP_POST;
        if (cjs_is_lit_int(rn, &lv) && lv != 0) return cjs_app1(p, "Math.floor", cgx_dcatn(JD, Lover ? Lover : cjs_ex(p, ln, JP_MUL), cjs_t(p, " / "), cjs_ex(p, rn, JP_MUL + 1), NULL));
        return cjs_h2(p, H_FDIV, "_fdiv", Lover ? Lover : cjs_ex(p, ln, 0), cjs_ex(p, rn, 0));
    }
    /* CGO_MOD */
    if (!Lover && cjs_is_lit_int(rn, &lv) && lv > 0 && cjs_nonneg(p, ln)) { *prec = JP_MUL; return cgx_dcatn(JD, cjs_ex(p, ln, JP_MUL), cjs_t(p, " % "), cjs_ex(p, rn, JP_MUL + 1), NULL); }
    *prec = JP_POST; return cjs_h2(p, H_MOD, "_mod", Lover ? Lover : cjs_ex(p, ln, 0), cjs_ex(p, rn, 0));
}
/* comparacoes: str por ponto de codigo, igualdade profunda para lista, mapa e any */
static CgxDoc *cjs_compare(CgxJs *p, const CgxNode *n, int *prec) {
    const int op = n->op, ak = n->a->ty.k, bk = n->b->ty.k;
    static const char *const js[] = {"===", "!==", "<", "<=", ">", ">="};
    if (op == CGO_EQ || op == CGO_NE) {
        const int deep = ak == CGT_LIST || bk == CGT_LIST || ak == CGT_MAP || bk == CGT_MAP || (ak == CGT_ANY && bk == CGT_ANY);
        if (deep) {
            *prec = op == CGO_NE ? JP_UN : JP_POST;
            CgxDoc *d = cjs_h2(p, H_EQ, "_eq", cjs_ex(p, n->a, 0), cjs_ex(p, n->b, 0));
            return op == CGO_NE ? cgx_dcat(JD, cjs_t(p, "!"), d) : d;
        }
        *prec = JP_EQ;
        long long z;
        if (n->b->k == CGN_INT && n->b->iv == 0 && n->a->k == CGN_BIN && n->a->op == CGO_MOD && cjs_is_lit_int(n->a->b, &z) && z != 0)       /* a % k == 0: o sinal do resto nao importa */
            return cgx_dcatn(JD, cjs_ex(p, n->a->a, JP_MUL), cjs_t(p, " % "), cjs_ex(p, n->a->b, JP_MUL + 1), cjs_t(p, op == CGO_EQ ? " === 0" : " !== 0"), NULL);
        if (ak == CGT_BOOL && bk == CGT_BOOL) {                      /* `x == true` e `x`; o TS estreita `const f = true` e reclama de `f === false` */
            const CgxNode *lit = n->b->k == CGN_BOOL ? n->b : n->a->k == CGN_BOOL ? n->a : NULL, *o = lit == n->b ? n->a : n->b;
            if (lit) {
                const int same = (lit->iv != 0) == (op == CGO_EQ);
                if (same) return cjs_ex0(p, o, prec);
                *prec = JP_UN; return cgx_dcat(JD, cjs_t(p, "!"), cjs_ex(p, o, JP_UN));
            }
            if (p->ts) return cgx_dcatn(JD, cjs_t(p, "("), cjs_ex(p, n->a, 0), cgx_dtextf(JD, " as boolean) %s ", js[op - CGO_EQ]), cjs_ex(p, n->b, JP_EQ + 1), NULL);
        }
        if (p->ts && cjs_litish(n->a) && cjs_litish(n->b))
            return cgx_dcatn(JD, cjs_t(p, "("), cjs_ex(p, n->a, 0), cgx_dtextf(JD, " as %s) %s ", cjs_tsbase(n->a->ty.k), js[op - CGO_EQ]), cjs_ex(p, n->b, JP_EQ + 1), NULL);
        return cgx_dcatn(JD, cjs_ex(p, n->a, JP_EQ + 1), cgx_dtextf(JD, " %s ", js[op - CGO_EQ]), cjs_ex(p, n->b, JP_EQ + 1), NULL);
    }
    *prec = JP_REL;
    if (ak == CGT_STR || bk == CGT_STR) {
        cjs_need(p, ak == CGT_ANY || bk == CGT_ANY ? H_CMP : H_SCMP);
        return cgx_dcatn(JD, cjs_app2(p, ak == CGT_ANY || bk == CGT_ANY ? "_cmp" : "_scmp", cjs_ex(p, n->a, 0), cjs_ex(p, n->b, 0)), cgx_dtextf(JD, " %s 0", js[op - CGO_EQ]), NULL);
    }
    if (ak == CGT_ANY && bk == CGT_ANY) { cjs_need(p, H_CMP); return cgx_dcatn(JD, cjs_app2(p, "_cmp", cjs_ex(p, n->a, 0), cjs_ex(p, n->b, 0)), cgx_dtextf(JD, " %s 0", js[op - CGO_EQ]), NULL); }
    return cgx_dcatn(JD, cjs_ex(p, n->a, JP_REL + 1), cgx_dtextf(JD, " %s ", js[op - CGO_EQ]), cjs_ex(p, n->b, JP_REL + 1), NULL);
}
/* `x in c` e contains(c, x) */
static int cjs_triv(const CgxNode *n);
static CgxDoc *cjs_contains(CgxJs *p, const CgxNode *c, const CgxNode *x, int *prec) {
    *prec = JP_POST;
    if (!cjs_triv(x) && !cjs_triv(c)) return cjs_h2(p, H_IN, "_in", cjs_ex(p, x, 0), cjs_ex(p, c, 0));       /* o Python avalia `x in c` com x primeiro */
    switch (c->ty.k) {
        case CGT_STR: return cjs_meth1(p, cjs_ex(p, c, JP_POST), "includes", cjs_ex(p, x, 0));
        case CGT_MAP: return cjs_meth1(p, cjs_ex(p, c, JP_POST), "has", cjs_ex(p, x, 0));
        case CGT_LIST: if (cjs_elem_scalar(c->ty.e)) return cjs_meth1(p, cjs_ex(p, c, JP_POST), "includes", cjs_ex(p, x, 0)); break;
    }
    return cjs_h2(p, H_HAS, "_has", cjs_ex(p, c, 0), cjs_ex(p, x, 0));
}

static CgxDoc *cjs_call(CgxJs *p, const CgxNode *n, int *prec) {
    const CgxBlk *a = &n->b1; *prec = JP_POST;
    if (n->bi == 0) {                                   /* funcao ou construtor da especificacao */
        CgxDoc **it = (CgxDoc **)malloc(sizeof(CgxDoc *) * (size_t)(a->n + 1));
        for (int i = 0; i < a->n; i++) it[i] = cjs_ex(p, a->p[i], 0);
        CgxDoc *d = cgx_dcat(JD, cgx_dtextf(JD, "%s%s", n->ref && n->ref->k == CGN_CLASS ? "new " : "", n->s), cjs_seq(p, "(", it, a->n, ")"));
        free(it); return d;
    }
    const CgxNode *x0 = a->n > 0 ? a->p[0] : NULL, *x1 = a->n > 1 ? a->p[1] : NULL;
    const int k0 = x0 ? x0->ty.k : CGT_ANY;
    switch (n->bi) {
        case CGB_LEN:
            if (k0 == CGT_LIST) return cgx_dcat(JD, cjs_ex(p, x0, JP_POST), cjs_t(p, ".length"));
            if (k0 == CGT_MAP) return cgx_dcat(JD, cjs_ex(p, x0, JP_POST), cjs_t(p, ".size"));
            if (k0 == CGT_STR) return cjs_h1(p, H_SLEN, "_slen", cjs_ex(p, x0, 0));
            return cjs_h1(p, H_LEN, "_len", cjs_ex(p, x0, 0));
        case CGB_PUSH: return cjs_meth1(p, cjs_ex(p, x0, JP_POST), "push", cjs_ex(p, x1, 0));
        case CGB_POP: return cjs_h1(p, H_POP, "_pop", cjs_ex(p, x0, 0));
        case CGB_STR:
            if (k0 == CGT_STR) { int pr; CgxDoc *d = cjs_ex0(p, x0, &pr); *prec = pr; return d; }
            if (k0 == CGT_BOOL) *prec = JP_TERN;
            return cjs_str_of(p, x0, 0);
        case CGB_INT:
            if (k0 == CGT_INT) { int pr; CgxDoc *d = cjs_ex0(p, x0, &pr); *prec = pr; return d; }
            if (k0 == CGT_BOOL) { *prec = JP_TERN; return cgx_dcatn(JD, cjs_ex(p, x0, JP_TERN + 1), cjs_t(p, " ? 1 : 0"), NULL); }
            return cjs_h1(p, H_INT, "_int", cjs_ex(p, x0, 0));
        case CGB_FLOAT:
            if (k0 == CGT_INT) { *prec = JP_ADD; return cjs_ex_z(p, x0, 0); }
            if (k0 == CGT_FLOAT) { int pr; CgxDoc *d = cjs_ex0(p, x0, &pr); *prec = pr; return d; }
            if (k0 == CGT_BOOL) { *prec = JP_TERN; return cgx_dcatn(JD, cjs_ex(p, x0, JP_TERN + 1), cjs_t(p, " ? 1 : 0"), NULL); }
            return cjs_h1(p, H_FLOAT, "_float", cjs_ex(p, x0, 0));
        case CGB_UPPER: return cjs_meth0(p, cjs_ex(p, x0, JP_POST), "toUpperCase");
        case CGB_LOWER: return cjs_meth0(p, cjs_ex(p, x0, JP_POST), "toLowerCase");
        case CGB_TRIM: return cjs_h1(p, H_TRIM, "_trim", cjs_ex(p, x0, 0));
        case CGB_SPLIT: return cjs_h2(p, H_SPLIT, "_split", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0));
        case CGB_WORDS: return cjs_h1(p, H_WORDS, "_words", cjs_ex(p, x0, 0));
        case CGB_JOIN:
            if (!cjs_triv(x0) && !cjs_triv(x1)) return cjs_h2(p, H_JOIN, "_join", cjs_ex(p, x1, 0), cjs_ex(p, x0, 0));      /* o Python avalia o separador primeiro */
            return cjs_meth1(p, cjs_ex(p, x0, JP_POST), "join", cjs_ex(p, x1, 0));
        case CGB_CONTAINS: return cjs_contains(p, x0, x1, prec);
        case CGB_INDEXOF: return cjs_h2(p, H_FIND, "_find", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0));
        case CGB_REPLACE: {
            const CgxNode *x2 = a->p[2];
            if (x1->k == CGN_STR && x1->s[0]) return cjs_meth1(p, cjs_meth1(p, cjs_ex(p, x0, JP_POST), "split", cjs_ex(p, x1, 0)), "join", cjs_ex(p, x2, 0));
            return cjs_h3(p, H_REPLACE, "_replace", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0), cjs_ex(p, x2, 0));
        }
        case CGB_STARTS: return cjs_meth1(p, cjs_ex(p, x0, JP_POST), "startsWith", cjs_ex(p, x1, 0));
        case CGB_ENDS: return cjs_meth1(p, cjs_ex(p, x0, JP_POST), "endsWith", cjs_ex(p, x1, 0));
        case CGB_SLICE: {
            CgxDoc *it[2]; int k = 0; it[k++] = cjs_ex(p, x1, 0); if (a->n > 2) it[k++] = cjs_ex(p, a->p[2], 0);
            if (k0 == CGT_LIST) return cjs_meth(p, cjs_ex(p, x0, JP_POST), "slice", it, k);
            cjs_need(p, k0 == CGT_STR ? H_SSLICE : H_SLICE);
            CgxDoc *all[3]; all[0] = cjs_ex(p, x0, 0); for (int i = 0; i < k; i++) all[i + 1] = it[i];
            return cjs_app(p, k0 == CGT_STR ? "_sslice" : "_slice", all, k + 1);
        }
        case CGB_KEYS:
            if (k0 == CGT_MAP) { *prec = JP_PRIM; return cgx_dcatn(JD, cjs_t(p, "[...") , cjs_meth0(p, cjs_ex(p, x0, JP_POST), "keys"), cjs_t(p, "]"), NULL); }
            return cjs_h1(p, H_KEYS, "_keys", cjs_ex(p, x0, 0));
        case CGB_PRINT: {
            CgxDoc *s = k0 == CGT_STR ? cjs_ex(p, x0, 0) : k0 == CGT_INT ? cjs_app1(p, "String", cjs_ex(p, x0, 0)) : cjs_str_of(p, x0, 0);
            return cjs_app1(p, "console.log", s);
        }
        case CGB_ABS: return cjs_app1(p, "Math.abs", cjs_ex(p, x0, 0));
        case CGB_MIN: return cjs_app2(p, "Math.min", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0));
        case CGB_MAX: return cjs_app2(p, "Math.max", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0));
        case CGB_SQRT: return cjs_h1(p, H_SQRT, "_sqrt", cjs_ex(p, x0, 0));
        case CGB_FLOOR: return cjs_app1(p, "Math.floor", cjs_ex(p, x0, 0));
        case CGB_POW: return cjs_h2(p, H_POW, "_pow", cjs_ex(p, x0, 0), cjs_ex(p, x1, 0));
        case CGB_SUM: return cjs_h1(p, H_SUM, "_sum", cjs_ex(p, x0, 0));
        case CGB_SORT:
            if (k0 == CGT_LIST && (x0->ty.e == CGT_INT || x0->ty.e == CGT_FLOAT)) { *prec = JP_POST; return cgx_dcatn(JD, cjs_t(p, "[..."), cjs_ex(p, x0, 0), cjs_t(p, "].sort((a, b) => a - b)"), NULL); }
            if (k0 == CGT_LIST && x0->ty.e == CGT_STR) { cjs_need(p, H_SCMP); return cgx_dcatn(JD, cjs_t(p, "[..."), cjs_ex(p, x0, 0), cjs_t(p, "].sort(_scmp)"), NULL); }
            return cjs_h1(p, H_SORT, "_sort", cjs_ex(p, x0, 0));
        case CGB_REVERSE:
            if (k0 == CGT_LIST) return cgx_dcatn(JD, cjs_t(p, "[..."), cjs_ex(p, x0, 0), cjs_t(p, "].reverse()"), NULL);
            return cjs_h1(p, H_REV, "_rev", cjs_ex(p, x0, 0));
        case CGB_MATCHES: return cjs_meth1(p, cgx_dcatn(JD, cjs_t(p, "new RegExp("), cjs_ex(p, x1, 0), cjs_t(p, ")"), NULL), "test", cjs_ex(p, x0, 0));
        case CGB_ERRMSG: return cjs_h1(p, H_ERRMSG, "_errmsg", cjs_ex(p, x0, 0));
    }
    cgx_err(p->g, n->line, "javascript: built-in `%s` has no JavaScript form yet", n->s);
    return cgx_dnil(JD);
}
static CgxDoc *cjs_ex0(CgxJs *p, const CgxNode *n, int *prec) {
    CgxB b = {0}; *prec = JP_PRIM;
    switch (n->k) {
        case CGN_INT:
            if (n->iv > 9007199254740991LL || n->iv < -9007199254740991LL) cgx_err(p->g, n->line, "javascript: the integer %lld is outside the exact range of a JavaScript number (2^53); use a float or a `raw` block", n->iv);
            cgx_bf(&b, "%lld", n->iv); if (n->iv < 0) *prec = JP_UN; break;
        case CGN_FLT: {
            char t[64]; cgx_fmt_float(n->dv, t, sizeof t);
            const size_t l = strlen(t); if (l > 2 && !strcmp(t + l - 2, ".0") && !strpbrk(t, "eE")) t[l - 2] = 0;
            cgx_bs(&b, t); if (signbit(n->dv)) *prec = JP_UN; break;
        }
        case CGN_STR: cjs_quote(n->s, &b, 0); break;
        case CGN_BOOL: cgx_bs(&b, n->iv ? "true" : "false"); break;
        case CGN_NULL: cgx_bs(&b, "null"); break;
        case CGN_VAR: cgx_bs(&b, !strcmp(n->s, "self") ? "this" : n->s); break;
        case CGN_LIST: {
            free(b.p); CgxDoc **it = (CgxDoc **)malloc(sizeof(CgxDoc *) * (size_t)(n->b1.n + 1));
            for (int i = 0; i < n->b1.n; i++) it[i] = cjs_ex(p, n->b1.p[i], 0);
            CgxDoc *d = cjs_seq(p, "[", it, n->b1.n, "]"); free(it); return d;
        }
        case CGN_MAP: {
            free(b.p); const int m = n->b1.n / 2; CgxDoc **it = (CgxDoc **)malloc(sizeof(CgxDoc *) * (size_t)(m + 1));
            for (int i = 0; i < m; i++) it[i] = cgx_dcatn(JD, cjs_t(p, "["), cjs_ex(p, n->b1.p[2 * i], 0), cjs_t(p, ", "), cjs_ex(p, n->b1.p[2 * i + 1], 0), cjs_t(p, "]"), NULL);
            CgxB tb = {0}; cgx_bs(&tb, p->ts ? "new Map<string, " : "new Map");
            if (p->ts) { cjs_tystr(p, cjs_elem_tid(p, n->ty.e), &tb); cgx_bs(&tb, ">"); }
            CgxDoc *d;
            if (m) { CgxDoc *arr = cjs_seq(p, "[", it, m, "]"); CgxDoc *one[1] = {arr}; d = cgx_dcat(JD, cjs_tb(p, &tb), cjs_seq(p, "(", one, 1, ")")); }
            else { cgx_bs(&tb, "()"); d = cjs_tb(p, &tb); }
            free(it); *prec = JP_POST; return d;
        }
        case CGN_INDEX: free(b.p); *prec = JP_POST; return cjs_idx_read(p, n->a->ty.k, cjs_ex(p, n->a, 0), cjs_ex(p, n->b, 0));
        case CGN_FIELD: free(b.p); *prec = JP_POST; return cgx_dcatn(JD, cjs_ex(p, n->a, JP_POST), cgx_dtextf(JD, ".%s", n->s), NULL);
        case CGN_MCALL: {
            free(b.p); *prec = JP_POST; CgxDoc **it = (CgxDoc **)malloc(sizeof(CgxDoc *) * (size_t)(n->b1.n + 1));
            for (int i = 0; i < n->b1.n; i++) it[i] = cjs_ex(p, n->b1.p[i], 0);
            CgxDoc *d = cgx_dcatn(JD, cjs_ex(p, n->a, JP_POST), cgx_dtextf(JD, ".%s", n->s), cjs_seq(p, "(", it, n->b1.n, ")"), NULL); free(it); return d;
        }
        case CGN_CALL: free(b.p); if (n->bi == 0 && n->ref && n->ref->k == CGN_CLASS) { *prec = JP_POST; } return cjs_call(p, n, prec);
        case CGN_UN: free(b.p);
            if (n->op == CGO_NOT) {
                const CgxNode *x = n->a;
                if (x->k == CGN_BIN && (x->op == CGO_EQ || x->op == CGO_NE) && x->a->ty.k != CGT_LIST && x->a->ty.k != CGT_MAP && x->b->ty.k != CGT_LIST && x->b->ty.k != CGT_MAP && !(x->a->ty.k == CGT_ANY && x->b->ty.k == CGT_ANY)) {
                    CgxNode flip = *x; flip.op = x->op == CGO_EQ ? CGO_NE : CGO_EQ; return cjs_compare(p, &flip, prec);
                }
                *prec = JP_UN; return cgx_dcat(JD, cjs_t(p, "!"), x->ty.k == CGT_ANY ? cjs_h1(p, H_TRUTH, "_truth", cjs_ex(p, x, 0)) : cjs_ex(p, x, JP_UN));
            }
            *prec = JP_UN; return cgx_dcat(JD, cjs_t(p, "-"), cjs_ex(p, n->a, JP_POST));
        case CGN_BIN:
            free(b.p);
            switch (n->op) {
                case CGO_ADD: { CgxDoc *t = cjs_template(p, n); if (t) return t; return cjs_arith(p, n->op, n->a, n->b, NULL, prec); }
                case CGO_SUB: case CGO_MUL: case CGO_DIV: case CGO_IDIV: case CGO_MOD: return cjs_arith(p, n->op, n->a, n->b, NULL, prec);
                case CGO_EQ: case CGO_NE: case CGO_LT: case CGO_LE: case CGO_GT: case CGO_GE: return cjs_compare(p, n, prec);
                case CGO_IN: return cjs_contains(p, n->b, n->a, prec);
                case CGO_AND: case CGO_OR: {
                    const int pr = n->op == CGO_AND ? JP_AND : JP_OR; *prec = pr;
                    CgxDoc *L = n->a->ty.k == CGT_ANY ? cjs_h1(p, H_TRUTH, "_truth", cjs_ex(p, n->a, 0)) : cjs_ex(p, n->a, pr);
                    CgxDoc *R = n->b->ty.k == CGT_ANY ? cjs_h1(p, H_TRUTH, "_truth", cjs_ex(p, n->b, 0)) : cjs_ex(p, n->b, pr + 1);
                    return cgx_dgroup(JD, cgx_dcatn(JD, L, cgx_dtextf(JD, " %s", n->op == CGO_AND ? "&&" : "||"), cgx_dnest(JD, p->o->indent, cgx_dcatn(JD, cgx_dline(JD), R, NULL)), NULL));
                }
            }
            cgx_err(p->g, n->line, "javascript: unexpected operator %d", n->op); return cgx_dnil(JD);
        case CGN_TERN: free(b.p); *prec = JP_TERN;
            return cgx_dcatn(JD, cjs_cond(p, n->a, JP_TERN + 1), cjs_t(p, " ? "), cjs_ex(p, n->b, JP_TERN), cjs_t(p, " : "), cjs_ex(p, n->c, JP_TERN), NULL);
        default: cgx_err(p->g, n->line, "javascript: unexpected expression node %d", n->k); free(b.p); return cgx_dnil(JD);
    }
    CgxDoc *d = cgx_dtext(JD, b.p ? b.p : ""); free(b.p); return d;
}
static CgxDoc *cjs_ex(CgxJs *p, const CgxNode *n, int minprec) { int prec; CgxDoc *d = cjs_ex0(p, n, &prec); return cjs_wrap(p, d, prec, minprec); }

/* ---- instrucoes ---- */
static CgxDoc *cjs_stmt(CgxJs *p, const CgxNode *s);
static CgxDoc *cjs_block(CgxJs *p, const CgxBlk *b, CgxDoc *prologue) {
    CgxDoc *in = prologue; int any = prologue != NULL;
    for (int i = 0; i < b->n; i++) {
        const CgxNode *s = b->p[i];
        if ((s->k == CGN_COMMENT || s->k == CGN_DOC) && !p->o->comments) continue;
        CgxDoc *d = cjs_stmt(p, s);
        if (!d) continue;
        CgxDoc *x = cgx_dcat(JD, cgx_dhard(JD), d);
        in = in ? cgx_dcat(JD, in, x) : x; any = 1;
    }
    if (!any) return cjs_t(p, "{}");
    return cgx_dcatn(JD, cjs_t(p, "{"), cgx_dnest(JD, p->o->indent, in), cgx_dhard(JD), cjs_t(p, "}"), NULL);
}
static CgxDoc *cjs_raw(CgxJs *p, const CgxNode *s) {
    const char *src = p->ts && s->raw[2] ? s->raw[2] : s->raw[1];
    if (!src) { cgx_err(p->g, s->line, "%s: this `raw` block has no `raw %s` version (it has: %s%s%s%s)", p->ts ? "typescript" : "javascript", p->ts ? "ts" : "js", s->raw[0] ? "python " : "", s->raw[1] ? "js " : "", s->raw[2] ? "ts " : "", s->raw[3] ? "c" : ""); return NULL; }
    CgxDoc *r = NULL; const char *q = src;
    while (*q) {
        const char *e = strchr(q, '\n'); size_t l = e ? (size_t)(e - q) : strlen(q);
        char *ln = (char *)malloc(l + 1); memcpy(ln, q, l); ln[l] = 0;
        CgxDoc *t = cgx_dtext(JD, ln); free(ln);
        r = r ? cgx_dcatn(JD, r, cgx_dhard(JD), t, NULL) : t;
        q += l + (e ? 1 : 0);
    }
    return r ? r : cgx_dnil(JD);
}
/* anotacao de tipo de uma declaracao `let` em TypeScript: so onde o TS nao a infere sozinho ou o autor a escreveu */
static int cjs_let_annot(const CgxNode *s, int cst) {
    if (s->ty.k == CGT_VOID) return 0;
    if (cst && cjs_litish(s->a) && (s->ty.k == CGT_INT || s->ty.k == CGT_FLOAT || s->ty.k == CGT_STR || s->ty.k == CGT_BOOL)) return 1;      /* `const x: string = "a"`: sem tipo literal */
    const int lit = (s->a->k == CGN_INT && s->ty.k == CGT_INT) || (s->a->k == CGN_FLT && s->ty.k == CGT_FLOAT) || (s->a->k == CGN_STR && s->ty.k == CGT_STR) || (s->a->k == CGN_BOOL && s->ty.k == CGT_BOOL);
    if (lit || (s->a->k == CGN_CALL && s->a->ref && s->a->ref->k == CGN_CLASS)) return 0;       /* `new C(...)` ja diz o tipo */
    if ((s->flag & 2) && (s->ty.k == CGT_INT || s->ty.k == CGT_FLOAT || s->ty.k == CGT_STR || s->ty.k == CGT_BOOL)) return 0;
    return 1;
}
/* expressao que nao tem efeito nem levanta erro: a ordem em que e avaliada nao se nota */
static int cjs_triv(const CgxNode *n) {
    switch (n->k) {
        case CGN_INT: case CGN_FLT: case CGN_STR: case CGN_BOOL: case CGN_NULL: case CGN_VAR: return 1;
        case CGN_FIELD: case CGN_UN: return cjs_triv(n->a);
        case CGN_BIN: return n->op != CGO_DIV && n->op != CGO_IDIV && n->op != CGO_MOD && n->op != CGO_IN && cjs_triv(n->a) && cjs_triv(n->b);
        case CGN_LIST: case CGN_MAP: for (int i = 0; i < n->b1.n; i++) if (!cjs_triv(n->b1.p[i])) return 0; return 1;
    }
    return 0;
}
static CgxDoc *cjs_t_end(CgxJs *p, CgxDoc *d) { return cgx_dcat(JD, d, cjs_t(p, ";")); }
static CgxDoc *cjs_assign(CgxJs *p, const CgxNode *s) {
    static const char *const as[] = {"+=", "-=", "*="};
    const CgxNode *t = s->a;
    if (t->k == CGN_INDEX) {
        const int ck = t->a->ty.k;
        const int hoist = s->op >= 0 && (!cjs_pure(t->a) || !cjs_pure(t->b));
        if (!hoist && s->op < 0 && !cjs_triv(s->b) && (!cjs_triv(t->a) || !cjs_triv(t->b))) {      /* `c[k] = v`: o Python avalia v ANTES de c e k */
            const int id = ++p->ntmp;
            return cgx_dcatn(JD, cjs_t(p, "{"), cgx_dnest(JD, p->o->indent, cgx_dcatn(JD, cgx_dhard(JD), cgx_dtextf(JD, "const _v%d = ", id), cjs_ex(p, s->b, 0), cjs_t(p, ";"), cgx_dhard(JD),
                cjs_t_end(p, cjs_idx_write(p, ck, cjs_ex(p, t->a, 0), cjs_ex(p, t->b, 0), cgx_dtextf(JD, "_v%d", id))), NULL)), cgx_dhard(JD), cjs_t(p, "}"), NULL);
        }
        if (!hoist) {
            CgxDoc *c = cjs_ex(p, t->a, 0), *k = cjs_ex(p, t->b, 0), *v;
            if (s->op < 0) v = cjs_ex(p, s->b, 0);
            else { int pr; v = cjs_arith(p, s->op, t->a, s->b, cjs_idx_read(p, ck, cjs_ex(p, t->a, 0), cjs_ex(p, t->b, 0)), &pr); }
            return cjs_t_end(p, cjs_idx_write(p, ck, c, k, v));
        }
        const int id = ++p->ntmp; CgxDoc *c = cgx_dtextf(JD, "_c%d", id), *k = cgx_dtextf(JD, "_k%d", id); int pr;
        CgxDoc *v = cjs_arith(p, s->op, t->a, s->b, cjs_idx_read(p, ck, cgx_dtextf(JD, "_c%d", id), cgx_dtextf(JD, "_k%d", id)), &pr);
        return cgx_dcatn(JD, cjs_t(p, "{"), cgx_dnest(JD, p->o->indent, cgx_dcatn(JD, cgx_dhard(JD), cgx_dtextf(JD, "const _c%d = ", id), cjs_ex(p, t->a, 0), cjs_t(p, ";"),
            cgx_dhard(JD), cgx_dtextf(JD, "const _k%d = ", id), cjs_ex(p, t->b, 0), cjs_t(p, ";"), cgx_dhard(JD), cjs_t_end(p, cjs_idx_write(p, ck, c, k, v)), NULL)), cgx_dhard(JD), cjs_t(p, "}"), NULL);
    }
    if (t->k == CGN_FIELD && s->op < 0 && !cjs_triv(s->b) && !cjs_triv(t->a)) {                  /* `f().x = v`: v primeiro */
        const int id = ++p->ntmp;
        return cgx_dcatn(JD, cjs_t(p, "{"), cgx_dnest(JD, p->o->indent, cgx_dcatn(JD, cgx_dhard(JD), cgx_dtextf(JD, "const _v%d = ", id), cjs_ex(p, s->b, 0), cjs_t(p, ";"), cgx_dhard(JD),
            cjs_ex(p, t, 0), cgx_dtextf(JD, " = _v%d;", id), NULL)), cgx_dhard(JD), cjs_t(p, "}"), NULL);
    }
    CgxDoc *T = cjs_ex(p, t, 0);
    if (s->op < 0) return cgx_dcatn(JD, T, cjs_t(p, " = "), cjs_ex(p, s->b, 0), cjs_t(p, ";"), NULL);
    if (s->op == CGO_ADD || s->op == CGO_SUB || s->op == CGO_MUL) return cgx_dcatn(JD, T, cgx_dtextf(JD, " %s ", as[s->op]), cjs_ex(p, s->b, 0), cjs_t(p, ";"), NULL);
    int pr; CgxDoc *v = cjs_arith(p, s->op, t, s->b, cjs_ex(p, t, 0), &pr);
    return cgx_dcatn(JD, T, cjs_t(p, " = "), v, cjs_t(p, ";"), NULL);
}
static CgxDoc *cjs_line_comment(CgxJs *p, const char *text) { CgxB b = {0}; cgx_bs(&b, "// "); cgx_bs(&b, text); return cjs_tb(p, &b); }
static int cjs_uses(const CgxNode *n, const CgxNode *ref) {
    if (!n) return 0;
    if (n->k == CGN_VAR && n->ref == ref) return 1;
    if (cjs_uses(n->a, ref) || cjs_uses(n->b, ref) || cjs_uses(n->c, ref)) return 1;
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) if (cjs_uses(bl[k]->p[i], ref)) return 1;
    return 0;
}
/* o corpo do laco pode inserir chave num mapa? (escrita por indice num mapa ou any, ou qualquer chamada que nao seja embutida) */
static int cjs_may_mutate_map(const CgxBlk *b);
static int cjs_mm(const CgxNode *n) {
    if (!n) return 0;
    if (n->k == CGN_ASSIGN && n->a && n->a->k == CGN_INDEX && (n->a->a->ty.k == CGT_MAP || n->a->a->ty.k == CGT_ANY)) return 1;
    if ((n->k == CGN_CALL && n->bi == 0) || n->k == CGN_MCALL || n->k == CGN_RAW) return 1;
    if (cjs_mm(n->a) || cjs_mm(n->b) || cjs_mm(n->c)) return 1;
    return cjs_may_mutate_map(&n->b1) || cjs_may_mutate_map(&n->b2) || cjs_may_mutate_map(&n->b3);
}
static int cjs_may_mutate_map(const CgxBlk *b) { for (int i = 0; i < b->n; i++) if (cjs_mm(b->p[i])) return 1; return 0; }
static CgxDoc *cjs_stmt(CgxJs *p, const CgxNode *s) {
    switch (s->k) {
        case CGN_COMMENT: if (!s->defs) return NULL; return cjs_line_comment(p, s->defs);
        case CGN_DOC: return cjs_line_comment(p, s->defs);
        case CGN_RAW: return cjs_raw(p, s);
        case CGN_LET: {
            cjs_name(p, s->line, s->s, 0);
            const int cst = (s->flag & 1) || !cjs_asg(p, s);
            CgxB h = {0};
            if (p->ts && s->ty.k == CGT_CLASS && (!cst || s->a->k == CGN_NULL)) cgx_bs(&h, ": any");          /* a UIR nao tem tipo opcional: classe que recebe null fica any */
            else if (p->ts && cjs_let_annot(s, cst)) { cgx_bs(&h, ": "); cjs_type(p, &s->ty, &h); }
            CgxDoc *d = cgx_dcatn(JD, cgx_dtextf(JD, "%s %s%s = ", cst ? "const" : "let", s->s, h.p ? h.p : ""), cjs_ex(p, s->a, 0), cjs_t(p, ";"), NULL); free(h.p); return d;
        }
        case CGN_ASSIGN: return cjs_assign(p, s);
        case CGN_RET: return s->a ? cgx_dcatn(JD, cjs_t(p, "return "), cjs_ex(p, s->a, 0), cjs_t(p, ";"), NULL) : cjs_t(p, "return;");
        case CGN_BREAK: return cjs_t(p, "break;");
        case CGN_CONT: return cjs_t(p, "continue;");
        case CGN_EXPR: return cjs_t_end(p, cjs_ex(p, s->a, 0));
        case CGN_THROW: {
            if (s->a->ty.k == CGT_CLASS && !strcmp(s->a->ty.cls, "Error")) return cgx_dcatn(JD, cjs_t(p, "throw "), cjs_ex(p, s->a, 0), cjs_t(p, ";"), NULL);
            if (s->a->ty.k == CGT_STR) return cgx_dcatn(JD, cjs_t(p, "throw "), cjs_app1(p, "new Error", cjs_ex(p, s->a, 0)), cjs_t(p, ";"), NULL);
            return cgx_dcatn(JD, cjs_t(p, "throw "), cjs_h1(p, H_EXC, "_exc", cjs_ex(p, s->a, 0)), cjs_t(p, ";"), NULL);
        }
        case CGN_IF: {
            CgxDoc *d = cgx_dcatn(JD, cjs_t(p, "if ("), cjs_cond(p, s->a, 0), cjs_t(p, ") "), cjs_block(p, &s->b1, NULL), NULL);
            if (s->b2.n == 1 && s->b2.p[0]->k == CGN_IF && s->b2.p[0]->flag) d = cgx_dcatn(JD, d, cjs_t(p, " else "), cjs_stmt(p, s->b2.p[0]), NULL);
            else if (s->b2.n) d = cgx_dcatn(JD, d, cjs_t(p, " else "), cjs_block(p, &s->b2, NULL), NULL);
            return d;
        }
        case CGN_WHILE: return cgx_dcatn(JD, cjs_t(p, "while ("), cjs_cond(p, s->a, 0), cjs_t(p, ") "), cjs_block(p, &s->b1, NULL), NULL);
        case CGN_FOR: {
            cjs_name(p, s->line, s->s, 0);
            const int up = !s->c || s->c->iv > 0; const long long step = s->c ? (s->c->iv < 0 ? -s->c->iv : s->c->iv) : 1;
            const int shadow = cjs_asg(p, s), stable = cjs_stable(p, s->b), id = ++p->ntmp;
            char iv[48], ev[48]; if (shadow) snprintf(iv, sizeof iv, "_i%d", id); else snprintf(iv, sizeof iv, "%s", s->s);
            snprintf(ev, sizeof ev, "_e%d", id);
            CgxDoc *init = cgx_dcatn(JD, cgx_dtextf(JD, "let %s = ", iv), cjs_ex(p, s->a, 0), NULL);
            CgxDoc *endd = cjs_ex(p, s->b, 0);
            if (!stable) init = cgx_dcatn(JD, init, cgx_dtextf(JD, ", %s = ", ev), endd, NULL);
            CgxDoc *cond = cgx_dcatn(JD, cgx_dtextf(JD, "%s %s ", iv, up ? "<" : ">"), stable ? cjs_ex(p, s->b, JP_REL + 1) : cgx_dtext(JD, ev), NULL);
            CgxDoc *inc = step == 1 ? cgx_dtextf(JD, "%s%s", iv, up ? "++" : "--") : cgx_dtextf(JD, "%s %s %lld", iv, up ? "+=" : "-=", step);
            CgxDoc *pro = shadow ? cgx_dcatn(JD, cgx_dhard(JD), cgx_dtextf(JD, "let %s = %s;", s->s, iv), NULL) : NULL;
            return cgx_dcatn(JD, cjs_t(p, "for ("), init, cjs_t(p, "; "), cond, cjs_t(p, "; "), inc, cjs_t(p, ") "), cjs_block(p, &s->b1, pro), NULL);
        }
        case CGN_EACH: {
            cjs_name(p, s->line, s->s, 0); if (s->s2) cjs_name(p, s->line, s->s2, 0);
            const int ck = s->a->ty.k; const char *kw = cjs_asg(p, s) ? "let" : "const";
            CgxDoc *coll, *vars;
            const int mut = ck == CGT_MAP && cjs_may_mutate_map(&s->b1);           /* o Python levanta erro se o mapa muda de tamanho durante o laco */
            if (s->s2) { vars = cgx_dtextf(JD, "%s [%s, %s]", kw, s->s, s->s2); coll = ck == CGT_MAP ? (mut ? cjs_h2(p, H_MAPIT, "_mapit", cjs_ex(p, s->a, 0), cjs_t(p, "true")) : cjs_ex(p, s->a, 0)) : cjs_h1(p, H_ITEMS, "_items", cjs_ex(p, s->a, 0)); }
            else {
                vars = cgx_dtextf(JD, "%s %s", kw, s->s);
                coll = ck == CGT_MAP ? (mut ? cjs_h2(p, H_MAPIT, "_mapit", cjs_ex(p, s->a, 0), cjs_t(p, "false")) : cjs_meth0(p, cjs_ex(p, s->a, JP_POST), "keys")) : ck == CGT_ANY ? cjs_h1(p, H_ITER, "_iter", cjs_ex(p, s->a, 0)) : cjs_ex(p, s->a, 0);
            }
            return cgx_dcatn(JD, cjs_t(p, "for ("), vars, cjs_t(p, " of "), coll, cjs_t(p, ") "), cjs_block(p, &s->b1, NULL), NULL);
        }
        case CGN_TRY: {
            CgxDoc *d = cgx_dcatn(JD, cjs_t(p, "try "), cjs_block(p, &s->b1, NULL), NULL);
            if (s->flag) {
                int used = 0; if (s->s2) { cjs_name(p, s->line, s->s2, 0); for (int i = 0; i < s->b2.n && !used; i++) used = cjs_uses(s->b2.p[i], s); }
                d = cgx_dcatn(JD, d, used ? cgx_dtextf(JD, " catch (%s) ", s->s2) : cjs_t(p, " catch "), cjs_block(p, &s->b2, NULL), NULL);
            }
            if (s->iv) d = cgx_dcatn(JD, d, cjs_t(p, " finally "), cjs_block(p, &s->b3, NULL), NULL);
            return d;
        }
    }
    cgx_err(p->g, s->line, "javascript: unexpected statement node %d", s->k);
    return NULL;
}

/* ---- funcoes e classes ---- */
static CgxDoc *cjs_zero(CgxJs *p, const CgxTy *t) {
    switch (t->k) {
        case CGT_INT: case CGT_FLOAT: return cjs_t(p, "0");
        case CGT_STR: return cjs_t(p, "\"\"");
        case CGT_BOOL: return cjs_t(p, "false");
        case CGT_LIST: return cjs_t(p, "[]");
        case CGT_MAP: { CgxB b = {0}; if (p->ts) { cgx_bs(&b, "new Map<string, "); cjs_tystr(p, cjs_elem_tid(p, t->e), &b); cgx_bs(&b, ">()"); } else cgx_bs(&b, "new Map()"); return cjs_tb(p, &b); }
        default: return cjs_t(p, "null");
    }
}
static void cjs_jsdoc_clean(const char *s, CgxB *o) { for (; *s; s++) { if (s[0] == '*' && s[1] == '/') { cgx_bs(o, "* /"); s++; } else cgx_bn(o, s, 1); } }
static int cjs_returns_null(const CgxBlk *b) {
    for (int i = 0; i < b->n; i++) {
        const CgxNode *n = b->p[i];
        if (n->k == CGN_RET && n->a && n->a->k == CGN_NULL) return 1;
        if (cjs_returns_null(&n->b1) || cjs_returns_null(&n->b2) || cjs_returns_null(&n->b3)) return 1;
    }
    return 0;
}
static CgxDoc *cjs_params(CgxJs *p, const CgxNode *fn) {
    CgxDoc **it = (CgxDoc **)malloc(sizeof(CgxDoc *) * (size_t)(fn->npar + 1));
    for (int i = 0; i < fn->npar; i++) {
        const CgxParam *pr = &fn->par[i]; CgxB b = {0};
        cjs_name(p, pr->line, pr->name, 0);
        cgx_bs(&b, pr->name);
        if (p->ts) { cgx_bs(&b, ": "); if (pr->ty.k == CGT_CLASS && pr->def && pr->def->k == CGN_NULL) cgx_bs(&b, "any"); else cjs_type(p, &pr->ty, &b); }
        CgxDoc *d = cjs_tb(p, &b);
        if (pr->def) d = cgx_dcatn(JD, d, cjs_t(p, " = "), cjs_ex(p, pr->def, 0), NULL);
        it[i] = d;
    }
    CgxDoc *r = cjs_seq(p, "(", it, fn->npar, ")"); free(it); return r;
}
static CgxDoc *cjs_fn(CgxJs *p, const CgxNode *fn, const CgxNode *cls) {
    const int ctor = cls && !strcmp(fn->s, "init");
    if (!ctor) cjs_name(p, fn->line, fn->s, cls != NULL);
    CgxB rb = {0};
    if (p->ts && !ctor) { cgx_bs(&rb, ": "); if (fn->ty.k == CGT_CLASS && cjs_returns_null(&fn->b1)) cgx_bs(&rb, "any"); else cjs_type(p, &fn->ty, &rb); }
    CgxDoc *head = cgx_dcatn(JD, cgx_dtextf(JD, "%s%s", cls ? "" : "function ", ctor ? "constructor" : fn->s), cjs_params(p, fn), cgx_dtextf(JD, "%s ", rb.p ? rb.p : ""), NULL);
    free(rb.p);
    /* o primeiro `doc` do corpo vira JSDoc (e, em JS, os tipos escritos pelo autor viram @param/@returns) */
    int skip = -1;
    if (p->o->comments) for (int i = 0; i < fn->b1.n; i++) { if (fn->b1.p[i]->k == CGN_DOC) { skip = i; break; } if (fn->b1.p[i]->k != CGN_COMMENT) break; }
    CgxDoc *jsdoc = NULL; int nl = 0; CgxDoc *lines[40];
    if (p->o->comments) {
        if (skip >= 0) { CgxB c = {0}; cgx_bs(&c, " * "); cjs_jsdoc_clean(fn->b1.p[skip]->defs, &c); lines[nl++] = cjs_tb(p, &c); }
        if (!p->ts) {
            for (int i = 0; i < fn->npar && nl < 36; i++) if (fn->par[i].has_ty) { CgxB c = {0}; cgx_bs(&c, " * @param {"); cjs_type(p, &fn->par[i].ty, &c); cgx_bf(&c, "} %s", fn->par[i].name); lines[nl++] = cjs_tb(p, &c); }
            if (!ctor && fn->has_ty && fn->ty.k != CGT_VOID && fn->ty.k != CGT_ANY) { CgxB c = {0}; cgx_bs(&c, " * @returns {"); cjs_type(p, &fn->ty, &c); cgx_bs(&c, "}"); lines[nl++] = cjs_tb(p, &c); }
        }
        if (nl) { jsdoc = cjs_t(p, "/**"); for (int i = 0; i < nl; i++) jsdoc = cgx_dcatn(JD, jsdoc, cgx_dhard(JD), lines[i], NULL); jsdoc = cgx_dcatn(JD, jsdoc, cgx_dhard(JD), cjs_t(p, " */"), cgx_dhard(JD), NULL); }
    }
    CgxBlk nb = fn->b1, rest = {0};
    if (skip >= 0) { CgxNode **tmp = (CgxNode **)malloc(sizeof(CgxNode *) * (size_t)(fn->b1.n + 1)); int w = 0; for (int i = 0; i < fn->b1.n; i++) if (i != skip) tmp[w++] = fn->b1.p[i]; rest.p = tmp; rest.n = w; nb = rest; }
    CgxDoc *body = cjs_block(p, &nb, NULL);
    free(rest.p);
    return cgx_dcatn(JD, jsdoc ? jsdoc : cgx_dnil(JD), head, body, NULL);
}
static CgxDoc *cjs_class(CgxJs *p, const CgxNode *c) {
    cjs_name(p, c->line, c->s, 0);
    CgxDoc *in = NULL;
    for (int i = 0; i < c->b1.n; i++) {                              /* campos, com o valor inicial por instancia */
        const CgxNode *f = c->b1.p[i]; if (f->k != CGN_FIELDDECL) continue;
        cjs_name(p, f->line, f->s, 1);
        CgxB h = {0}; cgx_bs(&h, f->s);
        if (p->ts) { cgx_bs(&h, ": "); if (f->ty.k == CGT_CLASS) cgx_bs(&h, "any"); else cjs_type(p, &f->ty, &h); }          /* referencia a classe pode ser null: a UIR nao tem tipo opcional */
        cgx_bs(&h, " = ");
        CgxDoc *x = cgx_dcatn(JD, cgx_dhard(JD), cjs_tb(p, &h), f->a ? cjs_ex(p, f->a, 0) : cjs_zero(p, &f->ty), cjs_t(p, ";"), NULL);
        in = in ? cgx_dcat(JD, in, x) : x;
    }
    int first = in == NULL;
    for (int i = 0; i < c->b1.n; i++) {
        const CgxNode *m = c->b1.p[i]; if (m->k != CGN_FN) continue;
        CgxDoc *d = cjs_fn(p, m, c);
        CgxDoc *x = first ? cgx_dcat(JD, cgx_dhard(JD), d) : cgx_dcatn(JD, cgx_dhard(JD), cgx_dhard(JD), d, NULL);
        in = in ? cgx_dcat(JD, in, x) : x; first = 0;
    }
    if (!in) return cgx_dcatn(JD, cgx_dtextf(JD, "class %s {}", c->s), NULL);
    return cgx_dcatn(JD, cgx_dtextf(JD, "class %s {", c->s), cgx_dnest(JD, p->o->indent, in), cgx_dhard(JD), cjs_t(p, "}"), NULL);
}

/* ---- varredura previa: quais declaracoes sao reatribuidas (let x const) e se ha raw ---- */
static void cjs_scan(CgxJs *p, const CgxNode *n) {
    if (!n) return;
    if (n->k == CGN_RAW) p->has_raw = 1;
    if (n->k == CGN_ASSIGN && n->a && n->a->k == CGN_VAR && n->a->ref && n->a->ref->id > 0 && n->a->ref->id < p->nasg) p->asg[n->a->ref->id] = 1;
    cjs_scan(p, n->a); cjs_scan(p, n->b); cjs_scan(p, n->c);
    for (int i = 0; i < n->npar; i++) cjs_scan(p, n->par[i].def);
    const CgxBlk *bl[3] = {&n->b1, &n->b2, &n->b3};
    for (int k = 0; k < 3; k++) for (int i = 0; i < bl[k]->n; i++) cjs_scan(p, bl[k]->p[i]);
}

/* ---- harness: uma linha por `test`, depois do codigo; imprime `test N: ok` ou `test N: FAIL ...` como o do Python ---- */
static void cjs_tests(CgxJs *p, const CgxNode *prog, CgxB *out) {
    int n = 0;
    for (int i = 0; i < prog->b1.n; i++) if (prog->b1.p[i]->k == CGN_TEST) n++;
    if (!n) return;
    cjs_need(p, H_EQ); cjs_need(p, H_REPR);
    CgxB lines = {0};
    int k = 0;
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *t = prog->b1.p[i]; if (t->k != CGN_TEST) continue;
        CgxDoc *d = cgx_dcatn(JD, cgx_dtextf(JD, "_ucge_t(%d, () => ", ++k), cjs_ex(p, t->a, 0), cjs_t(p, ", "), t->b ? cjs_ex(p, t->b, 0) : cjs_t(p, "null"), cgx_dtextf(JD, ", %d);", (int)t->iv), NULL);
        cgx_render(d, 100000, &lines); cgx_bs(&lines, "\n");
    }
    cjs_emit_text(
        "function _ucge_t(n" TS(": number") ", f" TS(": () => any") ", exp" TS(": any") ", mode" TS(": number") ")" TS(": void") " {\n"
        "    let got" TS(": any") ";\n    try {\n        got = f();\n    } catch (e) {\n"
        "        console.log(\"test \" + n + \": \" + (mode === 2 ? \"ok\" : \"FAIL raised \" + (e instanceof Error ? e.message : String(e))));\n        return;\n    }\n"
        "    if (mode === 2) {\n        console.log(\"test \" + n + \": FAIL did not raise\");\n        return;\n    }\n"
        "    const good = mode === 3 ? got === true : mode === 1 ? Math.abs(got - exp) < 1e-9 : _eq(got, exp);\n"
        "    console.log(\"test \" + n + \": \" + (good ? \"ok\" : \"FAIL got \" + _repr(got) + \" expected \" + _repr(exp)));\n}\n", p->ts, out);
    if (lines.p) cgx_bs(out, lines.p);
    free(lines.p);
}

static int cjs_gen(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests, int ts) {
    CgxJs P; memset(&P, 0, sizeof P); P.g = g; P.o = o; P.ts = ts; CgxJs *p = &P;
    cgx_ty_init(&P.T);
    P.nasg = g->nnodes + 2; P.asg = (unsigned char *)calloc((size_t)P.nasg, 1);
    const CgxNode *prog = g->prog; CgxDoc *all = NULL; int first = 1, prev_fn = 0;
    const int err0 = g->nerr_all;
    cjs_scan(p, prog);
    for (int i = 0; i < prog->b1.n; i++) {
        const CgxNode *d = prog->b1.p[i]; CgxDoc *x = NULL; int isfn = 0;
        if (d->k == CGN_FN) { x = cjs_fn(p, d, NULL); isfn = 1; }
        else if (d->k == CGN_CLASS) { x = cjs_class(p, d); isfn = 1; }
        else if (d->k == CGN_LET) x = cjs_stmt(p, d);
        else if (d->k == CGN_RAW) x = cjs_raw(p, d);
        else if (d->k == CGN_COMMENT && o->comments && d->defs) x = cjs_line_comment(p, d->defs);
        if (!x) continue;
        if (!first) { const int blank = isfn || prev_fn ? 1 : 0; for (int b = 0; b <= blank; b++) x = cgx_dcat(JD, cgx_dhard(JD), x); }
        all = all ? cgx_dcat(JD, all, x) : x; first = 0; prev_fn = isfn;
    }
    CgxB tb = {0};
    if (tests) cjs_tests(p, prog, &tb);                                  /* antes do preludio: os testes tambem pedem ajudantes */
    CgxB body = {0};
    if (all) { cgx_render(all, o->width, &body); cgx_bs(&body, "\n"); }
    if (ts) cgx_bs(code, "export {};\n\n"); else if (!P.has_raw) cgx_bs(code, "\"use strict\";\n\n");
    int anyh = 0;
    for (int h = 0; h < H_N; h++) if (P.need[h]) { cjs_emit_text(CGX_JS_H[h].text, ts, code); anyh = 1; }
    if (anyh) cgx_bs(code, "\n");
    if (body.p) cgx_bs(code, body.p);
    free(body.p);
    if (tests && tb.p) cgx_bs(tests, tb.p);
    free(tb.p);
    cgx_docs_free(&P.D); cgx_ty_free(&P.T); free(P.asg);
    return g->nerr_all == err0;
}
static int cgx_gen_js(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests) { return cjs_gen(g, o, code, tests, 0); }
static int cgx_gen_ts(Cgx *g, const CgxGenOpts *o, CgxB *code, CgxB *tests) { return cjs_gen(g, o, code, tests, 1); }
