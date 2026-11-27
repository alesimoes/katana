# Codegen: code generation from a code spec

Codegen is the **Unified Code Generation Engine**: one engine that turns a short code spec into code in several languages, through a typed AST, instead of asking the model to write the final code or filling text templates.

Status: phase 1 (design and core) and the phase 2 modules A (JavaScript and TypeScript backends, section 17), B (C and Go backends, sections 12b and 12c), C (type inference, section 5b), D (graphs, analyses, optimizer, section 16), I, J and K (registry, incremental AST, dependency graph, code cache, section 15) are done; phases 2 (the rest) and 3 are planned at the end of this page. The code is included in `src/katana.c` (after `coder/symedit.c`, in the order of `tools/codegen/codegencli.c`) but nothing in katana calls it yet: a local `#pragma GCC diagnostic` around the include block silences the unused-function warning until the `CodeGen` tool exists. The files are `static`, plain C, and also compile standalone with `tools/codegen/codegencli.c`. Phase 2 adds the `CodeGen` tool.

Names: the module is "codegen" and the input format is the "code spec" (files `.kcode`; not `.kspec`, which is the model specialization file, see `docs/specialization.md`). The internal prefixes `cgx_`, `Cgx` and `CGN_` mean "codegen" and are kept as they are.

## 1. Decision and idea

The owner's decision (2026-11-24): the model does **not** write the final code. It **describes what to build** in a compact structured specification (the code spec,
"code spec"); the engine turns that into code. Pipeline:

```
Intent (LLM) -> code spec text/JSON -> typed AST (UIR-H) -> CFG + SSA (UIR-G) -> analyses -> AST -> doc -> language adapter -> validator -> code
                    parse           check/infer         build            flow/opt         gen     pretty-printer        run/scan
```

Hybrid architecture: UIR + AST + generation, with a **fallback**: what the UIR cannot express goes in a `raw` block written by the model (code that is
**not trusted**), which becomes an opaque node of the AST, passes through the existing structural validation (scanner masks, bracket balance, the language
syntax tool) and is marked "untrusted" in the report.

Why bother, honestly (measured in section 12, nothing assumed): the spec is **not shorter than idiomatic code** (about 16% longer than hand-written
Python, about 14% shorter than the Python it generates). The expected gains are other: (1) **determinism**: the same spec gives the same code, and the same
semantics in every target; (2) **multi-language** from one description; (3) **validation before generation**: undeclared names, arity, missing returns,
obvious type errors, scope rules that differ between Python and JavaScript, all caught with a line and what was expected; (4) the model never has to know the
target's idioms, only a small protocol.

## 2. Code spec: the input syntax

Chosen for the small model (Qwen3.6-35B-A3B, trained on code, not on a private IR): **indentation pseudocode** that looks like Python/Rust/Kotlin hybrids it
already knows, one statement per line, no braces, no JSON quoting. A JSON equivalent is accepted (section 2.3). An s-expression form was considered and
dropped: parentheses cost the model more counting than indentation, and every token of an s-expression is unfamiliar surface.

### 2.1 Grammar (informal)

```
program   := { decl }
decl      := fn | class | const | test | raw | comment
fn        := "fn" NAME "(" [param {"," param}] ")" ["->" type]  NEWLINE INDENT {stmt} DEDENT
param     := NAME [":" type] ["=" const-expr]
class     := "class" NAME NEWLINE INDENT { field | fn } DEDENT          (no inheritance: UIR L3 limit, use raw)
field     := "field" NAME [":" type] ["=" const-expr]
const     := "const" NAME [":" type] "=" expr                           (module level; `let` is accepted too)
test      := "test" expr "==" expr | "test" expr "~=" expr | "test" call "raises" | "test" bool-expr
stmt      := "let" NAME [":" type] "=" expr              DECLARE
           | target ("=" | "+=" | "-=" | "*=" | "/=" | "%=") expr      ASSIGN   (target: name, xs[i], obj.field)
           | "return" [expr]                                          RETURN
           | "if" expr / "elif" expr / "else"  (blocks)               IF
           | "while" expr                                             LOOP
           | "for" NAME "in" expr ".." expr ["step" INT]              LOOP over a range (end exclusive); also `range(a, b)`
           | "for" NAME ["," NAME] "in" expr                          ITERATE (list, str, or map: `for k, v in m`)
           | "try" / "catch" [NAME] / "finally"  (blocks)             TRY_CATCH
           | "throw" expr | "break" | "continue" | call               THROW, BREAK, CONTINUE, CALL
           | "raw" LANG ["defines" NAME {"," NAME}] NEWLINE INDENT text DEDENT      RAW
           | "doc" TEXT                                                documentation line (docstring / JSDoc / comment)
expr      := cond "?" expr ":" expr | expr "or" expr | expr "and" expr | "not" expr | cmp
cmp       := add [("==" | "!=" | "<" | "<=" | ">" | ">=" | "in" | "not in") add]
add       := mul {("+" | "-") mul}      mul := unary {("*" | "/" | "//" | "%") unary}
unary     := "-" unary | postfix       postfix := primary { "(" args ")" | "[" expr "]" | "." NAME ["(" args ")"] }
primary   := INT | FLOAT | STRING | true | false | null | NAME | "(" expr ")" | "[" list "]" | "{" STRING ":" expr, ... "}"
type      := int | float | str | bool | any | void | list[T] | map[str,T] | ClassName     (aliases: string, number, boolean, dict, T[] ...)
```

Leniency chosen for a model that was trained on other languages (each accepted form costs one line in the parser and saves a failed attempt): `def`,
`function`, `func` for `fn`; `else if`, `elsif`; `raise`; `except`; `True/False/None/nil`; `&&`, `||`, `!`, `===`, `!==`; a trailing `:` on a block header;
`new Name(...)`; `foreach x in xs`; `t[]` and `list<T>`; `self`/`this` as an explicit first method parameter; `//` or `#` full-line comments (kept as
comments in the output when comments are on). What is **rejected with a message that says what to write instead**: `xs.push(3)` (write `push(xs, 3)`),
`import ...` (write a `raw` block), inheritance, conditions that are not bool, assignment to an undeclared name.

Errors always say the line and what was expected, e.g. `line 7: expected a parameter name, found the end of the line`, `line 3: variable `z` is not
declared (declare it with `let z = ...` first); names in scope: y, x`.

### 2.2 Operations of the UIR (what the engine knows)

| Op | Surface | Semantics fixed by the UIR (same in every target) |
|---|---|---|
| ADD, SUBTRACT, MULTIPLY | `+ - *` | numbers (int stays int, int op float is float); ADD also joins two `str`. `str + int` is an error: convert with `str(x)` |
| DIVIDE | `/` | true division, result float |
| IDIVIDE | `//` | integer **floor** division, both operands int |
| MOD | `%` | **floored** remainder (sign of the divisor, as Python); JS and C get a helper |
| COMPARE | `== != < <= > >=` | numbers with numbers, str with str (code-point order), same type for `==`; result bool |
| AND, OR, NOT | `and or not` | operands must be bool (no truthiness: it differs between languages); short-circuit |
| IN | `x in xs` | list membership, substring, map key |
| NEGATE | `-x` | numbers |
| DECLARE / ASSIGN | `let` / `=` | block scope; **no redeclaring and no shadowing a visible name** (Python and JS scope differently, so the rule is the intersection); module constants cannot be reassigned |
| FUNCTION | `fn` | parameters with optional types and constant defaults (a list or map default is created per call); return type optional (inferred); every path must return or throw |
| RETURN, IF, LOOP, ITERATE | as above | `for i in a..b` is a half-open range; the loop variable is local; `step` must be a non-zero integer literal (its sign picks the direction) |
| TRY_CATCH | `try/catch e/finally` | `catch` catches any error raised by `throw` (and by the target runtime); `errmsg(e)` is its text; `throw` takes a `str` or the caught `e` |
| CALL | `f(x)`, `Class(x)`, `obj.m(x)` | functions of the spec, constructors, methods, and the built-ins below |
| LIST / MAP literals | `[1, 2]`, `{"k": v}` | map keys are always `str` (JS objects stringify keys) |
| STRING ops | built-ins | `len upper lower trim split words join contains indexof replace startswith endswith slice reverse matches str int float` |
| list/map/number ops | built-ins | `push pop sort sum keys len abs min max sqrt floor pow print` |
| RAW | `raw LANG` | opaque, untrusted; names it `defines` enter the scope as `any` |
| CLASS | `class` | fields with constant defaults, an optional `init`, methods; `self` is implicit; no inheritance |

Known cross-target divergences the engine cannot remove and therefore documents and reports: division by zero (Python raises, JS gives Infinity, C is
undefined: guard it with an explicit `throw`), integer width (Python unbounded, JS 2^53, C 64 bit), `str()` of a float or bool (`2.0`/`True` in Python,
`2`/`true` in JS), and regular-expression dialects (`matches`).

### 2.3 JSON equivalent

A spec starting with `{` or `[` is JSON. Statements are `{"op": "FUNCTION" | "CLASS" | "DECLARE" | "ASSIGN" | "RETURN" | "IF" | "LOOP" | "ITERATE" |
"TRY_CATCH" | "THROW" | "CALL" | "METHOD" | "RAW" | "BREAK" | "CONTINUE" | "TEST", ...}`; an expression is either a **string written in the code spec expression
syntax** or an object (`ADD SUBTRACT MULTIPLY DIVIDE IDIVIDE MOD COMPARE AND OR NOT CALL METHOD VAR LIT LIST MAP INDEX FIELD` with `args`). The engine converts
the JSON to the normalized code spec text and runs the one parser (`Cgx.norm` keeps the text, error lines refer to it), so there is a single grammar to maintain
and the two forms cannot drift. An unknown `op` answers with the list of valid ones.

The owner's JSON style is accepted too: an operation object with `left`, `right` and `result` (`{"op": "ADD", "left": "a", "right": "b", "result": "r"}` is
`let r = a + b`), and `COMPARE` with `"cmp"`. In JSON a plain string is an **expression** (`"a + b"`, `"xs[0]"`); a string literal is `{"op": "LIT",
"value": "text"}`. Size, measured on three tasks (characters / 4, JSON with whitespace removed, the code spec without comments): task 01 JSON 66 vs code spec 32.5 tokens
(2.0x), task 04 121 vs 55.8 (2.2x), task 10 135.5 vs 55.3 (2.5x). The compact form is the primary one for the small model for that reason (and because it is
closer to code it was trained on); JSON stays for tools that emit structured data. Files: `tools/codegen/json/`.

## 2b. Engine components (the owner's requirements) and where each lives

The core of the engine is an **AST builder driven by a typed IR**: backends never fill templates or join strings; they walk typed nodes and build documents.
Generation is graph-based: the dependency graph orders the work, the IR graph feeds the analyses, and the AST is the only thing printed.

1. **Operation Registry** (`codegenops.c`, phase 2; seed: the `CGB` table and `CGO_NAME` in phase 1). One row per operation known to the engine: `name`, `arity`
   (min, max), `input types` and `output type` as type-table schemes (`len: forall a. list[a] | str | map[a] -> int`), `semantics` (a short normative text
   and an effect flag: pure, reads, mutates, io, throws), `level` L1/L2/L3, and **per-language mapping**: a template of *document constructors* (not a text
   template: receiver precedence, argument breaking and imports are handled by the printer) plus the imports it needs and the targets it is unsupported
   for (with the message). Extensible: a backend adds rows for its own library; a project can add an `extern` row (section 9) at run time. The validator,
   the inference, the analyses (effects, purity) and the backends all read this one table, so adding an operation is one row plus its tests.
2. **Typed IR with resolution** (`codegeninfer.c`, phase 2). Name resolution (`ref`), overload and ambiguity resolution (an operation row can have several
   signatures: `+` on numbers or on str; `contains` on str, list or map; the unifier picks the first signature that unifies and reports an **ambiguity** with
   the candidates when an `any` argument leaves more than one), numeric promotion (`int <: float`), and the gradual `any` where information is missing. The
   typed AST (UIR-H) is the IR the backends read; UIR-G (section 4) is derived from it.
3. **IR Optimization Engine** (`codegenopt.c`, phase 2): simplification, constant folding (SCCP-guided), dead-code and unreachable-branch elimination,
   unused-binding removal, and **dependency resolution** (a function that is never reached from the exported ones, or a `const` nothing reads, is reported;
   removal is opt-in). Passes run on UIR-H and UIR-G, preserve observable behaviour including thrown errors and effects, and report what they changed.
4. **Incremental AST** (`codegeninc.c`, phase 2). To change existing code the engine does **not** rebuild the program: the existing file is parsed by the coder
   scanner (`coder_parse`: symbols with byte ranges, the same parse `Edit` by symbol uses); only the spec's top-level units (a function, a class, a method)
   are generated, and each is spliced into the file at the range of the symbol it replaces (or before/after an anchor) through `ct_edit_symbol`. The unit
   of change is a symbol: nodes outside it are never touched, never reprinted, so the user's formatting and comments elsewhere survive; the symbol's
   `shape` hash (already in `CoderSym`) tells whether the new unit differs from the old one at all (no write when it does not). A `patch` spec form
   (`patch NAME` followed by statements to replace, insert before/after a numbered statement, or delete) edits statements **inside** a function: the engine
   parses the old function with the scanner to find the statement ranges and regenerates only the affected statements; this is the finer grain.
5. **Dependency graph** (`codegendep.c`, phase 2): nodes are the spec's units (functions, classes, constants, `raw` blocks, and `extern` symbols found in the
   index), edges from resolved references. Tarjan SCCs give the **cycles** (reported as errors for constants and class fields, allowed as recursion
   groups for functions); the condensation is topologically ordered, which is the **order of generation and of insertion** (definitions before uses for
   targets that need it, C and Go among them; the order of checking and inference too). Across files it is the same graph with module nodes and import edges.
6. **Code cache** (`codegencache.c`, phase 2): key = hash (FNV-1a 64 over the **normalized UIR**: the canonical dump of UIR-H, which ignores comments, spacing,
   JSON versus code spec, and alias spelling) + hash of each dependency unit's normalized UIR + target + generation options (width, comments) + **generator version**
   (a constant bumped whenever a backend or the printer changes). Value: the generated code and the report. Stored under `~/.katana/codegen/` as one file per key
   with an LRU cap; **invalidation by dependency** is automatic because the dependencies' hashes are part of the key (a changed callee changes the caller's key);
   `raw` blocks hash their text. A hit skips generation and returns the stored result marked `[cache]`; tests are re-run unless the key also covers the test
   results (they do not: execution is never cached).

Hash of the normalized UIR needs a canonical dump (`cgx_dump`, phase 2; node ids from `cgx_number` are stable per spec, so the dump is deterministic).

## 3. Data structures

All in `src/coder/codegen.c` (names `Cgx*`, `cgx_*`, `CGN_*` kinds, `CGO_*` operators, `CGT_*` simple types, `CGB_*` built-ins, `TYK_*` table types; the
prefix is unique in the single translation unit, checked by compiling together with `katana.c`).

- **Arena.** `Cgx` owns every allocation (`cgx_alloc` records the pointer; `cgx_free` releases all at once). No per-node free, no leaks by construction;
  growth of block vectors leaves the old vector in the arena. One `Cgx` is one request.
- **Nodes.** `CgxNode` is one struct for every kind (`k`): expressions (`INT FLT STR BOOL NULL VAR BIN UN CALL MCALL FIELD INDEX LIST MAP TERN`), statements
  (`LET ASSIGN RET IF WHILE FOR EACH TRY THROW BREAK CONT EXPR RAW DOC COMMENT`), declarations (`FN CLASS FIELDDECL TEST`) and `PROG`. Children: `a b c` and
  three blocks `b1 b2 b3` (then/else, body/catch/finally, call arguments). An `elif` is an `IF` nested in the `else` block with `flag = 1`, so printers can
  restore the chain.
- **Stable ids.** After parsing, `cgx_number` assigns `id` in pre-order (1..n): the same spec always numbers the same, so analyses, reports and the IR can
  refer to nodes by integer and tests can compare dumps.
- **Interned symbols.** `cgx_intern` maps a name to an integer (open-addressing hash, FNV-1a); `sid` of a node is the id of its name. Scope lookup, call
  graph and the IR use ids, not strings.
- **Source positions.** Every node has `line` (1-based). Errors are `(line, message)` pairs, at most 24 kept, the rest only counted.
- **Type table** (`codegentype.c`). Hash-consed types: one id per structure (`list[int]` is one integer everywhere), mutable type variables with union-find and
  path compression, levels for generalization, occurs check, gradual `any`, `T | null` (OPT), unions as sorted sets, tuples, function types, named classes,
  schemes by `TYK_GEN` placeholders. The phase 1 validator still uses the small inline `CgxTy` (kind + element kind); phase 2 replaces it with these.
- **Doc algebra** (`codegendoc.c`). Documents (`text line soft hard cat nest group`) in their own arena; the renderer is Lindig's "Strictly Pretty" with an
  explicit stack (no recursion, no quadratic backtracking: `fits` looks only up to the next permitted break).

## 4. Representation in three levels

1. **Code spec** (text or JSON): the model's surface. Only the parser sees it.
2. **UIR-H**: the typed AST of section 3. Language independent, structured control flow, every name resolved (`ref` points to the declaration), every
   expression typed (`ty`), built-ins identified (`bi`). This is what the checker annotates and what the backends read. Phase 1 stops here.
3. **UIR-G** (module D, `codegencfg.c`, section 16): a graph per function, built from UIR-H: basic blocks of three-address instructions (`op dst a b`, `call`, `load/store`
   for fields and elements, `phi`, `br`, `jmp`, `ret`, `throw`), edges for normal flow and for exceptions (a `try` block has an exceptional edge to its
   handler; `finally` is duplicated on both paths). Variables are in **SSA** (section 6.3) with one `phi` per merge. Backends do **not** read UIR-G:
   it exists for analyses and for transformations that are written back to UIR-H (constant folding, dead code removal), so generation stays AST-driven
   and the output stays readable.

## 5. Type system

Types: `int`, `float`, `str`, `bool`, `void`, `any` (gradual), `list[T]`, `map[str,T]`, tuples `(A, B)`, functions `(A, B) -> R`, optionals `T?`
(`T | null`), unions `A | B`, classes (nominal), and type variables for generics. The only subtyping is `int <: float` and `T <: T?`.

Why Hindley-Milner plus a gradual extension, and not either alone:

- The model writes **types sparsely** (a parameter without a type, `let xs = []`). HM with **let-polymorphism** infers them without annotations and gives
  generic functions for free (`fn first(xs) -> ...` is `forall a. list[a] -> a`). Union-find unification is near-linear in practice and gives the principal type.
- A target like Python or JS is dynamic and a `raw` block is unknown code: a **gradual** `any` that unifies with everything without binding anything
  (compatible both ways, never an error) keeps the system useful where information is missing and silent where it cannot know.
- Flow-sensitive refinement (for nullable and union types, after `if x != null`) is done **inside the inference over the AST** (section 5b): structured control
  flow makes the narrowing a scoped slot with an undo trail, which needs no CFG; the SSA/dataflow version (module D) can replace it later without changing the typed AST.
- Type errors name both types and the expected one (`ADD of int and str (expected two numbers or two str; convert with str(x))`).

Phase 1 behaviour (checker in `codegencheck.c`, intentionally simpler): declared types plus local inference of `let`, literals, built-ins and calls; list and
map element type is fixed by the first `push` or literal; return type inferred in two passes; `any` for the unknown.

## 5b. Type inference (`codegeninfer.c`, module C)

`int cgx_infer(Cgx *g, CgxTypes *T)` (and `cgx_infer_ex(g, T, flags, report)`) types the whole program; `cgx_check_ex(g, T, infer)` is the validator entry that
runs the phase 1 rules first and the inference after them (`codegencli check|gen|run|stats --infer` turns it on; `codegencli infer FILE [--lets] [--uses] [--check]`
prints errors, the typed signatures with their certainty, and the gradual checks). It is **optional**: nothing in `cgx_check` changed, the 60 checks of
`tools/codegen/test.sh` are the phase 1 behaviour. It also runs without `cgx_check` (it reports the scope errors itself) so that specs can be typed alone.
One call per parse (the results are stored in the AST).

**What it writes.** Every typed AST node gets `tid` (an id in the caller's `CgxTypes`; `TYK_GEN(i)` is the i-th type parameter of the enclosing
function, so a backend with generics reads `list['a] -> 'a` straight from the `FN` node, and a monomorphizing one instantiates it), `cert` (1 derived,
2 declared, 3 gradual: **never "exact"**) and, where a gradual value meets a concrete slot, `chk` = the type that value must have there. `chk` is a mark
on the AST for the backend to turn into a cast or an assertion; the engine inserts no run-time test itself. Nothing is written into `n->ty`, so the
phase 1 backends keep reading what they read.

**Algorithm.** Algorithm J (destructive unification over the union-find of `codegentype.c`) with generalization by levels (Remy): every variable carries the level
of the `let` depth where it was created, the occurs check lowers the levels of what a variable is bound to, and a function is generalized when its recursion
group is done (variables deeper than the outer level become `TYK_GEN`). Pieces added to plain HM:

1. **Call graph and SCCs** (Tarjan): functions, methods and module constants are nodes; edges come from `CALL`, `MCALL` (every method of that name) and uses of
   a constant. Groups are inferred callee first; inside a group a function is monomorphic (so mutual recursion types), after it the scheme is instantiated per call.
2. **Variable classes** (a bit mask of allowed kinds per variable, kept in the scheme): `a + b` on two unknowns gives `'a, 'a -> 'a where 'a in int|float|str`,
   `x * 2` gives `number`, `len(x)` gives `str|list|map`. This is what lets `fn add(a, b)` be generic and still reject `add(true, false)`. Binding a variable checks the
   mask; unifying two variables intersects them.
3. **Pending overloads** (index `x[i]`, `in`, `contains`, `for x in c`): resolved the moment the container type is known (after each statement); anything still
   open at the end of the group defaults to the list reading (`x[i]` and `for` on an unannotated parameter infer a list; annotate `str` to get the string one).
4. **Bidirectional**: the expected type (annotated parameter, declared return, annotated `let`, field default) is pushed into list and map literals, `?:` and
   `null`, so `let xs: list[float] = [1, 2]` and `return []` check against the annotation instead of being inferred and compared afterwards.
5. **Gradual `any`**: consistent with everything, never an error. A variable unified with `any` is bound to `any` (so `push(xs, anyValue)` makes `list[any]`: the
   list cannot later be trusted to hold only ints). Each `any -> concrete` boundary is recorded in `chk`.
6. **Flow typing** (`x != null`, `x == null`, `not`, `and`/`or` short circuit, `?:`, early exit by `return/throw/break/continue`, equality with a literal on `any`,
   unions and optionals, assignment of a non-null value): the narrowed type lives in a per-symbol slot with an undo trail; any assignment kills it (also in the
   enclosing scopes), loops and `try` kill what their bodies assign before they are entered, a user call kills the narrowed `obj.field` paths.
7. **Unions** only come from joins: `?:` branches, returns of an unannotated function, list and map literal elements, `try`/`catch` returns. Everywhere else a type
   mismatch is an error. Optional `T?` is `T | null`; `null` has type `'a?`.

**Typing rules** (what the error texts mention):

| Construct | Rule |
|---|---|
| literals | `int`, `float`, `str`, `bool`; `null : 'a?` |
| `+` `-` `*` | numbers (int op float is float, int op int is int); `+` also `str + str`; an unknown operand gets the class; `any` gives `any` (and a check) |
| `/` | two numbers, result `float`; `//` `%` two `int`, result `int` |
| `< <= > >=` | two numbers or two `str`; result `bool` |
| `== !=` | the two sides must be compatible (`int`/`float` mix, `T` with `T?`, `null` with an optional); result `bool` |
| `and or not` `if` `while` `?:` conditions | `bool` (no truthiness); `any` is accepted with a check |
| `xs[i]` `m[k]` `s[i]` | `list[T]` with `int` gives `T`, `map[T]` with `str` gives `T`, `str` with `int` gives `str` |
| `obj.f` `obj.m(..)` | class fields and methods; an unknown receiver is the one class that has that name, or an error asking for the annotation |
| `f(a, b)` | arity (defaults optional), each argument assignable to the instantiated parameter (`int` into `float` yes, `list[int]` into `list[float]` no), result = instantiated return |
| `Class(a)` | the `init` signature, result the class; no `init` means no arguments |
| `let x = e` | `x : typeof(e)`; `let x: T = e` checks `e <= T`; `let x: str = null` declares an optional `str?` |
| `x = e`, `x += e` | `e` assignable to the declared type of `x`; `+=` also needs the operator to type, and an `int` cannot take a `float` |
| `return e` | assignable to the declared return; without one, the join of all returns (value and bare `return` mixed is an error) |
| `for i in a..b` | `int` bounds, `i : int`; `for x in c`: `list[T]` gives `T`, `str` gives `str`, `map` gives the key (one name) or key and value (two) |
| `catch e` | `e : Error`; `errmsg(e) : str`; `throw` takes `str` or an `Error` |
| built-ins | one rule each (`len: str|list|map -> int`, `sort: list[a] -> list[a]` with `a` ordered, `sum: list[a] -> a` with `a` numeric, `contains`, `slice`, `keys`, ...): `inf_builtin` in `codegeninfer.c` is the single place, to be replaced by the Operation Registry rows (module I) |

**Errors** carry the line (the AST has no columns), what was expected, what was found, and a hint (`convert with int(x)`, `convert with str(x)`, `the value may be
null; test x != null first`, `write the comparison`, `annotate the parameter with the class name`, `did you mean total?`). The texts of the phase 1 checks that
the inference also covers are the same strings. After an error the node is typed `any` so one mistake does not cascade.

**Certainty report** (`cgx_infer_ex` with a buffer): one line per function and method with its scheme (`fn first: (list['a]) -> 'a [derived]`; classes as
`where 'a in int|float`), the fields, the gradual checks (`check line 3: a gradual value must be int here`) and the counts by grade. `[declared]` means every
parameter and the return are written in the spec, `[derived]` that something was inferred by unification, `[any]` that `any` is part of the signature or of the node
type. Derived is as strong as the typing rules above; it is **not** a proof about the program (see the limits).

**Complexity.** Unification is near-linear (union-find with path compression, hash-consed types: structural equality is an integer compare). Tarjan is
O(V+E). Generalization and instantiation walk the type once, O(size of the signature). The pending list is retried after each statement, O(P) per statement while
anything is open (P is small: only unresolved containers). Narrowing kills walk the trail, O(trail) per assignment. 3000 chained generic functions (33k nodes) and
one function with 3000 `let`/`if` pairs (27k nodes) type in under 70 ms.

**Limits (known, documented, covered by tests where they are visible).**
- Annotations cannot spell `T?`, `list[list[T]]`, `A | B` (the parser keeps one base kind): a nested annotation or a bare `list`/`list[any]` means "element to infer"; optionals come from `null`.
- There is no `is` operator in the grammar, so type tests narrow only through `== null`, `!= null` and equality with a literal.
- `int` and `float` share a variable in a generic function (`fn add(a, b)`): `add(1, 2.5)` is rejected (the two parameters have one type); annotate one as `float`.
- Lists and maps are invariant: `list[int]` does not go where `list[float]` is expected.
- Pending defaults: an unannotated container used only by `x[i]` or `for` is read as a list.
- Methods and fields are not generic per object: a class field with an unknown element type is fixed by its first use anywhere in the program.
- Narrowing of `obj.field` is one level (`x.f`), and any user call forgets it; a path through two fields (`a.b.c`) is not narrowed.
- At most 24 type parameters per function (the rest stay weak and become `any`), 39 parameters, 8 narrowing facts per condition.
- `any` is bound into a variable it meets, so a gradual value makes the containers it touches `any`-typed: that is sound, and loses precision.
- Every error is a line, not a column; type errors are not ranked (the first mismatch found reports).
- The unannotated results are [derived]: sound for the rules above, but the engine does not model run-time facts (list bounds, division by zero, integer width).

**Tests.** `sh tools/codegen/infer_test.sh`: 363 cases in `tools/codegen/infer/cases_*.txt` (typed programs, exact error texts, polymorphism, recursion and mutual
recursion, unions, flow typing, gradual, classes, bidirectional, generic list helpers), the 15 tasks with `--infer`, and `tools/codegen/infer_prop.py`, a soundness
property: programs well typed **by construction** (random typed expression and statement generators, annotated and unannotated parameters, calls between the generated
functions) are inferred, generated to Python, executed with random arguments, and the observed type of every result must fit the inferred one; wrong-typed return
annotations and wrong-typed arguments are injected and must be rejected. Failure of either side fails the script (the script was checked by injecting a typing bug).

## 6. Algorithms (decisions and why)

1. **Parsing**: hand-written, line-oriented with an indent stack, recursive descent with precedence climbing for expressions. No parser generator: the error
   messages ("expected X, found Y") and the leniency list are easier to control, and the grammar is small.
2. **Type inference** (`codegeninfer.c`, done, section 5b): algorithm J with union-find and levels (Remy): `let` generalizes variables whose level is
   deeper than the current one. `any` and `raw`-defined names are gradual. Classes get nominal types; method calls resolve through the class table.
3. **Control-flow graph and SSA** (phase 2, `codegenir.c`): blocks from structured nodes (no goto, so the CFG is reducible by construction). **Dominators**
   by Cooper-Harvey-Kennedy (iterate over the reverse post-order with the `intersect` on idoms: simple, fast on reducible graphs, no Lengauer-Tarjan needed
   at these sizes). **Dominance frontiers** from idoms (CHK), **phi placement** with the iterated dominance frontier, renaming by a dominator-tree walk
   (Cytron et al.). Chosen over Braun's on-the-fly SSA because the dominator tree is also needed for loop detection and dead-code reasoning.
4. **Data-flow analyses** (phase 2, `codegenflow.c`): one worklist solver over a lattice interface (`meet`, `transfer`, direction) and instances: **reaching
   definitions** (forward), **liveness** (backward, bit vectors), **constant propagation** (SSA sparse conditional constant propagation, Wegman-Zadeck, which
   also finds unreachable branches), **definite assignment** (forward, for `raw`-defined names and `try` blocks), **null-ness** (forward, feeds the optional type).
5. **Call graph** (phase 2): nodes are functions and methods (interned ids), edges from `CALL`/`MCALL` nodes already resolved by the checker (`ref`); SCCs by
   Tarjan give recursion groups (return-type inference order, "function never called" and mutual recursion warnings). Calls into the project's existing
   code come from the coder symbol index (section 9).
6. **Dead code and unused names**: from SCCP (unreachable blocks, always-true conditions), liveness (assigned and never read) and the call graph (functions
   nobody calls, reported as `[derived]`: by name, never "exact").
7. **Simplification and constant folding** (phase 2, `codegenopt.c`): fold on UIR-H guided by SCCP results; only rewrites the author would not notice
   (`2 * 3` to `6`, `if true` branches), never changes observable behaviour including errors, and is **off by default** for readability (the model
   asked for `2 * 3`); a report line says what it would fold.
8. **Generation**: AST walk to a **document** (section 3), rendered with a width (`CgxGenOpts.width`, default 88). Wadler/Lindig over Oppen's streaming
   printer: the document is built first so a group can choose flat or broken by look-ahead, which is what call arguments, list literals and function headers
   need; one algorithm serves every language, a backend only builds documents. Precedence is carried by each node so parentheses appear only where needed.
9. **Validation after generation** (phase 2): the language's own checker on the produced text (python `ast`, `node --check`, `gcc -fsyntax-only`), the
   katana scanner (`coder_parse`) to confirm every spec symbol exists with the expected kind, bracket balance by `se_mask`/`se_balance` for `raw` blocks,
   then the spec's `test` lines executed in a subprocess with limits.

## 7. Semantics: errors, scope, effects

- **Errors.** `throw` raises an error with a text (`str`); `try/catch e/finally`; `errmsg(e)` reads it. Backends map to `raise Exception(text)`/`throw new
  Error(text)`; the C backend implements them with `setjmp`/`longjmp` (section 12b), Go with `panic`/`recover` (section 12c). Error values are not typed beyond
  "Error" in the UIR (typed errors are phase 3).
- **Scope.** Lexical, block level; parameters and module constants are visible; a name cannot be declared twice or shadow a visible one. This is the
  intersection of Python (function scope) and JS (`let`, block scope): code that is valid here means the same in both.
- **Effects.** Only `print`, `push`/`pop`/`sort`-free mutation of lists and maps, field and element assignment, `throw`, and calls into `raw`. The call graph
  marks a function **pure** when it has none of these and calls only pure functions (phase 2); analyses and folding use it.
- **Truthiness.** None: conditions are bool.

## 8. `raw`, trust and the report

A `raw LANG` block (python, js, ts, c) is written by the model and inserted as an opaque node. One node can carry a version per language (adjacent blocks);
generation for a target without its version is an error that lists the versions present. `defines a, b` puts those names in scope as `any`. The report grades
every finding and never says "exact":

- `[structural]`: facts of this spec and the produced file (parsed, validated, scanner sees the symbols, syntax tool ok, brackets balanced);
- `[derived]`: by name or by analysis (callers, unused, unreachable, cross-target divergence notes);
- `[tested]`: the given `test` lines passed on the target when executed (only those cases);
- `[untrusted]`: each `raw` block, with lines, language, "balanced by the scanner mask; content not understood by the engine"; tests of a spec with `raw` are
  **not executed** unless the caller asks (`run=yes`), because the block is arbitrary code.

## 9. Integration with the rest of katana (phase 2)

- **Tool**: one tool `CodeGen(spec, target, path, symbol, where, run, comments)` registered like `Render` (full declaration in `src/tools/codegen.c`,
  loaded through `ToolSearch select:CodeGen`, not in the lean prompt list; the declaration is kept under the existing token limit and the tests that check
  it must stay green). Without `path` it only returns the code and the report. With `path` and no file it creates the file; with `path` and `symbol` it
  builds the JSON for `Edit` by symbol (`ct_edit_symbol`, `where` replace/before/after/body) so the insertion gets the existing bracket/scanner/LSP report
  too; without `symbol` in an existing file it inserts after the last top-level symbol found by `coder_parse`.
- **Index**: names in the spec that are not defined in it (`unknown function`) are looked up in the coder symbol index (`symindex.c`) of the destination
  project; a hit becomes an `extern fn name(params) -> type` known to the checker (arity and types from the signature scanner), so generated code can call
  existing project code and be checked against it.
- **Imports**: `import` is not UIR. A function that needs one gets it from a `raw` header, or (phase 3) from `extern module` declarations the backend
  turns into import lines. When inserting by symbol the report says which import line the spec needs and could not add.

## 10. Protocol with the model (phase 2/3)

1. The system prompt carries nothing (the tool is deferred); `ToolSearch select:CodeGen` returns a declaration whose description holds the grammar in about
   40 lines and 3 examples. Few-shot examples are the tasks of `tools/codegen/tasks/`.
2. The model writes the code spec (or the JSON) in the `spec` argument. 3. The engine answers with the code and the graded report, or with **the errors only**
   (line, what was expected, the offending line quoted): the model repairs the spec and calls again; the harness measures how many repairs it needs.
4. Training the model on the protocol is out of scope until the owner decides (the curriculum is final at the end of the UIOE): the plan is measured
   few-shot first.

## 11. Source map and API

| File | Content | Phase |
|---|---|---|
| `src/coder/codegen.c` | arena, interning, AST, lexer, parser (code spec + JSON), errors | 1 |
| `src/coder/codegencheck.c` | built-in table, semantic validator, statistics | 1 |
| `src/coder/codegentype.c` | type table, union-find unification, generalization, schemes | 1 |
| `src/coder/codegeninfer.c` | type inference (HM + levels + gradual + flow typing), certainty report, `cgx_check_ex` | 2 (module C) |
| `src/coder/codegendoc.c` | document algebra, renderer, `CgxGenOpts`, `CgxBackend` | 1 |
| `src/coder/codegenpy.c` | Python backend (reference) and the backend registry | 1 |
| `src/coder/codegenjs.c` | JavaScript and TypeScript backends (one generator, `ts` switches the annotations; section 17) | 2 (module A) |
| `src/coder/codegenc.c`, `codegenc_rt.inc` | C backend and its embedded runtime (generated from `tools/codegen/rt/cgrt.h` by `tools/codegen/rt2inc.py`) | 2 (module B) |
| `src/coder/codegengo.c`, `codegengo_rt.inc` | Go backend and its embedded runtime (generated from `tools/codegen/rt/cgrt.go`) | 2 (module B) |
| `src/coder/codegen_api.h` | the contract between phases (documentation only) | 1 |
| `src/coder/codegenreg.c` | Operation Registry: one data-driven table (arity, types, level, effect, per-target templates, simplification rules), loader, `cgx_reg_simplify` | I |
| `src/coder/codegendep.c` | canonical form and FNV-1a hash, dependency graph (Tarjan, stable order, cycles), disk code cache, cached generation | K |
| `src/coder/codegeninc.c` | incremental AST: Python scanner, UIR-H reader with node-to-bytes map, change operations, symbol actions, writers | J |
| `tools/codegen/codegencli.c` | standalone driver: `check gen run stats selftest` | 1 |
| `tools/codegen/test.sh` | tests (60 checks, optional ASan/UBSan with `SAN=1`) | 1 |
| `tools/codegen/test_reg.sh`, `test_dep.sh`, `test_inc.sh` (+ `test_inc.py`) | tests of I (38 checks), K (59), J (59, 45 of them modify real files) | I, J, K |
| `tools/codegen/ops/extra.ops`, `tools/codegen/inc/*.py` | example user operations; the Python files the incremental tests edit | I, J |
| `tools/codegen/infer_test.sh`, `infer/cases_*.txt`, `infer_prop.py` | inference tests: 363 cases, the soundness property | 2 |
| `tools/codegen/jstest.sh`, `tools/codegen/js/` | JS/TS tests: tasks on node and tsc, edge cases, Python x JS x TS differential, random programs (`jsfuzz.py`) | 2 (module A) |
| `tools/codegen/testc.sh`, `testgo.sh`, `difftest_targets.py`, `cases/`, `cases_c/` | C and Go backend tests: tasks, hand-written cases, random Python-versus-target differential programs | 2 (module B) |
| `tools/codegen/tasks/*.kcode`, `ref/*.py`, `bench.py` | the 15 tasks, hand-written Python, the measurement | 1 |
| `src/coder/codegencfg.c` | digraph (CSR), Tarjan, dominators (CHK), dominance frontier, natural loops, effects and call graph, CFG per function, pruned SSA (Cytron), out of SSA, verifier, dump | D |
| `src/coder/codegenflow.c` | worklist solver over bit-set lattices and its instances, constant folding, SCCP, dead values, findings report | D |
| `src/coder/codegenopt.c` | optimizer passes written back to UIR-H, inlining, `cgx_opt_run` | D |
| `tools/codegen/graphcli.h`, `graphtest.c`, `graphtest.sh`, `fuzz_gen.py`, `difftest.py` | commands `ir flow opt optgen irexec`, unit tests, the runner, the program generator and the differential test | D |

Build and test: `gcc -O1 -Wall -I. tools/codegen/codegencli.c -o /tmp/codegencli -lm`; `sh tools/codegen/test.sh`; `python3 tools/codegen/bench.py /tmp/codegencli`.

## 12. What was measured (phase 1, no model, no GPU)

15 tasks written by the author as code spec (sum with default, filter a list, count and frequency of words, iterative factorial, Fibonacci, binary search, error
handling, a class with errors, email validation, maximum, FizzBuzz, palindrome, primality, JSON file total, fetch a page title), each with given tests.
**Coverage**: 13 of 15 (87%) without `raw`; the Python generated from the spec passes **all** given tests (53 cases). The two that need `raw` are exactly the
L3 kind (file and JSON library, HTTP): task 14 is a hybrid (the I/O in `raw`, the loop in UIR), task 15 is a function that is entirely `raw`. The author
wrote both the specs and the language, so 87% is an upper bound for a model: how often Qwen produces valid code spec was **not measured** (no model was run:
the machine was under load and the owner's rule is no GPU). Size, in tokens (characters / 4), for the 13 tasks without `raw`:

| | spec (no comments, no `test`) | generated Python | hand-written idiomatic Python |
|---|---|---|---|
| tokens | 679 | 786 | 584 |

The spec is **0.86x** of the generated code and **1.16x** of idiomatic code (comprehensions, `Counter`, `re.fullmatch`, `max` are shorter than their
spelled-out loops). With the `test` lines and comments the spec is longer still. For `raw` tasks the spec is longer than the code (task 14: 78 vs 38).
So the UIR does **not** reduce tokens against what a model would write directly; whatever it brings is determinism, one description for several
languages, and validation before generation. The generated Python follows its own style (type hints, explicit loops), which is why it is longer than
idiomatic code. The table per task is printed by `tools/codegen/bench.py`.

## 12b. Backend C (`codegenc.c`)

Module B of the phase plan. The generator walks the typed AST (`CgxNode`, `n->ty`) exactly like the Python backend and builds a document for the pretty-printer;
the difference is the **runtime**: C has no lists, maps, sized strings or exceptions, so the generated file starts with a small runtime (`tools/codegen/rt/cgrt.h`,
about 700 lines, embedded in `codegenc_rt.inc` and copied into every output; `python3 tools/codegen/rt2inc.py` regenerates the `.inc`, `testc.sh` checks it is fresh).
The output is C99, **warning-free under `gcc -std=c99 -Wall -Wextra -Wpedantic` at `-O0 -O1 -O2 -O3 -Os`**, and clean under ASan and UBSan (leak checker included).
Every runtime function is `static inline` (an unused one is not a warning) and the file is one translation unit with the generated functions non-`static`
(so it can also be used as a library: `cg_init()` once, call, `cg_arena_free()`; `gen c --tests` appends a `main` with the `test` lines).

### Representation

| UIR | C | Notes |
|---|---|---|
| `int` | `long long` | every `+ - * // %` and unary `-` is a checked helper (`cg_iadd`, ...): overflow throws `integer overflow` (Python has unbounded ints: a documented divergence); `//` and `%` floor, divisor 0 throws |
| `float`, `bool` | `double`, `int` | `/` throws `division by zero` like Python; `str(float)` reproduces Python's `repr` (shortest digits, `1e+16`, `1.5e-07`, `100.0`) |
| `str` | `CgStr {const char *p; long long n}` | by value, bytes, no NUL needed (slices are views, no copy), the null string has `p == NULL`; `len`, indices and slices count **bytes** (ASCII and UTF-8 pass through intact, but `len("é")` is 2) |
| `list[T]` | `CgList *` = vector + `ek` (element kind: int, float, str, bool or any) | one C type for all lists: typed fast accessors for the four scalar kinds (`cg_li_get`), tagged storage for `any`; a list is a reference, aliasing works as in Python |
| `map[str,T]` | `CgMap *` | open-addressing hash over insertion-ordered arrays (iteration and `keys` order = Python's), same `ek` scheme, FNV-1a |
| class | `C_Name *` | struct allocated from the arena; `N_Name` is the constructor (field defaults, then `init`), `M_Name__method` the methods; defaults of omitted arguments are filled **at the call site** (a list or map default is created per call) |
| `any` | `CgV` | tagged value (`nil bool int float str list map obj`); `any` appears when the validator could not infer a type (untyped parameter, `list[list[int]]` element, `raw`-defined name) |
| `Error` (the `e` of `catch e`) | `CgStr` | the message; `errmsg(e)` is `e`, `throw e` rethrows it |
| names | `u_f`, `C_Cls`, `N_Cls`, `M_Cls__m`; locals as written | locals are only renamed when they collide with a C keyword, a macro or the `cg`/`u_`/`C_`/`M_`/`N_` prefixes |

### How each construct is handled

- **Typing and `any`.** The generator never invents types: it reads `n->ty` and the declarations. Where the type is `any` it emits a `CgV` and **dynamic** operations
  (`cg_varith`, `cg_veq`, `cg_vcmp`, `cg_vget`, ...) with run-time type errors; conversions between a static type and `any` are explicit (`cg_vint(x)`, `cg_ui(v)`).
  A typed list or map coming back from `any` goes through `cg_ul`/`cg_um`, which check the element kind (an empty list is re-tagged in place, so `let xs = []` passed to a
  `list[int]` parameter still aliases). Methods and fields on an `any` receiver use generated dispatch helpers (`cg_dm_deposit_1(recv, a0)`: a `switch` on the class id of the
  object), so `let accs = [Account(..), Counter()]` followed by `for a in accs: a.deposit(1)` works. A construct that cannot be typed at all is refused with a message that
  says what to annotate (`null` where a number is expected, `/=` on an `int`). The type inference of module C will turn most `any` into precise types without changing this file.
- **Exceptions.** `try/catch/finally` is `setjmp`/`longjmp` over a stack of handlers (`cg_enter`/`cg_leave`); `throw` stores the message and jumps; an uncaught error prints
  `uncaught error: ...` and exits. `return`, `break` and `continue` inside a `try` pop the handler and **inline the `finally` code** of every region they leave (the
  design of section 4: `finally` on both paths); an exception inside `catch` runs the `finally` and rethrows. Because of `setjmp`, every local and temporary of a function that
  contains a `try` is declared `volatile`; `-Wclobbered` is disabled in the file (the inlined helper parameters trigger it falsely, and everything live across the jump is volatile).
- **Evaluation order.** C leaves the order of function arguments and operands unspecified; the Python reference is left to right. Operands that call something (or read the heap
  while another operand calls) are hoisted into temporaries sequenced by the comma operator: `(cgt1 = f(x), g(cgt1, h(y)))`. Plain and short-circuit operators need nothing.
- **Memory.** One arena (64 KiB blocks, big requests get their own block); `cg_arena_free()` releases everything, so there is nothing to leak by construction (the differential tests
  run under LeakSanitizer). The last allocation grows in place (vectors) and the last string is appended in place (`s += "x"` in a loop is amortized O(1)); everything else is
  garbage until the arena is freed, so a program that builds millions of temporary strings uses memory proportional to that, and the arena is not for long-running servers.
- **Library calls.** All 31 built-ins are implemented. `sort` is a stable merge sort (a new list, like `sorted`); `matches` is a backtracking regex engine in the runtime
  (literals, `.`, classes, `\d \w \s \b`, groups, `|`, `* + ? {m,n}` and lazy forms, anchors, a step budget; no captures or back-references) checked against Python's `re`
  on 18 patterns x 20 subjects; `int("...")`/`float("...")` follow Python (whitespace, underscores, errors catchable).
- **Formatting.** Documents with `group`/`soft` like the other backends: a call whose arguments do not fit in `--width` breaks one argument per line.
- **`raw`.** `raw c` is inserted verbatim (module level: before the functions; in a body: as a statement). `defines y` names must be declared by the block as `CgV y = ...;`.
  A `raw` without a `c` version is an error that lists the versions present (tasks 14 and 15).

### Known divergences from the Python reference (all intentional, all reported by the validator or documented here)

integer width (64-bit, overflow throws); `len`/index/slice/`reverse`/`upper`/`lower` on non-ASCII text work on bytes; `max(1, 2.5)` and `sum` of an empty float list return floats where
Python returns the int; a float variable initialised from an int literal prints `3.0` (Python backend: `3`); runtime error texts match Python for the common ones
(`list index out of range`, `'key'`, `division by zero`) but not for all; regular expressions are the subset above; recursion depth is the C stack; `list[int]` passed where
`list[float]` is expected is copied (aliasing lost).

### Tests (module B)

`sh tools/codegen/testc.sh` (`SAN=1` for ASan+UBSan): the runtime `.inc` is fresh; the 13 raw-free tasks compile with no warning and pass their tests; refusals; five hand-written
cases (`cases/`: nested `any`, dynamic dispatch on objects, null/any, regex and string library against Python's `re`, control flow with `finally`) and one C-only case; and
`difftest_targets.py`, which generates random valid programs (int arithmetic with floor, lists, maps, strings, floats, `for`/`while`/`break`/`continue`, `try/catch/finally`, functions,
a class) and compares **the whole stdout** of the generated Python and the generated C. Result when this was written: 200 of 200 programs equal under ASan/UBSan (2925 output lines)
and 200 of 200 on each of two other seeds without sanitizers; `python3 tools/codegen/difftest_targets.py CODEGENCLI -n 200 --seed 1 [--san]` reproduces it.

## 12c. Backend Go (`codegengo.c`)

Same structure, much simpler runtime because Go already has strings, maps, a garbage collector and `panic`/`recover` (`tools/codegen/rt/cgrt.go`, embedded in `codegengo_rt.inc`; the
output is a single `package main` file). The output passes **`gofmt -l` unchanged and `go vet`**, and `go run` executes the harness (Go 1.26 was used).

| UIR | Go |
|---|---|
| `int`, `float`, `bool`, `str` | `int64` (checked `cgAdd`, `cgMul`, ...: overflow panics), `float64`, `bool`, `string` (bytes; a null `str` is `""`) |
| `list[int]` / `list[any]` | `*List[int64]` (generic, `Get/Set/Push/Pop`) / `Lister` (interface: the dynamic view every `*List[T]` implements, so a `list[int]` flows into `list[any]` without copying) |
| `map[str,T]` / `map[str,any]` | `*Map[T]` (insertion ordered) / `Mapper` |
| class | `*Name`, `newName(...)` constructor, methods with a `self` receiver |
| `any` | Go `any`; `cgAs[T](v)`, `cgBox`, `cgArith`, `cgEq`, `cgCmp`; methods and fields on an `any` receiver use generated type-switch helpers (`cgdm_deposit_1`) |
| `throw` / `try` | `panic(CgErr{msg})` / `cgTry(body, catch, finally)` with closures; `return`, `break`, `continue` inside a closure become control codes (1, 2, 3) that the code after `cgTry` acts on (the value of a `return` goes through `cgrv`); `finally` runs by `defer`, also when a `catch` panics |

Details that came from running `gofmt` and `go vet` on the output: variables never read get `_ = x`; the ternary is a `func() T {...}()` (only one branch runs); struct fields are aligned;
call arguments that do not fit break with a trailing comma (a new document node, `dbrk`, prints text only in the broken form); indentation is tabs; float `+ - *` and `str +` are written as
operators only when the expression is the whole statement (gofmt tightens operators by nesting depth, so inside other expressions they are `cgFAdd`/`cgCat` calls); `a && a`, `x = x` and
code after `return`/`throw`/`break` are dropped because `go vet` refuses them. Divergences from the C backend: no `raw` (the raw languages are python, js, ts and c; any `raw` is an error);
a `return`/`break`/`continue` that leaves a `finally` block is refused; `null` for `str` is `""`; operands are not hoisted (Go evaluates calls left to right, but a variable read versus a call in the
same expression is unspecified); `x[i] = f()` evaluates the container before `f()` (Python evaluates `f()` first).
`sh tools/codegen/testgo.sh` (skips with a notice when there is no `go`): 19 checks including the 13 tasks and the five cases; `difftest_targets.py --target go` compared 600 random programs
(seeds 1..200, 1000..1199, 3000..3199): all equal.

## 13. Phase plan

Modules that different agents can build **in parallel**, each against the contract in `src/coder/codegen_api.h` and the 15 tasks; every module ships tests
under `tools/codegen/` and keeps `sh tools/codegen/test.sh` green. Interfaces between modules are the AST (`CgxNode`) and the arena; nothing else is shared.

| # | Module (files) | Does | Interface | Done when |
|---|---|---|---|---|
| A | JS and TS backends (`codegenjs.c`) | `CgxBackend` for `js` and `ts` (types as annotations), helpers for `//` and `%` floor, `catch`, classes, harness for `node` | `gen(g, opts, code, tests)` registered in `CGX_BACKENDS` | the 13 raw-free tasks pass their tests on node 20; TS output passes a bracket/scanner check (no `tsc` here) |
| B | C backend (`codegenc.c`), then Go (`codegengo.c`) | **done** (sections 12b and 12c): a real runtime (arena, tagged values, lists, maps, strings, `setjmp` exceptions) instead of the limited MVP; 13 of 13 raw-free tasks, 200 random programs per seed, ASan/UBSan clean; Go with `gofmt` and `go vet` clean | same | tasks 01, 04, 06, 10, 12, 13 compile with `gcc -Wall -Werror` and pass; unsupported constructs give the exact message |
| C (done) | Type inference (`codegeninfer.c`) | HM with let-polymorphism over `codegentype.c`, gradual `any`, classes, replaces the type part of `cgx_check` | `int cgx_infer(Cgx *, CgxTypes *)`, same error texts, `n->ty` filled (plus `n->tid`) | all current error tests unchanged; `fn first(xs)` infers `list[a] -> a`; unification tests added to `selftest` |
| D | **done** (section 16). IR and analyses (`codegencfg.c`, `codegenflow.c`, `codegenopt.c`) | CFG, dominators (CHK), SSA, worklist solver, reaching defs, liveness, SCCP, definite assignment, call graph (Tarjan), unused/unreachable/pure reports, optional folding | `cgx_ir_build`, `cgx_flow_run`, `cgx_opt_fold` (see the API header) | dumps stable for the tasks; a seeded set of 20 programs with known dead code, unused names and unreachable branches is reported exactly, no false positive on the 15 tasks |
| E | Validator and runner (`codegenrun.c`) | per-target syntax tools, scanner cross-check (`coder_parse`: every spec symbol present with the right kind), raw balance (`se_mask`/`se_balance`), sandboxed execution of `test` lines (limits: CPU time, memory, tmp cwd, no env), graded report text | `int cgx_validate(Cgx *, const CgxBackend *, const char *code, CgxB *report)` | report grades match section 8; RAW never executes unless asked; timeouts and crashes are reported, not fatal |
| F | Tool and integration (`codegentool.c`, `src/tools/codegen.c`, table, `docs/tools.md`) | `CodeGen` tool, `path`/`symbol`/`where` through `ct_edit_symbol`, create/insert, index `extern` lookup, include in `katana.c`, `tools/tooltest/codegen.sh` | `codegen_tool(js, end, out, cap)` | declaration under the token limit (tests that check it green), `sh build.sh` and the tool tests (files, meta, find, misc, loopnotes, lsp, launch, symedit, render) green |
| G | Protocol and model measurement | the description text and few-shot for the tool, repair loop, a natural-language-to-code spec harness (20 requests) run with Qwen on CPU when the machine is free, repair counts | uses F | measured valid-first-try rate and repairs per task, reported honestly, with the raw fraction |
| H | Language growth (`codegen.c`, `codegencheck.c`, all backends) | tuples, closures/lambdas, `extern` declarations, modules/imports, typed errors, more built-ins (set, sort by key, string format), Go, Java and Rust backends | grammar change in section 2 first, then parser, checker, each backend | each addition covered by a task and by every backend, or rejected by the checker for the targets that lack it |
| I | Operation Registry (`codegenops.c`) | moves `CGB`, `CGO_*` and the builtin mappings of every backend into one extensible table (section 2b.1) | `const CgxOp *cgx_op_find(const char *name)`, `cgx_op_register(const CgxOp *)` | `codegencli ops` lists name, arity, types, level, targets; the checker and `codegenpy.c` read it; a new operation is one row; tests that every row has a mapping or an explicit "unsupported" per backend  **Done (section 15).** |
| J | Incremental AST (`codegeninc.c`) | symbol-level and statement-level patching of existing files (section 2b.4) | `int cgx_inc_apply(const char *file, Cgx *spec, ...)` over `coder_parse` ranges and `ct_edit_symbol` | editing one function of a 200-line file leaves every other byte unchanged (byte-compare test); an identical unit writes nothing; `patch` forms covered  **Done (section 15).** |
| K | Dependency graph and code cache (`codegendep.c`, `codegencache.c`) | section 2b.5 and 2b.6; canonical dump `cgx_dump` | `cgx_dep_build/order/cycles`, `cgx_cache_get/put(key)` | cycles found in seeded specs; order verified by a C backend test; a second call with the same spec hits the cache, a changed dependency misses, a generator version bump misses  **Done (section 15).** |

Order: A, B, C, D, E, I, J, K can start now and are independent (J needs F only for the tool wiring) (A and B touch only their own file plus one line in `CGX_BACKENDS`; C and D only read the AST). F needs
E's report shape and at least A; G needs F; H is continuous and must wait for a freeze of the grammar by the coordinator between rounds.

The Optimization Engine is part of module D; Typed IR resolution and ambiguity part of C; the Registry (I) should land **before** A and B grow their
mappings (or those two keep their mappings in a local table that I then absorbs).

## 14. Left out of phase 1

Java and Rust backends (JS and TS: section 17; C and Go: sections 12b and 12c); the Operation Registry as one table (done, section 15); the incremental AST for other languages than Python;  the `CodeGen` tool, the inclusion in `katana.c`, `docs/tools.md`, tooltests; insertion by symbol through `Edit`; the
validator/runner with the graded report (the CLI runs Python tests only, with `ulimit` and `timeout`, which is not a sandbox); the coder index lookup for
`extern` functions; tuples, closures, imports, inheritance, typed errors, regex outside `matches`, formatting; the protocol text for the model and any
measurement with the real model (**not run**: it needs the model and the machine); training or curriculum (outside this work by decision).

## 15. Registry, incremental AST, dependency graph and cache (modules I, J, K)

Built from `main` 646da16. Everything is `static`, standard library only, compiled by `tools/codegen/codegencli.c` and by `src/katana.c` (which defines `CGX_INC_KATANA`
before the includes; the build has no codegen warnings); the katana writer was compiled but not run. The hooks for the katana build are the macro `CGX_INC_KATANA` (writer through `ct_edit_symbol`) and the include order (the one in `src/katana.c`).

### 15.1 Operation Registry (`codegenreg.c`)

One table, loaded from data. The built-in operations are a text in the same format the owner can extend (`CGX_REG_DATA`), parsed on first use; the checker
(`codegencheck.c`) takes arity, usage text, argument type masks, result type and level from it, and the Python backend (`codegenpy.c`) renders each call from the
row's template (`py_tmpl`). The old `CGB` table, the `switch` of type rules and the `switch` of Python forms are gone; only the checks that look at several
arguments at once (`push` fixes the element type, `join` wants `list[str]`, `str` counts float/bool use, `contains` and `errmsg` special cases) stay as
named hooks in `cgx_builtin`, keyed by the `CGB_*` ids that the embedded text fixes by order.

Format (lines; `#` comment; the lines of an operation are indented):

```
op clamp 3 3 level=1 effect=pure          # name, min and max arity, level L1..L3, effect pure|reads|mutates|io|throws
  use clamp(x, lo, hi)                     # text shown in messages
  sem x limited to the range lo..hi        # normative semantics, one line
  arg 0 num : a number                     # allowed types: num all any int float str bool list map class void null, joined by |
  arg 1 num : a number
  ret num0                                 # int float str bool void any list[str] | =0 (type of arg 0) elem0 num0 num01 sum0
  py min(max({0}, {1}), {2})               # template per target: py js ts c; optional prec=N import=NAME
  js[map] Object.keys($0).length           # variant chosen by the type of argument 0 (js[map], ts[str]...)
  js unsupported: needs a helper           # explicit "not available" with its message
rule (ADD x 0) => x if int x               # simplification: (HEAD args) patterns, vars are one letter, conditions: int x, str x, pure x
```

Template language: `$N` argument N as an operand (parenthesized when needed), `{N}` argument N alone, `{N@P}` with minimum precedence P, `{N?}` empty if
absent, `{*N}` arguments N.. as a breakable parenthesized list, `{[i,j]}` the listed ones. The backend builds documents, so the same template breaks long
argument lists correctly.

Loading: `cgx_reg_load_file(path)` or `KATANA_KATANA_CODEGEN_OPS=file` (read at first use) or `codegencli --ops file ...`; an operation with an existing name replaces it;
errors carry `line N: ...`. A function of the spec with the name of any registered operation is rejected (the message lists them, including the user's).
`codegencli ops` lists name, arity, level, effect, result and targets; `codegencli ops --dump` writes the whole registry in the loadable format (the test loads
the dump and dumps again: byte-identical). Today every row has `py`, `js` and `ts` (a template or an explicit `unsupported`); `c` only for `len` (explicit
unsupported), the C backend (module B) adds its column. JS and TS columns are data only: no backend reads them yet.

Simplification (`cgx_reg_simplify`, `codegencli gen ... --simp`): rules rewrite the validated AST to a fixed point. The seed rules are the ones that hold in
every target: `x+0`, `0+x` (int only), `x-0`, `x*1`, `1*x`, `x*0` and `x-x` and `x%1` (int and pure), `x//1` (int), `not not`, `--x`, `and true`, `or false`,
`and false` and `or true` (pure `x`), `abs(abs x)`, `upper(upper s)`, `lower(lower s)`, `trim(trim s)`, `sort(sort xs)`, `str(str x)`, and `reverse(reverse s)` for str
only (for lists it would turn a copy into an alias). "Pure" means no effect and cannot throw: a call counts only if its registry effect is `pure`, a division
only with a non-zero literal divisor, an index never. `x/1` is not rewritten (true division gives a float). The 13 raw-free tasks, simplified, still pass.

### 15.2 Incremental AST (`codegeninc.c`)

**Reading.** `cgx_inc_parse` is a Python scanner of a subset: it splits the file into logical lines (strings, triple-quoted strings, brackets, comments and
backslash continuations are handled by the same tokenizer that later finds names), builds the block tree by indentation and keeps for every statement,
`def`, `class` the byte range (decorators included) and the line range. A statement's range is extended over its `elif/else/except/finally` clauses and over
its children. The file is refused (with the line) when the structure is not reliable: inconsistent indentation, unbalanced brackets or an unterminated string,
a block header without a body.

**UIR-H.** `cgx_inc_uir` writes the code spec of the file, one code spec line per statement, and runs the one parser on it (`cgx_parse`). What the code spec knows becomes `fn`, `class`,
`const`, `let`/assignment, `if/elif/else`, `while`, `for` (ranges and collections), `try/catch/finally`, `throw`, `return`, `doc`; everything else
(`import`, `with`, decorators, `lambda`, comprehensions it cannot restate, `*args`, async, one-line defs, class inheritance...) is a `raw python` block carrying the
statement's own text and the names it defines. If the parser rejects a line the statement responsible becomes `raw` and the file is translated again (up to 12
rounds, then everything is `raw`). `cgx_inc_range(inc, uir, node, ...)` gives any node's interval: its `line` is a code spec line, the line map points to the statement,
the statement holds the bytes. On the four fixtures 19 of 38 top-level units are structured and the rest `raw` (shapes.py: 2 of 6, because classes with a base
class stay `raw`).

**Changing.** A change specification is a list of operations; each runs on the text the previous one left, produces edits `[a,b) -> text` and everything outside
the edits is copied byte for byte (comments, spacing, quote style, order, `\r\n` endings). If any operation fails, nothing is written. After each operation the
result is scanned again and must still be structurally valid.

| operation | what changes |
|---|---|
| `add-function [after N \| before N \| end]` | new lines only; `end` goes before `if __name__ ...` (and its comment); `import math`/`import re` is added after the last import if the generated code needs it |
| `add-method CLASS [after NAME]` | new lines in the class body, with its indentation unit |
| `replace-function NAME` | the symbol's lines (leading comments stay; decorators are replaced with the definition). Identical text: no edit |
| `replace-body NAME` | from the end of the header to the end of the function; a leading docstring is kept unless the new body has `doc` |
| `rename OLD NEW` | only the identifier tokens: the definition and the uses; not strings, comments, imports, keyword arguments, attribute uses of a function name, or a function where the name is a parameter or a local |
| `add-param FN text [call=EXPR]` | one inserted piece in the header (before `*args` when present; multi-line headers get a new line in their own style, trailing comma kept) and, with `call=`, `name=EXPR` in every call |
| `insert FN start\|end\|before N\|after N [vars a b]` and `insert-py ...` | new lines at a statement boundary; comments above a statement stay with it; `start` is after the docstring |
| `delete-function NAME` | the symbol, the comments attached to it and the blank lines that followed |

Code spec payloads go through the real validator and Python backend (errors say `spec line N: ...` relative to the payload); for methods the engine declares the fields it finds
(`self.x`, class attributes) so `self.x` checks. `rename Class.method` renames the definition and every `.method`; it says so, because other classes may have it.
`Class.method` addresses members everywhere a name is taken.

**Writing.** `cgx_inc_apply_file(path, spec, writer, ctx, dry, ...)` returns the result and, through `cgx_inc_diff`, the symbol-level actions (`replace NAME`,
`replace Class.method` when only methods changed, `after`/`before NAME`, `delete NAME`). The default writer is atomic (temporary file in the same folder,
`rename`, permissions kept). With `-DCGX_INC_KATANA` the writer `cgx_inc_write_symedit` sends every action to `ct_edit_symbol` (structural validation, callers,
LSP, undo, the confirmation hook); after the actions it reads the file back and, if text outside symbols changed (imports), writes the expected final text
atomically. `codegencli inc apply FILE SPEC [--write] [--acts]` prints the result (or writes it), `inc read` lists the units with bytes and lines, `inc uir` prints the code spec.

### 15.3 Dependency graph and code cache (`codegendep.c`)

**Canonical form** (`cgx_dump`, `codegencli dump`): a text of the UIR without lines, spacing, JSON-versus-code spec and alias spelling, with types; comments and docstrings only
when they will be printed; declarations sorted by name. `codegencli key SPEC` prints the key of every unit.

**Graph.** Nodes are the units of one or more specs (modules) and module nodes; edges are `call`, `type` (a class named in a signature or on a receiver), `use` (a constant read),
`field` (a class field typed with a class) and `import` (explicit, or derived from a cross-module reference). Resolution is by name, same module first; names that are
locals (parameters, `let`, loop variables) are not constants. Algorithms: iterative Tarjan (a 2000-function ring does not touch the stack), stable topological order of the condensation
with a min-heap on declaration index (dependencies first; ties by file order; members of a group in file order), transitive dependents by reverse BFS. Cycles: a loop through
`use`/`field` edges (constant reading constant, a class containing itself) is an error with the path and lines and how to break it; a constant that calls a function that reads
it is an initialization-cycle error; recursion (self or mutual) is a note, or an error with the call path when `allow_rec` is 0 (`codegencli dep SPEC [--no-rec] [--nocheck]`).

**Cache.** Key per unit = mix64(FNV-1a64 of: generation context (target, `CGX_GEN_VERSION`, digest of the registry dump, width, comments, indent, extra flags), the unit's canonical hash, the
hash of its group (so mutual recursion shares fate), the sorted keys of the dependencies outside the group). Each dependency key already contains its own dependencies, so the
invalidation is transitive and exact: change `mid` and `mid` and `top` miss, a sibling still hits; change a constant and its readers, and their callers, miss; bump the version or load another
registry and everything misses. Value file `UCGC1 key length checksum meta` + payload, named `key.uc` in `$KATANA_KATANA_CODEGEN_CACHE` or `~/.katana/codegen-cache`. Write: temporary file + `rename`. Read: header, key, length and
FNV checksum are checked, a bad file is deleted, counted as `corrupt` and regenerated. LRU: a hit `utimes` the file; when the bytes written since the last scan exceed 1/8 of the limit
(`KATANA_KATANA_CODEGEN_CACHE_MAX`, default 64 MB) the folder is scanned and the oldest files go until 90% of the limit. `cgx_gen_cached` generates Python by unit (identical text to `cgx_gen_python`, checked on the 15 tasks, cold and warm)
and also keeps one entry for the whole program (a warm run is one read); other targets cache the whole program (the declaration order is part of that key), tests are never cached. `--topo` assembles in dependency order instead of file order
(for targets that need definitions first). Windows: the disk cache compiles to stubs (always a miss) for now.

**Measured** (`codegencli cachebench N`, chain of N functions, Python, this machine under other load, so the proportions are what counts):

| N | no cache | cold cache | warm cache | change the last function | change the first function |
|---|---|---|---|---|---|
| 300 | 4.1 to 7.2 ms | 9.6 to 14.8 ms | 3.0 to 5.1 ms (300 hits) | 1 miss | 300 misses |
| 2000 | 35 to 62 ms | 76 to 156 ms | 23 to 48 ms (2000 hits) | 1 miss | 2000 misses |

The honest reading: the Python backend takes about 15 to 30 microseconds per function, so a warm cache only saves about 30%, and the cold cache costs about twice generation (hashing and one file per unit). Computing keys
(canonical text and hashes) is most of the warm time. The cache pays when generation is expensive (a backend with inference, formatting or validation by an external tool); until then it is correct, cheap and measured, not a speed-up for Python.

### 15.4 Limits

- The reader is a subset scanner, not a full Python grammar: no `match`, no f-string nesting with the same quote, no Python 2. It refuses (instead of guessing) inconsistent indentation, unbalanced brackets and headers without bodies. Tabs count as 8 columns, mixed tabs and spaces are only as reliable as Python itself.
- Only Python is read and changed; the registry's `js`/`ts`/`c` columns are data without a backend.
- Operations work at the top level and one class level; nested functions are not addressable (a rename still reaches their tokens). `rename` is token-based without type information: attribute uses of a method name are renamed everywhere, strings such as `__all__ = ["name"]` are not.
- `replace-body` and `insert` need a signature the generator can restate (no `*args`, `**kwargs`, `/`, keyword-only marker); use `insert-py` or `replace-function` there. Names of existing locals are known to a code spec `insert` only through `vars`.
- `replace-function` drops decorators and the inline comments of the old definition (they are part of it); `rename`, `add-param`, `insert` and `delete-function` do not touch them.
- An inline comment at the end of a replaced body line goes with the line. The UIR-H of the file has no comments (they stay in the bytes).
- The registry has 192 operations and 128 rules at most; templates have no conditionals except the per-type variants; the checker's built-in-specific hooks are code, not data.
- The checker's table of functions was raised from 160 to 4096 entries (the 2000-function benchmark needed it); symbols per scope (600) and classes (40) are unchanged.
- The katana writer (`cgx_inc_write_symedit`) compiles inside `katana.c` but was not run (no model, no desktop here); the standalone writer and every test run without it.

## 16. UIR-G: graphs, analyses and the optimizer (module D)

Done. Files: `src/coder/codegencfg.c` (graphs, CFG, SSA), `codegenflow.c` (data-flow, SCCP, report), `codegenopt.c` (passes). Included in
the katana build like the rest of the core, and in `tools/codegen/codegencli.c` through `tools/codegen/graphcli.h`. Everything is plain C, `static`, `cgx_`/`Cgx`.
The katana file-level graph (`coder/graph.c`, a CSR cache of file relations in a store section) was read and **not reused**: it is a persisted, mmap-able
derived index of files, with typed weighted edges, and the UIR graphs are small, mutable, per function; only the CSR layout idea is the same (`CgxDg`).

```
UIR-H (AST, checked) --lower--> CFG per function --dominators--> pruned SSA --> SCCP / liveness / reaching defs / available exprs / definite assignment
        ^                                                                                 |
        +---------------- rewrites (node -> value map) <---- optimizer rounds <-----------+      report: findings with [structural] / [derived]
```

### 16.1 Data structures

- **`CgxDg`**: immutable digraph in CSR, successors `sp/s`, predecessors `pp/p`, and `pe[k]` = index in `s[]` of the edge behind predecessor slot `k`. Repeated
  edges are merged. An edge is one integer, and a phi argument is the predecessor slot (`cgx_dg_pslot`). Used for the CFG and for the call graph.
- **Bit sets** (`CgxW` = 64-bit words, `cgx_bit_*`), n sets of nbits contiguous: every data-flow value.
- **`CgxDom`**: `idom`, reverse post-order (`rpo`, `rponum`), dominator tree (CSR), DFS entry/exit times (`a dom b` in O(1)), dominance frontier (CSR).
  **`CgxLoops`**: natural loops per header with body bit sets and nesting depth.
- **`CgxFnIr`** per function or method: `var[]` (source variables and hidden ones: `$t` for the join of `and`/`or`/`?:`, `$t` for the counter of a `for`, the
  iterator of a `for each`, the carrier of a `finally`), `val[]` (SSA values: temporaries and versions of variables, `val 0` = undefined), `ins[]` (three-address
  instructions), `bb[]` (blocks: instruction ids, `exc` = target of the block's exceptional edge), `nm[]` (map AST node id -> value / instruction / block).
- **Instructions**: `param const copy bin un call mcall list map index field global setidx setfld iter hasnext next catch rawuse rawdef phi br jmp ret throw undef`.
  Operands are ints: `>= 0` a value, `< 0` a variable (only before SSA), so the same instruction format serves before and after SSA, and every analysis reads
  variables through `cgx_opvar`.
- **The node map** is what lets analyses write back to the AST: every expression node records the value its consumer used (`NM_OP`: instruction + operand slot, so a
  `VAR` node resolves to the SSA version at that use), or its temporary (`NM_VAL`); every `let`/assignment records its defining instruction (`NM_DEF`); every
  statement the block it starts in (`NM_BLK`). A node lowered more than once (a `finally` is copied on every exit path) has several entries, and a rewrite needs all of
  them to agree.

### 16.2 CFG construction

Blocks come from the structured nodes (no goto, so the graph is reducible except for what `finally` copies create). `if`/`while`/`for`/`for each`/`try` become
blocks and edges; `and`, `or` and `?:` are **control flow** (short circuit: the right side is its own block, the result joins in a hidden variable, SSA makes the phi), which
is what lets SCCP decide `false and f()`. `for i in a..b step k` keeps a hidden counter (the body may assign `i` without changing the iterations, as `range`).
Compound `x op= e` computes into a temporary and copies after: if the operation raises, `x` has not changed.

- **Exceptions.** Inside a `try`, an instruction that may raise (call that is not known to be total, `/ // %` with a divisor that is not a non-zero literal, indexing,
  mixed int/float arithmetic, `throw`, `raw`) ends its block with an **exceptional edge** to the `catch` block; a block's exceptional edge applies to the whole block, so the
  body of a `try` starts in a new block. Outside a `try` there are no exceptional edges (the exception leaves the function).
- **`finally`** is copied: once for the normal exit, once per `return`/`break`/`continue` that crosses it (innermost first, the returned value is copied into a temporary
  before the copies run), and one landing block for the exceptional exit that runs the copy and **rethrows** (an exceptional edge to the next outer handler, or out).
- `raw`: one `rawuse` that reads every variable in scope, then a `rawdef` per visible variable and per name it `defines` (opaque writes), so nothing is assumed across it.
- Unreachable statements (after `return`) still get blocks, with no predecessors; they are kept out of the graph's edges, dominators and SSA, and reported.

### 16.3 Algorithms and complexity

n blocks, e edges, v variables, w = v/64 words, and the usual "height" of the lattice.

| Part | Algorithm | Cost |
|---|---|---|
| Dominators | Cooper-Harvey-Kennedy: iterate over the reverse post-order with `intersect` on idoms | O(n^2) worst case, near O(e) on structured graphs (2 passes) |
| Dominance frontier | CHK runners from each join predecessor up to the idom (the entry counts a virtual extra predecessor, so a loop back to it is right) | O(sum of frontier sizes) |
| Dominator tree queries | DFS entry/exit times | O(1) per query |
| Natural loops | back edge u->h with h dom u; body by reverse reachability; same header merged | O(n e) worst, small in practice |
| SCC | Tarjan, **iterative** (explicit stacks; a 200000-node chain is tested) | O(n + e) |
| SSA | Cytron: iterated dominance frontier per variable, **pruned** with live-in (no phi where the variable is dead, which scoped `let`s would otherwise get at every loop header); renaming by an iterative walk of the dominator tree with per-variable stacks | O((n + e) v / 64 + phis) |
| Out of SSA | each phi gets a fresh hidden variable; `t = arg` is appended before the terminator of each predecessor and the phi becomes `d = t`; then versions return to their variable. Parallel-copy swap and lost-copy cases cannot happen: copy targets are never sources. Valid for an IR that was not transformed after construction | O(phis x preds) |
| Data-flow solver | one **worklist** over bit-set lattices (`CgxDfa`): direction, meet (union or intersection), gen/kill or a custom transfer, boundary value; processes in (reverse) RPO, requeues successors on change; blocks without information yet are the top of the lattice | O(h * e * w) |
| SCCP | Wegman-Zadeck: CFG edge worklist + SSA value worklist, lattice top > constant > bottom (height 3, so every value changes at most twice); phi meets only the **executable** predecessors; a `br` on a constant bool opens one edge | O(e + SSA edges) |
| Dead values | mark from roots (instructions with effects, terminators, anything that may raise), through operands, only in executable blocks | O(ins) |
| Call graph | edges from calls resolved by the checker (`ref`), methods called on an untyped receiver reach every method of that name; Tarjan; effect summaries by fixpoint inside each component, callees first | O(n + e) |

Measured: a 15 000 line function (15 001 blocks, 55 000 instructions, 5 000 phis) is analysed in 0.6 s and optimized (3 rounds) in 0.3 s.

### 16.4 Data-flow instances

| Analysis | Direction | Meet | Facts | Used for |
|---|---|---|---|---|
| Liveness | backward | union | variables read before written | pruned SSA |
| Reaching definitions | forward | union | definition sites (every instruction that writes a variable, not phi) | dragon book test; available to reports |
| Available expressions | forward | intersection | `op a b` over variables and constants, killed by a write to an operand | redundant-computation findings |
| Definite assignment | forward | intersection | variables certainly written | use before definition (and unresolved names) |
| SCCP | sparse | meet on the value lattice | constants per SSA value, executable blocks and edges | folding, dead branches, unreachable code |
| Value liveness (DCE) | backward on SSA | | values reachable from roots | dead assignments, unused variables |

### 16.5 Effects, purity and what may be removed

Every expression and function has a mask: `THROW` (may raise), `WRITE` (changes memory, prints, raw), `READ` (reads mutable memory), `LOOP` (may not terminate:
`while`, recursion). **Removable** = none of THROW, WRITE, LOOP. The table is conservative on purpose: `any` operands may raise; `int` with `float` may raise (a huge int
does not convert: `OverflowError`); `/` may raise unless both operands are float with a non-zero literal divisor; `//` and `%` unless the divisor is a non-zero int literal;
`int(x)` only for int/bool; `float(x)` only for a literal int or a float; `sqrt floor pow matches` always; indexing always; calls take the callee's summary (builtins have
their own row). A `catch` is assumed to catch everything raised in its body. A function is *pure* (removable) when its summary is, so a call to it can disappear
when its value is unused; recursion and loops never are.

### 16.6 Constant folding: the Python semantics, and what is refused

Folding happens in SCCP (per value) and is written back only when the node is **removable** (a constant that came through a `?:` with a condition that has effects is not).
`//` and `%` are floored (`-7 // 2 = -4`, `7 % -2 = -1`), `/` is true division (both operands must be exactly representable, |n| <= 2^53; the result must be finite),
`int(float)` truncates, `max/min` return the winner **with its own type** (`max(1, 1.0)` is `1`). Not folded: division by zero (the program must still raise), a result that
overflows 64 bits (Python's integers are unbounded; the generated code must compute the real value), `inf`/`NaN` results, int with float beyond 2^53, comparisons of
text that is not ASCII, `upper/lower/trim/indexof/slice/reverse` on non-ASCII text (concatenation, equality, `len` as code points, `contains`, `startswith` are fine), `sqrt`
of a negative, `str(float)`/`str(bool)` (the targets disagree), `bool == int` (Python says `True == 1`), lists, maps and field/element reads. `-0.0` is kept and printed
as `-0.0`; `x == x`, `x - x`, `x + 0.0` are not simplified for floats (NaN, inf, -0.0); `not (a < b)` is not turned into `a >= b` (NaN).

### 16.7 Findings and certainty

Each finding carries a grade, never "exact": **[structural]** is a fact about the spec text (a variable with no read, code after `return`, a `while true` with no `break`
in its condition literal, a recursive call), **[derived]** comes from an analysis (a branch that SCCP proves constant, a dead store, a path to the end of the function, a function
nobody calls). Kinds: `unreachable`, `dead-branch`, `unused-var`, `dead-store`, `use-before-def`, `undefined-name`, `missing-return`, `infinite-loop`; info only: `unused-function`
(possibly an entry point), `recursion`, `redundant-expr`, `unused-param`. The 15 tasks give no warning (the two `raw` tasks give the info "never called"). `codegencli flow spec`
prints them sorted by line, then the **generation order** (callees before callers, Tarjan order) and the size of the work.

### 16.8 The optimizer (`cgx_opt_run`, `CgxOptOpts.passes`)

| Pass | Does | Default |
|---|---|---|
| FOLD | replaces nodes SCCP proved constant (literals, propagated variables, builtins on constants); decides `if`/`elif`/`while` with a constant condition: the taken branch is spliced into the parent (unless a name it declares collides with a later declaration: then the dead branch is only emptied), `while false` is removed | on |
| ALGEBRA | `x+0 0+x x-0 x*1 1*x x//1` (int), `x*0 x%1` (int, operand removable), `x-x`, `x==x` (int/str/bool), `x*1.0` (float), `"" + s`, `a and true`, `true and b`, `a and false` (a removable), `or` likewise, `not not x`, `not (a == b)`, `- -x`, `true ? a : b` | on |
| COPY | `let b = a` ... use of `b` becomes `a` if `a` is still the version that reached the copy (checked on the dominator tree: the nearest definition of `a` above the use is the copy's source) | on |
| DCE | removes assignments whose value no live instruction reads when the right-hand side is removable; a dead `let x = f(a)` becomes the statement `f(a)`; a dead `let` of a variable that is assigned later (no `let`) keeps its declaration with a cheap value; removes pure expression statements and empty `if`s | on |
| UNREACH | removes statements whose block SCCP did not reach | on |
| TEMPS | `let t = e` followed by the only use of `t`: `e` moves into that statement. Only when `e` has no effect and reads no memory, or when the use is the whole right-hand side (`return t`, `let y = t`). Own pass, only in a round where nothing else changed | on |
| INLINE | a call to a non-recursive top-level function whose body is a single `return expr` of at most 14 nodes: arguments substituted (trivial arguments copied, others once or if removable and small; an argument with effects only if used once, unconditionally, and the body has none of its own); refused when a caller's local has the name of a function the body calls | off |
| GLOBALS | module `const` with a literal initializer counts as a constant (the name disappears from the code) | off |

Rounds repeat until nothing changes (at most 10; the second run over the output changes nothing, tested). The AST is edited in place, names are the author's, `cgx_number` runs
again at the end (ids stay unique and stable). `codegencli opt spec [--all]` prints the counts; `codegencli optgen python spec [--all] [--tests] [--recheck]` optimizes and generates
(`--recheck` runs the checker again on the result).

### 16.9 Tests and results

Run `sh tools/codegen/graphtest.sh [N]` (about 35 s with N=300; `SAN=1` for ASan/UBSan) and `sh tools/codegen/test.sh` (the 60 phase-1 checks, still green).

- **`graphtest.c`, 5 099 checks**: idoms, `dominates`, dominance frontier, reverse post-order, SCC (components and their order) and natural loops of **500 random graphs** compared with
  naive algorithms (sets to a fixed point, definition of the frontier, mutual reachability); the dragon book CFG (fig. 9.38: idoms, four loop headers and nesting depth) and the
  Cooper-Harvey-Kennedy irreducible graph (fig. 2); the worklist solver against round-robin iteration on 1 600 random instances (forward/backward x union/intersection);
  reaching definitions of the dragon book fig. 9.12 (`IN[B2] = {d1,d2,d3,d5,d6,d7}`), liveness and available expressions on hand-solved graphs; a 200 000 node chain; SSA verifier
  (every use dominated by its definition, phi arity, one definition per value) on the 15 tasks and on 80 generated programs, plus "no phi without a use" (pruned), a stable dump across builds
  and out-of-SSA leaving no phi; the call graph (mutual recursion as one component, callees-first order, effect summaries); 24 seeded programs whose warnings are matched exactly;
  about 70 folding and simplification cases on the Python edges (floor, division by zero, 64-bit overflow, 2^53, NaN, `-0.0`, accented text, short circuit with a raising operand,
  inlining, readable names); idempotence on the tasks and the corpus.
- **Differential test** (`difftest.py`, generator `fuzz_gen.py`): random valid programs (typed, scoped, loops bounded; try/catch/finally, break/continue, classes, lists, strings, recursion,
  module constants, plus patterns made to break a careless optimizer: `(a // b) * 0`, `(x > 0) and false`, copy then change the source, return inside `try` with a `finally` that
  reads a variable). For each, the Python before and after optimization (default passes, and `--all`) runs with the same random arguments (zero, negatives, `2**53+1`, `2**63-1`,
  `2**64+5`, `-2**70`, `10**30`, `nan`, `inf`, `-0.0`, `1e308`, empty and accented text, empty lists); the **value, the exception (type and text), the printed output and the lists
  mutated** must be identical. 1 000 programs, 62 860 calls (33 335 raise, 14 401 print, 7 255 with arguments beyond 64 bits): **0 differences**; the optimized result also passes
  the checker again, parses as Python, and the optimizer changed all 1 000 programs (17 021 folds and propagations, 14 224 algebraic rewrites, 16 383 dead statements, 4 709 unreachable
  statements, 4 649 decided branches, 503 copies, 402 temporaries, 803 inlined calls). Seeds are fixed, so a run is reproducible.
  Mutation check: breaking the floor in `//`, the removability guard of `x*0`, `and false`, `or true`, the float identity, the copy-source check, `finally` copies, the short-circuit
  rule, the exceptional edge or the removable mask each makes the test fail (10 of 11 mutants killed; the generator needed the "stress" statements to kill four of them; the survivor, a phi that ignores which edges are executable, only loses precision and is not a bug).
- **IR interpreter** (`codegencli irexec spec pre|ssa|out`): runs the IR of the scalar functions before SSA, in SSA (phi chosen by the arriving edge) and after out-of-SSA, and must equal
  the Python on 60 096 results (1 000 programs x 3 phases); it is what found that a `try` body must start a new block (an instruction before the `try` shared a block, and so the
  exceptional edge, with the protected call).
- The tasks, optimized with the default passes and with `--all`, still pass their own tests (26 runs).

### 16.10 Limits

- Only locals are tracked. Lists, maps and objects are opaque references: no alias analysis, no constant elements or fields, no value numbering across memory; `push`, `pop`, element and field
  stores and every impure call are barriers (they are never removed or reordered).
- Without an enclosing `try`, a raising instruction has no edge (the exception leaves the function), so facts about code after a call are optimistic about *that* path only in the
  sense that the path does not exist; inside a `try` the graph is conservative (the edge carries the state at the end of the block).
- Arithmetic that may raise only because of astronomically large integers (beyond ~1e308 mixed with floats) is treated as raising, so such code is never removed or folded; nothing
  else about big integers is approximated: operations that would overflow 64 bits are simply left to Python.
- Termination is never changed: `while` loops are kept unless their condition is a provable `false`; a call that may not terminate (recursion, loops) is kept; an endless loop is only
  *reported* (no reachable exit), not proven non-terminating or terminating for the general case.
- No loop-invariant motion, unrolling, strength reduction, common-subexpression elimination (the redundant computation is **reported**, as info), or code motion across statements.
  Inlining takes single-`return` functions only; temporaries only when adjacent.
- The phase-1 checker rule "every path returns" is syntactic (the last statement). The optimizer can prove the end unreachable and remove the final `return`; the Python is right,
  but a later `cgx_check` of the optimized tree can complain. A backend that needs the final `return` (C) should run the checker's rule on the original, or keep the tail.
- The unused-variable and dead-store findings count a variable as read when it is visible to a `raw` block (the block may read it).
- `codegentype.c` (HM) is not used here: types are the checker's (`ty`), `any` is treated as "may raise". The registry (module I) is used only to map a built-in name to its id (`cgx_op_find`); the effect table stays in `codegencfg.c`.
- The IR interpreter is a test tool (scalars, strings of small size, no lists/classes), not a backend.

## 17. JavaScript and TypeScript backends (module A, `src/coder/codegenjs.c`)

Registered as `javascript` (alias `js`, `.js`) and `typescript` (alias `ts`, `.ts`). One generator serves both: it walks the checked AST and builds a document for the
Wadler/Lindig printer; `ts` only turns on type annotations, which come from the IR type of each node through the type table of `codegentype.c` (`CgxTy` -> `CgxTid` ->
text: `list[int]` is `number[]`, `map[str,T]` is `Map<string, T>`, a class is its name, `any` is `any`, the caught error is `unknown`). `codegencli gen js|ts <spec> [--tests]` and
`codegencli run js|ts <spec>` (node; TypeScript is compiled first by the executable named in `KATANA_CODEGEN_TSC`, `--strict --target es2022`, and only generated when the variable is not set).

**Style, and why.** Functions and the harness helpers are `function` declarations, not arrows: they hoist (recursion and forward calls behave as in Python, where a function
body is resolved at call time), they have names in stack traces, and `this` is never captured by accident; arrows are used only for the `test` thunks and callbacks. Locals are
`const` unless the name is the target of an assignment anywhere (one pre-pass over the AST, keyed by node id), then `let`; module constants are always `const`. Statements end in
`;`, blocks use braces, `===` and `!==` only. A chain of `+` over `str` with a literal and a `str(x)` becomes a template string (`` `sum ${t} of ${s}` ``). Loops: `for (const x of xs)`
for lists and strings, `for (const [k, v] of m)` for maps, `for (let i = a; i < b; i++)` for ranges; if the bound is not provably stable (a literal, or an unassigned numeric
variable, or arithmetic of those) it is evaluated once in the loop head (`let i = 0, _e1 = len + n`), as `range` does; if the body assigns the loop variable, the loop runs on a hidden counter
and the body gets its own `let`. `catch (e)` becomes `catch {` when `e` is unused. A `doc` line is a JSDoc block; in JS the author's declared types add `@param {T}` and `@returns {T}`,
which gives editors type checking without TypeScript. The output is a classic script (`"use strict";` unless the spec has a `raw` block, whose code is not ours to restrict) and, in TS,
a module (`export {};` first, so top-level names never collide with the lib globals).

**Semantics, how they are kept identical to Python.** Where the JavaScript operator differs, the generator calls a *runtime helper* written once in `CGX_JS_H` (TypeScript text; the
type marks are cut for JS). Only the helpers a program uses, and the ones they need (a dependency list per helper), are copied, in front of the code. Operators whose meaning is the same
are emitted raw.

| UIR | JavaScript | Divergence removed |
|---|---|---|
| `a // b`, `a % b` | `Math.floor(a / 2)` for a non-zero literal, `a % 3` when `a` is provably >= 0 and the divisor a positive literal, `a % 3 === 0` for zero tests, else `_fdiv`/`_mod` (exact floor via the remainder) | truncation toward zero; `%` sign |
| `a / b` | `a / 2` for a non-zero literal, else `_div` | division by zero is `Infinity`/`NaN` in JS; the helper throws `division by zero` |
| `xs[i]`, `xs[i] = v` | `_at(xs, i)`, `_put(xs, i, v)`, `_sat(s, i)`, `_mget(m, k)`, `_ix(c, k)` for `any` | negative indices, `IndexError`/`KeyError` instead of `undefined` |
| map | `Map` (literal `new Map([["a", 1]])`, `m.set`, `m.has`, `.size`) | insertion order for every key (an object puts `"10"` before `"b"`), no `__proto__` keys |
| `len`, index, `slice`, `reverse`, `indexof` of `str` | `_slen`, `_sat`, `_sslice`, `_rev`, `_find` (fast path when no surrogate pair is present) | UTF-16 units instead of code points |
| `<` on `str` | `_scmp(a, b) < 0` | UTF-16 order differs from code point order for astral characters |
| `==` on list, map, `any` | `_eq` (deep); scalars `===` | reference equality |
| `str(x)`, `print(x)` | by static type: `String(n)`, `_fstr(x)` for float (`2.0`, `1e-05`, `1e+16`: CPython repr), `x ? "True" : "False"`, `_str` for the rest (Python repr of lists and maps; `list[float]` and `map[str,float]` pass a flag so `[1.0, 2.5]` keeps its `.0`) | `2` vs `2.0`, `true` vs `True`, `[object Object]` |
| `int(x)`, `float(x)` | `_int`, `_float` (Python whitespace set, `_` digit separators, `inf`/`nan`, the CPython error texts) | `Number("")` is 0, `parseInt("12abc")` is 12 |
| `sqrt`, `pow` | `_sqrt`, `_pow` raise `math domain error`/`math range error` | `NaN` and `Infinity` |
| `trim`, `words`, `split` | `_trim`, `_words` (the `str.isspace` set, linear scan), `_split` (empty separator is an error) | JS `trim` strips U+FEFF and keeps U+001C..1F and U+0085 |
| `sum`, `sort` | `_sum` (Neumaier compensated, as CPython >= 3.12 does for floats), `sort(xs)` copies; numbers `(a, b) => a - b`, `str` `_scmp` | float summation order, in-place sort |
| `pop`, `contains`, `replace`, `keys` | `_pop` (error on empty), `includes`/`has`/`_has`, `split(a).join(b)` (`_replace` for the empty pattern), `[...m.keys()]` | `undefined` results; `$&` patterns of `String.replace` |
| `any` as a condition or an operand of `and`/`or`/`not` | `_truth` (Python truth: `[]`, `""`, `0`, empty map are false) | JS truthiness |
| an `int` that becomes a `float` (`float(n)`, `n / m`, `n * 1.5`) | `(n + 0)` unless `n` is a literal or provably non-negative | JS keeps `-0` from `0 * -3`; `str(float(...))` would print `-0.0` where Python prints `0.0` |
| `b == true`, `b == false`, `b == c` on `bool` | `b`, `!b`; TS casts `(b as boolean) === c` | `const f = true` makes `f === false` a TS2367 error (the compiler narrows to the literal) |
| `throw "m"`, `throw e`, `errmsg(e)` | `throw new Error("m")`, `throw e`, `_errmsg(e)` | none (`catch` catches every error, including runtime ones) |
| class | JS class, fields as class fields (fresh per instance), `constructor` from `init`, `new C(...)`, `self` is `this` | none |

Order of evaluation follows the Python backend, not the JS defaults, where they differ and the operands have effects or can fail: `c[k] = v` evaluates `v` first (the JS form hoists it into
a block `{ const _v1 = v; _put(c, k, _v1); }`), `x in c` and `contains(c, x)` evaluate `x` first (`_in(x, c)`), `join(xs, sep)` evaluates `sep` first (`_join(sep, xs)`); compound
assignment to an element with a call in its target hoists the container and the key into temporaries. Iterating a map whose body may insert a key goes through `_mapit`, which raises
`dictionary changed size during iteration` like CPython. Temporaries and helpers start with `_`; the generator rejects user names that start with `_`, JavaScript keywords, and the globals
the output uses (`Math`, `Map`, `Error`, `console`, ...), with a message naming the name.

**What stays different, and is reported here instead of hidden.**
1. *Integers.* JS numbers are doubles: integers are exact to 2^53. A literal outside that range is a generation error; arithmetic that leaves it silently rounds (Python is unbounded). Spec
   authors that need big integers must use a `raw` block. `int` and `float` are one runtime type, so the *static* type decides how `str`/`print` format a number: a float variable that holds an
   integral value prints `2.0` (as Python does if every value in it really is a float); an `int` stored in a `float` variable (Python keeps it `int` and prints `3`) or an `any` holding an integral float
   prints the integer form. Lists and maps whose element type is `any` print integral floats without `.0`.
2. *Error texts* of the emulated runtime errors are those of CPython 3.14 (`division by zero`, `list index out of range`, `'k'` for a missing key, `expected a nonnegative input, got -1.0`, ...); older
   Pythons word a few differently (`math domain error`, `integer division or modulo by zero`). Errors that come from the JS runtime itself (`raw` code, `any` misuse: `TypeError: Cannot read ...`) have their own text;
   deep recursion raises `RangeError` at about 10^4 frames, `RecursionError` at 10^3.
3. *Regular expressions* (`matches`) are JS `RegExp` without the `u` flag (the ASCII-punctuation escapes Python allows stay valid): `\d`, `\w`, `\s` are ASCII-only (Python: Unicode), no
   `(?P<name>...)`, no inline flags, `$` does not match before a final newline. Patterns that mean the same in both dialects behave the same.
4. *Case mapping* (`upper`, `lower`) uses the Unicode tables of the runtime (`ß` becomes `SS` in both), which can differ between versions of Python and node for recently added characters.
5. *`Math.pow` and `Math.sqrt`* are IEEE `sqrt` and V8's own `pow`: the last bit of a non-integer power can differ from the C library's (not seen in the random tests below).
6. *Hoisting.* Functions are declarations, so a module constant initialised by calling a function defined later works in JS where Python raises `NameError`.
7. *Object printing.* `str()` of a class instance is `<Name object>` (CPython prints an address). A `raw` JS block that produces plain objects (`JSON.parse`) gives `any` values: `_ix`, `_iter`, `_items`, `_keys`,
   `_len` and `_has` accept `Map`, arrays, strings and plain objects.
8. *Modules.* Nothing is exported: the file is a script (or a TS module with no exports). Add `module.exports` or `export` in a `raw` block; imports are not UIR (section 2.1).
9. *TypeScript.* The UIR has no optional type: a class-typed field, a reassigned class-typed variable, a class parameter with a `null` default and a function that returns `null` are typed `any`
   (`T | null` arrives with the inference module). a `const` initialised by a literal (or by `float(3)`, `c ? 1 : 2`, `str(flag)`) gets its base type (`const n: number = 1`) and an `===` between two literal-typed operands is cast (`(1 as number) === 2`), because `tsc` types such constants as the literal and rejects the comparison. `any` elsewhere is `any`, the caught error is `unknown` (and goes to helpers that take `unknown`). Lists and maps of classes or of nested
   collections are `any[]` / `Map<string, any>` because `CgxTy` keeps only the base element kind. `target ES2022` (class fields, `catch {`; ES2019 with the fields rewritten by the compiler), `lib es2017` or newer (`Object.entries`, `padStart`).
10. *A `raw` block* needs a `raw js` version (TypeScript uses `raw ts` when present, else `raw js`); otherwise generation fails with the list of versions the block has (task 17 has only Python).

**Tests** (`sh tools/codegen/jstest.sh [N]`, `KATANA_CODEGEN_TSC=<tsc>` adds TypeScript; a `tsc` is one `npm i typescript` away and is not vendored): the 13 raw-free tasks pass their `test` lines on node 20 and,
compiled with `tsc --strict`, as TypeScript; task 14 runs against a real JSON file; three hand-written programs of edge cases (`tools/codegen/js/edge_*.kcode`: floor division and modulo for every sign
combination, zero divisors, float formatting, `int()`/`float()` parsing, `sum` of floats, unicode strings with astral characters, whitespace, negative indices, slices, maps with numeric-looking keys, loops
whose bound or variable changes in the body, `try/finally`, rethrow, classes, defaults, `any`) print the same lines on Python, JS and TS; and `jsfuzz.py` generates random valid programs (typed expression
generator over int, float, str, bool, `list[int]`, `list[str]`, `map[str,int]`, functions with defaults, a class, nested control flow, every operation that can fail wrapped in `try/catch` that prints `errmsg`) and
compares the standard output of the three targets line by line.
