# UCGE: Unified Code Generation Engine

Status: phase 1 (design and core) done on branch `feat-ucge`; module C (type inference, section 5b) done on `feat-ucge-infer`; phases 2 and 3 are planned at the end of this page. The core is **not yet part of the
katana build** (`src/katana.c` does not include it; the files are `static` and compile standalone with `tools/ucge/ucgecli.c`). Phase 2 adds the include
line after `coder/symedit.c` and the `CodeGen` tool.

## 1. Decision and idea

The owner's decision (2026-11-24): the model does **not** write the final code. It **describes what to build** in a compact structured specification (the CSP,
"code specification protocol"); the engine turns that into code. Pipeline:

```
Intent (LLM) -> CSP text/JSON -> typed AST (UIR-H) -> CFG + SSA (UIR-G) -> analyses -> AST -> doc -> language adapter -> validator -> code
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

## 2. CSP: the input syntax

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
"TRY_CATCH" | "THROW" | "CALL" | "METHOD" | "RAW" | "BREAK" | "CONTINUE" | "TEST", ...}`; an expression is either a **string written in the CSP expression
syntax** or an object (`ADD SUBTRACT MULTIPLY DIVIDE IDIVIDE MOD COMPARE AND OR NOT CALL METHOD VAR LIT LIST MAP INDEX FIELD` with `args`). The engine converts
the JSON to the normalized CSP text and runs the one parser (`Cgx.norm` keeps the text, error lines refer to it), so there is a single grammar to maintain
and the two forms cannot drift. An unknown `op` answers with the list of valid ones.

The owner's JSON style is accepted too: an operation object with `left`, `right` and `result` (`{"op": "ADD", "left": "a", "right": "b", "result": "r"}` is
`let r = a + b`), and `COMPARE` with `"cmp"`. In JSON a plain string is an **expression** (`"a + b"`, `"xs[0]"`); a string literal is `{"op": "LIT",
"value": "text"}`. Size, measured on three tasks (characters / 4, JSON with whitespace removed, the CSP without comments): task 01 JSON 66 vs CSP 32.5 tokens
(2.0x), task 04 121 vs 55.8 (2.2x), task 10 135.5 vs 55.3 (2.5x). The compact form is the primary one for the small model for that reason (and because it is
closer to code it was trained on); JSON stays for tools that emit structured data. Files: `tools/ucge/json/`.

## 2b. Engine components (the owner's requirements) and where each lives

The core of the engine is an **AST builder driven by a typed IR**: backends never fill templates or join strings; they walk typed nodes and build documents.
Generation is graph-based: the dependency graph orders the work, the IR graph feeds the analyses, and the AST is the only thing printed.

1. **Operation Registry** (`ucgeops.c`, phase 2; seed: the `CGB` table and `CGO_NAME` in phase 1). One row per operation known to the engine: `name`, `arity`
   (min, max), `input types` and `output type` as type-table schemes (`len: forall a. list[a] | str | map[a] -> int`), `semantics` (a short normative text
   and an effect flag: pure, reads, mutates, io, throws), `level` L1/L2/L3, and **per-language mapping**: a template of *document constructors* (not a text
   template: receiver precedence, argument breaking and imports are handled by the printer) plus the imports it needs and the targets it is unsupported
   for (with the message). Extensible: a backend adds rows for its own library; a project can add an `extern` row (section 9) at run time. The validator,
   the inference, the analyses (effects, purity) and the backends all read this one table, so adding an operation is one row plus its tests.
2. **Typed IR with resolution** (`ucgeinfer.c`, phase 2). Name resolution (`ref`), overload and ambiguity resolution (an operation row can have several
   signatures: `+` on numbers or on str; `contains` on str, list or map; the unifier picks the first signature that unifies and reports an **ambiguity** with
   the candidates when an `any` argument leaves more than one), numeric promotion (`int <: float`), and the gradual `any` where information is missing. The
   typed AST (UIR-H) is the IR the backends read; UIR-G (section 4) is derived from it.
3. **IR Optimization Engine** (`ucgeopt.c`, phase 2): simplification, constant folding (SCCP-guided), dead-code and unreachable-branch elimination,
   unused-binding removal, and **dependency resolution** (a function that is never reached from the exported ones, or a `const` nothing reads, is reported;
   removal is opt-in). Passes run on UIR-H and UIR-G, preserve observable behaviour including thrown errors and effects, and report what they changed.
4. **Incremental AST** (`ucgeinc.c`, phase 2). To change existing code the engine does **not** rebuild the program: the existing file is parsed by the coder
   scanner (`coder_parse`: symbols with byte ranges, the same parse `Edit` by symbol uses); only the spec's top-level units (a function, a class, a method)
   are generated, and each is spliced into the file at the range of the symbol it replaces (or before/after an anchor) through `ct_edit_symbol`. The unit
   of change is a symbol: nodes outside it are never touched, never reprinted, so the user's formatting and comments elsewhere survive; the symbol's
   `shape` hash (already in `CoderSym`) tells whether the new unit differs from the old one at all (no write when it does not). A `patch` spec form
   (`patch NAME` followed by statements to replace, insert before/after a numbered statement, or delete) edits statements **inside** a function: the engine
   parses the old function with the scanner to find the statement ranges and regenerates only the affected statements; this is the finer grain.
5. **Dependency graph** (`ucgedep.c`, phase 2): nodes are the spec's units (functions, classes, constants, `raw` blocks, and `extern` symbols found in the
   index), edges from resolved references. Tarjan SCCs give the **cycles** (reported as errors for constants and class fields, allowed as recursion
   groups for functions); the condensation is topologically ordered, which is the **order of generation and of insertion** (definitions before uses for
   targets that need it, C and Go among them; the order of checking and inference too). Across files it is the same graph with module nodes and import edges.
6. **Code cache** (`ucgecache.c`, phase 2): key = hash (FNV-1a 64 over the **normalized UIR**: the canonical dump of UIR-H, which ignores comments, spacing,
   JSON versus CSP, and alias spelling) + hash of each dependency unit's normalized UIR + target + generation options (width, comments) + **generator version**
   (a constant bumped whenever a backend or the printer changes). Value: the generated code and the report. Stored under `~/.katana/ucge/` as one file per key
   with an LRU cap; **invalidation by dependency** is automatic because the dependencies' hashes are part of the key (a changed callee changes the caller's key);
   `raw` blocks hash their text. A hit skips generation and returns the stored result marked `[cache]`; tests are re-run unless the key also covers the test
   results (they do not: execution is never cached).

Hash of the normalized UIR needs a canonical dump (`cgx_dump`, phase 2; node ids from `cgx_number` are stable per spec, so the dump is deterministic).

## 3. Data structures

All in `src/coder/ucge.c` (names `Cgx*`, `cgx_*`, `CGN_*` kinds, `CGO_*` operators, `CGT_*` simple types, `CGB_*` built-ins, `TYK_*` table types; the
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
- **Type table** (`ucgetype.c`). Hash-consed types: one id per structure (`list[int]` is one integer everywhere), mutable type variables with union-find and
  path compression, levels for generalization, occurs check, gradual `any`, `T | null` (OPT), unions as sorted sets, tuples, function types, named classes,
  schemes by `TYK_GEN` placeholders. The phase 1 validator still uses the small inline `CgxTy` (kind + element kind); phase 2 replaces it with these.
- **Doc algebra** (`ucgedoc.c`). Documents (`text line soft hard cat nest group`) in their own arena; the renderer is Lindig's "Strictly Pretty" with an
  explicit stack (no recursion, no quadratic backtracking: `fits` looks only up to the next permitted break).

## 4. Representation in three levels

1. **CSP** (text or JSON): the model's surface. Only the parser sees it.
2. **UIR-H**: the typed AST of section 3. Language independent, structured control flow, every name resolved (`ref` points to the declaration), every
   expression typed (`ty`), built-ins identified (`bi`). This is what the checker annotates and what the backends read. Phase 1 stops here.
3. **UIR-G** (phase 2, `ucgeir.c`): a graph per function, built from UIR-H: basic blocks of three-address instructions (`op dst a b`, `call`, `load/store`
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

Phase 1 behaviour (checker in `ucgecheck.c`, intentionally simpler): declared types plus local inference of `let`, literals, built-ins and calls; list and
map element type is fixed by the first `push` or literal; return type inferred in two passes; `any` for the unknown.

## 5b. Type inference (`ucgeinfer.c`, module C)

`int cgx_infer(Cgx *g, CgxTypes *T)` (and `cgx_infer_ex(g, T, flags, report)`) types the whole program; `cgx_check_ex(g, T, infer)` is the validator entry that
runs the phase 1 rules first and the inference after them (`ucgecli check|gen|run|stats --infer` turns it on; `ucgecli infer FILE [--lets] [--uses] [--check]`
prints errors, the typed signatures with their certainty, and the gradual checks). It is **optional**: nothing in `cgx_check` changed, the 60 checks of
`tools/ucge/test.sh` are the phase 1 behaviour. It also runs without `cgx_check` (it reports the scope errors itself) so that specs can be typed alone.
One call per parse (the results are stored in the AST).

**What it writes.** Every typed AST node gets `tid` (an id in the caller's `CgxTypes`; `TYK_GEN(i)` is the i-th type parameter of the enclosing
function, so a backend with generics reads `list['a] -> 'a` straight from the `FN` node, and a monomorphizing one instantiates it), `cert` (1 derived,
2 declared, 3 gradual: **never "exact"**) and, where a gradual value meets a concrete slot, `chk` = the type that value must have there. `chk` is a mark
on the AST for the backend to turn into a cast or an assertion; the engine inserts no run-time test itself. Nothing is written into `n->ty`, so the
phase 1 backends keep reading what they read.

**Algorithm.** Algorithm J (destructive unification over the union-find of `ucgetype.c`) with generalization by levels (Remy): every variable carries the level
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
| built-ins | one rule each (`len: str|list|map -> int`, `sort: list[a] -> list[a]` with `a` ordered, `sum: list[a] -> a` with `a` numeric, `contains`, `slice`, `keys`, ...): `inf_builtin` in `ucgeinfer.c` is the single place, to be replaced by the Operation Registry rows (module I) |

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

**Tests.** `sh tools/ucge/infer_test.sh`: 361 cases in `tools/ucge/infer/cases_*.txt` (typed programs, exact error texts, polymorphism, recursion and mutual
recursion, unions, flow typing, gradual, classes, bidirectional, generic list helpers), the 15 tasks with `--infer`, and `tools/ucge/infer_prop.py`, a soundness
property: programs well typed **by construction** (random typed expression and statement generators, annotated and unannotated parameters, calls between the generated
functions) are inferred, generated to Python, executed with random arguments, and the observed type of every result must fit the inferred one; wrong-typed return
annotations and wrong-typed arguments are injected and must be rejected. Failure of either side fails the script (the script was checked by injecting a typing bug).

## 6. Algorithms (decisions and why)

1. **Parsing**: hand-written, line-oriented with an indent stack, recursive descent with precedence climbing for expressions. No parser generator: the error
   messages ("expected X, found Y") and the leniency list are easier to control, and the grammar is small.
2. **Type inference** (`ucgeinfer.c`, done, section 5b): algorithm J with union-find and levels (Remy): `let` generalizes variables whose level is
   deeper than the current one. `any` and `raw`-defined names are gradual. Classes get nominal types; method calls resolve through the class table.
3. **Control-flow graph and SSA** (phase 2, `ucgeir.c`): blocks from structured nodes (no goto, so the CFG is reducible by construction). **Dominators**
   by Cooper-Harvey-Kennedy (iterate over the reverse post-order with the `intersect` on idoms: simple, fast on reducible graphs, no Lengauer-Tarjan needed
   at these sizes). **Dominance frontiers** from idoms (CHK), **phi placement** with the iterated dominance frontier, renaming by a dominator-tree walk
   (Cytron et al.). Chosen over Braun's on-the-fly SSA because the dominator tree is also needed for loop detection and dead-code reasoning.
4. **Data-flow analyses** (phase 2, `ucgeflow.c`): one worklist solver over a lattice interface (`meet`, `transfer`, direction) and instances: **reaching
   definitions** (forward), **liveness** (backward, bit vectors), **constant propagation** (SSA sparse conditional constant propagation, Wegman-Zadeck, which
   also finds unreachable branches), **definite assignment** (forward, for `raw`-defined names and `try` blocks), **null-ness** (forward, feeds the optional type).
5. **Call graph** (phase 2): nodes are functions and methods (interned ids), edges from `CALL`/`MCALL` nodes already resolved by the checker (`ref`); SCCs by
   Tarjan give recursion groups (return-type inference order, "function never called" and mutual recursion warnings). Calls into the project's existing
   code come from the coder symbol index (section 9).
6. **Dead code and unused names**: from SCCP (unreachable blocks, always-true conditions), liveness (assigned and never read) and the call graph (functions
   nobody calls, reported as `[derived]`: by name, never "exact").
7. **Simplification and constant folding** (phase 2, `ucgeopt.c`): fold on UIR-H guided by SCCP results; only rewrites the author would not notice
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
  Error(text)`; C has no exceptions, so a C backend rejects `try`/`throw` with a message (use error returns or `raw c`). Error values are not typed beyond
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
   40 lines and 3 examples. Few-shot examples are the tasks of `tools/ucge/tasks/`.
2. The model writes the CSP (or the JSON) in the `spec` argument. 3. The engine answers with the code and the graded report, or with **the errors only**
   (line, what was expected, the offending line quoted): the model repairs the spec and calls again; the harness measures how many repairs it needs.
4. Training the model on the protocol is out of scope until the owner decides (the curriculum is final at the end of the UIOE): the plan is measured
   few-shot first.

## 11. Source map and API

| File | Content | Phase |
|---|---|---|
| `src/coder/ucge.c` | arena, interning, AST, lexer, parser (CSP + JSON), errors | 1 |
| `src/coder/ucgecheck.c` | built-in table, semantic validator, statistics | 1 |
| `src/coder/ucgetype.c` | type table, union-find unification, generalization, schemes | 1 |
| `src/coder/ucgeinfer.c` | type inference (HM + levels + gradual + flow typing), certainty report, `cgx_check_ex` | 2 (module C) |
| `src/coder/ucgedoc.c` | document algebra, renderer, `CgxGenOpts`, `CgxBackend` | 1 |
| `src/coder/ucgepy.c` | Python backend (reference) and the backend registry | 1 |
| `src/coder/ucge_api.h` | the contract between phases (documentation only) | 1 |
| `tools/ucge/ucgecli.c` | standalone driver: `check gen run stats selftest` | 1 |
| `tools/ucge/test.sh` | tests (60 checks, optional ASan/UBSan with `SAN=1`) | 1 |
| `tools/ucge/infer_test.sh`, `infer/cases_*.txt`, `infer_prop.py` | inference tests: 361 cases, the soundness property | 2 |
| `tools/ucge/tasks/*.csp`, `ref/*.py`, `bench.py` | the 15 tasks, hand-written Python, the measurement | 1 |

Build and test: `gcc -O1 -Wall -I. tools/ucge/ucgecli.c -o /tmp/ucgecli -lm`; `sh tools/ucge/test.sh`; `python3 tools/ucge/bench.py /tmp/ucgecli`.

## 12. What was measured (phase 1, no model, no GPU)

15 tasks written by the author as CSP (sum with default, filter a list, count and frequency of words, iterative factorial, Fibonacci, binary search, error
handling, a class with errors, email validation, maximum, FizzBuzz, palindrome, primality, JSON file total, fetch a page title), each with given tests.
**Coverage**: 13 of 15 (87%) without `raw`; the Python generated from the spec passes **all** given tests (53 cases). The two that need `raw` are exactly the
L3 kind (file and JSON library, HTTP): task 14 is a hybrid (the I/O in `raw`, the loop in UIR), task 15 is a function that is entirely `raw`. The author
wrote both the specs and the language, so 87% is an upper bound for a model: how often Qwen produces valid CSP was **not measured** (no model was run:
the machine was under load and the owner's rule is no GPU). Size, in tokens (characters / 4), for the 13 tasks without `raw`:

| | spec (no comments, no `test`) | generated Python | hand-written idiomatic Python |
|---|---|---|---|
| tokens | 679 | 786 | 584 |

The spec is **0.86x** of the generated code and **1.16x** of idiomatic code (comprehensions, `Counter`, `re.fullmatch`, `max` are shorter than their
spelled-out loops). With the `test` lines and comments the spec is longer still. For `raw` tasks the spec is longer than the code (task 14: 78 vs 38).
So the UIR does **not** reduce tokens against what a model would write directly; whatever it brings is determinism, one description for several
languages, and validation before generation. The generated Python follows its own style (type hints, explicit loops), which is why it is longer than
idiomatic code. The table per task is printed by `tools/ucge/bench.py`.

## 13. Phase plan

Modules that different agents can build **in parallel**, each against the contract in `src/coder/ucge_api.h` and the 15 tasks; every module ships tests
under `tools/ucge/` and keeps `sh tools/ucge/test.sh` green. Interfaces between modules are the AST (`CgxNode`) and the arena; nothing else is shared.

| # | Module (files) | Does | Interface | Done when |
|---|---|---|---|---|
| A | JS and TS backends (`ucgejs.c`) | `CgxBackend` for `js` and `ts` (types as annotations), helpers for `//` and `%` floor, `catch`, classes, harness for `node` | `gen(g, opts, code, tests)` registered in `CGX_BACKENDS` | the 13 raw-free tasks pass their tests on node 20; TS output passes a bracket/scanner check (no `tsc` here) |
| B | C backend (`ucgec.c`) | scalars, `str` as `const char *`, lists as pointer + length (parameters expand), classes as structs + functions, defaults filled at call sites; `try/throw` rejected with a message | same | tasks 01, 04, 06, 10, 12, 13 compile with `gcc -Wall -Werror` and pass; unsupported constructs give the exact message |
| C (done) | Type inference (`ucgeinfer.c`) | HM with let-polymorphism over `ucgetype.c`, gradual `any`, classes, replaces the type part of `cgx_check` | `int cgx_infer(Cgx *, CgxTypes *)`, same error texts, `n->ty` filled (plus `n->tid`) | all current error tests unchanged; `fn first(xs)` infers `list[a] -> a`; unification tests added to `selftest` |
| D | IR and analyses (`ucgeir.c`, `ucgeflow.c`, `ucgeopt.c`) | CFG, dominators (CHK), SSA, worklist solver, reaching defs, liveness, SCCP, definite assignment, call graph (Tarjan), unused/unreachable/pure reports, optional folding | `cgx_ir_build`, `cgx_flow_run`, `cgx_opt_fold` (see the API header) | dumps stable for the tasks; a seeded set of 20 programs with known dead code, unused names and unreachable branches is reported exactly, no false positive on the 15 tasks |
| E | Validator and runner (`ucgerun.c`) | per-target syntax tools, scanner cross-check (`coder_parse`: every spec symbol present with the right kind), raw balance (`se_mask`/`se_balance`), sandboxed execution of `test` lines (limits: CPU time, memory, tmp cwd, no env), graded report text | `int cgx_validate(Cgx *, const CgxBackend *, const char *code, CgxB *report)` | report grades match section 8; RAW never executes unless asked; timeouts and crashes are reported, not fatal |
| F | Tool and integration (`ucgetool.c`, `src/tools/codegen.c`, table, `docs/tools.md`) | `CodeGen` tool, `path`/`symbol`/`where` through `ct_edit_symbol`, create/insert, index `extern` lookup, include in `katana.c`, `tools/tooltest/ucge.sh` | `ucge_tool(js, end, out, cap)` | declaration under the token limit (tests that check it green), `sh build.sh` and the tool tests (files, meta, find, misc, loopnotes, lsp, launch, symedit, render) green |
| G | Protocol and model measurement | the description text and few-shot for the tool, repair loop, a natural-language-to-CSP harness (20 requests) run with Qwen on CPU when the machine is free, repair counts | uses F | measured valid-first-try rate and repairs per task, reported honestly, with the raw fraction |
| H | Language growth (`ucge.c`, `ucgecheck.c`, all backends) | tuples, closures/lambdas, `extern` declarations, modules/imports, typed errors, more built-ins (set, sort by key, string format), Go, Java and Rust backends | grammar change in section 2 first, then parser, checker, each backend | each addition covered by a task and by every backend, or rejected by the checker for the targets that lack it |
| I | Operation Registry (`ucgeops.c`) | moves `CGB`, `CGO_*` and the builtin mappings of every backend into one extensible table (section 2b.1) | `const CgxOp *cgx_op_find(const char *name)`, `cgx_op_register(const CgxOp *)` | `ucgecli ops` lists name, arity, types, level, targets; the checker and `ucgepy.c` read it; a new operation is one row; tests that every row has a mapping or an explicit "unsupported" per backend |
| J | Incremental AST (`ucgeinc.c`) | symbol-level and statement-level patching of existing files (section 2b.4) | `int cgx_inc_apply(const char *file, Cgx *spec, ...)` over `coder_parse` ranges and `ct_edit_symbol` | editing one function of a 200-line file leaves every other byte unchanged (byte-compare test); an identical unit writes nothing; `patch` forms covered |
| K | Dependency graph and code cache (`ucgedep.c`, `ucgecache.c`) | section 2b.5 and 2b.6; canonical dump `cgx_dump` | `cgx_dep_build/order/cycles`, `cgx_cache_get/put(key)` | cycles found in seeded specs; order verified by a C backend test; a second call with the same spec hits the cache, a changed dependency misses, a generator version bump misses |

Order: A, B, C, D, E, I, J, K can start now and are independent (J needs F only for the tool wiring) (A and B touch only their own file plus one line in `CGX_BACKENDS`; C and D only read the AST). F needs
E's report shape and at least A; G needs F; H is continuous and must wait for a freeze of the grammar by the coordinator between rounds.

The Optimization Engine is part of module D; Typed IR resolution and ambiguity part of C; the Registry (I) should land **before** A and B grow their
mappings (or those two keep their mappings in a local table that I then absorbs).

## 14. Left out of phase 1

JS, TS, C, Go, Java, Rust backends; the Operation Registry as one table (only the built-in table of the checker exists); the incremental AST, dependency graph and code cache (designed in 2b, not built); CFG, SSA, dominators, the data-flow analyses, call graph, dead-code
and unused-name reports, constant folding; the `CodeGen` tool, the inclusion in `katana.c`, `docs/tools.md`, tooltests; insertion by symbol through `Edit`; the
validator/runner with the graded report (the CLI runs Python tests only, with `ulimit` and `timeout`, which is not a sandbox); the coder index lookup for
`extern` functions; tuples, closures, imports, inheritance, typed errors, regex outside `matches`, formatting; the protocol text for the model and any
measurement with the real model (**not run**: it needs the model and the machine); training or curriculum (outside this work by decision).
