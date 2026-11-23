# Code intelligence layer (design)

Status: design only (2026-11-23). Nothing in `src/` implements this, no model was run to write it and nothing here
was measured for this document. Numbers quoted from other documents carry their source. Every other number is marked
as a target or as an estimate. Section 11 lists what only a measurement would decide; none of those measurements was
run.

Read with: `docs/investigation-engine.md` (the research: what worked and what did not), `docs/embedding-usage.md`
(the optional embedding filter), `docs/investigate.md` (the 76-question bench), the `Find` section of `docs/tools.md`.

## 0. The decision and the facts under it

The owner's decision: the local model investigates code badly because it receives raw tools (`Read`, `Grep`, `Glob`,
`Find`) and has to decide everything alone. It reads whole files, forms no hypothesis, loops, and concludes from the
words of the request. So the engine gets its own **code intelligence layer**, larger than `Find`, that **conducts the
investigation before the agent sees the request** and hands the model an **evidence packet**. The model works as the
analyst: it reads the evidence, checks it, diagnoses. What to search, how to correlate and how to investigate is
decided by C code.

What exists today, checked in the code:

| Piece | Where | What it is | Limit that matters here |
|---|---|---|---|
| Incremental index | `src/tools/find.c` (`fd_update`, `fd_build_file`, `fd_invert`, `fd_save`, `fd_load`) | per file: 40-line chunks with stride 25 (`FD_CHUNK`, `FD_STRIDE`), three fields per chunk (path, defined symbols, body); file `.katana/cache/find-<hash>.idx` (`FD_VERSION` 4); re-reads only files whose mtime or size changed | whole index in memory; cap of 20000 files (`fd_max_files`) and 300 KB per file (`FD_MAXBYTES`) |
| Symbols | `fd_sym` in `find.c` | one name per line, by per-language line rules; stored as (line, name id) | **no end line**, no signature, no scope, no kind, no visibility. `fd_enclosing` answers "which function is this line in" with "the last symbol that starts before it", which is wrong after a function ends |
| References | the inverted index itself (`fd_refs`, `fd_calls`, `fd_also`) | whole-identifier term -> chunks naming it; one-part names confirmed word by word (`fd_has_word`) | a reference is "the name occurs": no distinction between call, use, declaration, comment or string; no caller symbol, only a chunk |
| File graph | `src/tools/findgraph.c` (`fg_build`) | nodes are **files**; four undirected edge types `sym`, `path`, `rare`, `co` in one CSR | no symbol-level edges, no direction, no call graph |
| Sparse activation | `fg_activate`, `fg_drop_request`, `fg_evidence` | request words dropped, best 40 files, 2 rounds, 20 winners, threshold 0.15; each graph-entered candidate carries a `why:` | ranking, not fact |
| Neighbour note | `fg_neighbor_note`, called at `src/agent/loop.c` just before `loop_plan_note` | `[katana: linked files you have not read yet: ...]`, at most 3 per result, 8 per request | reactive: only after the model already chose what to read |
| Repetition note | `rr_check` in `loop.c` | a repeated identical read is not run again | reactive |
| Read map | `ct_outline` in `src/tools/read.c` | map of at most 60 symbols for a large file | its own third set of line patterns; "a guide, not a parser" (`docs/tools.md`) |
| Rename / Replace / Patch | `src/tools/multiedit.c` (`me_words`) | textual, whole-word, skips comments and strings by a small per-type lexer | not scope-aware: "a field or local with the same name is renamed too" |
| Doc selection | `src/agent/docsel.c` (`ds_select`), called in `loop.c` where the user message is prepared | lines of the project `.md` files that match the request, 600-token budget, a `[katana: the engine picked this ...]` note appended to the user turn | `.md` only; this is the injection pattern the packet will follow |
| Language servers | `src/tools/lsp.c` | optional `LSP` tool that talks to an external server process | third-party programs; optional by nature |
| JSON by construction | `src/agent/callgrammar.c` (`cg_json_byte`, the grammar automaton it shares with `src/engine/forward.c`) | tool calls are valid by construction | reusable for the query expansion of section 5 |

Three places hold three different sets of "what is a symbol" rules (`fd_sym`, `ct_outline`, the comment/string lexer
of `me_words`). The layer replaces them with one.

What the research measured (`docs/investigation-engine.md`), which this design must respect:

| Fact | Value | Section there |
|---|---|---|
| Recall of gold files in the top 5, 46-question sample | 15.9% (BM25) -> 33.0% (activation) | 3.3 |
| Top 5 contains at least one gold file | 23.9% -> 41.3% | 3.3 |
| Top 1 is a gold file | 4.3% (both) | 3.3 |
| Gold files with none of the question's informative terms | 27 of 127 (21%), mostly Portuguese questions over English code | 1, 3.4 |
| Confidence separates right from wrong on investigation questions | no: a threshold calibrated for 90% top-1 precision on commits gives 0% top-1 precision on the questions | 3.6 |
| Static folder embeddings, the large model's input table | no gain | 4 |
| Retrieval embedding model (Qwen3-Embedding-0.6B) | 112 tokens/s on this notebook in the first implementation (owner's brief; the experiment was left incomplete, and this number is not yet written in any document of `docs/`) | - |
| Baseline agent run on the mandatory question | 8 calls, 11914 input tokens, 238 s, failed | `docs/investigate.md` |

Machine: i7-11800H (8 cores, 16 threads), 64 GB of RAM (`docs/knowledge.md`; the owner's brief says 61 GB usable),
RTX 3050 4 GB already full with the large model (`docs/embedding-usage.md` note 3).

**A tension that must be stated.** The research concluded "the engine must not inject an unrequested answer", and
`docs/embedding-usage.md` rule D26 forbids building candidates from the user's text before the model asks. The
owner's new decision is that the engine does investigate first. The two fit together only under the rule of section
1: what the engine found by structure is delivered as fact, what it guessed is delivered as several hypotheses with
evidence and the word "probable", and nothing is ever delivered as "the answer". With a top-1 precision of about 4%
measured for the guessing part, a packet that shows one confident candidate would be wrong most of the time. The
design is built around that number, not around the hope that it improves.

## 1. Vision

### 1.1 The layer

```
user request
    |
    v
+---------------------------- code intelligence layer (C, src/intel/) -----------------------------+
|  interpreter -> orchestrator -> { text search | structural queries | (semantic, optional) }      |
|                      |                     \__ code database (files, symbols, relations) __/     |
|                      v                                                                           |
|               evidence packet                                                                    |
+--------------------------------------------------------------------------------------------------+
    |                                  ^                         ^
    v                                  |                         |
user turn = request + packet     structural tools           per-result notes
    |                            (Def, Callers, ...)        (neighbours, repetition)
    v                                  |                         |
  large model (analyst) ---------------+-------------------------+
```

It enters in three places:

1. **Before the agent** (new): when the user message is prepared, for request types that need code context
   (explain a flow, locate, change, debug). The orchestrator runs and appends the evidence packet to the user turn,
   the way `ds_select` appends its note. Overview requests get the project card. Requests with no code question get
   nothing.
2. **As tools** (new): the exact structural queries are tools the model can call (`Def`, `Callers`, `Callees`,
   `Uses`, `Outline`, `Path`, `Similar`, `Tests`). The model asks for more with them when the packet has a gap.
3. **In the loop** (exists, improved): the neighbour note and the repetition note keep working, now fed by
   symbol-level relations instead of file-level ones.

### 1.2 The principle: EXACT and PROBABLE are different things and never mix

| | EXACT | PROBABLE |
|---|---|---|
| Question it answers | "given this symbol, what is its body, who calls it, what does it call, who imports this module" | "which symbol or file is the user talking about" |
| Produced by | parsing and table lookups | ranking: lexical search, activation, query expansion, optional embeddings |
| Property | deterministic: same tree, same answer; no score, no ranking error | a score with no calibration (research 3.6) |
| Failure mode | known, enumerable classes (macros, dynamic names, same name in two scopes); each fact says how it was resolved | unknown: a wrong candidate looks exactly like a right one |
| Delivered as | fact, with `file:line` and the relation that produced it | hypothesis, with its evidence and the word `probable`; always more than one when more than one exists |
| May be cut for budget | yes (ordered, counted, never silently) | yes (same) |
| May be asserted to the model as true | yes, within its resolution class | never |

"Exact" does not mean compiler-exact. It means **deterministic and structural**. Every exact relation carries a
resolution class so that a name-only match is never passed off as a resolved one:

| Class | Meaning | Shown as |
|---|---|---|
| `unique` | the name has one definition in the project | `exact` |
| `scoped` | several definitions; this one was chosen by a deterministic rule (same file, same class, imported module) | `exact` |
| `ambiguous(n)` | several definitions and no rule chooses; all n are listed | `exact, 1 of n definitions with this name` |
| `external` | no definition in the project (library, generated code) | `exact, defined outside the project` |
| `textual` | the relation comes from a name occurrence the parser could not classify (inside a macro body, a string that looks like a name) | `by name only` |

The mapping from the request to an entry point is always PROBABLE, with one exception: the request **names** an
identifier, a file path or a quoted message that exists in the tree. Finding that name is a lookup, so it is EXACT
(rules R7 to R9 of the research, section 5 there).

## 2. Components and modules, mapped to C

### 2.1 The owner's 13 components

| # | Component | File (proposed) | Reuses | Rewrites / new |
|---|---|---|---|---|
| 1 | Directory scanner | `src/intel/scan.c` | file listing of `fd_update` (`git ls-files --cached --others --exclude-standard`, or the walk with `.gitignore` shared with `Grep`: `ct_grep_collect`), `fd_skip_rel`, the binary test | cap of 20000 files removed; listing in a thread; change detection without a full `stat` sweep (2.6) |
| 2 | Language identification | `src/intel/lang.c` | the extension table of `fd_kind` | one enum per language instead of per family (C and C++ apart, JS and TS apart, Kotlin and Java apart); shebang line for extensionless scripts; `.h` decided C or C++ by content |
| 3 | File indexer | `src/intel/index.c` | `fd_build_file` (chunks, three fields, term table `FdTerms`, `fd_tokens`, `fd_fold`, `fd_stem`) | per-file segments on disk instead of one blob array rewritten whole; parse and tokenize in the thread pool of `src/engine/base.c` |
| 4 | Function and class extractor | `src/intel/parse.c`, `parse_brace.c`, `parse_indent.c`, `parse_shell.c`, `parse_md.c` | the keyword knowledge inside `fd_sym` (modifier and definition lists) and `ct_outline` | new: lexers and range-aware parsers (section 3). `fd_sym` and `ct_outline` are retired once the new parser passes the same tests |
| 5 | Symbol database | `src/intel/db.c` | hash and string pool of `FdTerms` | new tables: symbols with ranges, occurrences, relations (2.4) |
| 6 | Dependency graph | `src/intel/graph.c` | `fg_build`'s `path` edges (path strings, relative imports, Python imports), `co` edges (`git log`), CSR code | new: directed symbol-level edges (calls, uses, inherits, implements, tests) and module-level import edges; the file graph becomes a projection of them plus `rare` and `co` |
| 7 | Text search engine | `src/intel/search.c` | all of `fd_rank` (per-field BM25), `fg_drop_request`, `fg_activate`, `fg_evidence`; the regex engine `src/tools/regex.c` for literal and pattern search | chunk boundaries follow symbol ranges when a function fits (a function is one retrieval unit; long functions keep 40-line windows) |
| 8 | Semantic search engine | `src/intel/semantic.c` (optional, last phase) | the whole design of `docs/embedding-usage.md` (cache, budget, rank fusion, evidence line) | only the unit changes: one vector per symbol body instead of per 40-line chunk |
| 9 | Question interpreter | `src/intel/question.c` | request-word list of `fg_is_request`, stop words `fd_stopw`, the vocabulary-gap test `G` of `docs/embedding-usage.md` section 2 | new: request type, entities, relation terms, optional expansion by the large model (section 5) |
| 10 | Investigation orchestrator | `src/intel/investigate.c` | the policy specification of research 7.4 | new: state machine and per-type plans (section 6) |
| 11 | Context selection for the model | `src/intel/packet.c` | the budget and block logic of `ds_select`; the 1500-character discipline of `fd_show` | new: evidence packet (section 7) |
| 12 | Diagnosis engine | `src/intel/diagnose.c` | - | new, and deliberately small: it does not conclude. It computes guards on a call path, asymmetries between a working and a failing sibling, and the list of gaps (6.4). The conclusion is the model's |
| 13 | Investigation interface | `src/intel/tools.c` (agent tools), `src/intel/cli.c` (`katana intel ...`), and the hook in `src/agent/loop.c` | tool table `src/tools/table.c`, `katana tool` | new tools and commands (sections 4 and 8) |

### 2.2 The owner's 8 modules

| Module | Components | Files | Public prefix |
|---|---|---|---|
| Codebase Scanner | 1, 2 | `scan.c`, `lang.c` | `ixs_` |
| Code Parser | 4 | `parse*.c` | `ixp_` |
| Code Intelligence Database | 3, 5 | `index.c`, `db.c` | `ixd_` |
| Search Engine | 7, 8 | `search.c`, `semantic.c` | `ixt_`, `ixv_` |
| Dependency Engine | 6 and the structural queries | `graph.c`, `query.c` | `ixg_`, `ixq_` |
| Investigation Engine | 9, 10, 11, 12 | `question.c`, `investigate.c`, `packet.c`, `diagnose.c` | `ixn_`, `ixo_`, `ixe_`, `ixx_` |
| Local AI Engine | the large model, unchanged; one new entry: a short constrained side generation (5.3) | `src/engine/`, `src/agent/callgrammar.c` | - |
| Investigation Interface | 13 | `tools.c`, `cli.c`, hook in `loop.c` | `ct_` (tools), `ixc_` |

The prefix `ix_` is unused in `src/` today (checked by search). All files are parts of the single translation unit:
`src/katana.c` includes them after `tools/grep.c` (the listing helper) and before `tools/find.c`, which becomes a thin
tool over `search.c`. `find.c` and `findgraph.c` shrink to the tool's argument handling and output format; their
current output format does not change (the curriculum's `x-find6` family replays it byte for byte).

### 2.3 Interfaces between modules (summarized C signatures)

```c
/* ---- lang.c ---- */
typedef enum { IXL_NONE, IXL_C, IXL_CPP, IXL_JS, IXL_TS, IXL_PY, IXL_KOTLIN, IXL_JAVA, IXL_GO, IXL_RUST,
               IXL_SHELL, IXL_QML, IXL_PS, IXL_MD, IXL_CONFIG, IXL_TEXT } IxLang;
static IxLang ixs_lang(const char *rel, const char *head, size_t nhead);   /* extensao, shebang, conteudo do .h */

/* ---- scan.c ---- */
typedef struct { char *rel; long long mtime, size; IxLang lang; uint8_t flags; } IxEntry;  /* flags: test, doc, cfg, generated */
typedef struct { IxEntry *v; int n; int truncated; } IxListing;
static int  ixs_list(const char *root, IxListing *out, char *err, size_t ecap);      /* git ou caminhada */
static int  ixs_changed(const struct IxDb *db, const IxListing *now, int **changed, int **removed, int *nc, int *nr);

/* ---- parse.c: a interface estavel; tudo depois dela nao sabe qual analisador produziu os registros ---- */
typedef struct {
    uint32_t name, scope;          /* id na tabela de nomes; scope = indice do simbolo pai no arquivo, ou IX_NONE */
    uint32_t sig;                  /* id do texto da assinatura (uma linha, normalizada) */
    uint32_t line0, line1;         /* intervalo de linhas, inclusivo */
    uint32_t byte0, byte1, body0;  /* intervalo de bytes; body0 = primeiro byte do corpo (0 = sem corpo) */
    uint64_t shape;                /* impressao digital da forma do corpo (secao 4, Similar) */
    uint8_t  kind, vis, flags, conf;   /* IXK_*, IXV_*, IXF_*, confianca do intervalo: 2 exato, 1 heuristico, 0 so a linha */
} IxSym;
typedef struct {
    uint32_t name;                 /* identificador citado */
    uint32_t line; uint16_t col;
    uint32_t in_sym;               /* simbolo do arquivo que contem a ocorrencia, ou IX_NONE (nivel do arquivo) */
    uint8_t  kind, flags;          /* IXR_CALL, IXR_USE, IXR_TYPE, IXR_IMPORT, IXR_INHERIT, IXR_IMPLEMENT, IXR_WRITE; flags: em macro, em guarda */
} IxOcc;
typedef struct { uint32_t line, target_sym_hint; uint8_t kind; } IxGuard;   /* saida antecipada dentro de um corpo (6.4) */
typedef struct { IxSym *sym; int nsym; IxOcc *occ; int nocc; IxGuard *gd; int ngd; char *imports; int nimp; int errors; } IxParsed;
typedef struct IxParser {
    const char *name; uint32_t version;       /* a versao entra no registro do arquivo: mudou, o arquivo e reanalisado */
    int (*parse)(const char *text, size_t n, IxLang lang, struct IxNames *names, IxParsed *out);
} IxParser;
static const IxParser *ixp_for(IxLang lang);                 /* analisador proprio; um plugin pode registrar outro */

/* ---- db.c ---- */
typedef struct IxDb IxDb;
static IxDb *ixd_open(const char *root, int flags, char *err, size_t ecap);   /* IXO_RDONLY, IXO_NOCREATE, IXO_BACKGROUND */
static int   ixd_update(IxDb *db, int budget_ms);            /* incremental; devolve arquivos ainda pendentes */
static int   ixd_progress(const IxDb *db, int *done, int *total);
static void  ixd_close(IxDb *db);
static int   ixd_file(const IxDb *db, const char *rel);      /* indice do arquivo ou -1 */
static int   ixd_syms_named(const IxDb *db, const char *name, int icase, int *out, int cap);
static const IxSym *ixd_sym(const IxDb *db, int sym, int *file);
static int   ixd_sym_at(const IxDb *db, int file, int line); /* simbolo mais interno que contem a linha (intervalo real) */

/* ---- search.c ---- */
typedef struct { int file, sym, line0, line1; double score; uint8_t how; int via_file; char why[160]; } IxHit;  /* how: texto, aresta, termo expandido, literal */
static int ixt_search(IxDb *db, const struct IxQuestion *q, const char *path, IxHit *hits, int k);
static int ixt_literal(IxDb *db, const char *text, const char *path, IxHit *hits, int k);     /* mensagem entre aspas */
static double ixt_gap(const IxDb *db, const struct IxQuestion *q);        /* fracao de termos informativos ausentes do indice */

/* ---- query.c: consultas exatas (secao 4) ---- */
typedef struct { int sym, file, line; uint8_t rel, cls; uint16_t depth; int from; } IxFact;   /* cls: unique, scoped, ambiguous, external, textual */
static int ixq_def(IxDb *db, const char *name, const char *path, IxFact *out, int cap);
static int ixq_callers(IxDb *db, int sym, int depth, IxFact *out, int cap);
static int ixq_callees(IxDb *db, int sym, int depth, IxFact *out, int cap);
static int ixq_uses(IxDb *db, int sym, IxFact *out, int cap);
static int ixq_importers(IxDb *db, int file, IxFact *out, int cap);
static int ixq_dependents(IxDb *db, const char *module, int depth, IxFact *out, int cap);
static int ixq_similar(IxDb *db, int sym, int by, IxFact *out, int cap);          /* by: IXS_NAME, IXS_SHAPE */
static int ixq_tests(IxDb *db, int sym, IxFact *out, int cap);
static int ixq_outline(IxDb *db, int file, IxFact *out, int cap);
static int ixq_path(IxDb *db, int from, int to, int maxdepth, IxFact *out, int cap);
static int ixq_guards(IxDb *db, int sym, IxGuard *out, int cap);
static int ixq_body(IxDb *db, int sym, int maxlines, struct CtSb *sb);            /* texto numerado do corpo */

/* ---- question.c, investigate.c, packet.c ---- */
static int  ixn_interpret(IxDb *db, const char *request, struct IxQuestion *q);              /* regras; microssegundos */
static int  ixn_expand(struct Model *m, IxDb *db, struct IxQuestion *q, int budget_ms);      /* opcional: uma geracao curta */
static int  ixo_investigate(IxDb *db, const struct IxQuestion *q, const struct IxBudget *b, struct IxCase *out);
static int  ixe_packet(const IxDb *db, const struct IxCase *c, int max_tokens, char *out, size_t cap);   /* devolve trechos emitidos */
static int  ix_prepare(struct Model *m, const char *request, char *out, size_t cap);         /* o que o laco chama */
```

Rules of the interfaces:

- Nothing above `parse.c` knows which parser produced a record. That is what lets an optional plugin replace a
  parser for one language without touching the database or the queries (section 3.4).
- Every `IxFact` and every `IxHit` carries its provenance (file, line, relation or reason). No function returns a
  bare name.
- `ixq_*` functions never rank. They return everything up to `cap`, in a fixed order (definition file first, then
  path order, then line), and the true total, so the caller can print "(+N more)".
- `ixt_*` functions always rank and never return a fact.

### 2.4 Database on disk

Location: `<working folder>/.katana/cache/intel-<hash>/`, next to `find-<hash>.idx`, with the same fallback
(`~/.katana/cache`, `KATANA_CACHE_DIR`) and the same `.gitignore` containing `*`. A folder, not one file, so that an
update rewrites only what changed:

```
intel-<hash>/
  head.kix        header: magic "KIXH", format version, root path, parser versions, counts, state (complete | partial)
  names.kix       string pool + hash: identifiers, signatures, paths (append-only between compactions)
  files.kix       fixed records, one per file
  seg-NNNN.kix    segments: per-file payloads (symbols, occurrences, guards, imports, chunks with their terms)
  rel.kix         derived: resolved relations, CSR by source and by target (rebuilt from segments; can be deleted)
  post.kix        derived: inverted index for text search (rebuilt from segments; can be deleted)
  lock            advisory lock: one writer, many readers
```

Records (little endian, fixed size, read by `mmap`):

```
file record (48 bytes)
  path u32 (names) | lang u8 | flags u8 | parser_version u16 | mtime i64 | size u32 | content_hash u64
  segment u16 | offset u32 | length u32 | nsym u32 | nocc u32 | errors u16
  flags: test, doc, config, generated, vendored, too large (indexed for text only), parse failed (text only)

symbol record (48 bytes)                              -- IxSym above
  name u32 | scope u32 | sig u32 | line0 u32 | line1 u32 | byte0 u32 | byte1 u32 | body0 u32 | shape u64
  kind u8 | vis u8 | flags u8 | conf u8
  kind: function, method, constructor, class, struct, union, enum, enum member, interface/trait, namespace/module,
        typedef/alias, macro, global variable, constant, field, property, signal, heading, section
  vis:  public, exported, file-local (static, unexported, underscore-prefixed by convention), private, protected, unknown
  flags: declaration only (prototype), definition, async, test, entry point (main, exported handler, CLI command), generated

occurrence record (12 bytes)                          -- IxOcc above, packed
  name u32 | line u24 + kind u4 + flags u4 | in_sym u32 (file-local symbol index; column kept only for calls, in a side array)

relation record (16 bytes, derived)
  from_sym u32 | to_sym u32 | line u24 + type u8 | class u4 + flags u4 + pad
  type: calls, uses, defines (scope -> member), imports (file -> file or file -> external module),
        inherits, implements, tests, overrides, instantiates
  class: unique, scoped, ambiguous, external, textual (section 1.2)
```

Why two levels (occurrences in segments, relations derived): an occurrence depends only on its own file, so it
survives edits to other files. A relation depends on name resolution across files, so it is rebuilt from the
occurrences. Resolution is a hash join by name with the scoping rules of section 3.3, which is cheap, so `rel.kix` is
rebuilt incrementally for the changed names and whole only when the format changes. This is the same split `find.c`
already uses between the per-file blobs and `fd_invert`.

### 2.5 Versioning

- `head.kix` holds a format version. Another version: the folder is ignored and rebuilt, without an error (the rule
  `fd_load` already follows for `FD_VERSION`).
- Each file record holds the version of the parser that produced it. A parser fix bumps that parser's version, and
  only the files of that language are re-parsed, in the background.
- The derived files hold the count and a hash of the file table they were built from. A mismatch: rebuild them.
- A torn write is detected by the state field and the lock; segments are written to a temporary name and renamed.
- The database never stores source text. Bodies are read from the working tree at query time, after checking that
  the file's mtime and size still match the record; a mismatch triggers a re-parse of that one file before the
  answer. So an answer never quotes a stale line number.

### 2.6 Incremental update

```
ixd_update(db, budget_ms):
  1. listing: git ls-files (or the walk); compare (path, mtime, size) with files.kix        -> changed, new, removed
  2. for changed/new, in the thread pool: read, hash; if content_hash is unchanged, only refresh mtime
  3. parse + tokenize; write the payload to the current segment; mark the old payload dead
  4. names whose definitions or occurrences changed -> re-resolve the relations that mention them
  5. postings: remove and re-add the changed files' chunks
  6. past budget_ms: stop, state = partial, return the number of files left (queries say so; section 8.6)
  compaction: a segment with more than half of its bytes dead is rewritten, in the background
```

Edits made by the agent itself (`Edit`, `Write`, `Rename`, `Replace`, `Patch`) call `ixd_touch(path)` so the next
query re-parses that file without waiting for a listing. Today a file changed by the agent "is not in the graph until
the next index update and rebuild" (`docs/tools.md`); this closes that.

Step 1 is the cost floor of every call: `docs/tools.md` measured 102 ms for "file listing and stat" on the 5349-file
repository. Linear extrapolation gives about 2 s for 100 thousand files (an estimate, not measured), which is too slow
to pay on every tool call. Three measures, in order of effort: (a) inside one agent process, list at most once per
user request and once after any `Bash` call; (b) in a git repository, ask git only for what differs from the last
listing (`git status --porcelain` uses git's own stat cache; its cost on a large tree is unmeasured here); (c) a file
watcher thread (inotify on Linux), later. (a) is enough to start.

### 2.7 Size, memory and time for 100 thousand files

Measured today (`docs/tools.md`, `Find` index): 5349 files -> index file 18.5 MB, cold build 1.15 s, peak process
memory 55 MB; katana tree (991 files) -> 5.3 MB, 0.29 s.

Everything below is an **estimate** by linear extrapolation plus stated assumptions; none of it was measured.

| Item | Assumption | Estimate at 100k files |
|---|---|---|
| Text postings and chunks (today's content) | linear from 18.5 MB / 5349 files | about 350 MB |
| File records | 48 bytes each | 4.8 MB |
| Symbols | 15 per file on average (assumption) x 48 bytes | 72 MB |
| Occurrences | 150 classified identifier occurrences per file (assumption; locals excluded) x 12 bytes | 180 MB |
| Relations | about half of the occurrences resolve to a project symbol (assumption) x 16 bytes x 2 directions | 240 MB |
| Names | 2 million distinct strings x 24 bytes average (assumption) | 50 MB |
| **Total on disk** | | **about 0.9 GB** |

Ceilings (targets, to be enforced by the code, not predictions):

- **Disk:** 2 GB per project (`KATANA_INTEL_DISK_MB`, default 2048). Past it, the least recently queried
  directories fall back to text-only records.
- **Private memory:** 512 MB (`KATANA_INTEL_MEM_MB`). Everything large is `mmap`ed read-only, so it is page cache
  the system can reclaim, and the large model's memory is not competed for. This matters on the minimum machine
  (24 GB, `docs/handoff.md`) more than on this one.
- **Cold structural index of 100k files:** target under 60 s using all cores while the model is idle. The measured
  1.15 s for 5349 files is single-threaded lexical indexing; linear extrapolation is about 22 s single-threaded for
  the lexical part. Parsing adds work per byte and the thread pool removes most of it; the net is unknown until
  measured.
- **Warm start** (database present, nothing changed): target under 300 ms plus the listing cost of 2.6.
- **Update after editing one file:** target under 50 ms.
- **One structural query:** target under 5 ms. The current `Find` query is 1 to 7 ms (`docs/tools.md`).

## 3. Parsers

### 3.1 Languages by priority

Priority follows the owner's projects (katana in C; the desktop app in C++/QML; the bridge and tools in JS/MJS and
Python; the phone app in Kotlin; scripts in shell and PowerShell):

| Tier | Languages | Family | Parser file |
|---|---|---|---|
| 1 | C, C++ | braces, with the C-specific header heuristic | `parse_brace.c` |
| 1 | JavaScript, TypeScript, MJS, TSX | braces | `parse_brace.c` |
| 1 | Python | indentation | `parse_indent.c` |
| 2 | Kotlin, Java | braces | `parse_brace.c` |
| 2 | Go, Rust | braces | `parse_brace.c` |
| 3 | shell | shell | `parse_shell.c` |
| 3 | QML | braces (object tree, with embedded JS) | `parse_brace.c` |
| 3 | PowerShell | braces, `function Verb-Noun {` | `parse_brace.c` |
| 3 | Markdown | headings and fences | `parse_md.c` |
| - | config and text (toml, yaml, xml, cmake, gradle, txt) | none: text search only | - |

### 3.2 Strategy per family

All parsers share one **lexer layer** and differ in the **structure layer**. No parser builds a full syntax tree.

**Lexer layer (per language, table-driven).** One pass that classifies every byte as code, comment, string or
preprocessor, and emits tokens: identifier, keyword, punctuation, literal. Per-language tables: line comment,
block comment, string delimiters (including Python triple quotes and prefixes, JS template literals with `${ }`
nesting, Rust raw strings `r#"..."#`, Go raw strings, Kotlin triple quotes with `${ }`, C++ raw strings
`R"x(...)x"`, shell quoting and here-documents, PowerShell here-strings), character literals against Rust
lifetimes, JS regex literals against division (decided by the previous token). This is where `me_words` already has
a small version (comments and strings per file type); the lexer replaces it and `Rename` gains precision from it.

**Brace family (structure by brace depth).**

```
state: depth, a stack of open scopes {symbol index, depth at which its body opened}, a "header" token buffer
for each token:
  code at depth d outside any function body, or inside a class/namespace/impl/object body:
      accumulate the header (tokens since the last ';', '}' or '{' at this level)
      '{' -> classify the header:
               class-like keyword (class, struct, union, enum, interface, trait, impl, namespace, object, extern "C") -> open a type scope
               function header (rules below)                                                              -> open a function scope
               otherwise (initializer, lambda, block)                                                      -> open an anonymous scope
      ';' -> classify the header: prototype, typedef, global, field, import, using
  inside a function body:
      identifier followed by '(' and not a keyword        -> occurrence CALL (name, line, col)
      identifier after 'new'                              -> occurrence TYPE (instantiates)
      other identifier                                    -> occurrence USE (if it is not a local declared in this body)
      'if' ... then within the next statement a return/continue/break/throw/goto/exit -> guard (6.4)
      nested function, lambda, class                      -> nested scope (JS, Kotlin, Rust closures, C++ lambdas)
  '}' closing a scope -> line1, byte1 of that symbol
```

Function header rules:

| Language | A header is a function when | Name | Signature |
|---|---|---|---|
| C | at depth 0; tokens are `words... name ( ... )` then `{`; no `=` before the `(`; `name` is not a control keyword. K&R parameter declarations between `)` and `{` are accepted | last identifier before the first top-level `(` (skipping `(*name)(...)` function pointers and attribute macros written as `NAME(...)` before the type) | header tokens joined with single spaces, cut at 200 bytes |
| C++ | same, at namespace or class depth; `A::B::name(` gives scope `A::B`; `operator` forms; constructor (name equals class); trailing `const`, `noexcept`, `override`, `-> type`, initializer list `: a(b)` before `{`; `template<...>` prefix skipped by angle-bracket matching | as C, with the qualifier kept as scope | as C |
| JS / TS | `function name(`, `async function`, `function*`; class method `name(...) {`, `get`/`set`/`static`/`async` prefixes; `const name = (...) =>`, `name = function`, `name: function`, `name: (...) =>`, object-literal method shorthand; `export default function`; TS `name(...): type {`, overload declarations ending in `;` (declaration only) | the bound name; an anonymous callback gets the name `<callee>.callback@line` and is not listed in outlines | header up to `{` or `=>` |
| Kotlin / Java | `fun [<T>] [Receiver.]name(`; Java: `modifiers type name(...) [throws ...] {` inside a class body; constructors, `init` blocks, companion objects, property getters and setters | as declared; the receiver type becomes the scope for extension functions | header up to `{` or `=` (Kotlin expression bodies end at the end of the expression, found by bracket balance and line continuation) |
| Go | `func name(`, `func (r *T) name(` | receiver type is the scope | header up to `{` |
| Rust | `fn name` inside `impl T`, `impl Trait for T`, `trait`, `mod`; `macro_rules! name` | `impl` target is the scope; `impl Trait for T` also yields `implements` | header up to `{` or `;`; `where` clauses included |
| PowerShell | `function Verb-Noun {`, `filter` | as declared | header |
| QML | the object tree: `TypeName { ... }` is a scope of kind component; `id: name`, `property type name`, `signal name(...)`, `function name(...) { }`, `onSomething: { }` handlers | `id` names the enclosing object | header |

**C and C++ specifics (the hard case).**
- *Preprocessor.* Directives are lexed as their own class. `#define NAME` and `#define NAME(args)` define macro
  symbols, with the range covering the continuation lines. `#include "x"` is an import; `#include <x>` an external
  import.
- *Conditional compilation.* Both branches of `#if / #else` are parsed. Brace depth is saved at `#if` and restored
  at `#else` and `#elif`, so a `{` that appears in both branches is counted once. This is the standard heuristic
  and it is right when both branches are balanced the same way, which is the common style. katana itself uses
  `#ifndef _WIN32 ... #else ... #endif` around whole groups of functions (`find.c`, `findgraph.c`): both groups are
  indexed, each definition flagged with its condition text, and a name defined in both is class `scoped` with the
  condition shown.
- *Function-like macros that open or close braces, or that generate functions* (`DEFINE_HANDLER(name) {`): the
  header rule sees `NAME ( args ) {` at depth 0 with a single word before `(`. That shape is recorded as a function
  whose name is the first macro argument when there is one, with `conf = 1` (heuristic) and the macro name in the
  signature. `TOOLDEF("find_file", ...)` style tables are not functions; they are occurrences.
- *Depth recovery.* If depth goes negative or a function body would exceed 5000 lines, the parser resynchronizes
  at the next line that starts at column 0 with a header shape, marks the damaged symbols `conf = 0` (line only) and
  counts one error in the file record. A file with errors still serves text search and the symbols before the damage.
- *The single translation unit.* katana is one unit made of `#include "engine/x.c"` lines. Each part is parsed as
  its own file; `static` functions are **not** resolved as file-local when the file is included by another `.c`
  file (the include edge says so). Without this rule, almost every call in katana would be `external`.

**Indentation family (Python).**

```
logical lines: join lines inside open brackets and after a trailing backslash; skip strings and comments (lexer)
a line starting with def / async def / class at indentation i opens a symbol:
    line0 = the first decorator line directly above it (decorators are part of the range and are recorded:
            @staticmethod, @classmethod, @property set flags; @app.route("/x") and similar are kept as text in the
            signature, and the string argument is indexed as a literal so a URL or command name leads to the handler)
    body = following logical lines with indentation > i (blank and comment lines do not end the body)
    line1 = last body line
    scope = innermost open symbol with smaller indentation
class bases: class A(B, C): -> inherits B, C
module level: NAME = value -> global or constant; __all__ = [...] -> exported flags
imports: import a.b as c / from a.b import c, d as e / relative from . import x -> import records with the alias map
calls: identifier or dotted name followed by '(' inside a body; self.name( resolves inside the class first
guards: if ...: followed by return / continue / break / raise on the same or next logical line
```

Tabs and spaces are compared by expanding tabs to 8, as the language does; a file that mixes them inconsistently
is parsed line-only and flagged.

**Shell.** `name() {`, `function name {`; the body ends at the matching `}` found with shell quoting and
here-documents skipped. Calls: the first word of a simple command when it is a function defined in the project or a
script path that exists in the tree (`./x.sh`, `bash tools/y.sh`, `source z`). Scripts invoked by other files
(shell, Makefile, CI config, `package.json` scripts) produce `calls` relations from file to file. Guards:
`[ ... ] || return`, `|| exit`, `if ...; then return`.

**Markdown.** Headings are symbols of kind heading; the range runs to the next heading of the same or higher
level; fenced code is skipped for headings and indexed for text. Path-like strings and backticked identifiers are
occurrences of kind USE with the flag "in documentation", which never create call relations (the research found that
prose creating symbol edges connected every document to the code: `docs/investigation-engine.md` section 2).

### 3.3 What is extracted, and name resolution

Per symbol: name, kind, scope chain, signature, line and byte range, body start, visibility, flags, shape
fingerprint. Per body: calls, uses, type uses, writes to globals (assignment target is a known global), guards. Per
file: imports with aliases, exports, the include or import list.

Resolution of an occurrence `name` at a site in file F, inside symbol S (deterministic, in this order):

1. a local or parameter of S or of an enclosing function: not a project relation; dropped;
2. a member of the enclosing class or of the receiver named `self` / `this`: `scoped`;
3. a symbol defined in F: `scoped`;
4. a symbol imported into F by name or through an alias (`import { a as b }`, `from m import a`, `m.a(` with
   `import m`, C `#include` of the file that declares or defines it): `scoped`;
5. exactly one definition in the project with that name and a compatible kind: `unique`;
6. several definitions: `ambiguous(n)`, all kept;
7. none: `external`.

No type inference is attempted. `obj.method(` where `obj` is not `self` / `this` and not an imported module
resolves by method name alone (rules 5 to 7), so common method names (`get`, `run`, `init`) are `ambiguous` with a
large n. A name defined more than 3 times or used in more than max(30, 2% of files) files is treated as a hub and is
left out of call trees by default (the rule `fg_build` already applies to `sym` edges); it is still answered when
asked for directly.

`inherits` and `implements`: from class headers (`class A : B`, `class A extends B implements C`, `class A(B)`,
`impl T for U`, Kotlin `: Base(), Iface`, Go has none declared). `overrides`: same method name in a class and in a
resolved base. `tests`: section 4.

### 3.4 Expected precision and how each parser fails

No precision number exists yet. The table states expectations and the known failure classes; the first deliverable
of the build order (section 9, phase 1) is the measurement that replaces the expectations.

| Language | Definitions with correct range | Call edges | Known failures |
|---|---|---|---|
| C | high for conventional code | good by name; function pointers and dispatch tables are missed (a table `{"name", fn}` gives a USE of `fn`, not a call) | macros that hide braces or define functions by token pasting (`name##_impl`); unbalanced `#if` branches; K&R oddities; a call through a macro wrapper is attributed to the macro |
| C++ | medium | medium: overloads are one name; templates, ADL and virtual dispatch are not resolved | templates with `<` `>` confused with comparisons; operator overloads; lambdas as arguments; macros as in C; headers with inline bodies counted as definitions (correct, but duplicates names) |
| JS / TS | high for declared functions and classes; medium for functions assigned to properties | medium: `obj.method()` by name only; callbacks passed by reference are USE, not CALL | names built at run time (`obj[name]()`, event names as strings); re-exports and barrel files (`export * from`) need the import chain; JSX in `.tsx` lexed as tags; regex-versus-division mistakes damage one statement |
| Python | high | medium: methods by name only; decorators that replace functions; `getattr` | names built at run time; conditional definitions; star imports (resolved only when the target defines `__all__` or by listing its top level) |
| Kotlin / Java | high | medium: overloads, extension functions, lambdas with receivers | annotation processing and generated code; Kotlin DSL blocks parsed as anonymous scopes; expression bodies with complex continuation |
| Go | high | good by name; interface dispatch not resolved | methods with the same name on many types are ambiguous |
| Rust | high | medium: trait dispatch, macros (`macro_rules!` bodies and procedural macros are opaque) | generics with `<` `>`; lifetimes against character literals (handled in the lexer); `impl` blocks spread across files |
| Shell | medium | medium | `eval`, command names in variables, here-documents containing code |
| QML | medium | low to medium (signal and handler connections are by naming convention: `onFooChanged` <-> `foo`) | bindings as expressions, inline JS |
| PowerShell | medium | medium | dynamic invocation `& $cmd`, aliases |
| Markdown | high | not applicable | headings inside unclosed fences |

Failures common to all: **names in strings** (a command registered as `"find_file"`, a route, an event name, a key
in a dispatch table). These are not calls for any parser, including a full one. They are what makes a flow
"disappear" for a reader, and they are frequent in the owner's projects (tool tables, channel messages, session
names). The layer handles them outside the parsers: string literals that look like identifiers or command names are
indexed as occurrences of kind LITERAL, and an identifier-like literal shared by a few files is exactly the `rare`
edge that the research measured as useful (`P2 expansion without rare`: 34.2% -> 32.9%, section 3.5). Such a link is
shown as `by name only`, never as a call.

How a failure shows: a symbol with `conf < 2` prints `(range approximate)`; a file with parse errors prints
`(this file was only partly parsed)` in any answer that uses it; a query over an `ambiguous` name lists all
definitions. A failure is never silent and never turns into a wrong `exact`.

### 3.5 Own lightweight parsers against an embedded third-party parsing library

| | Own lightweight parsers | Embedded third-party incremental parsing library with generated grammars (permissive licence) |
|---|---|---|
| Precision of ranges and of syntax classification | heuristic; fails on the classes of 3.4 | a real syntax tree; right on templates, JSX, decorators, nested lambdas; still wrong on macros before preprocessing |
| Call resolution (which definition a call reaches) | by name and scope rules | **the same**: a syntax tree does not resolve names either; that needs a type checker or a language server |
| Size in the binary | estimated 3000 to 5000 lines of C in total, tens of KB compiled (estimate) | each generated grammar is a C file of several MB of tables (owner's brief; not measured here), times 10 or more languages; the runtime is small by comparison |
| Single translation unit rule | fits: more parts under `src/intel/` | generated grammars are separate C files with their own static tables and symbol names; including ten of them in one unit is possible but slows every build and risks name clashes |
| "Nothing third-party in the engine" rule | fits | breaks it. The rule says third-party programs enter only as optional plugins |
| Maintenance | every language quirk is ours to find and fix | grammars are maintained by others; upgrading means regenerating and re-testing |
| Robustness on broken code (mid-edit files) | resynchronization is ours | error recovery is a strength of such libraries |
| Speed | one pass per file, no tree allocation | fast as well; not the deciding factor |
| Build | nothing new | a code generator at build time, or vendored generated files |

**Recommendation.** Own lightweight parsers, behind the stable `IxParser` interface of 2.3, with two optional
sources of precision that stay outside the binary:

1. **Language servers, through the existing `LSP` tool** (`src/tools/lsp.c`). When a server for the language is
   installed and `KATANA_INTEL_LSP=1`, the structural queries `Def`, `Callers`, `Callees` and `Uses` ask the server
   first (operations `goToDefinition`, `findReferences`, `incomingCalls`, `outgoingCalls` already exist there) and
   fall back to the database on timeout or absence. This is the one path that gives **resolved** calls, which no
   parser gives. It costs a server start (seconds to minutes on a large project, unmeasured) and is never required.
2. **A parser plugin**: an external program that reads a file and prints the `IxParsed` records in a line format;
   registered per language in the user configuration; versioned like a parser. This is where a third-party parsing
   library can live, if a language turns out to need it. Not planned for the first phases.

Why this choice:

- The largest precision gap that matters for investigation is call **resolution**, and the embedded library does
  not close it. What it would improve (ranges in hard syntax) is the part the heuristics already do well on
  conventional code.
- The measured weakness is elsewhere: finding the entry point (top-1 about 4%) and the language gap (21% of gold
  files). A more precise syntax tree does nothing for either.
- The interface makes the decision reversible per language.

**What is uncertain.** (a) Whether the C header heuristic holds on the real trees. katana's own style (long lines,
`#ifndef _WIN32` groups, macro tables, one unit of included `.c` files) is a hard test and must be the first one.
(b) Whether C++ in the Qt application is tractable without a real parser; if any language ends up needing the
plugin, it is C++. (c) Whether name-only method resolution in Kotlin and JS produces call trees too ambiguous to be
useful; the hub rule may hide too much or too little. All three are answered by the phase 1 measurement, not by
argument.

## 4. Exact structural queries

Each query is a C function (2.3), a CLI command (`katana intel <query> ...`) and, for most, an agent tool. Output
is plain text in the style of `Find`: `file:line`, one fact per line, a closing line that says how to continue,
totals when something was cut. Every query accepts `path` to restrict to a subfolder. Every answer begins with the
resolution of the name asked for, so an ambiguous name is visible before anything else.

| Query (tool) | Signature | Returns |
|---|---|---|
| Definition with full body (`Def`) | `Def(symbol, path?, body?: bool = true, max_lines?: int = 120)` | all definitions of the name: `file:line0-line1`, kind, scope, visibility, signature; then the numbered body of each (cut at `max_lines` with `(+N lines; Read offset=.. limit=..)`); declarations (prototypes) listed after definitions |
| Callers (`Callers`) | `Callers(symbol, depth?: int = 1, path?)` | call sites grouped by calling function: `caller (file:line0-line1)  calls at line N: <source line>`; with `depth > 1` an indented tree upward; each edge with its class |
| Callees, tree to N levels (`Callees`) | `Callees(symbol, depth?: int = 2, path?)` | indented tree in **call order** (by line inside each body), each node `name (file:line) [class]`; hubs and externals collapsed into one line per level (`+ 7 external: printf, malloc, ...`); cycles marked `(recursive)` |
| Uses (`Uses`) | `Uses(symbol, kinds?: call,use,type,write,literal,doc)` | every occurrence that is not a call, grouped by file, with the enclosing symbol; `literal` and `doc` only when asked |
| Importers (`Importers`) | `Importers(file_or_module)` | files that import or include it: `file:line  import text` |
| Dependents of a module (`Dependents`) | `Dependents(path, depth?: int = 1)` | for a file or a folder: modules (folders or files) that depend on it, with the count of import and call relations and the three strongest examples; `depth` follows the relation transitively. The reverse (`Depends`) lists what the module needs |
| Similar implementations (`Similar`) | `Similar(symbol, by?: name,shape,both = both)` | by name: symbols whose split name shares its head or tail tokens with the target (`gclaude_start` ~ `gkatana_start`), or that differ by one token; by shape: symbols with a close shape fingerprint; each line says which: `same shape (calls 5 of the same 6 functions)`, `sibling name` |
| Tests of a symbol (`Tests`) | `Tests(symbol)` | test functions that call it (class `exact`), test files that mention it (`by name only`), and the test file paired by naming convention (`test_x.py`, `x_test.go`, `x.test.ts`, `XTest.kt`, `tests/x.sh`) marked `by convention` |
| Symbols of a file (`Outline`; also the map of `Read`) | `Outline(file_or_folder, depth?: int = 1)` | for a file: every symbol in file order with range, kind, visibility and signature, nested by scope. For a folder: the module map, one line per file with its exported symbols and its import counts in and out |
| Call path (`Path`) | `Path(from, to, max_depth?: int = 6)` | the shortest call chains from `from` to `to` (up to 3), each step `a (file:line) -> b at line N`; if none: says so, and gives the closest pair (the callee of `from` and the caller of `to` that share a file or a module), which is where a dynamic link may be |
| Guards on a body (`Guards`; used by the orchestrator, exposed for the model too) | `Guards(symbol)` | every early exit of the body: `line N: if (!s->channel) return;` with the kind (return, continue, break, throw, exit) |

Output example (the format is the contract; the content is illustrative):

```
Callers of listSessions: 1 definition (bridge/sessions.mjs:132-158, exported function)
bridge/server.mjs
  onMessage (301-365)            calls at 331:   const list = listSessions(state, { terminals: true });   [exact]
  handleStatus (402-420)         calls at 409:   res.json(listSessions(state));                           [exact]
bridge/grey-channel.mjs
  (file level)                   uses at 12:     import { listSessions } from './sessions.mjs';           [import]
2 callers in 1 file, 1 importer. Follow up with Callers on a caller, Def for a body, or Path between two names.
```

Shape fingerprint (for `Similar by=shape`): computed by the parser per function body as a 64-bit similarity hash
over (a) the set of callee names, (b) the sequence of statement kinds (if, loop, return, call, assign) in 4-grams,
(c) parameter count. Two bodies are "close" when the hashes differ in few bits. It is deterministic, so the query
is exact in the sense of section 1.2 (same tree, same answer), but "similar" is a heuristic notion: the output
always states the concrete overlap ("calls 5 of the same 6 functions") and never a percentage. The thresholds are
unmeasured.

Which of these become tools in the system prompt is a budget decision (each tool schema costs prompt bytes; the
`Find` declaration alone grew the prompt by about 850 bytes, `docs/tools.md`). Proposal: `Def`, `Callers`, `Callees`
and `Outline` in the core list; the others under "More tools", loaded with `ToolSearch`, like `Rename`. `Find
mode=refs` stays and is answered by `Uses` plus `Callers` in its current format.

Existing tools that improve without changing their contract:
- `Read` map: `ct_outline` replaced by `ixq_outline`, same output format, real ranges (the map can say
  `fd_rank 839-932`, which gives the model the exact `offset` and `limit`).
- `Find` `calls:` and `also referenced in:` lines: real call relations instead of "names used in the chunk".
- `fd_enclosing`: replaced by `ixd_sym_at`, which knows where a function ends.
- `Rename`: default `kinds=code` can skip occurrences that resolve to a different definition of the same name
  (class `scoped`), and warn on `ambiguous` ones instead of renaming them blindly.
- Neighbour note: neighbours chosen by call and import relations of the symbols actually read (the `Read` range),
  not by file-level weights.

## 5. Question interpreter

### 5.1 The structured request

```c
typedef enum { IXT_NONE, IXT_OVERVIEW, IXT_EXPLAIN, IXT_LOCATE, IXT_CHANGE, IXT_DEBUG } IxType;
typedef struct {
    char text[64]; uint8_t kind;      /* IXE_SYMBOL, IXE_PATH, IXE_LITERAL (texto entre aspas), IXE_COMMAND, IXE_CONCEPT */
    uint8_t origin;                   /* IXO_REQUEST (esta no pedido), IXO_EXPANDED (proposto pelo modelo, 5.3) */
    int df;                           /* arquivos do indice que o contem (0 = ausente) */
    int sym[4], nsym, file[4], nfile; /* resolucao exata, quando existe */
    float weight;                     /* idf */
    char from[64];                    /* para IXO_EXPANDED: o conceito do pedido que gerou este termo */
} IxTerm;
typedef struct IxQuestion {
    IxType type; uint8_t type_sure;   /* 1 = regra forte casou; 0 = padrao */
    IxTerm term[32]; int nterm;
    uint8_t rel;                      /* bits: IXQ_WHO_CALLS, IXQ_WHERE_DEFINED, IXQ_WHAT_USES, IXQ_FLOW, IXQ_NEGATED (nao acontece), IXQ_COMPARE (um funciona, outro nao) */
    int works, fails;                 /* indices em term[] do par "funciona / nao funciona", ou -1 */
    char dropped[200];                /* termos genericos descartados, para o log */
    double gap;                       /* fracao dos termos informativos com df = 0 */
    int lang_pt;                      /* o pedido tem palavras funcionais do portugues */
} IxQuestion;
```

### 5.2 Rules (no model, microseconds)

Applied in order by `ixn_interpret`:

1. **Tokenize** with `fd_fold` (accents folded) keeping original spellings; keep quoted strings whole (straight,
   curly and back quotes).
2. **Named entities, by lookup** (EXACT, never guessed):
   - a token with identifier shape (underscore, camelCase, `::`, `.`, or call parentheses) or any token that is an
     exact entry of the name table -> `IXE_SYMBOL` with its definitions (rule R7 of the research);
   - a token with `/` or a known extension, matched by path suffix -> `IXE_PATH` (R8);
   - a quoted string of 2 or more words, or an error-message shape (`error:`, `Exception`, a line with `:` and
     digits) -> `IXE_LITERAL`, searched literally (R9);
   - a single bare lowercase word that exists **only** as a string literal or a file name in the tree (a command
     name such as `gkatana`) -> `IXE_COMMAND`.
3. **Generic words dropped**: the request-word list of `fg_is_request` (find, explain, why, where, project, code,
   file, descubra, onde, projeto, arquivo, ...), the stop words `fd_stopw`, and any term in more than 15% of the
   files (the research's definition of an uninformative term). If everything would be dropped, nothing is. The
   dropped terms are kept in `dropped` for the log.
4. **Relation terms** set bits in `rel`: who calls / quem chama / chamado por; where defined / onde fica / onde esta
   definido; uses / usa / depende; how does ... work / como funciona / fluxo / passo a passo -> `IXQ_FLOW`;
   negation near a verb (does not, never, stopped, nao aparece, nao funciona, nao chega, some, falha) ->
   `IXQ_NEGATED`; two entities where one is said to work and the other not (funciona ... mas ..., works ... but ...)
   -> `IXQ_COMPARE` with `works` and `fails`.
5. **Request type**, by cue words in Portuguese and English, first match wins:

   | Type | Cues | Default when unsure |
   |---|---|---|
   | `IXT_OVERVIEW` | what does this project do, o que e este projeto, visao geral, how is it organised, estrutura, arquitetura; and no entity | - |
   | `IXT_DEBUG` | why ... not, por que ... nao, bug, erro, falha, trava, crash, does not appear, stopped working; or `IXQ_NEGATED`; or an `IXE_LITERAL` that looks like an error | - |
   | `IXT_CHANGE` | imperative verbs of change: change, add, remove, rename, fix, implement, refactor, altere, mude, adicione, remova, corrija, implemente, troque, crie | - |
   | `IXT_EXPLAIN` | how does, how is, explain, como funciona, como e que, explique, o que acontece quando; or `IXQ_FLOW` | - |
   | `IXT_LOCATE` | where, which file, onde, qual arquivo, em que lugar, find the | - |
   | `IXT_NONE` | a command to run, a greeting, a follow-up with no informative term ("ok", "continue", "sim, pode"), a web request, a request about a file the user attached | **this is the default** |

   When two types match (a "fix" that is phrased as "why"), DEBUG wins over CHANGE and CHANGE over EXPLAIN: the
   richer plan includes the poorer one. When no cue matches but there is at least one entity or two informative
   terms with `df > 0`, the type is `IXT_LOCATE` with `type_sure = 0`, the cheapest plan. A wrong type costs budget,
   not correctness: every plan delivers evidence, none delivers a conclusion.
6. **Vocabulary gap**: `gap` = fraction of informative non-entity terms with `df = 0`
   (`docs/embedding-usage.md` section 2). It needs no language detector and also catches a concept the code names
   differently.

The cue lists are compiled into the binary, in Portuguese and English, like the request words today. They contain
no word of any project.

### 5.3 Query expansion by the large model itself

The largest measured failure is vocabulary: a Portuguese description over English code, 21% of gold files with no
informative term of the question, and no word-based or static-embedding method reaching them (research 3.4, 4).
The large model is bilingual and knows how code is usually named. So the engine asks it **one** short question
before searching:

```
system:  You translate a request about a codebase into the words its source code probably uses.
         Answer with JSON only.
user:    Request: <the user's request, at most 400 characters>
         Concepts: <the informative non-entity terms, comma separated>
         Languages of the project: <from the scanner, e.g. Kotlin, JavaScript>
         For each concept give up to 4 terms: English words and likely identifier fragments.
answer:  {"terms": {"<concept>": ["...", "..."], ...}}
```

- **Constrained output.** The answer is forced to that JSON shape by the grammar automaton that already constrains
  tool calls (`src/agent/callgrammar.c`, `cg_json_byte`): an object of arrays of short strings, at most 6 concepts,
  4 terms each, 24 characters per term, identifier characters and spaces only. Temperature 0. A hard cap of 80
  generated tokens; the automaton closes the JSON when the cap is near (the "shortest path to the end" logic
  already exists there for the test budget).
- **Cost** (arithmetic from documented speeds, not measured): 50 to 80 generated tokens at 15 to 27 tokens/s
  (`docs/embedding-usage.md` section 0) is about 2 to 5 s. The prompt is about 120 to 200 tokens of new text; its
  prefill cost depends on the path (35 to 5000 tokens/s, same source) and is the unknown part. The fixed part of
  this prompt must sit behind a cached prefix so that only the request is new.
- **Where it runs.** As a side generation that must not disturb the conversation's cache: start from the cached
  system prefix (or a dedicated small prefix), generate, discard the positions. `src/agent/delegate.c` (sub-agent
  with a clean context) and `agent_compact` in `src/agent/context.c` are the existing precedents for a generation
  outside the main turn. How cheaply the state can be forked and dropped on the qwen35moe path (it has recurrent
  state besides the attention cache) **was not examined for this document** and is the main implementation risk of
  this step.
- **Hallucination control: a term is worth nothing until the index confirms it.**
  1. Each proposed term is tokenized like code (`fd_tokens`: split, stem). A term with `df = 0` is dropped.
  2. A term in more than 15% of the files is dropped (uninformative).
  3. A term already in the request is dropped (no information).
  4. Survivors enter the query as `IXO_EXPANDED` terms with their `from` concept, at a lower weight than request
     terms (provisional 0.5 x idf).
  5. A candidate reached **only** through expanded terms is always PROBABLE and its evidence says so:
     `matches "session" (the engine's guess for your word "sessao")`.
  6. The model's expansion can add candidates and reorder them; it can never remove one and never produces a fact.
  If no term survives, the expansion is recorded as "none of the guessed code terms exist in this project", which
  is itself a line of the packet's gap section.
- **Cache.** Two levels, both in `.katana/cache/intel-<hash>/`:
  - request level: `expand.tsv`, key = hash of (model fingerprint, prompt version, sorted concept list), value = the
    surviving terms. A repeated or rephrased request with the same concepts costs nothing.
  - concept level: `terms.tsv`, the **project's own bilingual term map**: concept -> confirmed code terms, with a
    use count. Before generating, concepts already in the map are answered from it; only unknown concepts are sent
    to the model. After a few requests in a project most concepts are known and the expansion stops costing
    generation at all. This is the "Portuguese-English term map taken from the folder itself" that
    `docs/tools.md` and the research name as the next experiment, built from use instead of from co-occurrence.
    The file is plain text and the owner can edit it.
- **When NOT to expand:**
  - the request has an exact entity (`IXE_SYMBOL`, `IXE_PATH`, `IXE_LITERAL`, `IXE_COMMAND`) and `gap` is below 0.5:
    the lookup is better than any guess (the same reasoning as rule D4 of `docs/embedding-usage.md`);
  - `gap = 0`: every informative term already exists in the code;
  - type `IXT_NONE` or `IXT_OVERVIEW`;
  - fewer than 2 informative concepts;
  - every concept is already in `terms.tsv`;
  - `KATANA_INTEL_EXPAND=0`, or the request budget does not allow it (headless runs with a time limit);
  - the previous expansion in this session took more than its time cap twice.
- **What is unknown.** Whether the expansion actually moves the hop slice. It is the cheapest idea available against
  the measured gap and it is unmeasured. Section 11 lists the one offline run that decides it (it needs the large
  model for about 46 short generations and nothing else).

## 6. Investigation orchestrator

### 6.1 State machine

```
S0 INTERPRET   ixn_interpret(request)                                 -> IxQuestion        (rules)
     type NONE -> stop, no packet
     type OVERVIEW -> S7 with the project card
S1 ENSURE      database ready? (8.6)                                  -> ready | partial | absent
     absent and cannot be built inside the budget -> stop, no packet (the tools still work)
S2 ENTITIES    for each exact entity: definitions, literal hits, path matches               (EXACT)
S3 EXPAND      conditions of 5.3 -> ixn_expand                        -> expanded terms     (optional, the only model call)
S4 SEARCH      ixt_search(question incl. expanded terms) -> top K_HYP candidate chunks      (PROBABLE)
               map each chunk to its innermost symbol (ixd_sym_at)   -> candidate entry symbols
               merge candidates that are the same symbol or caller/callee of each other into one hypothesis
S5 STRUCTURE   per hypothesis, per plan of the type (6.2): callers, callees, path, guards, similar, tests   (EXACT)
S6 DIAGNOSE    DEBUG only: guards on the paths, asymmetry of the works/fails pair, missing links            (EXACT)
S7 SELECT      order hypotheses; give each a share of the token budget; choose excerpts; compute the gaps
S8 EMIT        ixe_packet -> text appended to the user turn; one dim line on stderr; the whole case in the log
```

The owner's steps map to the states: interpret the context of the question (S0), identify concepts (S0, S3), search
the whole base (S4), find entry points (S2, S4), find related services, events and methods (S5: callees, uses,
literal links), walk calls and dependencies (S5), find conditions that interrupt the flow (S6: guards), compare with
other implementations (S6: similar, asymmetry), select evidence (S7), deliver the context to the model (S8), produce
a diagnosis with file and line (the model, from the packet; the engine supplies the `file:line` of every fact).

There is no loop back from S5 to S4 in the first version: one search, one structural expansion. The research's
simulated policy showed that triggers and jumps added little (anchor jump -1.3 points, stuck trigger neutral;
section 3.5 there) and that the initial ranking carried the gain. A second search round, seeded by the identifiers
found in the bodies of the first round's hypotheses, is the natural extension and is left out until measured.

### 6.2 Plan per request type

| Type | Search (S4) | Structure (S5, S6) | Packet content | Token ceiling (provisional) |
|---|---|---|---|---|
| OVERVIEW | none | none | **project card**: languages by bytes, build and entry files, top-level folders with file counts and their most-imported files, the module dependency summary (which folder depends on which), where the tests are, the docs `docsel` points to. Built from the directory tree and the import graph, **not** from embedding clusters (the research found those coarse: section 4, "Profile card") | 400 |
| LOCATE | yes, K_HYP = 5 | for each candidate: signature and range only; for an exact entity: its definition and the count of callers | candidates with `file:line0-line1`, signature, the matched lines; no bodies | 600 |
| EXPLAIN | yes, K_HYP = 3 | entry symbol of each hypothesis; `Callees` depth 2 in call order; `Callers` depth 1 (who starts the flow); literal links (string names that connect a registration to its use) | hypotheses; for the first one the **ordered flow**: entry -> step 1 -> step 2, each `file:line` with its signature; the body of the entry symbol when it fits | 1200 |
| CHANGE | yes, K_HYP = 3 | full body of the top candidates (the model must read a definition before editing it); `Callers` depth 1 with counts (the impact); `Tests`; `Similar by=name` (sibling implementations that may need the same change) | hypotheses with bodies; "impact: N call sites in M files" listed; related tests; siblings | 1500 |
| DEBUG | yes, K_HYP = 5 | entry points for each side of the story (where the thing is produced, where it should appear); `Path` between them; `Guards` on every body of the path; `Similar` and the **asymmetry** when the request names a working and a failing sibling; relations that end in `external`, `textual` or nowhere | several hypotheses, never one; the path with its guards; the asymmetry table; the gaps | 2000 |

What "asymmetry" is (S6): when the request says A works and B does not, and both are names found in the tree, the
engine lists the places that mention A and have no counterpart for B: files, functions, table entries, string
literals, config keys. Each is a fact of the form `linux/x.sh:41 handles "A"; no line of the project handles "B"
there`. It is a set difference over occurrences, so it is EXACT. It is the mechanical version of "compare with the
implementation that works". On the research's case study (`gclaude` works, `gkatana` does not appear), this is the
step that would be tried first; **it has not been run on that case**, and whether it reaches the gold files there
is unknown.

What "guards" are (S6): the early exits the parser recorded in each body on the path (`if (!x) return`, `continue`
in the loop that lists items, `throw`, a filter callback, a feature flag read from the environment or config). They
are shown in path order with the condition text. The engine does not evaluate them. It cannot know which one fires;
it shows the model where the flow can stop.

### 6.3 Budgets and parallelism

Time (targets; the only measured figure behind them is the 1 to 7 ms `Find` query and the 19 ms index load of
`docs/tools.md`):

| Stage | Budget | Over budget |
|---|---|---|
| S0 interpret | 1 ms | - |
| S1 ensure (warm, in-process) | 5 ms; first call of the process: the listing cost of 2.6 | continue with the database as it is and say `partial` |
| S2 entities | 10 ms | truncate, count |
| S3 expand | 6 s wall clock, 80 generated tokens (`KATANA_INTEL_EXPAND_MS`) | stop generating, use what parsed, do not retry in this request |
| S4 search | 20 ms | - |
| S5 structure | 50 ms per hypothesis, 200 ms total | stop expanding; hypotheses already expanded keep their facts, the others are listed without structure |
| S6 diagnose | 100 ms | drop asymmetry before guards |
| S7, S8 | 10 ms | - |
| **Total without expansion** | **under 400 ms** | |
| **Total with expansion** | **under 7 s** | |

The per-request cap `KATANA_INTEL_MS` (default 8000) bounds everything; on overflow the packet is emitted with what
exists and its header says which stages were cut.

Tokens: the ceilings of 6.2, with `KATANA_INTEL_TOKENS` as a global override (default 1500, hard maximum 3000),
counted as 4 bytes per token, the rule `docsel` already uses. What the ceiling costs: `docs/tools.md` puts the local
model's reading speed at about 60 tokens/s, so a 1500-token packet is about 25 s of reading before the first
generated token (arithmetic, not measured on the current prefill path, whose speed varies from 35 to 5000 tokens/s
by `docs/embedding-usage.md`). For scale, the baseline failed investigation read 11914 input tokens in 238 s
(`docs/investigate.md`). The packet pays for itself only if it replaces at least one or two exploratory rounds.
That is the bet, and it is unmeasured.

Parallelism: S2, S4 and the per-hypothesis parts of S5 are read-only over `mmap`ed tables and run on the thread pool
(`src/engine/base.c`), one task per hypothesis. S3 uses the model and therefore runs alone, with the pool idle; S2
runs before it, so the exact part of the packet is ready even if S3 times out. Background indexing (8.6) must stop
while the model computes: with most expert work on the CPU, a second CPU load slows every token
(`docs/embedding-usage.md` 3.5).

### 6.4 High uncertainty

The engine has no calibrated confidence (research 3.6), so it does not pretend to have one. It uses **structural**
signs of uncertainty, each cheap and deterministic:

| Sign | Meaning |
|---|---|
| no exact entity in the request | the entry point is a guess |
| `gap >= 0.5` | the request's words are mostly absent from the code |
| the top hypotheses are in unrelated modules (no import or call path between them within 3 hops) | the search is scattered |
| the best candidate entered through an edge or through expanded terms only | no direct textual support |
| the first two hypotheses have nearly the same score | no separation |
| the database is `partial`, or files of the hypotheses had parse errors | the structure may be incomplete |

Policy:

- **Never one hypothesis with an air of certainty.** When there is no exact entity, the packet shows at least 3
  hypotheses if 3 exist, in ranked order, and its header says "The engine could not identify the entry point with
  certainty."
- With two or more signs, the header says so in plain words and the budget is spread **evenly** across the
  hypotheses instead of concentrated on the first (with a top-1 precision near 4% and "top 5 contains gold" near
  41%, breadth is worth more than depth on the first candidate).
- With `gap >= 0.5` and no surviving expanded term, the engine emits **no hypotheses**: only the exact part (if
  any), the project card lines for the folders whose names match, and the gap statement. An empty-handed packet that
  says so is better than five random files.
- The words "confidence", "likely N%" and any percentage are forbidden in the packet, as in
  `docs/embedding-usage.md` 4.2. A score reads as a probability and nothing here behaves like one.

### 6.5 Provenance

Every statement in the packet is one of these forms, and the packet writer cannot emit anything else (the
`IxFact`/`IxHit` structures have no free-text field for claims; `why` is filled from a fixed set of templates):

| Statement | Provenance carried |
|---|---|
| a definition | `file:line0-line1`, parser confidence when below exact |
| a relation | `from (file:line) -> to (file:line)`, the relation type (`calls`, `imports`, `inherits`, ...), the resolution class |
| a candidate | `file:line0-line1`, how it entered: `words: a, b` / `via <file> (symbol X)` (the existing `fg_evidence` strings) / `matches "t" (the engine's guess for your word "c")` / `literal "text"` |
| a guard | `file:line`, the source line itself |
| an asymmetry | both sides: where A is handled, and the scope searched for B |
| a gap | what was searched, where, and that it returned nothing |

The full case (`IxCase`: the question object, every candidate with its score, every fact, what was cut and why,
the time of each stage) is written to the action log (`KATANA_LOG`) as one JSON line, so `katana agentlog` and the
evaluation scripts can audit what the engine told the model.

## 7. The evidence packet

### 7.1 Format

Plain text, appended to the user turn after the request and after the `docsel` note. English, like every
`[katana: ...]` note. Fixed section order; an empty section is omitted except `Not found`, which is always present.

```
[katana: evidence gathered by the engine before you saw this request. Lines marked exact come from the structure of
the code (definitions, calls, imports) and can be relied on. Lines marked probable are the engine's guesses about
what the request refers to; they may be wrong. Before you state something about a file, read the lines given here.
If what you need is under "Not found", look for it yourself with Find, Grep, Def, Callers or Read.]
Request read as: debug ("why does X not happen"). Named in the request: gkatana (command, exact: 2 files), gclaude (command, exact: 10 files).
The engine could not identify the entry point with certainty: 3 hypotheses follow, best first.

== Hypothesis 1 (probable): the terminal session list sent to the phone
  why: words "session", "terminal" (the engine's guess for your words "sessao", "terminal"); via bridge/server.mjs (symbol listSessions)
  entry: onMessage  bridge/server.mjs:301-365  function onMessage(ws, raw)
  flow (exact):
    1. onMessage            bridge/server.mjs:301      calls at 331 -> listSessions
    2. listSessions         bridge/sessions.mjs:132    calls at 140 -> terminalSessionIds
    3. terminalSessionIds   bridge/sessions.mjs:114
  can stop here (exact):
    bridge/sessions.mjs:118   if (!KNOWN_COMMANDS.includes(s.command)) continue;
    bridge/server.mjs:327     if (!client.approved) return;
  excerpt bridge/sessions.mjs:114-122 (exact, body of terminalSessionIds)
    114  export function terminalSessionIds(state) {
    ...

== Hypothesis 2 (probable): the install script that registers the commands
  why: literal "gkatana" at linux/install.sh:41
  ...

== Compared (exact): gclaude is handled in places where gkatana is not
  bridge/sessions.mjs:12    const KNOWN_COMMANDS = ['gclaude'];        no "gkatana" in this file
  ...

== Not found
  - No call path from linux/install.sh to bridge/sessions.mjs (they may be linked by a name in a string, which the engine does not follow).
  - "celular": no code term found for it (tried: phone, mobile, device; only "phone" exists, in 14 files, too common to use).
  - 2 more candidates were left out for space: android/.../ShadeActivity.kt:88-131, bridge/README.md:40-71.
[katana: end of evidence. 1380 tokens. It is a selection, nothing is hidden: every tool still sees the whole project.]
```

(The example is illustrative. Its file names come from the research's case study; its line contents are invented
to show the format and are not claims about that repository.)

Per-excerpt rule: every excerpt line has `file:line0-line1`, the mark `exact` or `probable`, and its relation to
the entry point (`body of`, `caller of`, `called by step 2`, `sibling of`, `test of`, `matched text`). An excerpt
that is only a ranked text match is `probable`; an excerpt reached from a hypothesis by a relation is `exact` **as a
relation** and inherits the hypothesis's doubt as to relevance. The header line of each hypothesis block carries
that doubt, so the model reads "probable hypothesis, exact structure inside it".

### 7.2 Token ceilings

| Part | Share of the ceiling |
|---|---|
| header and footer | fixed, about 120 tokens |
| exact entities named in the request | first, up to 25% |
| hypotheses | 60%, split by the rule of 6.4 (even split under high uncertainty; otherwise 50 / 30 / 20) |
| compared / asymmetry | up to 15% (DEBUG only) |
| not found | always at least 60 tokens reserved |

Ceilings per type as in 6.2 (400 / 600 / 1200 / 1500 / 2000 tokens; default global cap 1500, hard maximum 3000).
They are provisional. The buffer that carries the user turn in `loop.c` (`nl[16384]`) and `docsel`'s 8000-byte note
bound the sum today; a 2000-token packet is about 8000 bytes, so the buffers must be reviewed when the hook is
written.

Trimming order when over budget: excerpt bodies shrink to signature plus the lines with matches, then guards beyond
the first 5 per path, then callees at depth 2, then the lowest hypothesis (which moves into `Not found` as "left out
for space", with its `file:line`). The `Not found` section and the provenance of what remains are never trimmed.

### 7.3 How the model is told to use it

Three places, all short:

1. The packet's own first line (above). It is the complete instruction and it travels with the packet, so a session
   without the layer has no dangling rule in its prompt.
2. One sentence in the tool guide of the system prompt (`src/agent/prompt.c`, next to the `Find` sentence): "When a
   request arrives with engine evidence, start from it: read the lines it points to, confirm, and use Def, Callers
   and Callees to follow the code; if it says something was not found, search for that yourself."
3. The curriculum (section 10).

What the model must do with it: check before affirming (read the pointed lines, which are a `Read` with `offset` and
`limit` away, or already in the excerpt); treat `probable` as a place to look, not as an answer; ask for more with
the tools when a gap matters; say what it could not verify.

### 7.4 How it lives with what exists

- **The current prompt.** It says "In a project you do not know: Find first, then Read only the part it points to".
  With the packet, the engine has already done the first `Find`. The sentence stays (it is right when there is no
  packet) and gains the clause of 7.3.
- **`docsel`.** Unchanged and before the packet: documents are about the project's conventions, the packet is about
  the code. They share one budget rule (4 bytes per token) and the same opening `[katana: the engine picked ...]`
  style. If both are present the packet's header does not repeat `docsel`'s caveat.
- **Per-result notes.** `fg_neighbor_note` skips files already shown in the packet (they enter its "cited" set), so
  the first notes point outward from the packet instead of back into it. `rr_check` is unchanged. A `Read` of a
  range the packet already quoted in full is answered normally: the packet is not a tool result and must not block
  a read.
- **The rule of hiding nothing.** The packet orders and budgets; it never excludes. Three guarantees: (1) everything
  cut is counted and named in `Not found` with `file:line`; (2) `Read`, `Grep`, `Glob`, `Find` and the structural
  tools see the whole project and are never filtered by what the packet chose; (3) the footer says so to the model.
  No tool result is ever rewritten because of the packet.
- **Automatic seed.** The research's "no unrequested seed" becomes: no unrequested **answer**. Hypotheses are
  plural, marked, and each carries the evidence that produced it. If the minimal measures of section 11 show that
  even this hurts, `KATANA_INTEL_PACKET=exact` keeps only the exact part (entities named in the request, with
  definition, callers and tests), which is the R7 to R9 rule the research already endorsed.

## 8. Integration

### 8.1 Where in the loop

`src/agent/loop.c`, in the block that prepares the user message, right after the `ds_select` call and before
`vis_take` and the plan-mode note:

```c
if (cml && ix_on()) {       /* camada de inteligencia de codigo: investiga antes do modelo e entrega o pacote de evidencias */
    static char ixp[12000];
    const int n = ix_prepare(m, line, ixp, sizeof ixp);        /* 0 = nada a entregar */
    if (n) { /* line = line + "\n\n" + ixp, como o docsel faz */ }
}
```

`ix_prepare` receives the **original** request (before the `docsel` text, the folder-change note or the undo note
are added), so those notes are not interpreted as part of the question. Because the packet is part of `line`, it
goes where the user turn goes: `pend`, the history, the session file.

### 8.2 Detecting the request type at low cost

Section 5.2: rules over the request text plus hash lookups in the name table. No model call, well under a
millisecond after the database is mapped. The expensive step (5.3) runs only after the cheap ones have decided that
it can help. A request typed `IXT_NONE` costs the tokenization and nothing else.

### 8.3 Switches

| Variable | Default | Meaning |
|---|---|---|
| `KATANA_INTEL` | to be decided by the owner (see below) | `0` turns the whole layer off: no packet, structural tools answer from the old index or say unavailable, `Find` is today's `Find` |
| `KATANA_INTEL_PACKET` | `full` | `off`: tools only; `exact`: only named entities and their structure; `full`: with hypotheses |
| `KATANA_INTEL_EXPAND` | `1` when the packet is `full` | `0`: no model expansion |
| `KATANA_INTEL_TOKENS` | `1500` | global packet ceiling |
| `KATANA_INTEL_MS`, `KATANA_INTEL_EXPAND_MS` | `8000`, `6000` | time caps |
| `KATANA_INTEL_BG` | `1` | background indexing while waiting for the user |
| `KATANA_INTEL_MAX_FILES` | `200000` | above it, only the folders touched are indexed (8.6) |
| `KATANA_INTEL_MEM_MB`, `KATANA_INTEL_DISK_MB` | `512`, `2048` | ceilings of 2.7 |
| `KATANA_INTEL_LSP` | `0` | `1`: ask an installed language server first for resolved relations (3.5) |
| `KATANA_INTEL_TIME` | `0` | `1`: stage times on stderr, like `KATANA_FIND_TIME` |
| `KATANA_INTEL_DET` | `0` | `1`: deterministic mode for tests and curriculum generation (no time cuts, no model expansion unless a fixed table is given) |

Proposed default for `KATANA_INTEL`, for the owner to decide: **on for the database and the structural tools as
soon as phase 2 passes its criteria** (they are exact and replace heuristics already in use), and **the packet on
in `exact` mode first**, moving to `full` after the two minimal measures of section 11. The reason is the research:
the exact part has no measured downside, the probable part has a measured top-1 near 4%.

### 8.4 Headless mode (`-p`)

Same rules. No background indexing (there is no idle time). The database is built synchronously if the tree is
small enough to finish inside `KATANA_INTEL_MS`; otherwise the run proceeds with what exists (possibly nothing) and
the packet header or its absence reflects that. The expansion counts against the run's time limit, so `-p` with
`--max-turns` or an external timeout may set `KATANA_INTEL_EXPAND=0`. The JSON output of headless mode reports the
packet's token count and stage times in its usage object, so evaluations (`tools/investigate/eval.sh`, `katana
codebench`) can separate engine time from model time.

### 8.5 Resumed sessions and long conversations

- A packet is part of the user turn it was built for and is saved with it. On resume it is old text: the tree may
  have changed. The packet's header and footer are exact strings so `src/agent/context.c` can recognise a packet
  and, when compacting, replace it by one line (`[katana: evidence packet of an earlier request removed]`). Old
  packets are the first thing compaction drops.
- Line numbers in an old packet may be stale. The model is already taught that the facts notebook and the delta
  reading (`src/agent/facts.c`) override older text; the structural tools always answer from the current tree.
- Follow-up messages in the same conversation: a message with no informative term ("ok, fix it", "e o outro?")
  is `IXT_NONE` and gets no packet. A follow-up that names something new gets a packet for the new thing only;
  hypotheses already delivered in this session are not repeated (the session keeps the set of excerpt ranges already
  sent and the packet says "already shown above" for them).
- Changing folder (`cd`) closes the database handle and opens the one of the new folder, as `Find` does per root.

### 8.6 Projects without git, huge projects, folders that are not projects

- **No git.** The scanner falls back to the walk with `.gitignore` rules (already in `ct_grep_collect`). The `co`
  relation is skipped silently, as today. Nothing else depends on git.
- **Huge project (more than a few tens of thousands of files), first contact.**
  1. *Immediately* (target: within the listing time): the file table and the project card. Overview requests work.
  2. *On demand*: a request triggers indexing of the folders its entities and path terms point to, inside the
     request's time cap. Text search works over what is indexed; the packet header says "the engine has indexed
     N of M files so far; results are partial" and `Not found` says what was not searched.
  3. *In the background* (`KATANA_INTEL_BG`): while the agent waits for the user's next message, a low-priority
     thread indexes the rest, most recently modified files first; it stops at the next file boundary when a message
     arrives and whenever the model computes.
  4. `katana intel index [path]` builds everything explicitly, with progress, for the owner who wants it done.
  Past `KATANA_INTEL_MAX_FILES`, step 3 is off and only steps 1, 2 and 4 apply.
- **Generated and vendored code** (`node_modules`, `build`, `dist`, `vendor`, minified files, files over 300 KB):
  skipped as today (`fd_skip_rel`, `FD_MAXBYTES`), with one change: a large source file is indexed for symbols with
  streaming (the 300 KB cap was chosen for the in-memory blob), and the file record says what was skipped and why.
- **A folder that is not a project** (the home folder, `/tmp`, a downloads folder): the layer stays off unless the
  folder has a project marker (a git root, a build file, `KATANA.md`) or was indexed before. This keeps the rule
  the neighbour note already follows: "the loop never creates cache files for a folder that never used `Find`"
  (`docs/tools.md`). With no marker, the structural tools index on explicit request only.
- **Not writable.** The database goes to `~/.katana/cache`, as the `Find` index does.
- **Two katana processes in one folder.** One writer by advisory lock; the others read the last complete state.
- **Windows.** Later. `find.c`, `findgraph.c` and `multiedit.c` are compiled out there today (`#ifndef _WIN32`
  stubs). The layer needs: the file listing without `popen`, `mmap` through the file-mapping API, the lock, and a
  watcher. The parsers and queries are portable C. Until then the tools answer "not available on Windows yet" as
  the existing three do.

## 9. Build order

Effort is in working days of one agent and is an **estimate**. Each phase ends with something usable and a
criterion that can be checked without a measurement campaign.

| Phase | Deliverable | Done when | Effort | Depends on |
|---|---|---|---|---|
| 0. Extraction | `src/intel/` exists; scanner, term table, per-file indexing and the file graph moved out of `find.c` / `findgraph.c` with no behaviour change | `tools/tooltest/find.sh` and the curriculum `check` of the `x-find6` family pass byte for byte | 1 to 2 | - |
| 1a. Structural indexer, core | lexer layer; `IxParser` interface; database format of 2.4 with segments and `mmap`; incremental update; `katana intel index`, `stats`, `dump FILE` | a hand-checked golden set (about 30 files: katana's own hardest C files, the Qt app, the bridge, Python tools) where every function's `line0-line1` is listed; the parser matches it or the mismatch is a documented failure class of 3.4; re-index after touching one file re-parses one file | 4 to 5 | 0 |
| 1b. Parsers, tier 1 | C, C++, JS/TS, Python: definitions with ranges, calls, imports, inheritance, guards | golden set per language; on the katana tree, zero functions with `conf = 0` outside files listed as known hard cases | 4 to 6 (one language family per agent, in parallel after 1a's first two days) | 1a interface |
| 1c. Parsers, tiers 2 and 3 | Kotlin/Java, Go, Rust; shell, QML, PowerShell, Markdown | golden set per language (smaller) | 3 to 4 (parallel, one per agent) | 1a interface |
| 2. Structural queries as tools | `ixq_*`; tools `Def`, `Callers`, `Callees`, `Outline` in the core list; `Uses`, `Importers`, `Dependents`, `Similar`, `Tests`, `Path`, `Guards` under "More tools"; `Read` map, `Find` `calls:`, `fd_enclosing` and the neighbour note moved onto the database; `katana tool <Name>` for each | the five questions of the owner answered by command on the katana tree and on one large tree: which functions and classes exist, who calls a function, which files reference a symbol, which modules depend on which, where similar implementations are; each answer checked by hand on 10 symbols per language; `tools/tooltest` script for every tool | 3 to 4 | 1b |
| 3. Scale | 100k files: segments, memory ceiling, background and on-demand indexing, listing shortcut, lock | a synthetic or real tree of 100k files indexes inside the ceilings of 2.7, or the ceilings are rewritten with the measured values; kill during indexing leaves a usable database | 3 to 5 | 1a (parallel with 2) |
| 4. Interpreter, orchestrator, packet (no model) | `question.c`, `investigate.c`, `packet.c`, `diagnose.c`; hook in `loop.c`; `katana intel ask "<request>"` prints the packet without a model; `KATANA_INTEL_PACKET=exact` and `full`; log line with the full case | the request types of 5.2 classified correctly on the 76 questions plus 30 hand-written non-investigation requests (a table, checked by hand); packets for the 76 questions generated offline and inspected for format, budget and provenance; deterministic under `KATANA_INTEL_DET=1` | 5 to 7 | 2 |
| 5. Query expansion by the model | side generation with the JSON grammar; validation against the index; `expand.tsv`, `terms.tsv` | on 10 Portuguese requests the surviving terms are printed with their origin; a request repeated costs no generation; the conversation's cache is intact after the side generation (the next turn reads no extra tokens) | 2 to 4 (the cache question of 5.3 decides which end) | 4 |
| 6. Curriculum | families of section 10, generated by the engine with real packets and real tool outputs | `check` replays them byte for byte | 3 to 4 | formats of 2 and 4 frozen (parallel with 5) |
| 7. Semantic search (optional) | `semantic.c` over symbol bodies, following `docs/embedding-usage.md` | only started if the offline measure of section 11 moves the hop slice | 3 to 5 | `katana embed`, 4 |
| 8. Windows | listing, mapping, lock | the tool tests pass there | 2 to 3 | 3 |

Critical path: 0 -> 1a -> 1b -> 2 -> 4 -> 5, about 19 to 28 days of one agent (estimate). In parallel: the
tier 1 parsers among themselves; 1c with 2; 3 with 2 and 4; 6 with 5. With three agents the calendar time is
roughly half (estimate).

The order follows the owner's suggestion: phases 0 to 3 contain no model at all and already give the agent better
tools than it has; phase 4 is the investigation intelligence without the model; the model enters only in phase 5,
for one constrained question; semantic search is last and conditional.

## 10. Training

The model must meet the packet and the structural tools in training before it meets them in use
(`docs/curriculum.md`: the engine writes the text; `check` replays every conversation byte for byte). New
situations, as families of composed scenarios next to `x-find6`:

| Situation | What the conversation shows |
|---|---|
| `pk-right` | a request arrives with a packet whose first hypothesis is right; the model reads the pointed lines (one `Read` with `offset` and `limit`, or none when the excerpt is in the packet and suffices for a question), then answers with `file:line`. It does not re-search what the packet already found |
| `pk-wrong-first` | the first hypothesis is wrong and the second is right; the model reads the first, says it is not the place, reads the second, answers. The majority case by the research's numbers, so it must be the majority of the family |
| `pk-all-wrong` | no hypothesis is right; the model says so after checking, and investigates with `Find`, `Grep`, `Callers`; the answer cites what it read, not the packet |
| `pk-gap` | the answer depends on something listed under `Not found`; the model searches for exactly that, with the tool the gap suggests |
| `pk-gap-irrelevant` | a gap is listed that does not matter for the request; the model does not chase it |
| `pk-exact-only` | the packet has only the exact part (a named function: definition, callers, tests); the model uses it directly |
| `pk-none` | the same kinds of request with no packet at all; the model behaves as in `x-find6`. Keeps the model independent of the layer |
| `pk-change` | a change request; the packet lists callers and tests; the model reads the definition, edits, then checks or updates the callers the packet listed, and runs the listed test |
| `pk-debug` | a "why does X not happen" request; the packet shows a path with guards and a comparison; the model reads the guard lines, picks one **by reading**, explains with `file:line`, and names the other guards it ruled out or could not rule out |
| `pk-absent` | the thing asked about does not exist; hypotheses are near misses; the model reads, checks with `Grep`, says the project has no such thing |
| `pk-stale` | a resumed conversation with an old packet whose lines moved; the model re-reads before citing |
| `st-def`, `st-callers`, `st-callees`, `st-outline`, `st-path` | the structural tools used for what they are for: `Callers` before changing a signature; `Callees depth=2` to explain a flow; `Outline` of a folder to answer "how is this module organised"; `Def` instead of reading a whole file; an ambiguous name (`1 of 3 definitions`) resolved by asking with `path` |
| `st-approx` | a result flagged `(range approximate)` or `by name only`; the model confirms by reading before relying on it |
| unchanged | `exact`: a known literal still goes to `Grep` |

Rules that keep the training independent of internals:

- The model learns **the contract, not the machinery**: the marks `exact` and `probable`, the `Not found` section,
  the tools' names and outputs. No conversation mentions modules, the database, activation, expansion, parsers, or
  how a hypothesis was ranked. If the ranking is replaced, no training text changes.
- The final answer never says "the engine found" or quotes the packet as its source. It cites what the model read,
  with `file:line`.
- Visible reasoning never uses the order of hypotheses as an argument. Reading decides.
- The packet text in training data is produced by the real `ixe_packet` under `KATANA_INTEL_DET=1`, never written by
  hand, so a format change is caught by `check`. Expansion terms in generated data come from a fixed table inside
  the generator (the scenario knows which concept maps to which identifier), not from a model.
- Proportion: `pk-none` plus the existing families stay the majority until the packet is on by default; inside the
  packet families, wrong-first and all-wrong together outnumber right-first, to match what the research measured and
  to prevent the model from learning that the first hypothesis is the answer.

## 11. Risks and honest uncertainties

### 11.1 Where this layer can make things worse

| Risk | Why it is real | What limits it |
|---|---|---|
| **A wrong packet with an air of certainty** | measured: top-1 about 4%, top-5-has-gold 41%, and no confidence that separates them (research 3.3, 3.6). A model that anchors on the first hypothesis will be wrong more confidently than today | plural hypotheses, the `probable` mark, the forced "could not identify the entry point" header, even budget split, `pk-wrong-first` as the majority training case, `exact` mode as the fallback. None of these is proven to stop anchoring |
| **Tokens spent without help** | 1500 tokens is about 25 s of reading at 60 tokens/s (arithmetic) on every qualifying request, including those the model would have solved in two calls | type `IXT_NONE` as the default; small ceilings for LOCATE; `exact` mode costs little; per-type ceilings |
| **Latency before the first token** | expansion adds an estimated 2 to 5 s of generation plus an unknown prefill; the first index of a large tree adds more | time caps; the term map removes most expansions over time; background indexing |
| **Exact facts that are not** | heuristic parsers on macros, templates, dynamic names; name-only method resolution | resolution classes, `conf` flags, "partly parsed" notes; the golden sets of phase 1; never printing `exact` for `textual` or `ambiguous` |
| **The model stops investigating** | a packet that looks complete invites answering from it | the header instruction; `Not found` always present; training rules; unknown until agent runs |
| **Wrong request type** | cue words are crude; a debug request typed as locate gets a thin packet, a chat message typed as debug gets noise | default `NONE`; a wrong type costs budget, not correctness |
| **Conflict with the earlier design** | `docs/embedding-usage.md` D26 and research section 6 forbid an unrequested seed | this document narrows that to "no unrequested answer"; if the measure of 11.3 says even hypotheses hurt, `exact` mode restores the old rule |
| **Three sources of truth during the transition** | `fd_sym`, `ct_outline` and the new parser can disagree while both exist | phase 0 and 2 retire the old ones; the curriculum `check` detects format drift |
| **Disk and memory on big trees** | the 0.9 GB figure is an extrapolation with assumed densities | ceilings enforced by code; text-only fallback per directory |
| **Maintenance** | ten languages of heuristics | one lexer layer; the plugin and the language-server path as escape hatches; tier 3 languages may stay line-based with ranges |

### 11.2 What the research measured and what it did not

Measured (all in the Python prototype, `docs/investigation-engine.md`; the C port of the activation was **not**
re-measured, `docs/tools.md`):

- lexical search and activation recall on the 46- and 76-question sets, and the hop slice;
- that confidence does not transfer and nothing reaches 90% precision on the questions;
- that static embeddings and the large model's input table do not help;
- that a better initial ranking is worth more than triggers, in a **simulated** policy that reads the best chunk of
  each suggested file (optimistic for every policy alike, by its own statement).

Not measured, by anyone, anywhere in the repository:

- any agent run in which the model receives a packet, a hypothesis list or even the neighbour note ("whether the
  model follows the notes is unknown", `docs/tools.md`);
- whether Qwen3.6 calls `Find` and follows `calls:` at all ("no run with the model yet");
- whether symbol-level exact structure around a wrong entry point helps the model recover or sends it further away;
- the precision of any parser described here;
- query expansion by the large model;
- the retrieval embedding model's effect on recall (its experiment is incomplete; only its speed in the first
  implementation is known, 112 tokens/s by the owner's brief, too slow for interactive use without cache and
  batching);
- every time and size figure of this document that is not quoted from `docs/tools.md`.

So the design rests on one measured gain (activation over BM25, in simulation), several measured negatives that it
is built to respect, and unmeasured expectations for its two new ideas: exact structure and model-written expansion.

### 11.3 The minimal measures that would decide (listed only; none was run, no campaign is proposed)

| Question | Smallest measure | Needs the large model |
|---|---|---|
| Are the parsers good enough to call their output exact? | the golden sets of phase 1 (hand-listed ranges and callers for about 30 files); count mismatches by failure class | no |
| Does symbol-level structure reach the gold files that file-level activation misses? | offline, on the existing 46-question sample: for each question, are the gold files within the union of (top-5 candidates, their callers, their callees to depth 2)? Compare with top-5 alone (41.3% has-gold today) | no |
| Does expansion close the language gap? | the same sample: recall at 5 and at 10 on the 27 hop files with and without expanded terms | yes: about 46 generations of 80 tokens, a few minutes |
| Does the asymmetry step find the cause when a sibling works? | run it on the questions that name a working and a failing thing (the mandatory question is one); count gold files in its output | no |
| Does a packet help or hurt the agent? | the validation split (24 questions) of `tools/investigate/eval.sh`, three arms alternated in the same hour: no layer, `KATANA_INTEL_PACKET=exact`, `full`. Verdict, tool calls, input tokens, seconds | yes: this is the only expensive one, and the only one that answers the real question |
| Does the model anchor on hypothesis 1? | in the same runs: among failures with a packet, how often the answer cites the first hypothesis's file when it was wrong | from the same runs |
| What does the packet cost in time? | `KATANA_INTEL_TIME=1` on those runs: stage times and the prefill time of the packet | from the same runs |
| Are the size and time estimates of 2.7 right? | one cold index of the largest tree available, `KATANA_INTEL_TIME=1` | no |

The first, second, fourth and last need no model and come for free with phases 1 to 4. The third costs minutes.
Only the fifth is a real evaluation run, and it is the one that should gate turning `full` on by default.

### 11.4 Main uncertainties, in order of weight

1. **Whether a pre-built packet helps this model at all.** No agent run has ever tested the model's reaction to
   engine-supplied evidence. Everything from section 5 onward is a design for a behaviour that has not been
   observed.
2. **Whether the entry point can be found often enough.** Exact structure is only as useful as the place it starts
   from. If expansion does not move the hop slice, the packet's hypotheses stay near the measured 41% has-gold in
   five, and the layer's value shifts almost entirely to the structural tools (phases 1 and 2), which stand on their
   own.
3. **Whether lightweight parsers are precise enough on the owner's code**, C with macros and one translation unit
   first, C++/Qt second.
4. **The cost of the side generation** on the qwen35moe path without disturbing the conversation cache.
5. **Scale.** 100 thousand files is 19 times the largest tree ever indexed here; the extrapolations are linear and
   the densities assumed.
6. **Effort.** The estimates of section 9 assume the heuristics work on the first or second try per language; the
   hard cases of 3.4 are where days disappear.
