# Code intelligence layer: architecture decision

Status: design only (2026-11-23). Nothing in `src/` implements this. No model was run and nothing was measured to
write it. Every number is either quoted from another document (with its source) or marked **estimate**. Claims about
outside systems come from the author's knowledge; the ones checked on the web on 2026-11-23 are marked *(checked)*,
the others *(not verified)*.

Scope: this document is the **architecture decision and the critique** of the owner's proposal. The implementation
specification is being written in parallel as `docs/code-intelligence.md`; section 8 says how the two relate.

Naming note: file formats, protocols and research papers are named here because the comparison needs them. No
assistant product and no other inference engine is named, in line with the project rule. Nothing in this document
is meant to be read by the model.

Contents: 1 requirements, 2 state of the art, 3 alternatives, 4 detailed design, 5 relation to existing code,
6 plan, 7 risks and measurements, 8 reconciliation.

---

## 0. Summary of the decision

**Recommended: architecture E, a sharpened form of the layered hybrid (C).** In one paragraph:

> A universal structural index written in C, built by tolerant per-family scanners (not full parsers), stored as
> immutable memory-mapped segments of per-file facts (no B+ trees, no WAL, no SQL). Cross-file relations are
> resolved **at query time** from sorted arrays, and every relation carries a certainty grade (exact, derived,
> probable) and a provenance. Exactness beyond what scanners can give comes from **optional plugins used lazily on
> the investigation frontier** (a language server, or an imported precise index), never from a whole-repository
> precise pass. The vocabulary gap is attacked on the **query side** (glossary, cognates, one short constrained
> generation by the model), not on the corpus side (no corpus embeddings in the core). The investigation is a
> deterministic best-first search with playbooks per request type; the model is asked only **closed-set**
> questions (translate, pick among numbered candidates) and then receives an evidence pack with a token ceiling.

What differs from the owner's proposal, in order of importance:

1. **No database engine.** The index is derived data. Segments plus an atomically replaced manifest are enough.
   B+ trees, WAL and 64-bit ids everywhere are cost without a requirement behind them (4.2).
2. **No full parsers per language.** Three scanner families plus a line-rule fallback give definitions with exact
   extents, call sites, imports, guards and string keys for almost every language; precise name resolution is a
   plugin (4.3).
3. **Order of work: structure, then investigation, then (maybe) semantics.** The owner's order puts semantics second.
   The only measured facts about embeddings here are negative or slow (`docs/investigation-engine.md` 4; 112 tokens/s
   for the retrieval embedding model, owner's brief). The measured pain is the investigation.
4. **Four things the proposal does not have and the measurements ask for:** certainty and provenance on every
   edge; string keys (events, routes, configuration keys) as first-class link nodes; guard extraction and sibling
   asymmetry as deterministic diagnosis tools; and a specified evidence pack with gaps and a budget.
5. **The "Local AI Engine" is not a module of this layer.** It is the existing engine behind a five-function
   interface (4.10).
6. **The owner's own semantic search (CSSE) is adopted as the query-side bridge**, ahead of the model's expansion
   and of embeddings, with tiers and rank fusion instead of the weighted sum, grades instead of edge weights, and
   a small technical seed before any business vocabulary (4.12).
7. **Generic parts stay inside `src/coder/` for now**, written without code-specific types in their signatures,
   and are extracted when a second module kind exists (4.13).

---

## 1. Requirements

### 1.1 Who consumes the layer

| Consumer | What it can do | What it cannot do |
|---|---|---|
| The analyst model (Qwen3.6-35B-A3B) | read a short, well-marked pack and reason over it; choose among a few numbered options; follow one structural link at a time | form a search hypothesis from raw tools; translate a Portuguese request into the code's English identifiers reliably on its own; survive long tool loops (measured: reads whole files, loops, concludes from the words of the request; `docs/investigate.md` baseline: 8 calls, 238 s, wrong) |
| The user at the terminal | judge evidence, steer, pin and drop candidates | wait minutes for an index or for an answer |
| The agent runtime (`src/agent/loop.c`) | call a tool, prepend a note | depend on this layer (it must work with the layer absent) |
| Scripts and the desktop application | read JSON | parse terminal text |

Generation costs 15 to 27 tokens/s (owner's brief). A 30-token decision costs 1 to 2 s of generation plus prefill.
A 300-token free answer costs 11 to 20 s. This number shapes every choice about where the model enters.

### 1.2 Request types

| Type | Example | What a correct answer needs | Model needed? |
|---|---|---|---|
| R1 Overview | "what is this project" | languages, build and entry points, modules, the few central symbols of each, the docs headings. All facts | no (only to phrase the answer) |
| R2 Explain a flow | "how does the session list reach the phone" | an entry point, then an ordered chain of calls and of string-keyed hops (emit/handle, route/handler), each with file:line | to translate the request; to pick the entry among candidates |
| R3 Locate | "where is X" | definition with extent; if X is a description, a few candidates with the reason each matched | only when X is a description |
| R4 Change | "change the function that generates tokens" | the definition's full extent, its callers, its siblings of the same shape, the tests that reach it, the build target | to pick the target; the edit itself is the agent's |
| R5 Debug a negative | "why does X not appear / not happen" | the place where X would happen (sink), the paths that lead there, the **conditions that can stop each path**, and what differs from a sibling that works | to translate; to pick which hypotheses to deepen |
| R6 Impact | "what breaks if I change X" | reverse closure: callers, overriders, consumers of string keys, tests, build targets; probable items separated from exact ones | no |

R5 is the worst measured case and the reason this layer exists. It is also the one where plain retrieval is
weakest by construction: the cause does not look like the symptom, and often the cause is something **absent**.

### 1.3 Properties

| Property | Requirement | Why |
|---|---|---|
| Exactness is labelled | every fact and edge is `exact`, `derived` or `probable`, with provenance | a small model treats everything in its context as true; the research showed no score separates right from wrong (`docs/investigation-engine.md` 3.6) |
| Works without AI | every structural query and the pack skeleton | owner decision 2; also the only way the CLI is usable while no model is loaded |
| Language coverage | any text file gets something (lexicon, literals, outline); mainstream families get structure; precise resolution where a plugin exists | the owner's repositories mix Kotlin, JavaScript, C, QML, shell, PowerShell, Markdown |
| Latency | structural query: milliseconds. Staleness check per query: under 50 ms. Investigation before the analyst speaks: 5 to 20 s is acceptable, since today's failing loop takes 238 s (**estimate** of what is acceptable, not measured) | notebook; the model is the slow part |
| Incremental | cost proportional to what changed; an edit by the agent is visible to the next query | the agent edits and re-queries in the same request |
| Fresh by construction | no evidence line is printed without checking the cited file's size and time against the index | a stale file:line is worse than none |
| Offline, own code | no service, no external database, no SQL | owner decision 4 |
| Fits one compilation unit | plain C11, no generated parser tables of megabytes, no dependency | `src/katana.c` includes everything; agent plus tools are about 24,000 lines today |
| Scale ceiling | 100,000 files as the design ceiling; 5,000 files as the common case | the owner's target; the largest local test repository has 4,723 files |
| Portable | no POSIX-only assumption in the store | `Find` does not exist on Windows today (`src/tools/find.c`, the `_WIN32` stub) |
| Engine independence | the engine and the agent runtime compile and run without this layer | owner decision 1 |

---

## 2. State of the art

For each idea: what it is, where it helps here, what it costs, and the verdict for katana.

### 2.1 Symbol and navigation indexes

| Idea | What it is | Helps where | Cost / limit | Verdict |
|---|---|---|---|---|
| Tag files (ctags family) | per-language regular expressions or small hand parsers that emit definitions (name, file, line, kind) | R3 on names; outlines | definitions only, no references, no extents in the classic format; line-based | this is roughly today's `fd_sym`. Keep only as the fallback family |
| LSIF | a dump of language-server answers (definition, references, hover) as a graph in JSON lines, produced by a compiler-backed indexer | exact navigation without a running server | large files; produced only by per-language indexers that need a working build | idea kept: **precomputed exact edges can be imported**. Format not adopted |
| SCIP | successor of LSIF: per document, a list of occurrences (range, symbol string, role bits such as definition/import/write/read) plus symbol information and relationships (implementation, type definition); symbol strings are human-readable and stable across files | same, smaller and simpler | protobuf; still needs the per-language indexer and a working build | two ideas adopted: **occurrence + role bits** as the unit, and **stable readable symbol keys**. Import of such a file is an optional plugin (4.3.5) *(format details not re-verified)* |
| Compiler-fact graphs (the Kythe model) | nodes and edges emitted by instrumented compilers, language-neutral schema | cross-language exactness at very large scale | one extractor per compiler, hermetic builds | too heavy; the schema idea (anchor, node, typed edge) is what 4.1 uses |
| Stack graphs | name resolution expressed as per-file graphs that are stitched at query time, so indexing is file-incremental and needs no build | the property we want: **per-file facts, resolution at query time** | rules written per language in a graph DSL on top of a generated parser; hard to write; the public implementation appears archived (one registry record says so; no official notice found) *(checked, weak evidence)* | the architecture property is adopted (4.2.4); the mechanism is not |
| Code property graphs | syntax tree + control flow + data dependence in one graph, queried with a traversal language | R5: "which condition dominates this call", data flow | a real parser and a control-flow builder per language | adopted in a degraded, cheap form: **guards** (4.3.3). Full CFG and data flow: no |
| Datalog over extracted relations (the CodeQL / Glean / Doop style) | the program becomes tables of facts; queries are recursive joins | R6 closure, R5 reachability, arbitrary questions | the extractor needs the compiler; the query engine is a project in itself; a small model cannot write the queries | idea kept: **facts are flat sorted tables and a query is a join**. A fixed set of hand-coded joins (4.4), no query language |

### 2.2 Language servers

A language server answers position-based questions (definition, references, implementations, call hierarchy,
diagnostics) with compiler-grade precision. katana already has the client: `src/tools/lsp.c` (one server per command
and root, JSON-RPC with deadlines, ten operations, servers chosen by extension, override by `KATANA_LSP_<EXT>`).

Where it helps: exact answers for the languages that have a server installed. It matches the owner's rule exactly:
a third-party program, outside katana, optional.

Where it does not:

- **It is not an index.** The protocol is "open a document, ask about a position". Harvesting a whole repository
  means one `references` request per symbol: for 2 million symbols that is not a plan.
- **Start-up and memory.** A C/C++ server needs a compilation database to be right; a Rust server may index for
  minutes; servers hold from hundreds of megabytes to gigabytes *(orders of magnitude from experience, not
  measured here)*. `lsp.c` already allows 60 s for the first request.
- **Availability.** Nothing guarantees a server is installed, and several of the owner's file kinds (shell,
  PowerShell, configuration, Markdown) have none or a weak one.
- **Dynamic links** (an event name, a route string, a registry table) are invisible to it. Those are often the
  cause in R5.

Verdict: **lazy exactness on the frontier**. The own index proposes a few dozen candidates and edges; the plugin
confirms, corrects or extends exactly those, with a deadline; confirmed edges are cached as `exact` (4.3.5).

### 2.3 Retrieval

| Idea | What it is | Helps where | Cost / limit | Verdict |
|---|---|---|---|---|
| BM25 over chunks | today's `Find` | first hop when the request shares words with the code | measured: R@5 16.9% on 76 multi-hop questions; 21% of answer files share no informative word with the question (`docs/investigation-engine.md` 3.2, 1) | the ranking function stays; the **unit** changes from a 40-line chunk to a symbol (4.5.2) |
| Trigram index over the corpus (the code-search-engine design) | every 3-byte sequence maps to the files that contain it; a regular expression becomes a trigram query, then a real match on the survivors | substring and regular-expression search over very large corpora, cold cache | index size is a sizeable fraction of the source (about a fifth in the original description of the technique *(not verified)*); needed when a scan is too slow | **not in the core.** At the 100,000-file ceiling (about 1 GB of text, estimate) a parallel scan of memory-mapped files is likely sub-second warm; measure M10 before building it |
| Trigrams over the **lexicon** | index the 3-grams of the distinct identifier strings, not of the files | substring, typo, and cognate matching on names ("sess" finds `listSessions`) | tens of megabytes at the ceiling (estimate) | **adopted** (4.5.1). It is the cheap 90% of what the corpus index gives for code |
| Suffix arrays | exact substring in O(m log n) | same as corpus trigrams | several times the corpus size | no |
| Dense retrieval with a code or multilingual embedding model | one vector per unit, nearest neighbours by cosine | R3 by description; in principle the Portuguese/English gap | here: 112 tokens/s for the retrieval model (owner's brief). A 35 MB repository is about 10 million tokens (`docs/embedding-usage.md` 3.4), so a full index is about 25 hours (arithmetic). Static variants measured about 3% R@10 alone (`docs/investigation-engine.md` 4). And for R5 the cause is not similar to the symptom | **optional, last, rerank only**, exactly as `docs/embedding-usage.md` already designs; never a dependency of anything |
| Model-written summaries of every file or symbol at index time | the model describes each unit; search the descriptions | closes the vocabulary gap on the corpus side | at 15 to 27 tokens/s, a 40-token summary for each of 100,000 symbols is 40 to 70 hours (arithmetic) | no. Pay on the query, not on the corpus |
| Query expansion by the model | the model rewrites the request into the code's vocabulary | the vocabulary gap, at the cost of one short generation per request | hallucinated identifiers unless validated against the index | **adopted** with validation (4.6) |
| Co-change from version history | files changed together are related | R6 as a probable signal | measured neutral or slightly negative as a ranking signal (`docs/investigation-engine.md` 3.5) | kept as a low-weight `probable` edge only |

### 2.4 Ranked repository maps

A widely used open-source terminal pair-programming tool builds a "repository map": definitions and references
from generated parsers, a graph of files weighted by cross-references, personalized PageRank (files in the
conversation boosted), and a binary search on the number of entries to fit a token budget; the output is a compact
outline of signatures *(checked through third-party descriptions of its code, not its own documentation)*.

Helps: R1 directly; and as the standing "where am I" context for every other type. It is cheap, deterministic and
needs no model.

Limits: published use is with large models and budgets of thousands of tokens; a small model with a short
effective context needs a map of a few hundred tokens, **per request and personalized**, not a standing dump.
Verdict: **adopted**, as query Q13 with a hard token ceiling (4.4).

### 2.5 What the repair-agent literature showed

| Finding | Source | Consequence here |
|---|---|---|
| A fixed pipeline with **hierarchical localization** (files, then classes/functions shown as a skeleton of signatures, then lines) and no agent loop is competitive with agents at lower cost: 27.33% (first version) and 32.00% (revised) on SWE-bench Lite | Agentless, arXiv 2407.01489 *(checked: numbers and the three levels; the skeleton format itself not re-read)* | the engine conducts; the model picks among compact candidates level by level. This is owner decision 3, and it has support |
| The **interface** given to the model matters as much as the model: capped search results, windowed file viewing, concise feedback, guard rails on edits | SWE-agent, arXiv 2405.15793 *(not verified today)* | katana already does part of this (`loop.c` notes, file map on large reads). The pack and the tool outputs are designed as an interface, with ceilings |
| **Structure-aware search tools** (search class, search method in class, search code in method) beat text tools; context retrieved in strata | AutoCodeRover, arXiv 2404.05427 *(not verified today)* | the exact queries of 4.4 are the tools |
| A **typed graph** of files, classes and functions (contain, import, invoke, inherit) with a handful of graph tools lets **fine-tuned small models** localize well: 92.7% file-level accuracy with a fine-tuned 32B open model, with a 7B variant also reported | LocAgent, arXiv 2503.09089 *(checked: headline number and model sizes; per-model tables not read)* | the most relevant result for this owner: a mid-size model, **trained on the tool format**, with a typed graph. It supports both the graph and the owner's plan to train the model on the conventions |
| Letting the model write graph queries (a query language over a code graph) | several 2024 systems *(not verified)* | needs a model that writes a query language correctly. Not for this model: closed-set choices instead |
| Fault localization from failing tests (spectrum-based) | classic; used by AutoCodeRover | only when the project has tests that fail on the bug. Listed as a future plugin, not designed |

### 2.6 What did not work, or works only with a large model

- Free tool loops with raw text tools: measured failing here; they work with large models because the model
  supplies the hypotheses.
- Confidence thresholds on retrieval scores: measured not to transfer (`docs/investigation-engine.md` 3.6).
- Static embeddings and the large model's input table: measured useless here (same document, 4).
- Injecting a guessed answer before the model asks: 0% top-1 precision at any threshold (same, 3.6).
- Long standing repository maps and whole-file context: need long effective context.
- Model-written queries, model-written index-time summaries, multi-sample voting over patches: need speed or
  strength this machine does not have.
- Anchor jumps to the rarest question word: measured slightly harmful (same, 3.5).

---

## 3. Alternative architectures

### 3.1 The alternatives

- **A. The owner's proposal.** Own parsers for functions and classes, own symbol database with segments, journal,
  B+ trees and 64-bit ids, dependency graph, textual and semantic search, question interpreter, orchestrator,
  diagnosis, interface. Eight modules. Order: structural index, semantics, investigation.
- **B. Language servers as the source of truth** (plugins) plus a minimal own index (lexicon, outline) for
  everything a server does not cover.
- **C. Layered hybrid.** Own universal structural index (light, tolerant) + optional precision plugins + optional
  semantics + hierarchical localization with the model in the loop.
- **D. The cheapest.** Text and trigram search, co-change graph, ranked map. No structure beyond line-rule symbols.
- **E. Fact objects, lazy linking, frontier exactness** (this document's proposal). C, made specific in four ways:
  1. compiler-style storage: one immutable fact record per file ("object"), packed in segments; cross-file
     resolution is a query-time join ("link"), not a stored global graph;
  2. certainty and provenance are part of the data model, not of the presentation;
  3. precision plugins are consulted only on the frontier of an investigation and their answers are cached;
  4. intelligence is spent on the **query** (expansion, closed-set choices), never on the corpus.

### 3.2 Comparison

Qualitative grades are the author's judgement. Sizes and times are estimates unless a source is given.

| | A owner's proposal | B servers + minimal index | C layered hybrid | D text + co-change + map | **E objects + lazy link + frontier** |
|---|---|---|---|---|---|
| Exactness of definitions | high where a parser exists | exact where a server exists, low elsewhere | high (syntactic) | low (line rules) | high (syntactic, with extents) |
| Exactness of references and calls | medium: own name resolution per language is the hard 80% | exact where a server exists | medium, exact with plugin | none (word match) | medium, labelled; exact on the frontier with plugin |
| Dynamic links (events, routes, keys) | not in the proposal | invisible | depends on design | by shared rare words only | first-class (`key` entities) |
| Language coverage | as many as parsers written; each one is weeks | as many as servers installed | universal, graded | universal, shallow | universal, graded (3 families + fallback) |
| Build cost | very high: parsers + storage engine + all the rest | low for the index, medium for harvesting logic | high | low | medium-high: no parsers, no storage engine |
| Maintenance | high: every language evolves; storage engine bugs are data-loss bugs | medium: server versions, flags, time-outs | medium | low | medium: scanners are small; store is trivially rebuildable |
| Index size at 100,000 files | large: 64-bit ids, B+ tree slack, WAL | small own index; servers hold gigabytes in RAM | medium | small to medium (a corpus trigram index is the big part) | about a quarter of the source (estimate, 4.2.6) |
| Query latency | milliseconds | 0.05 to tens of seconds per request, cold start up to a minute (`lsp.c` deadlines) | milliseconds; plugin calls bounded | milliseconds | milliseconds; plugin calls bounded and cached |
| Incremental | yes, complex (in-place trees, journal replay) | servers do it themselves | yes | yes | yes, simple (new segment, new manifest) |
| Fits "own, C, no third party in the engine" | yes | core depends on third parties being present: **no** for the "works without" rule | yes | yes | yes |
| Serves R5 (debug a negative) | only if the investigation is built, which is last | call hierarchy only; no guards, no asymmetry | yes if designed in | weakly | designed for it (guards, key hops, sibling asymmetry) |
| Main risk | never reaching the investigation: the budget is spent on parsers and a database | absent or slow servers make the product inconsistent | scope creep across four layers | does not fix the measured failures | scanner precision on hard syntax (C++ templates, macros, JS dynamic code); mitigated by labels and plugins |

### 3.3 Recommendation

**E, built in the order of section 6.** Justification against the requirements:

- *Exactness labelled, works without AI, universal coverage:* only C and E give all three; A gives exactness only
  after per-language parser work; B fails "works without"; D fails exactness.
- *Fits one compilation unit, own code:* E avoids both megabyte-sized generated parser tables and a storage
  engine. Estimated size of the whole layer: 9,000 to 13,000 lines of C (**estimate**), against about 24,000 for
  agent and tools today.
- *R5:* E is the only one whose data model contains what a "why not" question needs (guards, keys, asymmetry).
- *Latency and incremental:* immutable segments make both simple.
- *Risk:* E delivers value at the end of each phase (6.2); A delivers its distinguishing value last.

D is not wasted: **phase 1 of E is D done properly** (lexicon, text search, ranked map), so the cheapest
alternative is the first deliverable, not a competitor.

**What the owner gains:** a much smaller and safer storage layer; no parser treadmill; the investigation two
phases earlier; honest labels the model can be trained on; a place for language servers and for precise index files
that respects the plugin rule.

**What the owner loses:** own precise name resolution inside the binary (overloads, templates, inheritance
dispatch stay `derived` or `probable` without a plugin); a general-purpose database he could reuse for other things;
semantic search as a core feature; and in-place update structures that would matter only far above the 100,000-file
ceiling.

---

## 4. Detailed design

Module name, decided by the owner: **coder** (other module kinds, for documents or data, may exist later; see
4.13). Source in `src/coder/` (the skeleton `types.c` and `coder.c` with the `katana coder <sub>` dispatcher is
already on the main branch), functions `coder_*`, types `Coder*`, seed data in `data/coder/`, the store in
`<folder>/.katana/coder/`. Comments in Portuguese without accents, names in English, as the rules require.

```
                        user (CLI, interactive)          agent runtime (thin adapter)
                                   |                               |
                       +-----------v-------------------------------v-----------+
                       |  interface: commands, JSON, evidence rendering  (4.9) |
                       +-----------+-------------------------------------------+
                                   |
        +--------------------------v---------------------------+        +----------------------+
        |  orchestrator: playbooks, hypotheses, budget   (4.7) |<------>| AI port (5 functions)|---> inference engine
        +-----+---------------------+--------------------+-----+        +----------------------+
              |                     |                    |
     +--------v-------+   +---------v--------+   +-------v--------+
     | interpreter 4.6|   | search      4.5  |   | evidence  4.8  |
     +--------+-------+   +---------+--------+   +-------+--------+
              |                     |                    |
        +-----v---------------------v--------------------v-----+        +----------------------+
        |  structural queries: exact joins over facts    (4.4) |<------>| precision plugins    |---> language server,
        +--------------------------+---------------------------+        | (lazy, cached) 4.3.5 |     precise index file
                                   |                                    +----------------------+
        +--------------------------v---------------------------+
        |  store: manifest + immutable segments          (4.2) |
        +--------------------------^---------------------------+
                                   |
        +--------------------------+---------------------------+
        |  scan + extract: families, tolerant            (4.3) |
        +------------------------------------------------------+
```

Everything below the orchestrator works with no model loaded.

### 4.1 Data model

#### 4.1.1 Entities

| Entity | Identity inside a snapshot | Stable key (across snapshots) | Main fields |
|---|---|---|---|
| `file` | `uint32` ordinal | path | language, family, kind flags (code, doc, test, config, generated, vendored), size, mtime, content hash (64-bit), line count, segment and offset of its fact record |
| `name` | `uint32` ordinal in the lexicon | the string | raw spelling, folded subwords, file frequency |
| `symbol` (a definition) | (`file`, `uint16` local index) packed in `uint32` where it fits, else `uint64` | hash of path + container chain + name + kind + ordinal among equals | name id, kind (function, method, type, field, constant, variable, macro, module, section, target, key-definition), container (local index), extent (first line, last line), header span (the signature), flags (exported, static, test, declaration-only), arity when visible |
| `occurrence` | implicit: position in its symbol's reference list | - | name id, role bits (call, read, write, type-use, import, inherit, declare, key-use), first line in the symbol, count; **not** one record per token |
| `key` (a string literal used as a link) | a `name` with the `key` flag | the normalized string | class (route, event, config key, environment variable, command name, message, path, table/column, other) |
| `guard` | belongs to a symbol | - | line, kind (early exit, enclosing condition, filter call, allow-list membership, feature or platform test, null/empty test, try/catch swallow), the condition's span, the names it reads, what it skips (the rest of the function, a loop item, a block) |
| `unit` (build or package) | `uint16` | path of its build file | name, kind (executable, library, package, script entry), member files or globs |
| `doc section` | a `symbol` of kind section | path + heading chain | heading text, extent |

Deliberate choices:

- **Positions are not stored per token.** A fact record keeps, for each symbol, the *set* of names it references
  with role bits and the first line. Exact lines of every use are recomputed by scanning the one file when a query
  prints them (a 10 KB file lexes in well under a millisecond, estimate). This cuts the index by a large factor
  and makes printed lines fresh by construction.
- **32-bit ordinals inside a snapshot, 64-bit hashes only as stable keys.** The owner proposed 64-bit ids. A
  snapshot at the ceiling has about 100,000 files, a few million symbols and names (estimate): 32 bits hold them,
  and halve every posting and adjacency list. Stable 64-bit keys are used where identity must survive re-indexing:
  the plugin answer cache, the session's pinned evidence, the evidence ids in a saved pack.

#### 4.1.2 Relations

| Relation | From, to | How it is produced | Stored or computed |
|---|---|---|---|
| `contains` | file or symbol to symbol | syntax | stored (container index) |
| `refers` | symbol to name, with role | syntax | stored (reference list) |
| `resolves` | (symbol, name, role) to symbol(s) | resolution rules (4.4.1) or a plugin | **computed at query time**; plugin answers cached |
| `calls` | symbol to symbol | `resolves` restricted to role call | computed |
| `imports` | file to file or to external module | syntax + path resolution | stored (target path or module string) |
| `inherits`, `implements` | type to type | syntax (`resolves` with role inherit) | computed |
| `uses-key` | symbol to key, with side (produce, consume, define, unknown) | syntax + verb table (emit/on, publish/subscribe, post/handle, set/get, register/lookup) | stored as a reference with role key-use and side bits |
| `guards` | guard to the calls and exits it dominates | syntax (4.3.3) | stored in the symbol's guard list |
| `tests` | test symbol to symbol | `calls` from a file flagged test | computed |
| `builds` | unit to file | build-file scanners | stored |
| `co-changed` | file to file | version history, optional | stored in a side table; always `probable` |
| `mentions` | doc section to name | prose scan restricted to identifier-shaped words that exist in the lexicon | stored |
| `file-link` | file to file, weighted | aggregate of unique `resolves`, `imports`, shared keys | materialized (the only global graph), for ranking |

#### 4.1.3 Certainty and provenance

Every relation instance handed to a consumer carries a grade and a source. The grade is a property of how the
fact was obtained, never of a score.

| Grade | Mark in text | Meaning | Examples |
|---|---|---|---|
| `exact` | `=` | true by construction of the syntax, or confirmed by a compiler-grade source | "function `f` is defined at a.c:10-42"; "a.c includes b.h and b.h resolves to this file"; "the name `gkatana` occurs in exactly these 2 files of the indexed set"; an edge confirmed by a language server |
| `derived` | `~` | produced by the resolution rules with a single candidate | call to `f` where exactly one visible definition named `f` exists with a compatible arity |
| `probable` | `?` | several candidates, or a heuristic | call to `open` with 14 definitions; method call through an untyped receiver; key match by string equality between an emitter and a handler; co-change; cognate match; similarity |

Provenance is a small enum plus a version: `scan:<family>@<version>`, `resolve:<rule>`, `plugin:lsp:<server>`,
`plugin:index-file`, `history`, `expand:model`, `expand:glossary`, `expand:cognate`, `embed`. The scanner version
is part of each fact record, so a scanner fix invalidates only that family's records.

Two rules protect the meaning of `exact`:

1. An **absence** ("no reference to X anywhere") is `exact` only relative to the indexed set, and the pack states
   the set's limits in its GAPS section (skipped directories, files over the size cap, families without call
   extraction, unresolved dynamic calls).
2. A path through the graph has the grade of its weakest edge.

### 4.2 Storage

#### 4.2.1 Position

The index is **derived, rebuildable, read-mostly data**. That single fact removes most of what a database engine
exists for:

| Proposed element | Verdict | Reason |
|---|---|---|
| manifest, catalog, numbered segments for incremental indexing | **keep** | this is the right shape (the same as every log-structured search index) |
| versioned serialization, no `fwrite` of a struct | **keep** | explicit little-endian fields, varints, magic, version, checksum per section |
| inverted index, adjacency lists, hash indexes | **keep** | as immutable sorted arrays and open-addressing tables inside each segment |
| journal / WAL | **drop** | a WAL protects data that exists nowhere else. Here the source files are the truth; after a crash the worst case is re-scanning the files of one unfinished segment. Atomic replace of the manifest gives consistency |
| B+ trees | **drop** | they pay for in-place updates and range scans on a mutable set. Segments are immutable: a sorted array with binary search (or a hash table) is smaller, faster and has no rebalancing code to get wrong. A B+ tree would be justified only if a single segment's dictionary no longer fits a memory map, far above the ceiling |
| 64-bit ids everywhere | **change** | 32-bit ordinals per snapshot, 64-bit stable keys where identity must persist (4.1.1) |
| separate `symbols`, `references`, `graph`, `search` stores | **change** | one segment file with sections; per-file fact records are the primary data and the sections are indexes over them |
| FTS5 (SQLite) for text search | **do not use** | see 4.2.7 |

#### 4.2.2 Layout on disk

Location: `<folder>/.katana/coder/` with the same fallback rule the caches use today (`~/.katana/cache`,
`KATANA_CACHE_DIR`; see `src/agent/prefix_cache.c`), and a `.gitignore` that ignores everything.

```
.katana/coder/
  manifest            current snapshot: generation, scanner versions, list of live segments, file table
  manifest.lock       advisory lock for the single writer
  seg-000017.kci      immutable segment
  seg-000018.kci
  link-000042.kci     derived global tables for generation 42 (file-link graph, ranks); rebuildable
  plugin.kcc          append-only cache of plugin answers keyed by stable keys + content hashes
  history.kch         optional co-change side table
```

**Manifest** (small; rewritten whole on each update: write to a temporary name, flush, rename):

```
magic, format version, generation (uint64), root path hash, checksum
scanner versions per family
segments[]: id, byte length, checksum, live file count, dead file count
files[] sorted by path: path, size, mtime, content hash, language, flags, segment id, record offset
skipped[]: path prefix or file, reason (ignored, binary, too large, unreadable)      <- feeds the GAPS section
```

At the ceiling the file table is about 100,000 entries of roughly 80 bytes with prefix-compressed paths: about
8 MB (estimate). If rewriting 8 MB per update ever matters, the file table moves into a segment of its own and the
manifest keeps only the segment list; not needed at the start.

**Segment** (immutable, memory-mapped, read without copying):

```
header: magic, version, section table (kind, offset, length, checksum)
S1 file records        one per file: symbols[], per-symbol reference list, guards[], imports[], keys[], outline
S2 lexicon             distinct names of this segment, sorted; raw strings in a pool; flags; file frequency
S3 lexicon hash        open addressing: hash(name) -> lexicon ordinal
S4 name postings       per name: sorted file ordinals, delta + varint; role-union bits per (name, file)
S5 definition table    (name ordinal, file ordinal, symbol index, kind) sorted by name
S6 subword postings    folded subword -> lexicon ordinals            (search)
S7 lexicon trigrams    3-gram -> lexicon ordinals                    (substring, typo, cognate)
S8 field statistics    per symbol: field lengths for ranking; per subword: document frequency
S9 doc and comment terms  term -> (file, symbol) with field bits     (prose side of the ranking)
```

Varints are the usual 7-bit continuation bytes; postings are delta-coded and split in blocks of 128 with a skip
entry, so intersection can skip. Nothing more elaborate is justified before measuring.

#### 4.2.3 Incremental update

```
update(root):
  take manifest.lock (non-blocking; if busy: skip, see 4.2.5)
  list files (git ls-files when a repository, else walk with ignore rules)      <- reuse of Find's discovery
  diff against manifest.files by (size, mtime); for candidates, compare content hash
  changed + new files  -> scan (in parallel) -> one new segment
  deleted + changed    -> their old records become dead (the manifest no longer points to them)
  write new manifest (generation + 1), rename over the old one
  if segments > 8 or dead ratio > 30%: merge the smallest segments into one (same code path as a build)
  rebuild link-<generation> if file-link inputs changed (incremental: only rows of changed files and of files
     that referred to names whose definition set changed)
  release lock; delete segments no manifest references (on Windows: on next start, if still mapped)
```

A query reads across live segments and merges, as any segmented index does. With at most 8 segments the merge
cost is negligible against the model's time.

**Edits made by the agent** do not wait for a sweep: the `Edit`, `Write` and `MultiEdit` tools call `coder_touch(path)`,
which re-scans that one file into an in-memory overlay segment for the process. The overlay is flushed as a real
segment at the next update.

**Freshness without a sweep per query:** a full `stat` sweep of 100,000 files is too slow to run on every query
(tens to hundreds of milliseconds, estimate). So: one sweep at session start, one in the background at most every
few seconds while idle, `coder_touch` for own edits, and **verification at print time**: before any evidence line is
shown, the cited file's size and mtime are compared with the manifest; on mismatch that file is re-scanned on the
spot and the line recomputed. No stale file:line can be printed.

#### 4.2.4 Linking at query time

There is no stored global symbol graph. "Who calls `f`" is:

```
defs   = S5 lookup of name f across segments                      -> candidate definitions
files  = S4 postings of name f with the call role bit             -> files that may call it
for each file (ranked, capped): read its fact record, keep symbols whose reference list has (f, call)
resolve each (caller, f) against defs with the rules of 4.4.1     -> grade per edge
```

This is a join over sorted arrays and costs milliseconds for any single symbol (estimate). It is what makes
indexing strictly per file, with no global invalidation when one file changes. The price: whole-graph algorithms
need a materialized graph, and exactly one is kept, at **file** granularity (`link-<generation>`: a CSR of
file-to-file weights by relation type, plus a rank per file and the best symbols per file). It is the input of the
ranked map and of spreading in search, and it is small (4.2.6).

#### 4.2.5 Concurrency and failure

| Situation | Behaviour |
|---|---|
| Several readers | each maps the segments of the manifest it read: snapshot isolation by immutability |
| Two processes want to update | one holds `manifest.lock`; the other keeps its snapshot and covers its own dirty files with the in-memory overlay. Nobody blocks |
| Reader holds an old segment that was merged away | on POSIX the unlinked file stays valid while mapped; on Windows deletion is retried later |
| Crash while writing a segment | the manifest never pointed to it; the orphan file is deleted at the next update |
| Crash while writing the manifest | the rename did not happen: the old manifest is intact |
| Corrupt section (checksum) | that segment is dropped and its files re-scanned; never an error to the model |
| Format or scanner version change | old records of the affected families are ignored and rebuilt lazily, hottest files first |
| Index directory not writable | index in memory for the session, say so once on stderr |

#### 4.2.6 Size and build time (estimates, to be measured: M8)

Assumptions for the ceiling: 100,000 files, 10 KB average (1 GB of text), 20 symbols per file, 15 distinct
referenced names per symbol, 1.5 million distinct names.

| Part | Arithmetic | Size |
|---|---|---|
| file table | 100,000 x 80 B | 8 MB |
| symbol records | 2,000,000 x 24 B | 48 MB |
| reference lists | 30,000,000 entries x 2.5 B | 75 MB |
| guards, imports, keys | assumed a third of the reference lists | 25 MB |
| lexicon strings + hash | 1,500,000 x (18 + 8) B | 39 MB |
| name postings | 10,000,000 (name, file) pairs x 2 B | 20 MB |
| definition table | 2,000,000 x 8 B | 16 MB |
| subword and trigram postings over the lexicon | 1,500,000 x (4 + 15) entries x 1.5 B | 43 MB |
| prose terms | assumed | 20 MB |
| file-link graph | 3,000,000 edges x 9 B | 27 MB |
| **total** | | **about 320 MB, a third of the source** |

For the common case (5,000 files) the same arithmetic gives about 16 MB, the size class of today's `Find` index
(18.5 MB for 5,349 files, `docs/tools.md` as quoted in `docs/embedding-usage.md`). Everything is memory-mapped, so
resident memory is what queries touch.

Build time: a tolerant scanner should process on the order of 100 MB/s per core (estimate; today's Python
prototype is no guide). With 8 cores the ceiling case is tens of seconds dominated by reading the files; the
common case is about a second. Both numbers are guesses until M8.

#### 4.2.7 FTS5: do not use

1. It is SQL and a third-party engine inside the binary: two of the owner's rules at once. The amalgamation is an
   order of magnitude more code than the whole of katana's agent and tools *(size not re-verified)*.
2. Its tokenizers are for natural language. Code needs identifier splitting, exact identifiers, roles and scopes;
   a custom tokenizer would be written anyway, and the role and symbol structure does not fit a document/term
   model.
3. What it provides is ranked text search over documents, which is precisely what was measured insufficient here
   (R@5 16.9%). The bottleneck is vocabulary and structure, not the quality of the text engine.
4. Its strengths (crash safety, incremental merge, maturity) answer problems this design removes by making the
   index derived and segmented.

What is lost by not using it: a debugged merge policy and phrase/proximity queries for free. Phrase search stays
with the existing `Grep` scan.

### 4.3 Scanners and extractors

#### 4.3.1 Strategy: families, not parsers

A full parser per language is the largest cost in the owner's proposal and the least necessary. What the
investigation needs from syntax is: where definitions begin and end, which names each definition uses and how,
which conditions can stop it, and which strings link it to other code. That is recoverable from **lexing plus
block structure** for almost every language.

| Family | Languages (examples) | Block structure from | Declaration recognition | Fidelity |
|---|---|---|---|---|
| F1 brace | C, C++, Java, C#, JavaScript, TypeScript, Go, Rust, Kotlin, Swift, PHP, Dart, Scala, QML, CSS-like | matching `{}`, `()`, `[]` after a real lexer (comments, strings, raw and template strings, character literals, preprocessor lines, regular-expression literals in JS) | the **header**: the token run between the previous `;`, `{` or `}` and an opening `{` (or a terminating `;` for prototypes). A per-language table classifies headers (keywords, position of the name, parameter list) | definitions and extents high; C++ templates and macros that hide braces degrade to `probable` extents |
| F2 indent | Python, YAML-with-code, Nim-like | indentation stack; continuation lines and brackets handled by the lexer | `def`, `class`, decorators, assignments at module and class level | high |
| F3 keyword-end | Ruby, Lua, shell, PowerShell, Pascal-like, Elixir | opener and closer keyword tables | `function`, `def`, `module`, `class`, shell `name() {` | medium; when the stack does not balance, fall back to F5 for that file |
| F4 data and markup | Markdown, JSON, YAML, TOML, XML/HTML, SQL, CMake, Make, Dockerfile, package and project files | headings, keys, tags, targets | sections, keys (as `key` entities), targets, dependencies (as `unit` and `imports`) | high for what it claims |
| F5 fallback | anything else that is text | none | today's line rules (`fd_sym` in `src/tools/find.c`) | low; symbols without extents; marked as such |

Language identification: extension table, then shebang, then a few content probes; one table, no dependency.
Unknown text files still get lexicon, keys and prose terms.

One lexer core with per-language switches (comment and string syntax, keyword set) serves all families. The
per-language part is a **table** of a few dozen lines, not a parser.

#### 4.3.2 What a scanner emits per file

```
outline    : symbols with kind, name, container, extent, header span, flags
references : per symbol, the set of (name, role bits, first line, count)
             role from local syntax: name followed by "(" -> call; after "new" or in a type position -> type-use;
             left of "=" or with "++" -> write; in an import/include/use/require -> import; after extends/implements/":" in a
             type header -> inherit
receivers  : for a call a.b.c(), the chain is kept ("a.b" as receiver text) to help resolution and plugins
imports    : raw target and, when it resolves to a file of the index, that file
keys       : string literals that look like identifiers, routes, dotted keys, environment names, file paths, or that are
             arguments of a verb in the produce/consume table; with side bits
guards     : see 4.3.3
prose      : comment and docstring terms attached to the nearest symbol; headings for doc files
shape      : a compact signature of each function for similarity (4.4, Q11)
```

#### 4.3.3 Guards

A guard is anything in a function that can make the rest not happen. Recognized from block structure and
keywords, no control-flow graph:

| Kind | Pattern (family F1; equivalents in F2, F3) | Recorded |
|---|---|---|
| early exit | `if (C) return/throw/continue/break/goto ...;` or `if (C) { ... exit }` where the block's last statement is an exit | condition span, exit kind, what is skipped |
| enclosing condition | a call or exit located inside `if (C) {` / `else` / `case` / `while` | the chain of enclosing conditions for each recorded call |
| filter | call of a filtering verb (`filter`, `where`, `grep`, `find`, `some`, `includes`, `contains`, `has`, `startsWith`, `matches`) in the expression that feeds a loop or a result | the predicate span |
| allow-list membership | a condition that tests membership of a value in a named collection; and the collection's literal definition if it is in the index | the collection name and its literal members |
| feature, platform, configuration | a condition that reads a `key`, an environment variable, a build flag (`#if`, `#ifdef`) | the key |
| silent failure | `catch` or `rescue` whose block neither rethrows nor logs; an ignored return of a call whose name suggests failure reporting | the block |

A guard record names the identifiers its condition reads. That is the bridge to the next hop of a "why not"
investigation: who writes those identifiers.

#### 4.3.4 How scanners fail

- Unbalanced blocks (macros, generated code, unusual syntax): the file keeps F5 symbols and full lexicon, keys and
  prose; its record is flagged `degraded` and that flag reaches the GAPS section when the file is evidence.
- A header that no rule classifies: the block is kept anonymous (its references still belong to the enclosing
  symbol).
- Files over a size cap or that look generated or minified: lexicon only; listed in `skipped`.
- A scanner never aborts indexing; a per-file time cap ends the scan and flags the record.

#### 4.3.5 Precision plugins (optional, lazy, cached)

A plugin implements one small interface and is consulted **only for the entities on the frontier** of a query or
investigation:

```
typedef struct CoderPrecise {
    const char *name;
    int (*available)(const char *root, int language);                 /* barato; nao inicia nada */
    int (*definition)(CoderLoc use, CoderLoc *out, int cap, int deadline_ms);
    int (*references)(CoderLoc def, CoderLoc *out, int cap, int deadline_ms);
    int (*callers)(CoderLoc def, CoderLoc *out, int cap, int deadline_ms);
    int (*callees)(CoderLoc def, CoderLoc *out, int cap, int deadline_ms);
    int (*implementations)(CoderLoc def, CoderLoc *out, int cap, int deadline_ms);
} CoderPrecise;
```

- **Language-server plugin:** an adapter over the transport that already exists in `src/tools/lsp.c`
  (`lsp_request`, server table, deadlines). The server is a third-party program outside katana: exactly the
  owner's plugin rule.
- **Precise-index-file plugin (later, optional):** reads a precomputed occurrence index produced by an external
  indexer during the project's build, if the user has one. No server process, no latency; needs a small reader.
- **Use:** when a query result has `derived` or `probable` edges and a plugin is available for the language, the
  top N edges (N around 20) are sent for confirmation with a total deadline (default 3 s when a server is already
  running, none started just for a query unless the user or the playbook asks). Confirmed edges become `exact`,
  refuted ones are dropped, extra ones are added. Answers are cached in `plugin.kcc` keyed by the stable symbol
  key and the content hashes of the files involved, so a second investigation in the same code pays nothing.
- **Never required:** with no plugin the same queries return, with weaker grades and an entry in GAPS.

### 4.4 Structural queries

#### 4.4.1 Resolution rules (the "link" step)

For a reference (`caller`, `name`, `role`, `receiver text`):

```
1. candidates = definitions named `name` whose kind fits the role (call -> function/method/macro/constructor; inherit -> type)
2. keep candidates visible from the caller's file: same file; a file it imports (transitively through re-export tables
   where the family records them); same package or directory for languages with package scope; any, for C-like
   linkage when a declaration of that name is visible
3. if the receiver text is "this"/"self" or absent inside a type: prefer members of the enclosing type and of its
   declared bases
4. drop candidates whose visible arity cannot accept the call's argument count (when both are known)
5. one candidate left      -> derived
   several                 -> probable, all listed, ordered by: same file, same directory, import distance, rank
   none                    -> unresolved (external, dynamic, or outside the index): a GAP, not an error
6. a plugin answer replaces the result -> exact
```

Names used in more than max(30, 2%) of the files or defined in more than 3 places never create **file-link**
edges (the hub rule already validated in `src/tools/findgraph.c`), but they are still resolved when asked for.

#### 4.4.2 The queries

All are available as CLI commands, as tool operations and as functions; all return JSON and a compact text form;
every item has a grade.

| # | Query | Input | Output |
|---|---|---|---|
| Q1 | `outline` | file | the tree of symbols with kind, extent, signature line |
| Q2 | `symbol` | name or pattern, optional kind and path | definitions: `path:first-last kind signature`, container chain |
| Q3 | `def` | a use (path:line[:col]) or a name | the definition(s) it resolves to, with grade |
| Q4 | `refs` | a symbol | uses grouped by file and by enclosing symbol, with role and the source line; total counts; grade per group |
| Q5 | `callers` / `callees` | a symbol, depth (default 1, max 3), cap | a tree, each edge with grade and the call line; string-key hops shown as such |
| Q6 | `hierarchy` | a type | bases, derived types, overriding and overridden members |
| Q7 | `guards` | a symbol, optionally a target call or line in it | the guards that dominate the target (or all), condition text, what each skips, and the names each condition reads |
| Q8 | `writers` / `readers` | a name (field, variable, collection, key) | symbols that write it and symbols that read it |
| Q9 | `key` | a string key | producers, consumers and definitions of that key, by side |
| Q10 | `path` | two symbols or files | the shortest chains between them over calls, imports and key hops, each with the grade of its weakest edge |
| Q11 | `similar` | a symbol | siblings by shape: same container kind and base, same arity, same name pattern (shared prefix or suffix subwords), overlapping callee set; with a side-by-side diff of callee sets and guards |
| Q12 | `asymmetry` | two names (one that works, one that does not), or one name and its siblings | places where the first occurs and the second does not, grouped by symbol and ranked by role: collection literal, `case` label, registration call, configuration key, guard condition first |
| Q13 | `map` | optional focus (files, symbols, a query), token ceiling | ranked outline: modules, then files, then signatures, cut to the ceiling by binary search on the number of entries |
| Q14 | `impact` | a symbol or a set of changed lines | reverse closure by depth: callers, overriders, key consumers, tests, build units; exact, derived and probable in separate groups; co-change partners last |
| Q15 | `entry` | none or a unit | entry points: `main`-like functions, exported commands, route and handler tables, build units, scripts named in project files |
| Q16 | `imports` | a file or unit | what it imports and what imports it; cycles |
| Q17 | `unused` / `absent` | a name | "defined and never referenced", or "referenced and never defined", within the indexed set, with the set's limits |
| Q18 | `slice` | path:line | the enclosing symbol, the guards above the line, the names the line reads and their writers: one screen of "what must be true for this line to run" |

Q7, Q8, Q11, Q12 and Q18 do not exist in the owner's list and are the ones aimed at R5. Q12 deserves emphasis: the
first question of the bench ("`gclaude` works, `gkatana` does not appear", `docs/investigation-engine.md` 3.7) is
answered structurally by "where does the working name occur and the failing one not": no retrieval, no model.
Whether that generalizes is measurement M4.

### 4.5 Search

#### 4.5.1 Channels

| Channel | Finds | Mechanism | Grade of a hit |
|---|---|---|---|
| exact name | an identifier, a path fragment, a quoted string that is a key | lexicon hash, path table | exact |
| subword | names containing the query's subwords (`session` finds `listSessions`, `SESSION_MAX`) | subword postings | exact (the name does contain it) |
| lexicon trigram | substrings, typos, truncated words, cognates after folding | trigram postings over names, then verification | probable |
| ranked fields | a description in the code's vocabulary | BM25 with fields, unit = symbol | probable |
| concept and operation | a description in the user's vocabulary: entity + action ("atualizar estoque") | the knowledge base of 4.12: concept terms and verb classes mapped onto name subwords by position (verb first in a function name) | probable, with the base row as evidence |
| literal text | a message, a regular expression | parallel scan (the existing `Grep` machinery), pre-filtered by the lexicon when the pattern contains an identifier-shaped run | exact |
| spreading | things linked to what the other channels found | the measured activation (2 rounds, k winners) over the typed file-link graph | probable, with the linking edge as evidence |
| meaning (optional) | reorder of a pool | the design of `docs/embedding-usage.md`, unchanged in rules; unit becomes the symbol card | probable; off by default |

#### 4.5.2 The unit becomes the symbol

Today's unit is a 40-line window with stride 25. The new unit is a **symbol card**: a function, a type, a section,
or (for files with no symbols) a block separated by blank lines. Fields and starting weights:

| Field | Content | Weight (starting value, to tune) |
|---|---|---|
| name | the symbol's subwords | 4 |
| path | directory and file subwords | 3 |
| container and signature | enclosing names, parameter names and types | 2 |
| prose | attached comment or docstring terms | 2 |
| keys | string keys used | 2 |
| body names | referenced names' subwords | 1 |

The request-word filter (measured: R@5 15.9% to 23.6%) and the fold and stem rules of `src/tools/find.c` carry
over unchanged. Whether the symbol unit beats the window is measurement M3; the argument for it is that the
result is directly a definition with an extent, which every later step needs anyway.

#### 4.5.3 Fusion

Reciprocal rank fusion across channels, with the reasons already written in `docs/embedding-usage.md` 4.1 (no
score scale to calibrate). Exact-name and exact-key hits are not fused: they are listed first, as facts. Each
candidate keeps the list of channels and terms that produced it; that list is its evidence line.

### 4.6 The interpreter

Input: the request text. Output: a typed request, anchors, and an expanded term set. It runs in three stages, the
third only when needed.

**Stage 1: rules (microseconds, no model).**

- Request type by cue tables in Portuguese and English ("por que nao", "why does not", "where", "onde", "o que
  acontece se eu mudar", "explain", "como funciona", "what is this project", imperative edit verbs), with R3 as the
  default. The type only selects a playbook; a wrong type costs efficiency, not correctness.
- Anchors, strongest first: `path:line` and stack-trace lines; quoted strings (looked up as keys and as literal
  text); words that are exact entries of the lexicon or of the path table; identifier-shaped words not in the
  lexicon (a likely typo or an **absent** thing, which is itself a finding for R5).
- For R5: split into the **expected effect** and, when present, the **working sibling** ("X works, Y does not").
- Informative terms: what is left after the request-word and stop-word filters.
- Vocabulary gap `G`: the fraction of informative terms absent from the index (the definition of
  `docs/embedding-usage.md` section 2).

**Stage 2: bridges without a model (microseconds).**

- **Knowledge base (the owner's CSSE, assessed in 4.12):** concepts with their terms in Portuguese, English and
  code style, and the operation taxonomy by verb (arquivo/file, lista/list, tela/screen, enviar/send, salvar/save,
  senha/password, fila/queue, ...), loaded from text files in `data/coder/kb/`, not compiled in. A proposal is kept
  only if it exists in this repository's lexicon.
- **Cognates:** fold accents, map a short table of suffixes (-cao/-tion, -dade/-ty, -mente/-ly, -avel/-able,
  -ario/-ary, -encia/-ence), then match by shared prefix of at least 5 letters through the lexicon trigrams
  (sessao/session, conexao/connection, autenticacao/authentication, mensagem/message).
- Every bridged term is `probable`, weighted below a literal hit, and named in the evidence ("matched `session`,
  from `sessao` by cognate").

How much of the gap these two close is unknown and is the cheapest decisive measurement of all (M1: offline, no
model, on the existing hop slice).

**Stage 3: expansion by the model (one short generation; only if `G` is still high, or no anchor survived, and
a model is available).**

```
system : fixed text, cached as a prefix: "You map a request about a code base to search terms. Answer JSON only."
user   : the request + the repository's vocabulary card (below)
grammar: {"en": [up to 8 lowercase words], "ids": [up to 6 identifier guesses], "pick": [up to 8 numbers]}
limits : max 60 output tokens, temperature 0, deadline 6 s
```

The **vocabulary card** is what makes this safe for a small model: a numbered list of the repository's most
salient subwords and keys (by rarity times spread, grouped by top-level directory; about 150 entries, about 500
tokens, estimate), built without a model and cached with the index. `pick` is a closed-set choice from that list;
`en` and `ids` are free guesses.

Safeguards:

1. Every free term is **validated against the lexicon**: exact, then subword, then trigram neighbour. A term with
   no counterpart is dropped and counted; it never reaches the search.
2. Terms present in more than 15% of the files are dropped (the informative-term rule of the research).
3. Expanded terms weigh half of original terms that hit, and the evidence line says which request word they came
   from and by which route (`expand:model`).
4. The expansion can add candidates; it can never remove one that literal terms produced.
5. Cached by (request text, index generation, model fingerprint). Deterministic settings, so tests and curriculum
   generation can replay it.
6. Budget: one call per request. If the deadline passes or the JSON is empty, the investigation continues with
   stages 1 and 2 and says so in GAPS ("request terms not found in code: ...").
7. The same call returns nothing else: no hypothesis, no answer, no file name. Its output is never shown to the
   analyst as fact.

Cost (arithmetic from the owner's speed): 60 tokens at 15 to 27 tokens/s is 2 to 4 s, plus the prefill of about
600 tokens whose speed depends on the path (35 to 5000 tokens/s, owner's brief; about 10 s at the 60 tokens/s of
`docs/tools.md` when nothing is cached). The card and the system text are
a fixed prefix per repository, so the existing prefix cache (`src/agent/prefix_cache.c`) should make the prefill
mostly free after the first request; that is assumption A1, to be checked by M6.

### 4.7 The orchestrator

#### 4.7.1 Core loop

The investigation is a best-first search over entities with typed expansions, a list of hypotheses, and a budget.
It is ordinary code; the model is a function it may call.

```
struct Investigation {
    type; anchors; terms;
    frontier   : priority queue of (entity, reason, score, depth)
    evidence   : list of (id, entity or relation, grade, provenance, lines)
    hypotheses : list of (statement template, supporting evidence ids, contradicting ids, state)
    gaps       : list of (kind, detail)
    budget     : { wall_ms, model_calls, model_tokens, plugin_ms, pack_tokens }
}

run(inv):
    seed(inv)                                   # playbook-specific: anchors -> entities
    while budget left and frontier not empty and not playbook.done(inv):
        e = frontier.pop()
        for step in playbook.expansions(e):     # Q-queries of 4.4, each cheap
            results = step.query(e)
            record evidence; derive hypotheses; push new frontier items with scores
        if playbook.wants_choice(inv):          # 4.7.3
            ask the model (closed set), reorder or prune the frontier
        if frontier items have derived/probable edges and a plugin is available: confirm top ones (4.3.5)
    pack = assemble(inv)                        # 4.8
```

Scores are structural and local: grade of the edge that led here, depth, rank of the file, whether the entity
touches an anchor, whether it is a test or generated file. No global confidence is computed or claimed.

#### 4.7.2 Playbooks

| Type | Seed | Expansions (in order) | Done when | Model calls |
|---|---|---|---|---|
| R1 overview | none | `entry`, `map` (ceiling about 600 tokens), units from build files, docs headings (via the existing `docsel`), language shares | always one pass | 0 |
| R3 locate | anchors, else search | `symbol`; if several, `outline` of the top files to give context | one exact hit, or top 8 candidates assembled | 0, or 1 choice when the candidates do not fit |
| R2 explain | entry candidates by search + `entry` | forward: `callees` depth up to 3, breadth capped by rank; `key` hops (producer to consumers); stop at sinks (I/O, UI, network verbs) | a chain from an entry to a sink exists, or depth exhausted | 1 expansion if `G` high; 1 choice of entry |
| R4 change | as R3 | the definition's full extent; `refs` (counts + top 5); `similar`; `tests`; unit and its test command from the brief (`src/agent/brief.c`) | assembled | 0 or 1 |
| R5 debug a negative | two ends: **sink** candidates (where the effect would happen) and **source** candidates (where it starts), plus the sibling if named | from the sink, backward: `callers`; `slice` on the call line; `guards` on each function of the path; for each guard, `writers` of the names its condition reads; `key` hops; `asymmetry` between the working and failing names; `absent` on anchors that are not in the lexicon; `similar` to the working implementation with a diff | every path from a source to the sink has its guards listed, or budget spent | 1 expansion if `G` high; up to 2 choices |
| R6 impact | the symbol or changed lines | `impact` to depth 3 | assembled | 0 |

Hypotheses in R5 are generated from templates, each tied to evidence, never free text:

| Template | Raised by |
|---|---|
| "`<name>` is missing from `<collection or switch>` at `<loc>` where `<sibling>` is present" | `asymmetry` |
| "the path stops at `<loc>` when `<condition>`" | `guards` |
| "`<key>` is consumed at `<loc>` and never produced" (or the reverse) | `key` |
| "`<symbol>` is defined and never called on this path" | `callers` returning nothing |
| "`<name>` from the request does not exist in the code" | `absent` |
| "the failure is swallowed at `<loc>`" | silent-failure guard |
| "`<sibling>` does `<calls>`; `<target>` does not" | `similar` diff |

The analyst model ranks and tests these; the engine does not choose a winner. This respects the measured result
that the engine must not inject an answer (`docs/investigation-engine.md` 3.6) while still giving the model what it
could not build itself: the hypotheses.

#### 4.7.3 The model in the loop: when it pays

One closed-set choice costs: the prefill of the new candidates (about 8 cards of 40 to 60 tokens, so 300 to 500
tokens) plus about 20 generated tokens (1 to 1.5 s at the owner's speeds). The reading speed is the unknown that
dominates: `docs/tools.md` puts it at about 60 tokens/s, and the owner's brief gives 35 to 5000 tokens/s depending on
the path. At 60 tokens/s a choice is 6 to 10 s, nearly all of it reading; at a few hundred tokens/s it is 2 to 4 s
(**arithmetic**; M6 fixes it). The design therefore treats a model call as expensive: cards are kept to one or
two lines, and the first rule of the table below is "do not ask".

| Situation | Ask the model? |
|---|---|
| A unique exact anchor resolved | no |
| All candidates fit in the pack | no: include them all and let the analyst read them |
| More candidates than fit, of similar structural score | **yes**: "which of these numbered items relate to the request" (grammar: a list of numbers) |
| The frontier has more branches than the budget can expand | **yes**: "which to follow next" (grammar: one or two numbers) |
| The request shares no term with the code after stage 2 | **yes**: expansion (4.6) |
| Deciding whether a hypothesis is true | no: that is the analyst's job, with the pack in front of it |
| Writing a query, a plan, or free text inside the loop | never |

Default budget: 1 expansion (only for terms the knowledge base of 4.12 does not cover) + 1 choice, 15 s wall
time, 3 s of plugin time; a second choice only if M6 shows a call costs under about 4 s. With zero model calls the
playbook still completes; choices are replaced by the structural order.

**Cheaper choice (to be verified, M7):** a choice among up to about 10 labelled options can be read from the
next-token distribution after a single forward pass, without generating: the engine compares the probabilities of
the label tokens. That would make a rerank cost one prefill and no generation. It needs the AI port to expose
`score_labels` (4.10). Whether this model's label probabilities rank as well as its generated answer is unknown.

### 4.8 The evidence pack

The pack is the contract between the engine and the analyst. It is the same object in three renderings: compact
text for the model, a readable form for the terminal, JSON for programs.

#### 4.8.1 Text form for the model

```
[katana evidence | type: debug-negative | index: 4723 files, checked now | 2140 of 2400 tokens]
REQUEST   why does gkatana not appear on the phone when gclaude does
TERMS     gkatana = name in 2 files | gclaude = name in 10 files | "phone" ~ no code term (bridged: none)

FACTS
E1 = bridge/sessions.mjs:41-78  function describeSessions(list)
     52|   if (!KNOWN.includes(s.cmd)) continue;                  guard: skips this item
E2 = bridge/sessions.mjs:12     const KNOWN = ["gclaude", "claude"]      collection literal
E3 = bridge/server.mjs:210-233  function onListSessions(req)     calls E1 at 221
E4 ~ linux/install.sh:88        defines command gkatana          (derived: shell function header)

LINKS
E3 -> E1  = call (same module import)
E1 uses E2 = read at 52

HYPOTHESES (from structure; not verified)
H1 ? gkatana is missing from KNOWN (E2) where gclaude is present; E1:52 skips items not in KNOWN
H2 ? no handler registers the key "sessions" for gkatana (searched: bridge/, linux/)

GAPS
G1 calls through `handlers[name]` in bridge/server.mjs:140 are dynamic: targets unknown
G2 not indexed: android/build/ (ignored), 3 files over 300 KB
G3 no precision plugin for JavaScript in this session: call edges are name-based

NEXT  Coder refs E2 | Coder guards E3 | Coder open E1 | Coder asymmetry gclaude gkatana
```

(The example's code lines are illustrative; they are not quotes of any repository.)

Rules:

- **Three grades, three marks** (`=`, `~`, `?`), in a fixed column. FACTS holds only `=` and `~`; anything `?` is
  under HYPOTHESES or carries its mark on a LINK.
- **Ids are stable inside a session** (`E7`, `H2`, `G1`), so the model, the user and the tools refer to the same
  thing, and the final answer can be checked: every `file:line` the analyst cites must be in the pack or in
  something it read afterwards (rule already specified in `docs/investigation-engine.md` 7.4, item 4).
- **Snippets are the smallest useful lines:** the signature line and the lines that are evidence (a guard, a call,
  a collection literal), with their numbers. Never a whole function unless it is under about 15 lines.
- **GAPS is mandatory**, even when empty ("none known"). It lists what was not indexed, what was dynamic, what was
  cut by the budget, which request terms found nothing, and which grades would improve with a plugin.
- **No number that looks like a probability.** No "confidence", no percentages, no similarity unless the optional
  meaning channel produced the item, and then in the wording fixed by `docs/embedding-usage.md` 4.2.
- **NEXT** lists up to four follow-up operations that the engine judges most informative, as ready tool calls.

#### 4.8.2 Token ceiling

Default 1,500 tokens for the agent pre-pack and 2,400 for the interactive interface, where the user chose to wait
(`KATANA_CODER_PACK_TOKENS`), counted with the real tokenizer through the AI port when a model is loaded, else
characters / 3.5. The pack is read by the model before it writes anything: at about 60 tokens/s (`docs/tools.md`)
1,500 tokens are about 25 s and 2,400 about 40 s (arithmetic). The example above shows the larger form. Allocation when over the ceiling: header and REQUEST fixed; FACTS 55%; LINKS
10%; HYPOTHESES 20%; GAPS 10%; NEXT 5%. Trimming order: fewer snippet lines per fact, then fewer facts (lowest
structural score first, never one cited by a kept hypothesis), then fewer hypotheses. What was trimmed becomes a
GAP line ("14 more candidates not shown: Coder more").

Both values are starting guesses: 1,500 holds about 7 facts with snippets, 2,400 about 12; both are small against
the baseline's failed run (11,914 input tokens summed over 8 calls, `docs/investigate.md`). The pack pays for itself
only if it replaces at least one or two exploratory rounds; that is what M5 measures.

#### 4.8.3 JSON form

```
{"version": 1, "type": "debug-negative", "index": {"generation": 42, "files": 4723, "checked": true},
 "request": "...", "terms": [{"text": "gkatana", "kind": "name", "files": 2, "route": "literal"}],
 "evidence": [{"id": "E1", "grade": "exact", "source": "scan:brace@3", "path": "...", "first": 41, "last": 78,
               "kind": "function", "name": "describeSessions", "lines": [{"n": 52, "text": "...", "note": "guard"}]}],
 "links": [{"from": "E3", "to": "E1", "relation": "calls", "grade": "exact", "source": "plugin:lsp"}],
 "hypotheses": [{"id": "H1", "template": "missing-from-collection", "supports": ["E1", "E2"], "state": "open"}],
 "gaps": [{"id": "G1", "kind": "dynamic-call", "path": "...", "line": 140}],
 "next": [{"op": "refs", "target": "E2"}], "budget": {"tokens": 2140, "ceiling": 2400, "model_calls": 1, "ms": 7400}}
```

### 4.9 Interface with the user

Command group `katana coder` (every command accepts `--json` and `--cwd`):

| Command | Does |
|---|---|
| `katana coder index [path]` | build or update; prints files scanned, per-family counts, degraded and skipped files, time, size |
| `katana coder status` | generation, freshness, segments, families, plugins available, what is skipped and why |
| `katana coder map [--focus X] [--tokens N]` | Q13 |
| `katana coder symbol|def|refs|callers|callees|guards|writers|readers|key|path|similar|asymmetry|impact|entry|imports|absent|slice|outline ...` | Q1 to Q18, one line per item: grade mark, `path:line`, kind, signature or source line |
| `katana coder search "words"` | 4.5, with the channel and terms behind each candidate |
| `katana coder ask "question" [--no-ai] [--pack-only] [--tokens N]` | interpreter + orchestrator; prints the progress of the investigation, then the pack; unless `--pack-only`, hands the pack to the analyst and prints its answer with the cited evidence ids resolved to `path:line` |
| `katana coder` (no arguments) | interactive investigation session |

Interactive session: the investigation state stays alive between lines.

```
coder> why does gkatana not appear on the phone when gclaude does
  [interpret] type debug-negative; names: gkatana (2 files), gclaude (10 files)
  [search]    sink candidates: 5      [trace] 3 paths, 4 guards      [compare] asymmetry: 2 places
  [pack]      9 facts, 2 hypotheses, 3 gaps, 2140 tokens
  ... the pack, in the readable form ...
coder> /open E1            show the source around an evidence item
coder> /expand E3 callers  run a query from an item and add the result to the pack
coder> /drop E4   /pin E2  steer what the analyst will see
coder> /why H1             show the chain of evidence behind a hypothesis
coder> /gaps               what was not looked at, and how to close each gap (plugin, path, budget)
coder> /ask                send the current pack to the analyst
coder> /json  /save file   export
```

Readable form: the same sections as 4.8.1, with colour for the grade (the terminal layer in
`src/agent/terminal.c` already handles colour and width), source lines printed with line numbers, and one line per
stage while the investigation runs so the wait is explained. The desktop application consumes the same JSON as a
stream of stage events; nothing in this layer knows about it.

### 4.10 The AI port (narrow interface to the inference engine)

The layer sees the engine only through this structure. The engine includes nothing of the layer.

```
typedef struct CoderAi {
    void *ctx;
    int (*ready)(void *ctx);                                   /* ha modelo carregado? */
    int (*count_tokens)(void *ctx, const char *text, size_t n);
    /* geracao curta, temperatura 0, presa a uma gramatica JSON; devolve bytes escritos ou <0 (prazo, erro) */
    int (*generate_json)(void *ctx, const char *prefix_key, const char *system, const char *user,
                         const char *grammar, int max_tokens, int deadline_ms, char *out, size_t cap);
    /* opcional: probabilidade de cada rotulo como proximo token, sem gerar (uma passada) */
    int (*score_labels)(void *ctx, const char *prefix_key, const char *system, const char *user,
                        const char *const *labels, int n, float *prob);
    /* opcional: vetor de significado; NULL quando nao ha modelo de embeddings */
    int (*embed)(void *ctx, const char *text, size_t n, float *vec, int dim, int deadline_ms);
} CoderAi;
```

- `prefix_key` lets the engine reuse a cached prefix (the vocabulary card, the system text).
- The grammar is the JSON grammar machinery that already constrains tool-call values (`src/agent/callgrammar.c`
  refers to the engine's JSON grammar in `src/engine/forward.c`).
- When `ready` is false every caller has a model-free path. `katana coder` commands other than the final analyst
  answer never load a model on their own initiative.
- The analyst's final answer is **not** part of this port: it is an ordinary agent turn (4.11) or, in the CLI, an
  ordinary one-shot prompt.

### 4.11 Integration with the agent (thin adapters)

Three adapters, each small, each removable:

1. **Tool.** One tool with an `op` enum, in the style of the existing `LSP` tool, because a small model chooses
   badly among many similar tools. The tool name the model was trained on is `Find` (`docs/curriculum.md`, family
   `x-find6`); the adapter keeps `Find` working with its current two modes mapped to `search` and `refs`, and adds
   the operations (`def`, `callers`, `callees`, `guards`, `key`, `similar`, `asymmetry`, `impact`, `map`, `more`,
   `open`) under the same tool or under a new name `Coder`, to be decided together with the training plan. Output
   uses the pack's line format and grade marks, with the existing character budget.
2. **Pre-pack.** For requests the interpreter classifies as R2, R5 or R6 with at least one anchor or bridged term,
   the runtime calls the orchestrator before the first model turn and prepends the pack as a `[katana: ...]` note,
   at the place where `cd_note` is prepended today (`src/agent/loop.c`). This reverses the "no unrequested seed"
   decision of `docs/embedding-usage.md` (D26), on purpose and narrowly: that decision was about injecting a
   *guessed answer*; the pack injects *graded facts, open hypotheses and gaps*. It stays behind a switch
   (`KATANA_CODER_PACK`, default off until M5 says it helps) and is never produced for R1/R3 requests that the
   tools answer directly.
3. **Per-result note.** The neighbour note (`fg_neighbor_note`, called in `src/agent/loop.c` after `Read`, `Grep`
   and `Find`) keeps its place and its limits (3 items, 8 notes per request) but its content comes from typed
   edges with grades: "callers not read yet", "guard at line N reads `x`, written in ...".

Also: `coder_touch` from the editing tools (4.2.3); the citation check before the final answer; and, for the
curriculum, new conversation families that show the model reading a pack, citing evidence ids, testing a
hypothesis with one operation, and saying "not found in the indexed set" when GAPS says so. The model must be
trained on this format before the pre-pack is turned on by default; the format in 4.8.1 is therefore to be frozen
early (phase 2) even while its content grows.

### 4.12 The owner's own semantic search (CSSE) and its knowledge base

The owner's proposal, received while this document was being written: a semantic search with **no model and no
embeddings in the core**. Five mechanisms (lexical; conceptual, by a dictionary of concepts with Portuguese,
English and code-style synonyms and weights; structural; behavioural, by a taxonomy of operations per verb, queried
as entity + operation; relational, by a graph with a weight per edge kind), own morphological normalization, an
inverted index with BM25 from term to symbol with field weights, the score
`0.20 L + 0.25 C + 0.20 B + 0.25 G + 0.10 S` with illustrative weights, and a dictionary that grows from the
code's names and from hand-written domain files. All patterns are to be registered in a knowledge base.

**Verdict: adopt it as the query-side bridge of this architecture, with three changes** (ranking by fusion and
tiers instead of the weighted sum; edge weights replaced by grades; a small seed first). It is the same bet this
document already made in 4.6 (spend on the query, not on the corpus), made larger, inspectable and owned.

#### 4.12.1 Against the alternatives

| | CSSE (concept + operation base) | Language server | Retrieval embeddings | Hierarchical localization with the model | Ranked map |
|---|---|---|---|---|---|
| Attacks the vocabulary gap | **yes, directly**, for what the base covers | no | in principle yes | yes (the model translates) | no |
| Cost per query | microseconds, zero tokens | 0.05 s to tens of seconds | here 112 tokens/s: unusable without a full prior index | 6 to 10 s per call at 60 tokens/s (4.7.3) | milliseconds |
| Cost per corpus | none (base is per language and domain, not per repository) | server indexing | about 25 hours for a 35 MB repository (2.3) | none | none |
| Deterministic, explainable | **yes**: the evidence is a row of the base | yes | no: a cosine | partly | yes |
| Works offline, no GPU, no model loaded | yes | yes if installed | no | no | yes |
| Structural exactness | none: it finds where to start | **exact** | none | none | none |
| Covers code whose names say nothing (`process`, `handle`, `run`) | no | not applicable | sometimes (reads the body) | sometimes | no |
| Covers a domain nobody registered | **no** | not applicable | yes | yes | not applicable |
| Fails how | silently narrower recall; confident wrong pair matches | time-out, absent | plausible wrong neighbour, no explanation | wrong pick, slow | shows central code, not relevant code |

They are not competitors. CSSE and the embeddings compete for one job (the first hop from the user's words);
the language server and the map do other jobs. For that one job CSSE is the right first choice on this machine:
it costs nothing per query, and its failures are visible and fixable by editing a text file. The model's
expansion is the fallback for what the base lacks; embeddings stay last and optional.

#### 4.12.2 Where it wins and where it loses

Wins: determinism (tests and the curriculum can replay it byte for byte, which neither the model nor a cached
embedding can promise); explainability ("`estoque` -> concept INVENTORY -> `stock` in the function name" is a line
the pack can print and the user can dispute); zero tokens and zero latency; offline, no GPU; entity + operation
queries, which plain BM25 cannot express.

Loses:

- **Coverage.** A business dictionary (approval, payment, refund, inventory) covers none of the owner's own
  projects. The concepts of an inference engine, a bridge and an audio application are in no general list
  (4.12.5). Uncovered words fail silently unless reported, so reporting them is mandatory.
- **Maintenance.** "Register all the patterns" is unbounded work with no finish line. It is a permanent cost that
  a trained model does not have. It must be made cheap (text, provenance, a `gaps` list fed by real requests) and
  it must stop being a precondition: the system has to be useful with a small base.
- **Ambiguity.** `order` (purchase or sort), `set` (verb or collection), `post` (HTTP or publish), `check`,
  `record`, `token` (lexer, language model, authentication: all three exist in the owner's code). Position in the
  identifier helps for verb against noun; it does not separate two nouns.
- **Language.** Trying Portuguese and English rules on the same word creates collisions (`data`, `time`, `pasta`,
  `nota`). A normal form should be accepted only when it exists in the base or in the repository's lexicon.
- **It is still vocabulary.** The research found that 21% of answer files share no informative word with the
  question, and that learned token association added nothing (`docs/investigation-engine.md` 3.3). A curated
  dictionary is a different thing from mined association, but the claim that it closes the gap is unmeasured, and
  for R5 the cause often shares no *concept* with the symptom either. That part is the job of the structural
  playbook (guards, asymmetry), not of any search.

#### 4.12.3 How it combines with the rest

```
request
  -> rules: type, anchors (exact names, paths, quoted text)                              facts
  -> CSSE: normalization, concepts, operations -> terms that exist in this lexicon       probable, evidence = base row
  -> words still uncovered?  -> cognate rule; then ONE model expansion for those words   probable, evidence = route
       -> what the lexicon confirms is proposed as a new base row (provenance "model", pending review)
  -> search channels (4.5) over symbol cards, fused
  -> structural expansion from the hits (4.4, 4.7): exact from here on
  -> optional: embeddings reorder the pool, only if the above is measured insufficient
```

- CSSE's five mechanisms map onto this design without a second engine: lexical = the ranked-fields channel;
  conceptual and behavioural = the concept and operation channel of 4.5.1; structural = the symbol card and its
  kind; relational = the spreading channel and, after the first hop, the exact queries.
- The **operation taxonomy has a second use** that matters more for R5 than search does: classifying what a
  function *does* from the verbs of the calls in its body (writes, sends, deletes, notifies). That turns "who
  updates X" into a structural query (Q8 with an operation filter) and labels guards and sinks in the playbooks.
- The model's expansion becomes rarer as the base grows, which is the right direction for a slow model. Rows it
  proposes are never trusted automatically: they enter as `model`, weigh less, and wait for review.
- The naming-pattern part of the base (`*Handler`, `on*`, publish-shaped and subscribe-shaped calls) is what the
  `key` entities of 4.1 need for their produce/consume side. This document's "verb table" is that file.

#### 4.12.4 The score formula

`Score = 0.20 L + 0.25 C + 0.20 B + 0.25 G + 0.10 S` is a reasonable sketch and the wrong final form, for five
reasons:

1. **Normalization to 0..1 destroys comparability.** Dividing a component by the query's maximum gives 1.0 to the
   best candidate whether it is a perfect match or a faint one. The research tried exactly this shape
   (`bm25_norm + mu * cos_norm`): weight 0.25 tied, 0.5 and 1.0 hurt (`docs/investigation-engine.md` 4).
2. **Absent components are not zero.** When the request has no recognized concept, a quarter of the scale is
   dead and the remaining weights no longer mean what they meant.
3. **G is not independent.** The graph is seeded from the other components, so it amplifies a wrong seed, and its
   agreement with them carries no information (measured: same document, 3.6).
4. **A sum hides the conjunction.** The value of the behavioural search is entity AND operation. In a sum, a
   strong lexical score alone outranks a complete pair match.
5. **Five weights cannot be calibrated here.** The only labelled set is 76 questions over 3 repositories; the
   research's learned ranker, trained leave-one-repository-out, was neutral to modest. Weights fitted on that will
   not transfer, and the owner ruled out measurement campaigns.

Proposed instead, needing no calibration:

- **Tiers before scores.** Tier 1: exact anchors (a name, a path, a quoted key). Tier 2: complete concept +
  operation pair on a symbol's name. Tier 3: everything else. A lower tier never outranks a higher one.
- **Inside a tier, reciprocal rank fusion** of the channels that produced candidates for this query. Rank fusion
  has no scale to normalize, and an absent channel simply does not vote.
- **G only reorders and annotates; it never introduces a candidate into the top without a mark** ("via ..."), as
  `Find` does today.
- **Edge weights (1.00 / 0.95 / 0.95 / 0.70 / 0.30) become the grades of 4.1.3**, not numbers: direct call
  resolved = exact or derived; interface or event by string = probable; same module or domain = not an edge, a
  prior. A number such as 0.95 looks measured and is not.
- The owner's weighted sum stays available as a mode for experiments, with its components printed per candidate
  in the CLI and JSON, and **never shown to the model** (no number that reads as a probability, 4.8.1).

How to choose between modes without a campaign: one offline run of the existing bench per mode (M12), no model;
if the two modes differ by less than the bench's noise (a few questions), keep fusion because it has no
parameters to maintain.

#### 4.12.5 The smallest base that already gives value

Order of work for the owner's projects (an inference engine in C; a bridge in Node, Kotlin and Go; an audio
application in C++ and QML). Sizes are proposals, not measured needs.

| Order | Layer | Content | Why first |
|---|---|---|---|
| 1 | Normalization rules | accent folding; camel, Pascal, snake, kebab; Portuguese and English suffix rules; protected forms for the known collisions | everything else depends on it; most already exists in `find.c` |
| 2 | Operation taxonomy, technical verbs | about 15 classes with Portuguese and English verbs: create, read, update, delete, send, receive, list, register, start, stop, load, save, parse, render, connect, wait, retry, validate, fail | domain-independent; serves search **and** the body classification used by R5 |
| 3 | Request-language core | about 150 Portuguese words that users type and code names in English: arquivo, pasta, lista, tela, botao, janela, mensagem, erro, fila, sessao, usuario, senha, conexao, rede, configuracao, memoria, tempo, tamanho, ... | this is the measured gap (Portuguese requests over English code) and it is the same for every project |
| 4 | Software-engineering concepts | cache, buffer, queue, thread, lock, token, stream, socket, session, channel, event, callback, plugin, config, log, test, build, install | every one of the owner's projects uses these; the business list uses none |
| 5 | Naming and wiring patterns | handler, listener, callback, `on*`, emit/on, publish/subscribe, route registration, signal and slot, command tables | feeds `key` sides and entry-point detection |
| 6 | Abbreviations | cfg, ctx, msg, req, res, buf, len, idx, tmp, fn, cb, kv, tok, attn, vk | cheap; code-style synonyms |
| 7 | Domain file: inference | model, weights, tensor, layer, expert, quantization, sampling, prefill, context window, grammar, shader, GPU buffer, tokenizer, embedding | katana itself; a per-project file, not the seed |
| 8 | Domain file: bridge and phone | session, pairing, device, notification, permission, launcher, service, intent, socket, command, approval | the bridge and the phone application |
| 9 | Domain file: audio and interface | track, clip, mixer, bus, sample rate, buffer size, plugin host, latency, playback, transport, meter, QML item, property, signal | the audio application |
| last | Business concepts | approval, cancellation, payment, refund, inventory, order, invoice | the examples of the proposal; none of the three projects needs them now |

Layers 1 to 3 are the minimum that can move the measured number (M1 measures exactly that). Layers 7 to 9 should
be **project files that live in each repository** (`.katana/coder/kb/` or a tracked folder), not in katana's seed.

Growth from the code: mining names is safe only as **proposals with provenance** (the research measured no gain
from learned association); a mined row never overrides a curated one and is off until reviewed. The reliable
source of growth is the `gaps` list: request words that reached nothing, collected from real use.

### 4.13 What is generic and what is specific to code

Other module kinds (documents, data) may come. What this design contains, split by reuse:

| Part | Generic | Specific to code |
|---|---|---|
| Store | segments, manifest swap, section table, checksums, versioned serialization, varint postings, file mapping and locking, overlay, compaction | the content of a file's fact record |
| Scan | file discovery, ignore rules, change detection by size, time and hash, parallel scan driver | language families, lexers, declaration tables |
| Text | folding, splitting, stemming, stop and request words, the knowledge-base loader with layers and provenance, the lexicon with subwords and trigrams | camel and snake splitting, the operation taxonomy read from call verbs |
| Search | inverted index with fields, BM25, rank fusion, tiers, evidence per candidate | symbol cards, roles, the concept position rule in identifiers |
| Graph | CSR storage, rank, spreading with winners | reference resolution, call edges, guards, keys, asymmetry |
| Investigation | budget, frontier loop, hypothesis and gap records, the pack with grades, ids, GAPS, ceiling and three renderings, the interactive session shell, the AI port | playbooks, hypothesis templates, the structural queries |
| Plugins | the lazy-confirm-and-cache pattern | the language-server adapter |

Recommendation: **do not create a common directory now; write the generic parts as if it existed.** Concretely:

- Inside `src/coder/`, the generic files (`store.c`, `postings.c`, `text.c`, `kb.c`, `rank.c`, `pack.c`, `shell.c`,
  `ai.c`) take no code-specific type in their signatures: they deal in opaque record bytes, term ids, entity ids and
  a callback table. That discipline costs almost nothing while writing and is what makes extraction a file move.
- Extraction to a shared folder happens when the second module kind is started, and is shaped by what that module
  actually needs. One example is not enough to know the right boundary: a documents module will want sections and
  citations where code has symbols and calls, and guessing its needs now would freeze the wrong interface.
- Two things are worth making common from the first day because they are already shared in spirit with existing
  code: the cache directory rule (`src/agent/prefix_cache.c`) and the tokenization helpers of `src/tools/find.c`.
  They move once, not twice.

The cost of this choice: one refactoring later (estimate: 1 to 2 agent-days if the signature rule was kept). The
cost of the opposite choice is designing a framework for a client that does not exist yet.

---

## 5. Relation to what exists

| Existing piece | File | Fate | Detail |
|---|---|---|---|
| File discovery (git listing, ignore rules, size and binary filters) | `src/tools/find.c` (`fd_update`, `fd_skip_rel`, `fd_is_binary`) | **survives** | moves under the scanner; the 20,000-file cap (`fd_max_files`) goes away |
| Fold, stem, identifier split, stop words | `src/tools/find.c` (`fd_fold`, `fd_stem`, `fd_tokens`) | **survives** | becomes the subword and prose tokenizer |
| Request-word filter | `src/tools/findgraph.c` (`fg_drop_request`, `fg_is_request`) | **survives** | measured gain; used by the interpreter |
| BM25 with fields | `src/tools/find.c` (`fd_rank`) | **survives as a function**, unit changes | 40-line chunks replaced by symbol cards |
| Line-rule symbols | `src/tools/find.c` (`fd_sym`) | **demoted** | family F5 only |
| Sparse activation, hub rule, k winners | `src/tools/findgraph.c` (`fg_activate`) | **survives as the spreading channel** | over typed file-link edges instead of `sym`/`path`/`rare`/`co` |
| `sym` and `path` edges | `src/tools/findgraph.c` (`fg_build`) | **replaced** | by resolved references and imports, with grades |
| `rare` edges | same | **replaced** | by `key` entities and lexicon lookups |
| `co` edges | same | **survives, low weight, probable** | side table; measured neutral to negative |
| Index file `find-<hash>.idx`, whole-file rewrite, full graph rebuild on change | `fd_save`, `fd_load`, `fg_build` | **discarded** | replaced by segments and query-time linking |
| `Find` output shape, `why:` line | `fd_show`, `fg_evidence` | **survives as a rendering** | fed by the new search; keeps the trained format |
| `mode=refs` | `fd_refs` | **replaced** by Q4 | same output shape at first |
| Neighbour note | `fg_neighbor_note` + call in `src/agent/loop.c` | **survives** as adapter 3 | new content |
| Documentation line selection | `src/agent/docsel.c` | **survives unchanged** | called by the R1 playbook; doc sections also become symbols |
| Project brief (build, test, layout) | `src/agent/brief.c` | **survives unchanged** | read by `entry`, by R4 (test command) and by the map |
| Language-server client | `src/tools/lsp.c` | **survives twice** | still a tool; its transport also backs the precision plugin |
| Embedding usage rules | `docs/embedding-usage.md` | **survive as the rules of the optional meaning channel** | except D26 for the pre-pack, as argued in 4.11 |
| Research bench | `tools/findbench/`, `tools/investigate/` | **survives** | the instrument for section 7 |
| Loop notes (plan, repetition, stuck) | `src/agent/loop.c` | **untouched** | |

Migration, with `Find` never broken:

1. The new module is built next to `Find`; `katana coder` commands are the only way to reach it.
2. When symbol-card search exists, `Find mode=search` gets a switch (`KATANA_FIND_BACKEND=code`) and both back ends
   are compared on the existing bench (M3).
3. When the switch wins or ties, it becomes the default, `mode=refs` moves to Q4, and `find.c`'s index code and
   `findgraph.c`'s builder are deleted. The tool name and output shape stay until the model is retrained.
4. Windows gets the capability for the first time with the new store (file mapping and locking behind two small
   functions), instead of porting the old one.

---

## 6. Plan

Efforts are **estimates in agent-days** for implementation, assuming the conventions of this repository (scripts
of replacement with asserts, one compilation unit). They do not include training the model.

### 6.1 Phases

| Phase | Deliverable | Done when | Effort |
|---|---|---|---|
| P1 Store, scan, lexicon | discovery, family detection, lexer core, segments and manifest, incremental update, overlay, lexicon with subwords and trigrams, text and name search, `katana coder index/status/symbol/search` | a 5,000-file folder indexes and updates incrementally; a killed indexer leaves a usable index; two processes do not corrupt it; name, subword and substring lookups answer in milliseconds | 6 to 8 |
| P2 Structure | F1 and F2 scanners with a first set of language tables (C, C++, JavaScript/TypeScript, Kotlin, Java, Python, Go, Rust), F4 for Markdown, JSON, YAML, build files; F5 fallback; symbols with extents, reference lists with roles, imports; Q1 to Q6, Q13, Q15, Q16; file-link graph and rank; **pack text format frozen** | `outline`, `def`, `refs`, `callers`, `map` work on the owner's three test repositories and on katana itself; degraded files are reported, never fatal | 9 to 12 |
| P3 Diagnosis primitives | guards, keys with sides, writers and readers, Q7 to Q12, Q14, Q17, Q18; shape signatures | each query returns graded results on the test repositories; the first bench question's asymmetry is produced by Q12 without a model | 6 to 8 |
| P4a Knowledge base and interpreter rules (no model) | base loader with layers and provenance, normalization, layers 1 to 6 of 4.12.5, concept and operation channel, tiers and fusion, `gaps` list, cognates, request rules and anchors; `katana coder kb show/why/check/gaps` | a Portuguese request over English code yields terms that exist in the lexicon, each with the base row that produced it; with the base removed the search equals the lexical one | 5 to 7 |
| P4b AI port and expansion | `CoderAi` with `generate_json` wired to the engine, vocabulary card, expansion of uncovered words with validation and cache, proposed rows | uncovered words yield validated terms with a model and a GAP line without one; nothing unvalidated reaches the search | 3 to 4 |
| P5 Orchestrator, pack, interface | best-first loop, six playbooks, hypothesis templates, budget, pack in three renderings, `katana coder ask`, interactive session | every request type produces a pack under the ceiling with a GAPS section, with zero model calls and with the default budget | 8 to 10 |
| P6 Agent adapters | tool operations behind `Find` or `Coder`, `coder_touch`, graded neighbour note, pre-pack behind a switch, citation check, curriculum families specified | the agent runs with the layer on and off; `Find`'s trained format is unchanged | 4 to 5 |
| P7 Precision plugins | `CoderPrecise`, language-server adapter over `lsp.c`, answer cache; later the index-file reader | with a server installed, frontier edges come back `exact` within the deadline; without one, nothing changes | 4 to 6 |
| P8 Meaning channel (optional) | `embed` in the port, symbol-card vectors, the rules of `docs/embedding-usage.md` | only if its section 7 measurement says the hop slice moves and the speed allows | 3 to 5 |

Total without P8: **45 to 60 agent-days** (estimate). The knowledge base's content beyond layers 1 to 6 is the
owner's ongoing work and is not in this total.

### 6.2 What the owner has at the end of each phase

| After | The owner can |
|---|---|
| P1 | find any identifier, fragment of an identifier or string across a large folder instantly from the terminal; the index survives crashes and concurrent sessions. This is alternative D, delivered |
| P2 | navigate structure without AI and without a language server: outline, definition, references, callers, a ranked map with a token ceiling; `Find` can switch to symbol units |
| P3 | ask the deterministic diagnosis questions by hand: what can stop this function, who writes this, where does A appear and B not, what is the impact of changing X |
| P4a | type a request in Portuguese and see which code terms it maps to and by which row of the base; see the list of words the base does not cover |
| P4b | have the model cover those words, with every proposal checked against the code |
| P5 | ask a "why does X not happen" question and get an evidence pack, then the analyst's answer with citations; steer the investigation interactively |
| P6 | have the agent use all of it |
| P7 | get compiler-grade edges where a language server is installed |

### 6.3 Parallelism and order

```
P1 --> P2 --+--> P3 --+--> P5 --> P6
            |         |
            +--> P4a -+          (P4a needs only the lexicon; it can start after P1)
            +--> P4b -+          (needs P4a and the engine)
            +--> P7              (needs only Q3 to Q6 and lsp.c)
            +--> more language tables for F1/F2/F3 (independent, about half a day each, estimate)
P8 last, gated
```

Earliest value: P1 and P2 are useful alone. The first point where the measured failure can improve is the end of
P5 (pack) or, for the agent, P6. If the owner wants evidence earlier that the whole direction pays, the shortest
path is: P1, a thin slice of P2 (F1 for the two languages of one test repository), Q12 and Q7 from P3, and a
hand-assembled pack fed to the bench (M4 and M5). That is about 12 agent-days (estimate) to the first decisive
measurement.

---

## 7. Risks, uncertainties, and the minimal measurements

### 7.1 Risks

| Risk | Why it is real | Mitigation |
|---|---|---|
| The model does not use the pack well: trusts `?` items, ignores GAPS, or ignores the pack | every prior note-based intervention depended on the model's obedience; only agent runs show it | fixed marks and sections; training families; the pre-pack is off until M5 |
| Scanner precision on hard syntax (C++ templates and macros, heavily dynamic JavaScript, shell) | no parser | `degraded` flags, grades, plugins; language tables are cheap to fix |
| Name-based call edges are too ambiguous to be useful in some code bases (common method names) | resolution without types | receiver text, import visibility, arity; M11 measures the ambiguous share; plugins where it is bad |
| The vocabulary gap is not closed by glossary, cognates and expansion | nothing measured yet; it is the largest measured failure | M1 and M2 before investing in P4 beyond the rules |
| The playbooks are rigid and real questions do not fit the six types | types were chosen from a small bench | default type R3; every playbook ends in a pack with NEXT operations, so the analyst can continue by hand |
| Time: the investigation plus the analyst is slower than the user accepts | 15 to 27 tokens/s; prefill unknown per path | budget, zero-model path, prefix cache, `score_labels` |
| The pre-pack repeats the "unrequested seed" mistake | measured 0% top-1 precision for guessed answers | the pack carries no answer; switch; M5 |
| One compilation unit grows by a third or more | 9,000 to 13,000 lines estimated | separate folder `src/coder/`, one include block, no engine dependency |
| The size and time estimates of 4.2.6 are wrong by a large factor | arithmetic on assumed averages | M8 early in P1; positions are already not stored, which is the main lever |
| Training cost: the model must learn the pack format | new format | freeze the format in P2; keep `Find`'s trained shape until retraining |

### 7.2 Uncertainties the author cannot resolve from here

1. Whether Q12 (asymmetry) and Q7 (guards) reach the answer files of real "why not" questions, or only of the
   one case that inspired them.
2. Prefill speed for a few hundred new tokens after a cached prefix, which sets the price of every model call.
3. Whether label probabilities from one forward pass rank candidates usefully with this model.
4. How often a language server is actually installed and correctly configured for the owner's projects.
5. Whether the symbol card is a better retrieval unit than the 40-line window for prose-like requests.
6. Outside-system facts marked *(not verified)* in section 2.

### 7.3 Minimal measurements that would decide (none executed)

| # | Decides | Minimal measurement | Needs the large model? |
|---|---|---|---|
| M1 | whether glossary + cognates are worth shipping, and how much gap is left for the model | on the existing hop slice (27 answer files, 18 questions; `tools/findbench`): recall at 10 with and without the two bridges, in the Python bench | no |
| M2 | whether model expansion closes the gap | the 18 hop questions: one expansion each with validation against the copy's lexicon; recall at 10 of answer files | yes, 18 short generations |
| M3 | symbol card or 40-line window | recall at 5 and 10 on the 76 questions with both units, same ranking function | no |
| M4 | whether asymmetry and guards deserve their place | for the bench questions that name a working sibling or an effect: is an answer file in the top 5 of Q12 / on a Q7 path | no |
| M5 | **whether the whole direction pays** | `tools/investigate/eval.sh` on the validation questions, with and without a pre-pack (hand-assembled is enough for the first run): pass rate, time, tokens | yes, one eval run each way |
| M6 | the model budget (number of calls per investigation) | time of one closed-set choice: 400 new tokens after a cached prefix + 20 generated tokens; alternated runs in the same hour | yes, a few calls |
| M7 | `score_labels` or generated choices | agreement between the two on about 30 choices taken from M5 logs | yes, about 30 forward passes |
| M8 | segment design and the ceiling | build time, update time after touching 1% of files, and index size on the largest local repository and on one public checkout near 100,000 files | no |
| M9 | plugin deadline and whether lazy confirmation is usable | latency of 20 frontier confirmations, cold and warm, for two servers on local projects | no |
| M10 | whether a corpus trigram index is ever needed | warm and cold time of a parallel literal and regular-expression scan over the M8 checkout | no |
| M11 | how much `derived` there is | share of call references that resolve to one candidate, per language, on the local repositories; and, where a server exists, agreement of 200 sampled `derived` edges with it | no |

| M12 | the ranking mode of 4.12.4 (tiers + fusion, or the weighted sum) and whether layers 1 to 3 of the base are enough | the same run as M1 with: lexical only; base layers 1 to 3; layers 1 to 6; each under both modes; recall at 5 and 10, all questions and hop files | no |

M1, M3, M4, M8 and M12 need no model and no new C code beyond prototypes; they should run before P3 is finished.
M5 is the one that can stop the project, and the plan reaches it after about 12 agent-days (6.3).

---

## 8. Reconciliation with `docs/code-intelligence.md`

The specification existed when this document was finished (read on 2026-11-23 in the other agent's worktree,
1,931 lines, not yet merged; it may have changed since). It still uses the earlier names (`src/intel/`,
`katana intel`, `.cbi`, `Ix*`, `data/intel/kb/`); the owner's naming decision (`coder`) applies to both documents.

### 8.1 Where the two documents agree (reached independently)

| Point | Both say |
|---|---|
| Store | derived cache; immutable mapped segments; atomic manifest swap; no WAL, no B+ trees; 32-bit indexes on disk and 64-bit stable keys outside; explicit serialization |
| Relations | call and use relations resolved at query time; only the file graph is materialized |
| FTS5 | not adopted, for the same reasons |
| Parsers | own lightweight scanners behind an interface; no embedded parsing library; language servers through `src/tools/lsp.c` as the optional source of resolved calls |
| Exact against probable | never mixed; nothing that reads as a probability reaches the model |
| Model | one constrained expansion validated against the index; the model is the analyst; the core runs with no model |
| Diagnosis | guards and the comparison with a working sibling are part of the packet |
| Semantics | embeddings last and conditional; CSSE as the search core |
| `Find` | kept working, same trained format, old index retired after parity |

That two designs converged on the store and on lazy linking is the strongest support either has; neither is
measured.

### 8.2 Keep from the specification

- Its **phase A0** (move scanner and tokenizer out of `find.c` with byte-for-byte unchanged behaviour) and its
  golden sets per language as the "done" criteria. This document's P1 and P2 should adopt both.
- Its **packet layout by hypothesis blocks** (entry, flow, "can stop here", excerpt). For a model that reads top
  to bottom it is easier to follow than this document's FACTS / LINKS split with cross-referenced ids. Keep that
  layout; add from here the stable evidence ids, the three grades and the mandatory GAPS content rules.
- Its **token ceilings per request type** and the default of 1,500, and its time budgets per stage.
- Its **knowledge-base file format** (tab-separated rows with provenance, layers, negative rows) and the `kb`
  commands.
- Its treatment of headless mode, resumed sessions, folders that are not projects, and the training section.
  This document does not cover them.

### 8.3 Change in the specification

| Topic | Specification | This document's position | Why |
|---|---|---|---|
| Certainty grades | two: exact, probable | three: exact, **derived**, probable (4.1.3) | a call resolved by name with a single visible candidate is neither compiler-grade nor a guess. Marking it exact overstates it; marking it probable buries the most useful edges. A plugin confirmation is what promotes it |
| Language servers | asked first for `Def`, `Callers`, `Callees` when switched on | **lazy confirmation of frontier edges, with a cache keyed by content hashes** (4.3.5) | asking first puts a slow, possibly cold process on the path of every query; confirming after keeps millisecond answers and still ends with exact edges where it matters |
| Ranking of CSSE | weighted sum as default, rank fusion as an alternative, weights calibrated by a grid over the 76 questions | **tiers + rank fusion as default**, the sum as the experimental mode (4.12.4) | 76 questions over 3 repositories cannot calibrate five weights; the research already showed a normalized weighted sum losing |
| Order of CSSE | phase A5 before the investigation, 5 to 8 days, whole base | split: P4a with layers 1 to 6 only; domain files per project; business concepts last (4.12.5) | the owner's projects need technical vocabulary; "register everything" must not block the investigation |
| String keys | named as a known gap ("linked by a name in a string, which the engine does not follow") | **first-class `key` entities with produce and consume sides** (4.1.1, Q9) | events, routes and command names are how the owner's bridge code is wired; this is where "why does X not appear" questions break |
| Positions | occurrences stored | **per-symbol name sets with role bits; lines recomputed at print time** (4.1.1) | smaller index and no stale line can be printed. To be settled by M8 |
| Model in the loop | expansion only | expansion plus at most one closed-set choice when candidates do not fit (4.7.3) | cheap to leave out at first: the specification's simpler version is the right first build, and the choice is added only if M6 shows a call is affordable |
| Queries | the owner's five plus guards, similar, path | add `writers`/`readers`, `key`, `asymmetry` as a standalone query, `slice`, `absent` (4.4.2) | the R5 playbook needs them as primitives the user can also run by hand |
| Tools for the model | `Def`, `Callers`, `Callees`, `Outline` as separate core tools | one tool with an operation enum, or `Find` extended | a small model chooses badly among many similar tools; to be decided with the training plan, since either way the model must be trained on it |
| Names | `intel`, `.cbi`, `Ix*` | `coder` everywhere | owner's decision |

### 8.4 Where the specification is right and this document was adjusted

- The reading cost of the pack: this document first set a 2,400-token ceiling and estimated a model choice at 2 to
  5 s. The specification's arithmetic with the 60 tokens/s reading speed of `docs/tools.md` is the sounder base;
  4.7.3 and 4.8.2 now use it, and the default budget dropped to one choice.
- The effort totals are compatible: the specification gives 31 to 46 agent-days for the full design, this document
  45 to 60 including plugins, the extra queries and the interactive interface. Both are estimates.

### 8.5 Proposed outcome

One document should remain as the build reference: the specification, amended by the table of 8.3, with this
document kept as the record of why (requirements, state of the art, alternatives). The decisions the owner has to
take, because the two documents differ and no measurement exists: two or three grades; language server first or
lazily; the default ranking mode; how much of the base precedes the investigation.
