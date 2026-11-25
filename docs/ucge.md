# UCGE: Unified Code Generation Engine

Status: phase 1 (design and core) done on branch `feat-ucge`; phase 2 module A (JavaScript and TypeScript backends, section 15) is on branch `feat-ucge-js`; the rest of phases 2 and 3 is planned at the end of this page. The core is **not yet part of the
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
- Flow-sensitive refinement (for nullable and union types, after `if x != null`) is a **pass over UIR-G** using the dataflow framework of section 6, not
  part of unification: unification stays simple and the flow facts come from the one analysis engine.
- Type errors name both types and the expected one (`ADD of int and str (expected two numbers or two str; convert with str(x))`).

Phase 1 behaviour (checker in `ucgecheck.c`, intentionally simpler): declared types plus local inference of `let`, literals, built-ins and calls; list and
map element type is fixed by the first `push` or literal; return type inferred in two passes; `any` for the unknown.

## 6. Algorithms (decisions and why)

1. **Parsing**: hand-written, line-oriented with an indent stack, recursive descent with precedence climbing for expressions. No parser generator: the error
   messages ("expected X, found Y") and the leniency list are easier to control, and the grammar is small.
2. **Type inference** (phase 2, `ucgeinfer.c`): constraint-free algorithm W with union-find and levels (Remy): `let` generalizes variables whose level is
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
| `src/coder/ucgetype.c` | type table, union-find unification, generalization, schemes | 1 (not yet wired) |
| `src/coder/ucgedoc.c` | document algebra, renderer, `CgxGenOpts`, `CgxBackend` | 1 |
| `src/coder/ucgepy.c` | Python backend (reference) and the backend registry | 1 |
| `src/coder/ucgejs.c` | JavaScript and TypeScript backends (one generator, `ts` switches the annotations; section 15) | 2 (module A) |
| `src/coder/ucge_api.h` | the contract between phases (documentation only) | 1 |
| `tools/ucge/ucgecli.c` | standalone driver: `check gen run stats selftest` | 1 |
| `tools/ucge/test.sh` | tests (60 checks, optional ASan/UBSan with `SAN=1`) | 1 |
| `tools/ucge/jstest.sh`, `tools/ucge/js/` | JS/TS tests: tasks on node and tsc, edge cases, Python x JS x TS differential, random programs (`jsfuzz.py`) | 2 (module A) |
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
| C | Type inference (`ucgeinfer.c`) | HM with let-polymorphism over `ucgetype.c`, gradual `any`, classes, replaces the type part of `cgx_check` | `int cgx_infer(Cgx *, CgxTypes *)`, same error texts, `n->ty` filled (plus `n->tid`) | all current error tests unchanged; `fn first(xs)` infers `list[a] -> a`; unification tests added to `selftest` |
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

C, Go, Java, Rust backends (JS and TS: section 15); the Operation Registry as one table (only the built-in table of the checker exists); the incremental AST, dependency graph and code cache (designed in 2b, not built); HM inference (the table exists, the checker does not use it); CFG, SSA, dominators, the data-flow analyses, call graph, dead-code
and unused-name reports, constant folding; the `CodeGen` tool, the inclusion in `katana.c`, `docs/tools.md`, tooltests; insertion by symbol through `Edit`; the
validator/runner with the graded report (the CLI runs Python tests only, with `ulimit` and `timeout`, which is not a sandbox); the coder index lookup for
`extern` functions; tuples, closures, imports, inheritance, typed errors, regex outside `matches`, formatting; the protocol text for the model and any
measurement with the real model (**not run**: it needs the model and the machine); training or curriculum (outside this work by decision).

## 15. JavaScript and TypeScript backends (module A, `src/coder/ucgejs.c`)

Registered as `javascript` (alias `js`, `.js`) and `typescript` (alias `ts`, `.ts`). One generator serves both: it walks the checked AST and builds a document for the
Wadler/Lindig printer; `ts` only turns on type annotations, which come from the IR type of each node through the type table of `ucgetype.c` (`CgxTy` -> `CgxTid` ->
text: `list[int]` is `number[]`, `map[str,T]` is `Map<string, T>`, a class is its name, `any` is `any`, the caught error is `unknown`). `ucgecli gen js|ts <spec> [--tests]` and
`ucgecli run js|ts <spec>` (node; TypeScript is compiled first by the executable named in `UCGE_TSC`, `--strict --target es2022`, and only generated when the variable is not set).

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
10. *A `raw` block* needs a `raw js` version (TypeScript uses `raw ts` when present, else `raw js`); otherwise generation fails with the list of versions the block has (task 15 has only Python).

**Tests** (`sh tools/ucge/jstest.sh [N]`, `UCGE_TSC=<tsc>` adds TypeScript; a `tsc` is one `npm i typescript` away and is not vendored): the 13 raw-free tasks pass their `test` lines on node 20 and,
compiled with `tsc --strict`, as TypeScript; task 14 runs against a real JSON file; three hand-written programs of edge cases (`tools/ucge/js/edge_*.csp`: floor division and modulo for every sign
combination, zero divisors, float formatting, `int()`/`float()` parsing, `sum` of floats, unicode strings with astral characters, whitespace, negative indices, slices, maps with numeric-looking keys, loops
whose bound or variable changes in the body, `try/finally`, rethrow, classes, defaults, `any`) print the same lines on Python, JS and TS; and `jsfuzz.py` generates random valid programs (typed expression
generator over int, float, str, bool, `list[int]`, `list[str]`, `map[str,int]`, functions with defaults, a class, nested control flow, every operation that can fail wrapped in `try/catch` that prints `errmsg`) and
compares the standard output of the three targets line by line.
