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
3. **UIR-G** (module D, `ucgecfg.c`, section 15): a graph per function, built from UIR-H: basic blocks of three-address instructions (`op dst a b`, `call`, `load/store`
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
| `src/coder/ucge_api.h` | the contract between phases (documentation only) | 1 |
| `tools/ucge/ucgecli.c` | standalone driver: `check gen run stats selftest` | 1 |
| `tools/ucge/test.sh` | tests (60 checks, optional ASan/UBSan with `SAN=1`) | 1 |
| `tools/ucge/tasks/*.csp`, `ref/*.py`, `bench.py` | the 15 tasks, hand-written Python, the measurement | 1 |
| `src/coder/ucgecfg.c` | digraph (CSR), Tarjan, dominators (CHK), dominance frontier, natural loops, effects and call graph, CFG per function, pruned SSA (Cytron), out of SSA, verifier, dump | D |
| `src/coder/ucgeflow.c` | worklist solver over bit-set lattices and its instances, constant folding, SCCP, dead values, findings report | D |
| `src/coder/ucgeopt.c` | optimizer passes written back to UIR-H, inlining, `cgx_opt_run` | D |
| `tools/ucge/graphcli.h`, `graphtest.c`, `graphtest.sh`, `fuzz_gen.py`, `difftest.py` | commands `ir flow opt optgen irexec`, unit tests, the runner, the program generator and the differential test | D |

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
| D | **done** (branch `feat-ucge-graph`, section 15). IR and analyses (`ucgecfg.c`, `ucgeflow.c`, `ucgeopt.c`) | CFG, dominators (CHK), SSA, worklist solver, reaching defs, liveness, SCCP, definite assignment, call graph (Tarjan), unused/unreachable/pure reports, optional folding | `cgx_ir_build`, `cgx_flow_run`, `cgx_opt_fold` (see the API header) | dumps stable for the tasks; a seeded set of 20 programs with known dead code, unused names and unreachable branches is reported exactly, no false positive on the 15 tasks |
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

JS, TS, C, Go, Java, Rust backends; the Operation Registry as one table (only the built-in table of the checker exists); the incremental AST, dependency graph and code cache (designed in 2b, not built); HM inference (the table exists, the checker does not use it);  the `CodeGen` tool, the inclusion in `katana.c`, `docs/tools.md`, tooltests; insertion by symbol through `Edit`; the
validator/runner with the graded report (the CLI runs Python tests only, with `ulimit` and `timeout`, which is not a sandbox); the coder index lookup for
`extern` functions; tuples, closures, imports, inheritance, typed errors, regex outside `matches`, formatting; the protocol text for the model and any
measurement with the real model (**not run**: it needs the model and the machine); training or curriculum (outside this work by decision).

## 15. UIR-G: graphs, analyses and the optimizer (module D)

Done on branch `feat-ucge-graph`. Files: `src/coder/ucgecfg.c` (graphs, CFG, SSA), `ucgeflow.c` (data-flow, SCCP, report), `ucgeopt.c` (passes). Not part of the
katana build yet (like the rest of the core): `tools/ucge/ucgecli.c` includes them through `tools/ucge/graphcli.h`. Everything is plain C, `static`, `cgx_`/`Cgx`.
The katana file-level graph (`coder/graph.c`, a CSR cache of file relations in a store section) was read and **not reused**: it is a persisted, mmap-able
derived index of files, with typed weighted edges, and the UIR graphs are small, mutable, per function; only the CSR layout idea is the same (`CgxDg`).

```
UIR-H (AST, checked) --lower--> CFG per function --dominators--> pruned SSA --> SCCP / liveness / reaching defs / available exprs / definite assignment
        ^                                                                                 |
        +---------------- rewrites (node -> value map) <---- optimizer rounds <-----------+      report: findings with [structural] / [derived]
```

### 15.1 Data structures

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

### 15.2 CFG construction

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

### 15.3 Algorithms and complexity

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

### 15.4 Data-flow instances

| Analysis | Direction | Meet | Facts | Used for |
|---|---|---|---|---|
| Liveness | backward | union | variables read before written | pruned SSA |
| Reaching definitions | forward | union | definition sites (every instruction that writes a variable, not phi) | dragon book test; available to reports |
| Available expressions | forward | intersection | `op a b` over variables and constants, killed by a write to an operand | redundant-computation findings |
| Definite assignment | forward | intersection | variables certainly written | use before definition (and unresolved names) |
| SCCP | sparse | meet on the value lattice | constants per SSA value, executable blocks and edges | folding, dead branches, unreachable code |
| Value liveness (DCE) | backward on SSA | | values reachable from roots | dead assignments, unused variables |

### 15.5 Effects, purity and what may be removed

Every expression and function has a mask: `THROW` (may raise), `WRITE` (changes memory, prints, raw), `READ` (reads mutable memory), `LOOP` (may not terminate:
`while`, recursion). **Removable** = none of THROW, WRITE, LOOP. The table is conservative on purpose: `any` operands may raise; `int` with `float` may raise (a huge int
does not convert: `OverflowError`); `/` may raise unless both operands are float with a non-zero literal divisor; `//` and `%` unless the divisor is a non-zero int literal;
`int(x)` only for int/bool; `float(x)` only for a literal int or a float; `sqrt floor pow matches` always; indexing always; calls take the callee's summary (builtins have
their own row). A `catch` is assumed to catch everything raised in its body. A function is *pure* (removable) when its summary is, so a call to it can disappear
when its value is unused; recursion and loops never are.

### 15.6 Constant folding: the Python semantics, and what is refused

Folding happens in SCCP (per value) and is written back only when the node is **removable** (a constant that came through a `?:` with a condition that has effects is not).
`//` and `%` are floored (`-7 // 2 = -4`, `7 % -2 = -1`), `/` is true division (both operands must be exactly representable, |n| <= 2^53; the result must be finite),
`int(float)` truncates, `max/min` return the winner **with its own type** (`max(1, 1.0)` is `1`). Not folded: division by zero (the program must still raise), a result that
overflows 64 bits (Python's integers are unbounded; the generated code must compute the real value), `inf`/`NaN` results, int with float beyond 2^53, comparisons of
text that is not ASCII, `upper/lower/trim/indexof/slice/reverse` on non-ASCII text (concatenation, equality, `len` as code points, `contains`, `startswith` are fine), `sqrt`
of a negative, `str(float)`/`str(bool)` (the targets disagree), `bool == int` (Python says `True == 1`), lists, maps and field/element reads. `-0.0` is kept and printed
as `-0.0`; `x == x`, `x - x`, `x + 0.0` are not simplified for floats (NaN, inf, -0.0); `not (a < b)` is not turned into `a >= b` (NaN).

### 15.7 Findings and certainty

Each finding carries a grade, never "exact": **[structural]** is a fact about the spec text (a variable with no read, code after `return`, a `while true` with no `break`
in its condition literal, a recursive call), **[derived]** comes from an analysis (a branch that SCCP proves constant, a dead store, a path to the end of the function, a function
nobody calls). Kinds: `unreachable`, `dead-branch`, `unused-var`, `dead-store`, `use-before-def`, `undefined-name`, `missing-return`, `infinite-loop`; info only: `unused-function`
(possibly an entry point), `recursion`, `redundant-expr`, `unused-param`. The 15 tasks give no warning (the two `raw` tasks give the info "never called"). `ucgecli flow spec`
prints them sorted by line, then the **generation order** (callees before callers, Tarjan order) and the size of the work.

### 15.8 The optimizer (`cgx_opt_run`, `CgxOptOpts.passes`)

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
again at the end (ids stay unique and stable). `ucgecli opt spec [--all]` prints the counts; `ucgecli optgen python spec [--all] [--tests] [--recheck]` optimizes and generates
(`--recheck` runs the checker again on the result).

### 15.9 Tests and results

Run `sh tools/ucge/graphtest.sh [N]` (about 35 s with N=300; `SAN=1` for ASan/UBSan) and `sh tools/ucge/test.sh` (the 60 phase-1 checks, still green).

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
- **IR interpreter** (`ucgecli irexec spec pre|ssa|out`): runs the IR of the scalar functions before SSA, in SSA (phi chosen by the arriving edge) and after out-of-SSA, and must equal
  the Python on 60 096 results (1 000 programs x 3 phases); it is what found that a `try` body must start a new block (an instruction before the `try` shared a block, and so the
  exceptional edge, with the protected call).
- The tasks, optimized with the default passes and with `--all`, still pass their own tests (26 runs).

### 15.10 Limits

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
- `ucgetype.c` (HM) and the registry (module I) are not used here: types are the checker's (`ty`), `any` is treated as "may raise".
- The IR interpreter is a test tool (scalars, strings of small size, no lists/classes), not a backend.
