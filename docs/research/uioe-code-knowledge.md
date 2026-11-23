# UIOE research: code knowledge infrastructure, indexing and source federation

Status: research study, no code, nothing measured (owner decision of 2026-11-22).
Access date of every URL below: 2026-11-23.

This is a reference study, so it names the studied projects and gives URLs. It
summarizes in its own words and copies no long text.

## 0. How to read this document

Every claim carries a tag:

- **[V]** VERIFIED: seen in the cited source during this study (URL in the section
  or in the source list in section 17).
- **[R]** RECALLED: from the author's prior knowledge, not re-checked now. Treat
  as a lead to confirm before building on it.
- **[V-]** the fetched page did not contain the detail, so the detail is [R].
  Where a page did not cover something I say so instead of guessing.

Scope. The UIOE (Unified Intelligence Orchestration Engine) is a layer that runs
BEFORE the LLM. Real architecture, as given by the owner:

```
USER INPUT -> UIOE (Interpret, Classify, Route, Retrieve, Orchestrate)
           -> SOURCES (Filesystem, Web Search, Financial APIs, Code Repositories,
                       Legal Sources, Databases)
           -> CONTEXT BUILDER -> LLM EXECUTION
```

It must be deterministic, explainable, with no embeddings and no external
service in the core, and it feeds a small, slow, local model. The code index is
ONE source ("Code Repositories"). This study therefore has two halves:

- Part A (sections 1 to 8, 11, 12): how robust systems represent, index,
  update and query code knowledge, and cross-process links. This is the "Code
  Repositories" source.
- Part B (sections 9, 10, 13): how heterogeneous sources are federated, how any
  source result becomes one "evidence" record, and what "robust" means for
  legal, financial and database sources.
- Sections 14 to 17: comparison table, the 10 lessons, the gap list, sources.

Katana baseline assumed (from the task brief, not re-read from the code):
own tolerant parsers for 60+ languages/formats with no name resolution (a call
is its final name), immutable-segment index with BM25, trigrams and a pt/en
knowledge base, symbols and references, text-key links across files/languages
(routes, message types, events, env vars), and a tiered-crawler indexing design
(docs/code-intelligence.md, docs/code-index-lifecycle.md).

---

## 1. Language Server Protocol (LSP)

Source: LSP 3.17 spec, https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/

**Data model.** [V] Request/response over JSON-RPC between an editor client and a
language server. Documents are identified by URI plus an increasing integer
version. There is no persistent index in the protocol; the server holds whatever
state it likes and answers per request.

**Capabilities.** [V] `initialize` must be the first request. The client
announces `ClientCapabilities`, the server answers `ServerCapabilities`; each
side ignores capabilities it does not understand. Position encoding is also
negotiated (client lists supported encodings, server picks one, default
UTF-16). Lesson: capability negotiation is how a protocol survives servers of
very different strength. A server that cannot do "find references" simply does
not declare it.

**Staleness and cancellation.** [V] A client cancels with `$/cancelRequest`; a
cancelled request must still get a response (not left hanging), with error code
`RequestCancelled`. `ContentModified` (-32801) says the document changed under
the computation. The spec says a result computed on an older state "might still
be useful", and a client capability (`staleRequestSupport`) says whether the
client cancels stale requests and which it retries. So LSP treats staleness as
a negotiated, per-request-kind policy, not a bug.

**Partial results and progress.** [V] A client opts in with a partial result
token; the server streams chunks through `$/progress`, and the final response
is empty to avoid double counting. Partial results are only usable after an
error if the error is a cancellation, and then the client should show the
result may be incomplete. Work-done progress has begin/report/end. This is a
standard way to tell a consumer "this answer is incomplete, and here is how far
indexing is".

**Latency.** [R] The interactive contract is "answer in tens to hundreds of
milliseconds or be cancelled"; servers keep the workspace in memory, which is
why startup (cold index) can take seconds to minutes on big projects and why
many servers show a progress bar.

**Precision.** [R] Only as precise as the server: servers backed by a compiler
front end (gopls, rust-analyzer, clangd, tsserver) resolve names and types;
servers over heuristics may not. The protocol carries no confidence field.
[V-] The parts of the spec on `workspace/symbol` and `didChangeWatchedFiles`
were beyond the fetched text.

**Limits.** No cross-language semantics, no provenance, no confidence, no
stable cross-session symbol identity (a symbol is a position in a document at a
version).

**Katana: TAKE** the capability-declaration idea (each source/analyzer declares
what it can answer, so the router never asks for what does not exist), the
cancellation-with-response rule (a deadline always yields an answer object, even
"partial: timed out"), and the begin/report/end progress notion for indexing
state. **ADAPT** `ContentModified` into an evidence field: the UIOE should know
the version/hash of the content the answer was computed on. **REJECT** a
long-lived LSP process per language as a dependency: the UIOE core stays inside
the binary and does not need a compiler front end.

---

## 2. LSIF and SCIP

Sources: LSIF overview https://microsoft.github.io/language-server-protocol/overviews/lsif/overview/ ;
SCIP announcement https://sourcegraph.com/blog/announcing-scip ;
SCIP schema https://raw.githubusercontent.com/sourcegraph/scip/main/scip.proto ;
SCIP site https://scip-code.org/

### LSIF

[V] A graph of vertices (documents, ranges, results) and edges (LSP request
kinds, `contains`). Results hang on ranges, not single positions, to keep it
small; the format can be emitted as a stream. It deliberately carries no symbol
semantics (no notion of what is a definition, whether a method overrides
another) and no symbol database; it covered a fixed set of requests (document
symbols, links, definition, declaration, type definition, references,
implementation). The overview page presents it as an early-stage spec.

### SCIP

[V] Why it replaced LSIF according to Sourcegraph: no machine-readable schema
(SCIP is Protobuf), large in-memory structures to read/write the graph, opaque
numeric IDs that make payloads undebuggable (SCIP uses readable symbol
strings), and global incrementing IDs that block partial updates. Reported
numbers (from Sourcegraph's own post, so vendor claims): LSIF about 4x larger
gzipped and 5x uncompressed; a Meta engineer reported 8x smaller and 3x faster
to process in Glean; one indexer swap gave a 10x CI speedup, though the post
says the gain is not due to the protocol alone.

[V] Data model (from `scip.proto`):
- `Index` = one workspace root, with `Metadata` first (streaming-friendly),
  `Document`s, optional `external_symbols`.
- `Document` = one file: language, canonical relative path (forward slashes, no
  `.` or `..`), occurrences, symbols, and a declared `position_encoding`
  (UTF-8/16/32 offsets).
- `Occurrence` = range + symbol string + `symbol_roles` bitset (Definition,
  Import, ReadAccess, WriteAccess...) + optional syntax kind, diagnostics,
  `enclosing_range` (nearest non-trivial enclosing AST node, used for call
  hierarchy and breadcrumbs) and `override_documentation` (per-occurrence docs
  when the exact context, such as a generic instantiation, is known).
- `SymbolInformation` = docs, kind, display name, `enclosing_symbol`,
  `Relationship`s (reference, implementation, type definition, definition
  flags that control which navigation features follow the link).
- Symbol string grammar: global = scheme + package (manager, name, version) +
  a chain of typed descriptors (namespace, type, term, method, parameter, type
  parameter, meta, macro); local = `local <id>`, and the spec says local symbols
  MUST be used only for entities local to one document.

[V] Indexer list on scip-code.org: C#/VB, C/C++, Dart, Go, Java/Scala/Kotlin,
PHP, Python, Ruby, Rust, TypeScript/JavaScript. The page says nothing about
incremental indexing, and the Sourcegraph post describes incremental indexing
as something SCIP enables later, not something delivered at announcement time.
[R] In practice indexers are run per project in CI, and a repository has one
index per commit, so "incremental" is mostly at the document level in the
consumer (replace the document, keep the rest).

**Precision and labels.** [R] SCIP indexers wrap real compilers or type
checkers, so results are precise for what the compiler sees (and fail or
degrade when the project does not build). Symbol identity includes package and
version, which is what makes cross-repo links possible: the reference's symbol
string equals the definition's symbol string when both indexes are known.

**Katana: TAKE** (1) readable, structured symbol strings with a scheme and a
`local` escape: a katana symbol id should be human-debuggable, for example
`lang path#descriptor`, and an explicit class of "local to file, never linked";
(2) the document-granular unit of replacement (path + content hash = the
atomic unit of update); (3) per-occurrence role bits (definition, import, read,
write), cheap and very useful to rank references; (4) `enclosing_range`-like
enclosing symbol on every occurrence, which gives the model a small, correct
excerpt boundary. **ADAPT** symbol strings: katana has no name resolution, so
its ids are "candidate" ids (qualified name as written, plus path and language)
and must be labeled as such; two different things with one id are a homonym
class, not an error (see section 11.5). **REJECT** the assumption of a build per
language: katana's value is exactly the zero-build tolerant path.

---

## 3. Kythe

Source: https://kythe.io/docs/schema/

**Data model.** [V] A graph of nodes and typed edges. Each node has a `node/kind`
fact. Identity is a "VName" of five strings: corpus, root, path, language,
signature. A file node has empty language and signature, so an anchor's VName
maps to its file's VName by dropping those two fields. Anchors tie byte offsets
(`loc/start`, exclusive `loc/end`) to semantic nodes and may overlap.

**Edge schema.** [V] `defines/binding` (anchor on the identifier to the node it
binds), `defines` (wider body span), `ref` (use), `childof` (containment),
`ref/call` with `/direct` and `/implicit` variants, plus inheritance
(`extends`), `typed`, overrides, documentation, generated-code edges.

**Cross-language.** [V] The page is brief: the `language` field separates
sources; cross-tool links are expressed with `generates` (code generation),
`imputes` (a source span maps to a generated node), `denotes`, and `satisfies`
(implicit interface satisfaction, as in Go). Protocol Buffers has per-language
rules. So Kythe's cross-language story is "make the glue an explicit edge with
its own kind", not "pretend one namespace".

**Uncertainty handled structurally.** [V] No numeric confidence. Instead:
`completedby`/`complete` facts for declaration vs definition, `*/speculative`
edges for template results that were not realized at index time, `lookup` nodes
for dependent names that cannot be resolved yet, and a stated preference rule
(complete definition over complete declaration over arbitrary node). The idea
to take: the kind of the edge IS the uncertainty label ("speculative" is a
different edge kind, not a flag nobody reads).

**Incrementality.** [R] Kythe is built as per-compilation-unit extraction plus
batch post-processing into serving tables; it is not incremental at the fact
level. Its strength is Google-scale cross-reference serving, its cost is the
need for a build-integrated extractor.

**Katana: TAKE** the idea that cross-language links are first-class edges with
their own kind (`route-key`, `event-key`, `env-key`, `generates`), and that
"speculative" is a different edge kind. **ADAPT** the five-part identity: katana
can use (project, path, language, qualified-name-as-written). **REJECT** the
compiler-extractor model and the heavy serving-table pipeline.

---

## 4. Glean

Source: https://glean.software/docs/introduction/

**Data model.** [V] Immutable facts described by user-defined schemas, forming a
DAG, stored in a RocksDB-based backend with automatic de-duplication. Glean
typically stores what clients need (definition and cross-reference locations),
not full ASTs. Schemas are user-defined; language-neutral abstractions are built
by deriving facts with the query language.

**Angle.** [V] A declarative, Datalog-like language with extensions, able to
derive new facts from rules, and (per the page) limited to non-recursive
queries.

**Incrementality, scale.** [V-] The introduction page says nothing about stacked
databases or hiding facts, and gives no latency numbers. [R] The mechanism
documented elsewhere in the Glean docs is that an incremental database is
stacked on top of a base and "hides" the facts of changed files, so queries see
base plus delta without rewriting the base; and Meta reports it indexes very
large monorepos. Treat both as leads to verify.

**Precision.** Precision is whatever the per-language indexer emits; Glean is a
store plus query layer and does not make approximate data exact.

**Katana: TAKE** the stacking principle (immutable base + small delta that
shadows by file key), which katana already has in spirit with immutable
segments; the rule "a file replaced = all facts of that file hidden" is the
cleanest correctness rule for update. **ADAPT** derived facts: katana can
precompute a few derived relations (caller-of by final name, route-key matches)
as segment-level derived tables rebuilt per segment. **REJECT** a general logic
query language in the core (cost, determinism of evaluation order, and the
model never writes Angle); keep a fixed set of named, bounded queries.

---

## 5. CodeQL and Semgrep

Sources: https://codeql.github.com/docs/codeql-overview/about-codeql/ ;
https://docs.semgrep.dev/writing-rules/data-flow/taint-mode/overview

### CodeQL

[V] A relational database is extracted per language from source; compiled
languages need the extractor to observe the normal build (each compiler
invocation feeds the extractor), interpreted ones are extracted directly.
Multi-language repositories give one database per language. Queries are in QL,
an object-oriented logic language, over classes wrapping the tables; the
database includes a data flow graph (the page does not discuss taint tracking
details). [R] Datalog-style evaluation with recursion, global data flow with
sources, sinks and sanitizers, databases built once and queried many times;
rebuild is whole-database, not incremental.

**Lesson.** Maximum precision (typed, build-faithful) with the highest cost:
a build, minutes to hours, and one database per language, so cross-language flow
is outside its model.

### Semgrep

[V] Rules are patterns over syntax trees. Taint mode: sources, sinks,
sanitizers (and propagators, a paid feature). Default analysis is within one
function; cross-function in a file and cross-file analysis are paid options,
and cross-file needs a lot of memory (the docs state at least 4 GB per core).
Reports show only one taint trace, not all. The docs list the false-positive
and false-negative levers: `exact` settings for sources, sinks and sanitizers
change what is flagged or hidden, unknown propagators and cross-file flows are
missed.

**Lesson.** Semgrep sits where katana sits: syntactic, tolerant, no full type
resolution, useful and explicit about which analysis level produced a finding.
[R] Pattern matching is metavariable-aware over a normalized AST per language.

**Katana: TAKE** from Semgrep the vocabulary: source / sink / sanitizer as
LABELS on occurrences (for example "this call is a SQL sink by name") and the
explicit "analysis level" of a finding (intra-function / intra-file /
cross-file), because it tells the consumer how far to trust it. **ADAPT** a tiny
taint relation at name level, flagged "name-level, unresolved". **REJECT**
Datalog/QL and any build-observing extraction.

---

## 6. tree-sitter, stack graphs, universal-ctags

Sources: stack graphs https://github.blog/open-source/introducing-stack-graphs/ ;
tree-sitter queries https://tree-sitter.github.io/tree-sitter/using-parsers/queries/index.html ;
ctags man page https://docs.ctags.io/en/latest/man/ctags.1.html

### tree-sitter

[V] The queries page only confirmed there is a pattern-matching language over
syntax trees. [R] The rest is from prior knowledge: a generated parser per
language producing a concrete syntax tree; incremental reparse after an edit
(you tell it the edit range, it reuses unchanged subtrees); error recovery that
keeps parsing and marks `ERROR` and `MISSING` nodes, so broken code still
yields a tree; `tags.scm` queries that name definitions and references by
capture. No name resolution at all. It is the standard substrate for tolerant
structure and is what katana's own tolerant analyzers do by hand.

### stack graphs

[V] GitHub's goal: precise navigation for all hosted code with no per-repo
configuration, no build, no CI. Each file is analyzed alone into a small graph
of definitions, references and possible name-resolution steps; at query time
the graphs of the files in a commit are merged and a path search finds a
definition for a reference, using a symbol stack to reject invalid paths. The
rules per language are graph construction rules over tree-sitter trees. Edges
can carry precedence (to model shadowing, or to show all conflicting
definitions by giving them equal precedence). The post itself lists limits:
path search per query may be wasteful, and flow-dependent or type-dependent
resolution (Python dataflow, Java inheritance and generics) is harder than the
examples shown; precise navigation launched for Python first. [R] The project
was later archived by GitHub; confirm before depending on it.

**What it covers.** Lexical scoping, imports and module resolution, qualified
names: the part of name resolution decidable from syntax and the files of the
commit. **What it cannot do.** Anything needing types (method resolution through
a receiver's type, overloads), dynamic features, generated code, and the
build's dependency resolution.

**Lesson for ambiguity.** Stack graphs make "several candidates" a first-class
result with precedence, and make resolution a query-time merge of immutable
per-file facts, so one changed file never invalidates others. That is the best
available template for a tolerant resolver.

### universal-ctags

[V] Tag file lines hold name, file, an address (line number or search pattern,
`mixed` by default), and extension fields (kind, scope, signature, typeref).
Kinds are per language; parsers can be user-defined by regex (`--regex-<LANG>`,
`--mline-regex-<LANG>`). Output is sorted by name by default, `--append` adds to
an existing tag file, and the man page itself warns that line-number addresses
go stale when files change (pattern addresses are the stale-tolerant form,
truncated at 96 characters by default). [R] ctags does no name resolution:
definitions only, and it is the baseline "good enough" navigation that works
everywhere.

**Katana: TAKE** from stack graphs: per-file immutable facts with local
scope/import resolution done at index time, cross-file candidate set built at
query time, candidates ranked by precedence and returned as a LIST with a
reason, never silently collapsed. From ctags: store a search-pattern/line-text
anchor next to a line number so stale positions can be re-found (a "re-anchor"
step). **ADAPT** tree-sitter's `ERROR`/`MISSING` idea: katana's tolerant
analyzers should record per file a parse-quality flag (clean / recovered /
text-only fallback) and expose it. **REJECT** adopting tree-sitter or
stack-graphs as dependencies (katana owns its analyzers; stack-graphs is Rust
and possibly unmaintained [R]).

---

## 7. Sourcegraph navigation and search, Zoekt, clangd, rust-analyzer

Sources: https://sourcegraph.com/docs/code-navigation/precise-code-navigation ;
https://github.com/sourcegraph/zoekt/blob/main/doc/design.md ;
https://clangd.llvm.org/design/indexing ;
https://rust-analyzer.github.io/book/contributing/architecture.html ;
https://swtch.com/~rsc/regexp/regexp4.html

### Sourcegraph precise vs search-based navigation

[V] Precise navigation uses uploaded language-specific SCIP indexes (manual,
CI, or auto-indexing); search-based navigation needs no index. Sourcegraph uses
precise results whenever available and falls back to search-based otherwise.
The page does not describe nearest-commit or staleness logic. [R] The product
shows precise vs search-based results with labels in the UI, and when the
indexed commit differs from the viewed commit it adjusts ranges using a diff
("nearest commit" with position translation). Confirm before relying on this.

**Lesson.** A tiered answer: precise if present else heuristic, and the user is
told which. This is the shape the UIOE needs, with `precision` as a field of
every result.

### Zoekt

[V] Trigram index with byte positions per trigram, so a literal is matched by
two trigrams at the right distance; regexes are reduced to literals to find
candidates then run on candidates. Index about 3x corpus size (3.5x with
metadata). Each shard is one mmap-friendly file (content, file names, posting
lists, branch masks, metadata); uint32 offsets cap content near 1 GB per
shard; one goroutine per shard, so big repos are split. Branch handling: a
bitmask per file blob marks the branches containing it, so identical content
across branches is stored once. Queries are boolean trees that need a positive
atom; negations only prune. Ranking mixes proximity, word-boundary, recency,
filename length, comment/string context, symbol-definition match, and an
optional BM25 scorer. Upgrades generate new shards and delete the old ones; a
manager polls and reindexes changed repos. [V-] Per-commit indexing is not on
that page.

### Trigram index for grep (Russ Cox)

[V] Posting lists per trigram; a regex becomes an AND/OR query over trigrams
any match must contain; the candidate files are then verified with the real
regex, so the final answer is exact. Index about 20% of source (77 MB for
420 MB). Limits: patterns with fewer than three literal characters cannot be
filtered; case-insensitive matching multiplies alternatives and cut the
speed-up from about 100x to about 10x; the indexer skips files with invalid
UTF-8, very long lines, or too many distinct trigrams; single machine. Key
principle: the index is a FILTER, the verification is exact.

### clangd

[V] Data: `Symbol`s, `Ref`s, `Relation`s, with opaque `SymbolID`s such that two
declarations of the same thing get the same id and merge into one symbol. A
`MergedIndex` layers indexes. Layers: dynamic (open files, always fresh, keeps
definitions from going stale while you edit), background (project-wide on a
thread pool, with a cached per-file `.idx` file checked before re-indexing),
optional static (built offline, loaded at startup) and remote. [V-] How
conflicts between layers are resolved and per-file hashing are not on the page;
[R] the background index keys shards by file digest and re-indexes when the
digest or compile command changed, and the dynamic layer's data for a file
overrides the background layer's data for the same file.

### rust-analyzer and Salsa

[V] Salsa is an incremental, on-demand, key-value style computation engine;
inputs are file texts and a crate graph; a global revision counter is bumped on
change and stale threads are cancelled by unwinding; core invariant: typing
inside a function body never invalidates global derived data. Syntax trees are
per-file values, may be incomplete, and carry no semantic data. The parser
never fails; it returns a tree plus errors, and analysis follows the same
pattern ("always partially usable"). Layering keeps the syntax crate
independent of everything else.

**Katana: TAKE**
1. Zoekt/csearch principle: index = filter, verification = exact on the real
   bytes at answer time. Every trigram or BM25 hit that reaches the model should
   be re-read from the file and checked against the current content.
2. clangd layering: a small always-fresh "dynamic" layer for open/recently
   changed files that overrides the background index for the same file key.
3. Salsa invariant: split per-file "signature" data (names, exports) from
   "body" data (local references), so editing a function body does not
   invalidate link tables. The UIOE should be built on per-file derived tables
   keyed by content hash.
4. "Never fail" parsers returning (result, errors).

**ADAPT** the branch bitmask to a content-hash dedupe across worktrees/copies.
**REJECT** a full Salsa-style dependency-tracked query engine (determinism is
easier with plain per-file memoized tables); the uint32 shard cap is a reminder
to bound segment size.

---

## 8. Cross-process and cross-language linking by text keys

Sources: MCP tools spec https://modelcontextprotocol.io/specification/2025-06-18/server/tools
(for the tool-schema parallel); OpenTelemetry attribute spec
https://opentelemetry.io/docs/specs/semconv/general/attributes/ ; AsyncAPI
concepts https://www.asyncapi.com/docs/concepts

Fetch results were weak here: the AsyncAPI concepts page only listed section
names (Channel, Message, Producer, Consumer, Application, Server, Protocol), and
the OpenTelemetry page only covered `service.name`, `service.namespace` and
`client.service.*`/`server.service.*`. So the details below are [R] unless said.

**API descriptions (OpenAPI).** [R] A document lists paths, methods, parameters,
schemas and operation ids. It is a CONTRACT, which makes server-to-client links
exact when both sides cite the same path and method, and it is the only place
where the parameter/response SHAPE is declared. Gaps: the contract may drift
from the code; a client can call a path built by string concatenation. Linking
quality classes: (a) contract file exists and matches both sides; (b) server
route found in code and client literal found in code; (c) client URL only
partially known (prefix, template variable).

**Messages and events (AsyncAPI).** [V] AsyncAPI's concept set includes channel,
message, producer, consumer, application, server, protocol. [R] A channel is a
named address (topic, queue), an operation says whether an application sends or
receives on it, and bindings carry protocol detail (Kafka, AMQP, MQTT). It is
the OpenAPI of events, and its channel name is the join key between producer
and consumer, exactly the katana "event key".

**Distributed tracing and service maps.** [V] OpenTelemetry names services with
`service.name` and `service.namespace`, and has client/server service
attributes so a span can record both sides. [R] A trace carries a context
header (`traceparent`) from caller to callee, so observed edges are RUNTIME
facts with real counts and latency; service maps (Jaeger, Zipkin, Tempo, cloud
APM products) are derived by aggregating parent-child spans across services.
These maps are exact for exercised paths and blind to unexercised ones. Static
text-key links are the complement: they cover unexercised paths but offer no
proof they run.

**Config and secrets as edges.** [R] Security and infrastructure tools build
graphs where an environment variable or secret name connects code (reads
`os.environ["X"]`), manifests (Kubernetes, Compose, Terraform, CI files) and
runbooks. They do this by names, and the usual failure modes are renamed
variables, defaults hidden in templating, and prefix conventions.

**Quality ladder for approximate links.** [R] synthesized from the above, with
the labels the UIOE should use:

| Class | Evidence | Label |
|---|---|---|
| contract | both sides cite the same declared operation/channel | `linked/contract` |
| exact-key | same full literal key on both sides, no contract | `linked/key-exact` |
| pattern | one side templated or prefix-only | `linked/key-pattern` |
| name-only | only an identifier name matches (env var, short event) | `linked/name-only` |
| observed | runtime trace proves it | `observed` (only if a trace source exists) |

**Katana: TAKE** the join-by-declared-key rule and the contract-over-key
priority (when an OpenAPI/AsyncAPI/.proto document exists for the key, attach it
to the edge as supporting evidence). **ADAPT** observed edges as an optional
source (a trace or log file the user already has) with its own higher rank.
**REJECT** claiming a service map; katana can state "static link candidates",
never "service A calls service B".

---

## 9. Federated search and mediators: integrating heterogeneous sources

Sources: https://en.wikipedia.org/wiki/Federated_search (surfaced by search, not
read in full); web search on connectors, permissions and rank fusion (see
section 17; only snippets were read). Depth here is limited by weak sources;
[R] marks the rest.

**Architecture.** [V, from search summaries] Federated search has two moving
parts: coordinating the queries sent to each engine (translating the query per
source) and fusing the results back. Known problems listed in that literature:
query translation, merging hit lists, and de-duplication. One patent summary
says a broker transforms the query per source, merges the results and removes
duplicates so they appear to come from one source; a broker must also map the
user's identity per source because one person may have different accounts on
different systems, and access policies can be per document or per data source.

**Result merging without comparable scores.** [V, from a library snippet] A
federation module defaulting to Reciprocal Rank Fusion with constant 60 is
cited; the point is that rank-based fusion needs no score normalization when raw
scores are not comparable across backends. [R] RRF (Cormack, Clarke and
Buettcher, SIGIR 2009) sums 1/(k + rank) over lists; it is deterministic,
parameter-light and a strong baseline. [V-] I found no source on whether to
apply permission filtering before or after fusion; [R] reasoning: filter at the
source (so ranks are computed over permitted items only), because filtering
after fusion leaks the existence of hidden items through rank gaps and counts.

**Mediator vs warehouse.** [R] Classical data integration offers two models:
virtual (query-time mediator with wrappers per source, global-as-view or
local-as-view mappings) and materialized (copy everything into one store).
Enterprise search products usually crawl and index connector output into a
central index (materialized) and fall back to live calls for sources that
cannot be crawled (permissions, freshness, rate limits). The UIOE has the same
choice per source: local files and the code index are materialized and fast;
the web, financial APIs and legal sources are virtual and slow, rate-limited,
and variable.

**Connector contract (what every connector must declare).** [R], synthesized:
1. identity and kind of source; 2. capabilities (search? fetch by id? list?
filter by date?); 3. cost class (instant / seconds / network) and rate limit
with current budget; 4. freshness model (live, crawled at T, daily snapshot)
and the timestamp of last sync; 5. permission model (for the local single-user
katana this reduces to "which roots and which credentials the user configured",
but the field must exist so credentialed web or database sources are never mixed
into an answer the user did not authorize); 6. result schema = the evidence
record of section 10; 7. failure vocabulary (timeout, rate-limited, auth,
partial, empty-certain vs empty-unknown).

The last item matters: "no results" must be split into "searched and found
nothing" and "could not search". This is the most common silent failure of
federated systems.

**Katana: TAKE** per-source query translation as a small deterministic function,
RRF as the fusion default when scores are incomparable (plus per-source weights
by source quality, section 10), source-side permission filtering, and the
explicit "searched but empty" vs "not searched" distinction. **ADAPT** the
broker to a router that records WHY each source was or was not consulted
(explainability). **REJECT** large crawl-everything connectors and learned
rankers.

---

## 10. One evidence format and how to fuse sources of unequal quality

Sources: W3C PROV overview https://www.w3.org/TR/prov-overview/ ; MCP tools
https://modelcontextprotocol.io/specification/2025-06-18/server/tools ; the
SCIP/Kythe designs above.

**What exists.**
- [V] W3C PROV: provenance is information about the entities, activities and
  people involved in producing a datum, to judge quality, reliability and
  trustworthiness. [V-] The fetched overview did not define the terms; [R] the
  model has Entity, Activity, Agent and relations such as `wasDerivedFrom`,
  `wasAttributedTo`, `generatedAtTime`.
- [V] MCP tool results can be structured (`structuredContent` conforming to an
  optional `outputSchema`, which clients should validate) or unstructured
  content items; items can carry annotations such as audience, priority and
  `lastModified`; errors split into protocol errors and tool execution errors
  (`isError`). The spec says clients must treat tool annotations as untrusted
  unless from trusted servers, servers must validate inputs, rate-limit and
  sanitize outputs, and clients should validate results before giving them to
  the model and log tool usage. MCP gives the transport envelope; it defines no
  confidence, no provenance beyond resource URIs, and no freshness contract
  beyond optional `lastModified`.
- [V] SCIP and Kythe are internal evidence formats for code: position, role,
  symbol id, language, and (Kythe) typed uncertainty edges.

**Proposed Evidence record** (a design, not a verified standard):

| Field | Meaning | Idea from |
|---|---|---|
| `id` | stable hash of (source, locator, content digest) | SCIP symbol strings, content addressing |
| `claim` | the atomic fact or excerpt, size bounded | Kythe facts |
| `source` | connector id and kind (fs, web, api, db, code, legal) | federation |
| `locator` | path+range, URL, API endpoint+params, SQL text, citation | SCIP occurrence, legal ids |
| `retrieved_at` | when the bytes were obtained | PROV `generatedAtTime` |
| `source_time` | when the source says it was valid/published/modified | MCP `lastModified`, legal in-force date, quote timestamp |
| `content_digest` | hash of the exact bytes the claim came from | clangd/Zoekt content hashing |
| `derivation` | `exact` / `derived` / `heuristic` / `inferred` + method name | Kythe speculative edges |
| `precision` | label ladder (section 8, 11) | Sourcegraph precise vs search |
| `confidence` | ordinal (`high`, `medium`, `low`), NOT a made-up probability | see below |
| `verification` | `verified-at-answer-time` / `index-only` / `unverified`, and how | trigram "index filters, regex verifies" |
| `conflicts` | ids of evidence that contradict this one | federation de-duplication |
| `scope_limits` | what was NOT searched (partial index, timeouts, rate limit) | LSP partial results |
| `rights` | permission/credential class and sensitivity | enterprise ACL |

**Why ordinal confidence.** [R] A numeric probability from heuristics is
invented precision, and a small model will over-trust a decimal. Use a short
ordinal derived by RULE from `derivation`, `verification` and age, and keep the
rule visible, so confidence is explainable: for example, `high` only if exact
AND verified against current bytes AND from a source whose quality class is
authoritative.

**Fusion across sources of different quality.** [R] design:
1. Source quality class is a static, declared property (authoritative primary
   source such as official legal text or the user's current file; secondary such
   as a reputable API aggregator; tertiary such as a general web page; derived
   such as a heuristic link). It is a prior, not a score.
2. Same claim from independent sources: merge into one evidence with several
   `supports` references, do not repeat it. Independence matters: two pages
   copying the same press release are one source (check same digest or origin).
3. Contradiction: do not average. Keep both, mark `conflicts`, order by quality
   class then `source_time`, and tell the model "sources disagree".
4. Time-sensitive facts (quotes, laws in force, versions): the newest
   `source_time` from a source of adequate class wins, with the age printed. A
   local file newer than its index entry beats the index.
5. Ranking inside the budget: RRF per source for relevance, then a quality/age
   adjustment by fixed rules, then a per-source cap so a verbose source cannot
   crowd out the rest (the small model's context is the scarce resource).
6. Evidence that fails verification is dropped or demoted with a reason, never
   silently kept.

**Katana: TAKE** the record with ordinal confidence and rule-derived labels;
`retrieved_at` vs `source_time` as two separate fields; `scope_limits` as
mandatory. **ADAPT** PROV by using only three relations (derived-from,
attributed-to, generated-at) and no RDF. **REJECT** probabilistic fusion and
learned calibrators for the core.

---

## 11. Robustness: partial index, stale data, corruption, concurrency, homonyms, uncertainty

This is the central section. Each topic lists what the studied systems do and
the rule the UIOE should follow.

### 11.1 Partial index
- LSP: a server may answer while still loading; progress notifications and
  partial-result tokens let a client tell. [V]
- clangd: queries are answered immediately from the dynamic layer (open files)
  while the background index is still building; the static index avoids
  waiting; partial coverage is the normal state. [V]
- Sourcegraph: precise when an index exists, else search-based. [V]
- Rule: every index exposes `coverage` = which roots, which tiers, which
  fraction of files are at which level (crawler tier), and every answer carries
  `scope_limits` computed from it. An empty answer from a partially built index
  is "unknown", not "none".

### 11.2 Stale data
- Git untracked cache trusts directory mtimes; the docs say it works only if the
  filesystem updates a directory's mtime when files change, and give a test
  command. [V] Git also trusts the external fsmonitor's list of changed files,
  so correctness equals the monitor's accuracy. [V] The page did not cover
  racy-git (changes within the same timestamp tick as the index write); [R] git
  handles it by treating entries whose mtime equals the index time as suspect
  and re-hashing them.
- inotify: the queue can overflow (an overflow event is generated and
  applications should rebuild their cache), watches are not recursive, rename
  pairing is inherently racy, per-user limits exist, network filesystems and
  pseudo-filesystems are not reported, identical events coalesce so counts are
  unreliable, mmap writes are not reported, and file names may be stale when
  processed. [V]
- Watchman [R]: a service that watches trees and answers "what changed since
  clock token T"; the clock token makes the question restartable, and on watcher
  loss it reports a fresh-instance flag so clients do a full scan. (The fetched
  page was only a moved-docs notice.)
- ctags: line-number addresses go stale; pattern addresses are tolerant. [V]
- LSP: `ContentModified` and stale-request policy. [V]
- Bazel remote cache: editing an input during a build can upload invalid
  results; there is a flag to guard against concurrent changes; undeclared
  environment or tool differences produce wrong cache hits; cache poisoning
  cannot be traced because entries are stored by hash. [V]
- Rule: three independent staleness defenses, in order: (1) event hints from a
  watcher (cheap, may be wrong); (2) validation by cheap stat (size + mtime,
  with a "racy window" treated as dirty) before trusting any indexed span;
  (3) a periodic full reconcile that treats the file system as truth and
  repairs the index. After a watcher overflow or restart, do (3) at once. When
  serving, verify the span against the current bytes (index filters, file
  verifies).

### 11.3 Corruption and crash safety
- Immutable segments (Zoekt shards, SCIP/LSIF files, Glean facts) are replaced
  whole; upgrades produce new shards and delete old ones. [V for Zoekt]
- Git's split index: a big immutable shared index plus a small mutable delta;
  the shared file is deleted only after an expiry age and is touched when in
  use, so a reader never loses a file it is still using. [V]
- clangd's per-file `.idx` cache is reloaded when valid and rebuilt otherwise.
  [V that it exists; the validity rule is [R]]
- Rule: write the new segment under a temp name, fsync, atomic rename, then
  publish a manifest atomically; every segment carries a format version and
  checksum; a failed checksum quarantines that segment and the affected files
  fall back to a lower tier (live grep) with `scope_limits` saying so. Never
  repair in place. Garbage-collect old segments only after no reader holds them
  (reference counted or expiry-aged, as git does).

### 11.4 Concurrency
- Salsa: global revision counter, cancel stale readers, answers computed
  against a snapshot (`Analysis` snapshot model). [V]
- Zoekt: shard-level parallelism, each shard single-threaded. [V]
- Bazel: concurrent source edits invalidate results. [V]
- Rule: readers take a snapshot of the manifest (a version number) and keep it
  for the whole answer; writers publish a new manifest; the evidence record
  stores the snapshot id. Cancel by deadline, always return an object.

### 11.5 Ambiguity of names (homonyms)
- ctags and tree-sitter tags: no resolution, so same name = same entry list. [R]
- Stack graphs: several candidate definitions are valid outputs; precedence
  decides which are shown, equal precedence shows all conflicts. [V]
- clangd/SCIP: identical declarations merge into one symbol id; different
  entities get different ids because package/scope are part of the id. [V]
- Kythe: preference rule for declarations vs definitions; `lookup` node for
  unresolved dependent names. [V]
- Rule (katana's calls are final names): a reference resolves to a CANDIDATE
  SET. Output the set size and ranking evidence (same file, same imported
  module, same package directory, same language, arity if parseable, import
  graph proximity). If the set is larger than N, say "N candidates, top ones
  are..." instead of pretending to pick one. For a very common name (`get`,
  `run`, `close`) refuse to link by name alone and say why (stoplist or
  inverse-frequency threshold).

### 11.6 Quality and labeling of approximate links
- Kythe: the kind of edge encodes it (speculative). [V]
- Semgrep: analysis level of a finding and `exact` options. [V]
- Sourcegraph: precise vs search-based. [V]
- Stack graphs: precedence. [V]
- Rule: two orthogonal labels on every link: `derivation` (how it was made:
  contract, exact-key, pattern, name-only) and `verification` (checked against
  current bytes yes/no). Never one blended score. Table in section 8.

### 11.7 Expressing uncertainty to the consumer (the small LLM)
- LSP: "result may be incomplete" for partial results. [V]
- MCP: `isError`, annotations untrusted unless from a trusted server, outputs
  must be sanitized. [V]
- Rule: put uncertainty in words the model reads in a fixed vocabulary, at the
  top of the item, not in a trailing note. Example prefix tags: `[exact,
  verified]`, `[candidate 1 of 7, name only]`, `[index only, file changed
  since]`, `[stale 14d]`, `[sources disagree]`, `[not searched: rate limited]`.
  A small model follows short tagged prefixes better than paragraphs. [R: a
  judgment, not measured here.]

---

## 12. Indexing in tiers and incremental crawlers

Sources: git-update-index https://git-scm.com/docs/git-update-index ; inotify(7)
https://man7.org/linux/man-pages/man7/inotify.7.html ; Bazel remote caching
https://bazel.build/remote/caching ; Nix https://nixos.org/guides/how-nix-works/ ;
Cox trigram note (section 7).

**Content addressing.** [V] Bazel keeps an action cache (action hash to result
metadata) and a content-addressable store for outputs; the action hash comes
from declared inputs, command line and listed environment variables, and
correctness needs reproducible actions; undeclared tool or environment
differences cause wrong hits. [V] Nix store paths embed a cryptographic hash of
the package's build dependency graph, so changing inputs yields a new path and
the old one is untouched. [R] The common idea: key derived data by the hash of
all its inputs and never mutate it; "what do I need to redo" becomes "which keys
are missing".

For katana: the key of a per-file derived record is (file content digest,
analyzer name, analyzer version, relevant config). Bumping an analyzer version
invalidates exactly its records. This also gives deduplication (the same file in
two checkouts) for free. The Bazel warning applies: an input that changes while
being analyzed must be detected (stat or hash before and after, discard on
mismatch).

**Tiers.** [R] synthesized, consistent with clangd's layers: tier 0 file list +
stat (seconds, whole tree), tier 1 text index (trigrams/BM25, fast), tier 2
tolerant parse (symbols/refs), tier 3 links (keys across files), tier 4 optional
heavier derived tables. Crawlers advance tiers in priority order (open files,
recently modified, recently queried, rest); each file records its achieved tier
and analyzer version, and coverage reports are computed from that.

**Watchers.** Use inotify-class events only as hints (11.2), with a stat-based
validator and a periodic reconcile. Scan cost control: honour ignore rules (git
ignore), cap file size, skip binary and generated files by name and by sniffing,
and bound per-file work (the trigram note lists skipping files with invalid
UTF-8, very long lines or too many distinct trigrams, a precedent for safe
skipping with a recorded reason).

**Skipping is data.** Every skipped file gets a reason code in the index
(`too-large`, `binary`, `ignored`, `analyzer-failed`, `unreadable`), so
`scope_limits` can say precisely what was left out.

**Katana: TAKE** content-digest keys, atomic publication, reasons for skipping,
reconcile-after-watcher-loss. **ADAPT** git's racy-file rule to the crawler.
**REJECT** relying on an external watcher daemon in the core (it can be an
optional plugin, per the project rule on third parties).

---

## 13. Connector patterns and what "robust" means per source

Sources: MCP spec (section 10); OWASP SQL injection
https://community.owasp.org/attacks/SQL_Injection (redirect target, not read);
P2SQL paper https://arxiv.org/pdf/2308.01990.pdf ; BIRD
https://arxiv.org/pdf/2305.03111 ; text-to-SQL benchmark
https://arxiv.org/pdf/2308.15363 ; SecureSQL
https://aclanthology.org/2024.findings-emnlp.346.pdf (the last four known only
from web search snippets). Legal and financial specifics are mostly [R]; the ELI
register page returned no content, so nothing about ELI is verified.

### 13.1 Connector patterns

| Pattern | What it gives | Main risk | Robust handling |
|---|---|---|---|
| MCP server | tool list with JSON Schema input and optional output schema, structured result, annotations | annotations untrusted unless server trusted; outputs may carry injected instructions; no freshness contract [V] | validate inputs and outputs against schemas, treat output text as DATA not instructions, log calls, timeouts, map to Evidence with `source_time` unknown unless provided |
| OpenAPI-described HTTP API [R] | typed operations, parameters, error shapes | spec drift, pagination, rate limits, auth | generate requests from the spec only, validate responses against it, record request URL and response date |
| GraphQL [R] | client chooses fields, introspection | expensive nested queries, partial data with errors side by side, field-level auth | fixed query templates, depth/cost limits, treat `errors` plus `data` as partial, never run model-written queries |
| JDBC/ODBC [R] | any SQL database through a driver | injection, destructive statements, over-broad reads, slow queries | read-only role, parameterized statements, statement allowlist, row and time limits, schema-scoped views |
| text-to-SQL [V partly] | natural-language access to tables | prompt-to-SQL injection, wrong-but-plausible results | see 13.2 |

### 13.2 Text-to-SQL risks

- [V, from search results] "Prompt-to-SQL (P2SQL)" names attacks that make an
  LLM generate malicious SQL; a study found LangChain-based web apps highly
  susceptible, including with the unmodified middleware version tested, across
  7 LLMs, and also studied defenses. A separate EMNLP 2024 Findings paper
  reports high attack success on LLM database interfaces via prompt injection
  and inference attacks, and that a chain-of-thought defense did not
  significantly improve accuracy. These are search-snippet summaries, not full
  reads.
- [V, from search results] BIRD measures execution accuracy: whether the rows
  returned equal the gold query's rows. Scores in the snippets were around 46
  to 55 percent for a GPT-4 baseline and about 61 percent for one tuned method
  on the test set. So even strong systems return a wrong result in a large
  share of hard cases, and the wrong result is valid SQL that looks plausible.
  I found no source measuring injection impact on BIRD itself.
- [R] design consequences: the SQL a model writes is untrusted input, so the
  executor must not rely on the prompt for safety. Controls that do not depend
  on the model: a read-only database account on a set of views, a parser that
  accepts only a single SELECT with an allowlisted function and table set, a
  statement timeout, a row cap, no multi-statement text. For wrong results: show
  the SQL and row count with the evidence; echo the interpretation ("I read
  `last month` as 2026-10-01..2026-10-31"); prefer curated parameterized queries
  (a catalog of named reports) over free text-to-SQL; if free SQL is used, mark
  `derivation: model-generated`, `verification: unverified`, confidence at most
  `medium`.
- The UIOE is deterministic and runs before the LLM, so the safest architecture
  is that the UIOE never lets a model write SQL for the core. Database source =
  named, parameterized query templates selected by deterministic routing, with
  free-form SQL as an explicit, labeled, human-approved mode.

### 13.3 Legal sources (what robust means)

[R] throughout; the one attempted official source (ELI register) returned no
text.
- Identity of the text: cite by a stable identifier that names the act and the
  version: the European Legislation Identifier (ELI) exists for EU and member
  states, and many jurisdictions have URN-style identifiers (for example the
  Brazilian LexML URN scheme). Store the identifier, the article/paragraph
  path, and the consolidation date.
- Version and in-force: a law has an enacted text, consolidated versions, and
  validity dates (entry into force, repeal, transitional periods); "what applied
  on date D" is a temporal query. Evidence must carry the text version, the date
  range in force, and the date asked about. Without a date the answer is
  "current as of retrieval on `retrieved_at`".
- Hierarchy and conflict: constitution, statute, regulation, local acts, case
  law; a later statute can repeal an earlier one tacitly. A deterministic router
  cannot decide legal conflicts; it must surface both texts and mark
  `conflicts`, and the answer must say it is not legal advice.
- Citation rules: quote the exact excerpt with its locator; never paraphrase as
  quote; give the official URL and retrieval date; mark secondary sources
  (commentaries, blogs) as lower class.
- "Never invent": if no legal text with an identifier is retrieved, the system
  states "no source found", and the model must not supply article numbers from
  memory. Evidence records without `locator` are not accepted for legal claims.

### 13.4 Financial sources (what robust means)

[R] throughout.
- Every number is a tuple: value, unit and currency, instrument identifier
  (ticker plus exchange, or ISIN), timestamp with time zone, provider, and kind
  (last trade, close, bid/ask, adjusted close, indicative). A bare "price" is
  not usable.
- Delay: many free feeds are delayed or end-of-day; the evidence carries
  `source_time` and the UIOE computes age; past a per-field threshold (intraday
  quote: minutes; fundamentals: until next filing) mark `stale` in the text the
  model sees.
- Corporate actions and revisions: adjusted vs unadjusted prices, restated
  financials and revised macro series change history; store the "as of"
  (vintage) when the provider offers it.
- Two providers disagreeing: keep both, label by provider, do not average.
- Rate limits and terms of use: connectors declare quotas; on exhaustion the
  answer says "not fetched: rate limited", not an older cached number presented
  as current. A cached value is allowed only with its own age.
- Arithmetic on numbers is done by deterministic tools and reported with
  inputs; the model does not compute on ungrounded figures. Not advice.

### 13.5 Web and filesystem sources

- Web: content changes; store `retrieved_at`, the final URL after redirects,
  content digest, and the page's own date if parseable (as `source_time`, low
  confidence). Pages can contain prompt injection; they are DATA. [R] Source
  quality by class, never by popularity alone.
- Filesystem: strongest freshness (it is the truth), no provenance beyond path
  and mtime, so authorship and intent are unknown; for code, link to the code
  index; for documents, extract with a parser and record extractor name and
  version.

**Katana: TAKE** parameterized named queries as the database connector, schema
validation of every connector result, per-field staleness rules, a required
locator for legal evidence. **ADAPT** MCP as the generic connector transport
with Evidence mapping on the katana side (the project already speaks MCP).
**REJECT** free-form model-written SQL in the core and any "most likely answer"
fallback when a source is silent.

---

## 14. Comparison table

Legend: Update: F = per file/document, B = batch rebuild, L = layered/overlay,
Q = on-demand memoized. Precision: P = precise (resolved), H = heuristic,
F = text filter only.

| System | Data model | Update | Precision | Uncertainty expressed | Cost | Main limit | Katana |
|---|---|---|---|---|---|---|---|
| LSP | live in-memory per server, requests | edit events, revision | P to H, server-dependent | cancel, partial, ContentModified | memory, startup | no identity, no index, no confidence | TAKE capabilities, cancellation; REJECT dependency |
| LSIF | graph vertices/edges | B | P | none | big files | numeric ids, no incrementality | REJECT (superseded) |
| SCIP | documents, occurrences, symbol strings | F (consumer side) | P (compiler-based) | roles, local vs global | build per language | needs working build | TAKE symbol grammar, roles, enclosing range |
| Kythe | typed graph, VName | B | P | edge kinds (speculative, lookup) | heavy | compiler extractors | TAKE edge-kind-as-label, cross-language edges |
| Glean | immutable facts, schemas, Angle | stacked deltas [R] | per indexer | by schema | server | non-recursive queries, ops | TAKE stacking/shadow-by-file |
| CodeQL | relational DB per language | B | P | none built in | build, minutes+ | one language per DB | REJECT; TAKE source/sink vocabulary |
| Semgrep | AST patterns, taint | on-demand | H to P (level) | analysis level, exact flags | low to high | one trace, paid depth | TAKE labels and levels |
| tree-sitter | incremental CST, queries | F, incremental reparse | H (syntax only) | ERROR/MISSING nodes [R] | low | no names | ADAPT parse-quality flag |
| stack graphs | per-file graphs, path search | F | P for lexical scope | precedence, multiple candidates | moderate | types, dynamic code | TAKE candidate sets with precedence |
| universal-ctags | tag file | append/rebuild | H (definitions) | pattern vs line address | very low | no refs | TAKE re-anchor |
| Sourcegraph nav | SCIP + search fallback | per commit upload | P else H | precise vs search-based | high | needs indexers | TAKE tiered answer with label |
| Zoekt | trigram shards, branch masks | new shards | F + verify | none | 3x size | memory/shard caps | TAKE filter-then-verify |
| csearch | trigram posting lists | rebuild | F + exact verify | none | 20% size | short patterns | TAKE principle |
| clangd | symbols, refs, merged layers | L (dynamic over background) | P | none | high memory | needs compile commands | TAKE layering |
| rust-analyzer/Salsa | query graph | Q incremental | P | partial results, error trees | memory | complexity | TAKE signature vs body split |
| OpenAPI/AsyncAPI | contract documents | file edit | exact when present | drift unknown | low | drift, dynamic URLs | TAKE contract priority |
| OTel tracing | runtime spans | streaming | exact for observed | absent = unknown | runtime overhead | unexercised paths | ADAPT as optional source |
| git index/untracked cache/fsmonitor | stat cache | per change | trust model | none | low | mtime/monitor trust | TAKE validate-then-trust |
| inotify/Watchman | change events | streaming | hint | overflow, fresh-instance [R] | low | loss, races | ADAPT as hint only |
| Bazel/Nix | content-addressed derived data | by input hash | exact if hermetic | cache poisoning risk | disk | hermeticity | TAKE digest keys |
| Federated search | wrappers + merge | per source | varies | usually none | per source | incomparable scores, ACL | ADAPT router + RRF |
| MCP | tools, schemas, structured output | live | per server | isError, untrusted annotations | per call | no freshness/confidence | ADAPT as transport |
| text-to-SQL | LLM writes SQL | live | wrong-but-plausible | none | model calls | injection, accuracy | REJECT in core; named queries |

---

## 15. The 10 most important lessons for the UIOE

1. **The index filters, the source verifies.** Zoekt, csearch and clangd's
   dynamic layer keep exactness by re-checking real bytes. Every span in an
   evidence pack must be re-read from the file (or source) at answer time, or
   carry `verification: index-only`.
2. **Approximation is a first-class, labeled answer, never a silent
   downgrade.** Sourcegraph (precise vs search-based), Kythe (speculative
   edges), Semgrep (analysis level): the label travels with the datum.
3. **Two orthogonal labels, derivation and verification, plus a rule-derived
   ordinal confidence.** No invented probabilities.
4. **Homonyms return candidate sets with reasons.** Stack graphs' precedence and
   SCIP's package-qualified ids show the two honest options: resolve with
   evidence or list all. Katana's name-only calls need a candidate count and a
   refusal threshold for very common names.
5. **Unit of update = file keyed by content digest; derived data keyed by
   (digest, analyzer, version).** Bazel and Nix show why; SCIP's document
   granularity and Glean's hide-by-file show how to update without global
   invalidation.
6. **Layer the index: immutable base plus fresh overlay.** clangd's dynamic over
   background and git's split index. Readers snapshot a manifest; writers
   publish atomically; old data is removed only when unreferenced.
7. **Watchers are hints; stat validation and periodic reconcile are truth.**
   inotify overflow, rename races and non-recursion, plus git's reliance on
   mtimes and the monitor, are the documented reasons.
8. **Coverage and skip reasons are data.** A partial index is normal. "Searched
   and empty" differs from "could not search"; every answer carries
   `scope_limits` computed from coverage and skip codes.
9. **Cross-language links are edges with their own kinds and a quality ladder**
   (contract, exact-key, pattern, name-only, observed). Contracts (OpenAPI,
   AsyncAPI, .proto) outrank bare string equality, and static links must not be
   described as proven runtime calls.
10. **For non-code sources, robust means: never invent, always cite with
    locator, mark age and version, keep disagreeing sources side by side, and
    never let a model write executable queries for the core.** One Evidence
    record for every source, with `retrieved_at` and `source_time` kept apart,
    permitted-only retrieval, and short tagged uncertainty prefixes that the
    small model reads first.

---

## 16. What these systems have that katana does not (yet): layers and functions

(Based on the task brief's description of katana, not on re-reading its code;
check each item against docs/code-intelligence.md before planning.)

Code-knowledge layer:
- Stable, structured, human-readable symbol identifiers with a global/local
  split and a package/module component (SCIP). Katana has names, not ids.
- Per-occurrence role bits and enclosing-symbol on every occurrence (SCIP).
- Candidate-set resolution with precedence and reasons, and a per-file
  import/scope resolution step that needs no build (stack graphs).
- Parse-quality flag per file (clean, recovered, text-only) (tree-sitter,
  rust-analyzer "always partially usable").
- Re-anchoring of stale positions by stored line text or pattern (ctags).
- Typed uncertainty on edges: speculative/pattern/name-only as different edge
  kinds (Kythe).
- Contract-aware linking: attaching OpenAPI/AsyncAPI/.proto documents as
  supporting evidence of a link; per-link quality class.
- Signature/body split so body edits do not invalidate link tables (Salsa).

Index lifecycle layer:
- Dynamic overlay layer that overrides the background index per file (clangd),
  with snapshot reads (manifest versions).
- Content-digest keys including analyzer version, plus a before/after stability
  check while analyzing (Bazel).
- Atomic publish, checksums, quarantine and fallback tier on corruption; old
  segment expiry tied to readers (git split index).
- Watcher overflow handling and full reconcile; racy-timestamp rule (inotify,
  git).
- Coverage report and skip-reason codes feeding `scope_limits`.
- Cancellation with mandatory response and partial-result markers (LSP).
- Capability declaration per analyzer/source (LSP).

UIOE-level layer (federation; none of it belongs to the code index):
- Router that records why each source was or was not consulted.
- Connector contract: capabilities, cost, rate budget, freshness, permissions,
  failure vocabulary including "empty-certain" vs "empty-unknown".
- Single Evidence schema with provenance (retrieved_at, source_time, digest,
  locator, derivation, verification, conflicts, scope_limits).
- Rank-based fusion (RRF) with per-source quality priors and caps; duplicate and
  independence detection; conflict surfacing instead of averaging.
- Source-specific freshness policies: quotes, legal in-force dates and
  consolidated versions, web page dates.
- Safe database connector: read-only role, single-SELECT allowlist,
  parameterized named queries, limits.
- Output sanitization and "tool output is data" handling for web and MCP
  results; audit log of every source call.
- A deterministic explanation trace (interpret, classify, route, retrieve,
  orchestrate) attached to the pack so an answer can be audited.

Open questions for later studies: (a) which parts of Glean's incremental design
(stacking, hiding) match katana's segment model, to be confirmed in Glean's own
docs; (b) legal identifier schemes for Brazil (LexML) and the EU (ELI) were not
verified; (c) Watchman's clock and fresh-instance semantics were not verified;
(d) the status of GitHub's stack-graphs project; (e) permission filtering
before vs after fusion has no verified source.

---

## 17. Source list (access date 2026-11-23 for all)

Fetched and read (possibly partially; see notes in the sections):
- LSP 3.17 specification (first 100k of about 407k characters read):
  https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
- LSIF overview: https://microsoft.github.io/language-server-protocol/overviews/lsif/overview/
- SCIP announcement: https://sourcegraph.com/blog/announcing-scip
- SCIP schema: https://raw.githubusercontent.com/sourcegraph/scip/main/scip.proto
- SCIP site: https://scip-code.org/ (redirect of https://scip.dev/)
- Kythe schema: https://kythe.io/docs/schema/
- Glean introduction: https://glean.software/docs/introduction/
- GitHub stack graphs: https://github.blog/open-source/introducing-stack-graphs/
- Sourcegraph precise navigation: https://sourcegraph.com/docs/code-navigation/precise-code-navigation
- Zoekt design: https://github.com/sourcegraph/zoekt/blob/main/doc/design.md
- Russ Cox, Regular Expression Matching with a Trigram Index: https://swtch.com/~rsc/regexp/regexp4.html
- clangd indexing design: https://clangd.llvm.org/design/indexing
- rust-analyzer architecture: https://rust-analyzer.github.io/book/contributing/architecture.html
- CodeQL overview: https://codeql.github.com/docs/codeql-overview/about-codeql/
- Semgrep taint mode: https://docs.semgrep.dev/writing-rules/data-flow/taint-mode/overview
- universal-ctags man page: https://docs.ctags.io/en/latest/man/ctags.1.html
- git-update-index: https://git-scm.com/docs/git-update-index
- inotify(7): https://man7.org/linux/man-pages/man7/inotify.7.html
- Bazel remote caching: https://bazel.build/remote/caching
- How Nix works: https://nixos.org/guides/how-nix-works/
- W3C PROV overview: https://www.w3.org/TR/prov-overview/
- MCP tools (2025-06-18): https://modelcontextprotocol.io/specification/2025-06-18/server/tools
- OpenTelemetry general attributes: https://opentelemetry.io/docs/specs/semconv/general/attributes/
- AsyncAPI concepts index page: https://www.asyncapi.com/docs/concepts

Fetched but yielded nothing useful (do not cite for the topic): Sourcegraph code
search features page (no navigation content); tree-sitter code-navigation page
(404); tree-sitter queries page (intro only); Watchman install page (moved
notice, new location https://meta.github.io/watchman/docs/install); EUR-Lex ELI
register page (empty); OWASP SQL injection page (redirected to
https://community.owasp.org/attacks/SQL_Injection, not read).

Known only from web search result summaries (not opened):
- P2SQL injection: https://arxiv.org/pdf/2308.01990.pdf
- SecureSQL, EMNLP Findings 2024: https://aclanthology.org/2024.findings-emnlp.346.pdf
- BIRD benchmark: https://arxiv.org/pdf/2305.03111
- Text-to-SQL LLM benchmark evaluation: https://arxiv.org/pdf/2308.15363
- Federated search overview: https://en.wikipedia.org/wiki/Federated_search
- A library's RRF federation module (vector search, constant 60):
  https://docs.rs/crate/llm-kernel/0.9.0/source/src/search/federation.rs
- Enterprise secure-search patents (identity mapping, merging, dedupe), for
  example https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8601028
  and https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8332430

Everything else tagged [R] (Watchman semantics, Glean stacked databases,
tree-sitter error nodes, the RRF paper, OpenAPI/GraphQL/JDBC behavior, legal and
financial practice, Brazilian LexML) comes from the author's memory and was not
checked against a source in this study.
