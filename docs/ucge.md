# UCGE: Unified Code Generation Engine

Status: phase 1 (design and core) done on branch `feat-ucge`; phases 2 and 3 are planned at the end of this page. The core is **not yet part of the
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
| `src/coder/ucgec.c`, `ucgec_rt.inc` | C backend and its embedded runtime (generated from `tools/ucge/rt/cgrt.h` by `tools/ucge/rt2inc.py`) | 2 (module B) |
| `src/coder/ucgego.c`, `ucgego_rt.inc` | Go backend and its embedded runtime (generated from `tools/ucge/rt/cgrt.go`) | 2 (module B) |
| `src/coder/ucge_api.h` | the contract between phases (documentation only) | 1 |
| `tools/ucge/ucgecli.c` | standalone driver: `check gen run stats selftest` | 1 |
| `tools/ucge/test.sh` | tests (60 checks, optional ASan/UBSan with `SAN=1`) | 1 |
| `tools/ucge/testc.sh`, `testgo.sh`, `difftest.py`, `cases/`, `cases_c/` | C and Go backend tests: tasks, hand-written cases, random Python-versus-target differential programs | 2 (module B) |
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

## 12b. Backend C (`ucgec.c`)

Module B of the phase plan. The generator walks the typed AST (`CgxNode`, `n->ty`) exactly like the Python backend and builds a document for the pretty-printer;
the difference is the **runtime**: C has no lists, maps, sized strings or exceptions, so the generated file starts with a small runtime (`tools/ucge/rt/cgrt.h`,
about 700 lines, embedded in `ucgec_rt.inc` and copied into every output; `python3 tools/ucge/rt2inc.py` regenerates the `.inc`, `testc.sh` checks it is fresh).
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

`sh tools/ucge/testc.sh` (`SAN=1` for ASan+UBSan): the runtime `.inc` is fresh; the 13 raw-free tasks compile with no warning and pass their tests; refusals; five hand-written
cases (`cases/`: nested `any`, dynamic dispatch on objects, null/any, regex and string library against Python's `re`, control flow with `finally`) and one C-only case; and
`difftest.py`, which generates random valid programs (int arithmetic with floor, lists, maps, strings, floats, `for`/`while`/`break`/`continue`, `try/catch/finally`, functions,
a class) and compares **the whole stdout** of the generated Python and the generated C. Result when this was written: 200 of 200 programs equal under ASan/UBSan (2925 output lines)
and 200 of 200 on each of two other seeds without sanitizers; `python3 tools/ucge/difftest.py UCGECLI -n 200 --seed 1 [--san]` reproduces it.

## 12c. Backend Go (`ucgego.c`)

Same structure, much simpler runtime because Go already has strings, maps, a garbage collector and `panic`/`recover` (`tools/ucge/rt/cgrt.go`, embedded in `ucgego_rt.inc`; the
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
`sh tools/ucge/testgo.sh` (skips with a notice when there is no `go`): 19 checks including the 13 tasks and the five cases; `difftest.py --target go` compared 600 random programs
(seeds 1..200, 1000..1199, 3000..3199): all equal.

## 13. Phase plan

Modules that different agents can build **in parallel**, each against the contract in `src/coder/ucge_api.h` and the 15 tasks; every module ships tests
under `tools/ucge/` and keeps `sh tools/ucge/test.sh` green. Interfaces between modules are the AST (`CgxNode`) and the arena; nothing else is shared.

| # | Module (files) | Does | Interface | Done when |
|---|---|---|---|---|
| A | JS and TS backends (`ucgejs.c`) | `CgxBackend` for `js` and `ts` (types as annotations), helpers for `//` and `%` floor, `catch`, classes, harness for `node` | `gen(g, opts, code, tests)` registered in `CGX_BACKENDS` | the 13 raw-free tasks pass their tests on node 20; TS output passes a bracket/scanner check (no `tsc` here) |
| B | C backend (`ucgec.c`), then Go (`ucgego.c`) | **done** (sections 12b and 12c): a real runtime (arena, tagged values, lists, maps, strings, `setjmp` exceptions) instead of the limited MVP; 13 of 13 raw-free tasks, 200 random programs per seed, ASan/UBSan clean; Go with `gofmt` and `go vet` clean | same | tasks 01, 04, 06, 10, 12, 13 compile with `gcc -Wall -Werror` and pass; unsupported constructs give the exact message |
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

JS, TS, Java and Rust backends (C and Go are done, sections 12b and 12c); the Operation Registry as one table (only the built-in table of the checker exists); the incremental AST, dependency graph and code cache (designed in 2b, not built); HM inference (the table exists, the checker does not use it); CFG, SSA, dominators, the data-flow analyses, call graph, dead-code
and unused-name reports, constant folding; the `CodeGen` tool, the inclusion in `katana.c`, `docs/tools.md`, tooltests; insertion by symbol through `Edit`; the
validator/runner with the graded report (the CLI runs Python tests only, with `ulimit` and `timeout`, which is not a sandbox); the coder index lookup for
`extern` functions; tuples, closures, imports, inheritance, typed errors, regex outside `matches`, formatting; the protocol text for the model and any
measurement with the real model (**not run**: it needs the model and the machine); training or curriculum (outside this work by decision).
