# When the engine calls the embedding model (usage model)

Status (2026-11-25): the embedding model is wired into the engine through the embedding service (`src/app/embsvc.c`:
`es_available()`, `es_vec()`, `es_rank()`; 256-dimension Matryoshka vectors; content-keyed append-only cache in
`~/.katana/cache/embed-<model fingerprint>.vec`; loaded on its own thread while the agent prompt loads,
`agent/prefix_cache.c`). It is used in three places, always as a reorder-and-suggest filter with a time budget, and the
tools fall back to the old path without error when there is no model, no time left or `KATANA_EMBED=0`:
`Find` (semantic channel in `coder/search.c`, budget `KATANA_EMBED_FIND_MS`, default 2000 ms), memory recall
(`agent/memory.c`, the 200 most recent facts, `KATANA_EMBED_MEM_MS`) and `Bash` output trimming (`agent/exec.c`, failure
windows, `KATANA_EMBED_BASH_MS`, default 1500 ms). Nothing about its quality was measured after wiring it in. The sections
below are the original design and are kept for the reasoning; where they say the design is not implemented, read this
paragraph. Before that, on 2026-11-23, what existed was: **`katana embed` and the C
function `embed_text()` (`src/app/embed.c`) run google/embeddinggemma-2 (text tower, 0.27 G parameters, 768 dimensions
with Matryoshka 128/256/512/768) on the CPU in float32**, with `katana convert` producing the GGUF; see `docs/embed.md`.
It matches the reference implementation (cosine 0.99999999 in F32, 0.9999 in Q8_0) and found the right function for 10 of
10 code queries among 10 snippets. This is a different model from the Qwen3-Embedding-0.6B named in section 0 (the owner
chose embeddinggemma-2 for the filter on 2026-11-23); wherever this document says "Qwen3-Embedding", "last token" or
"1024 dimensions", read the facts of `docs/embed.md` instead: bidirectional attention, **mean pooling**, a task prefix on
queries *and* a `title: ... | text: ...` prefix on documents, truncate-and-renormalise Matryoshka. Costs were observed
only under load (about 10 to 25 ms per token on two busy threads; `docs/embed.md`, "Cost"), so the cost parameters below
are still provisional and section 7 still lists what to measure. Numbers quoted from other documents carry their source.

The design is the owner's:

- The **engine** (C code) is the front door of every operation.
- The **embedding model** is an optional search filter. It is consulted only inside a search, to reorder
  candidates by meaning. It never decides, never excludes, and is never the front door.
- The **large model** (Qwen3.6-35B-A3B) reasons and decides.
- **Situating the model is fact**: folder, branch, languages, entry points, layout, docs. Always, deterministic.
- **Filtering what is relevant is a suggestion with evidence**: never an exclusion, never presented as fact.
- **No unrequested seed.** `docs/investigation-engine.md` section 3.6 measured that confidence does not separate
  right from wrong across repositories: a threshold calibrated for 90% top-1 precision on commits would inject a
  seed into 21 of 75 investigation questions with top-1 precision 0%.

## 0. Facts this design rests on

| Fact | Value | Source |
|---|---|---|
| Embedding model | Qwen3-Embedding-0.6B, Q8_0, one file of 639,150,592 bytes in `/mnt/dados/embed-models/qwen3-embedding-0.6b/`; dense qwen3 architecture, 1024-dimension vectors; about 55 times smaller than the large model | file on disk; owner's brief |
| Current `Find` | per-field BM25 over 40-line chunks (stride 25) + request-word filter + sparse activation over a file graph, each candidate with evidence; the query itself takes 1 to 7 ms; index in `<folder>/.katana/cache/find-<hash>.idx` | `docs/tools.md`, "Find (navigation)"; `src/tools/find.c`, `src/tools/findgraph.c` |
| Activation constants | best 40 files form the active set (`FG_K_FILE`), 2 rounds, 20 winners per round (`FG_K_GRAPH`), threshold 0.15 (`FG_THETA`); the research run kept 39 nodes above threshold on average | `src/tools/findgraph.c`; `docs/investigation-engine.md` 3.3 |
| `Find` output | default 6 candidates, max 12, about 1500 characters in total (`KATANA_FIND_CHARS`); a graph-entered candidate has a `   why: via <file> (symbol X)` line | `src/tools/find.c` (`ct_findtool`, `fd_show`) |
| Neighbour note | `[katana: linked files you have not read yet: ...]`, at most 3 neighbours per result and 8 notes per request, after `Read`, `Grep` or `Find` | `src/tools/findgraph.c` (`fg_neighbor_note`, `FG_NOTES_MAX`), called in `src/agent/loop.c` before `loop_plan_note` |
| Research recall, 46-question sample | BM25 R@5 15.9% / R@10 28.3%; activation R@5 33.0% / R@10 44.9%; top 5 contains a gold file in 41.3% of questions | `docs/investigation-engine.md` 3.3 |
| Hop slice | 27 of 127 answer files (21%), in 18 questions, contain none of the question's informative terms; mostly Portuguese questions about English code; no word-based method improves it | `docs/investigation-engine.md` 1 and 3.4 |
| Earlier embedding attempts | static folder embeddings and the large model's input table: about 3% R@10 alone, no gain on the hop slice | `docs/investigation-engine.md` 4 |
| Machine | i7-11800H (8 cores, 16 threads), 64 GB RAM, RTX 3050 Laptop 4 GB | `docs/knowledge.md` |
| GPU occupancy by the large model | dense part 1.9 GB + state about 0.2 GB; the rest holds 19 of 256 expert slots per layer; one expert is 1.96 MB | `docs/new-models.md`; `docs/knowledge.md` |
| Large model speed | generation 15 to 27 tokens/s; prefill 35 to 5000 tokens/s depending on the path | owner's brief (`docs/handoff.md` has 23-27 tok/s generation runs) |
| Execution | the two models run in sequence: while one computes the other waits | owner's brief |

Two caveats about these facts:

- The research numbers are from the Python prototype. `docs/tools.md` says they were not reproduced in C.
- Properties of the embedding model that this document assumes from its public description, not checked here:
  queries take a task instruction and documents do not, the vector is taken from the last token, and shorter
  vectors can be obtained by truncating and renormalising. `katana embed` must confirm each from the GGUF
  metadata and by test before the cache format depends on it.

## 1. Request types

For each type: what the engine does today without embeddings, where a meaning vector changes something, and where
it does not.

### 1.1 Project overview ("what does this project do")

- **Without embeddings.** The system prompt already carries the facts: working folder, git root and branch,
  project files (CMake, Make, Node.js, ...), the folder tree, the parent folder (`src/agent/prompt.c`, the
  environment block). `src/agent/docsel.c` adds the lines of the project's `.md` files that match the request,
  chosen by token rarity, in milliseconds. The model then reads the README or the entry point.
- **With embeddings.** Nothing. There is no search to filter: the answer is the layout and the docs, which are
  facts. An embedding ranking of "files that look like an overview" is exactly the unrequested inference the
  owner ruled out.
- **Verdict:** never call.

### 1.2 Explain a flow ("how does X work")

- **Without embeddings.** `Find` with a few words gives the entry chunk. The `calls:` line, `mode=refs` and the
  neighbour note carry the model from one file to the next. These are structural hops and the graph handles
  them.
- **With embeddings.** Only the **first hop** can change: when the user's words for X are not the code's words
  (another language, or a concept the code names differently), the lexical seed is wrong and the graph spreads
  from the wrong place (`docs/investigation-engine.md` 3.4: "the graph cannot bridge that gap, because the seed
  itself is wrong"). A meaning ranking can put the right chunk in front of the model.
- **Where it does not help.** Hops 2 and later. "Who calls this" is a fact from the index. A vector that is
  "similar" to a function is not its caller.
- **Verdict:** call on the first `Find` when the vocabulary-gap test of section 2 fires. Not on `mode=refs`.

### 1.3 Locate ("where is X")

- **Without embeddings.** If X is an identifier, a file name or a quoted message: `Grep`, `Glob` or
  `Find mode=refs` give the exact place. If X is a description: `Find` search.
- **With embeddings.** Only for the description case, and only when the lexical ranking is weak (section 2).
- **Verdict:** exact name, never. Description, conditional. This is the main use.

### 1.4 Change ("change the function that generates tokens")

The steps: find by concept, read the whole definition, see the uses, edit, verify.

- **Without embeddings.** Step 1 is a `Find` search. Steps 2 to 5 are `Read` with offset and limit,
  `Find mode=refs`, `Edit`, `Bash`.
- **With embeddings.** Step 1 only, same rule as 1.3. A wrong first candidate costs more here than in a question,
  because the model may edit the wrong function. So the evidence line must say the candidate is unverified
  (section 4), and the curriculum must show the model reading the definition before editing (section 8).
- **Verdict:** conditional on step 1. Never on steps 2 to 5.

### 1.5 Debug ("why does X not happen", several hops)

- **Without embeddings.** This is the worst case measured: the cause is in files that do not contain the words
  of the question. The tools are `Find`, `calls:`, `mode=refs`, the neighbour note.
- **With embeddings.** The first `Find` benefits as in 1.2. Later `Find` calls in the same request are written
  by the large model after it has read code, so they tend to contain identifiers and English words. For those
  the lexical ranking is strong and the vocabulary-gap test will not fire.
- **Where it does not help.** A question about something that is *absent* ("does not appear") has no chunk that
  means it. The embedding returns its nearest chunk anyway, with a normal-looking score. This is the main source
  of confident false positives (section 5).
- **Verdict:** conditional on the first search; expect few calls after it.

### 1.6 Requests with no search

Commands (`Bash`), an edit of a file the user named, conversation, web (`WebSearch`, `WebFetch`), tasks, plan
approval, MCP tools: the embedding model is never loaded and never called. If a session has none of the
qualifying `Find` searches, the embedding model costs zero memory and zero time.

`ToolSearch` keyword queries (a search over tool names and descriptions, `src/tools/toolsearch.c`) are a search,
but over a few dozen short texts that the model already sees listed by name. Not a target now; see D17.

## 2. Decision table

The engine decides with tests that cost microseconds and use only what the `Find` index already has.
Definitions used by the table:

- **Informative terms:** the query terms left after the request-word filter (`fg_drop_request`) and the stop
  words.
- **Vocabulary gap `G`:** the fraction of informative terms that do not exist in the index (document frequency
  0). This is the engine's language test: it needs no language detector and no word list, and it also catches a
  concept the code names differently. A Portuguese query over English code has a high `G`.
- **Identifier query:** every informative term has identifier shape (underscore, camelCase, a dot or `::`, or it
  is an exact entry of the symbol table).
- **Covered top:** the first candidate's chunk contains every informative term, at least one of them in the
  path or in a defined symbol, and the candidate entered by text match (not by an edge).
- **Pool:** the files that survive the activation threshold, before the cut to `limit` (up to 40 lexical files
  plus up to 40 that entered through edges; 39 on average in the research). The embedding only reorders the
  pool's best chunks, at most `N_POOL` of them.

Provisional parameters are named `P-...` and listed in section 3.6.

| # | Condition (what the engine sees) | Action | Certainty |
|---|---|---|---|
| D1 | `KATANA_EMBED` is not `1`, or the model file is not found | never call; `Find` is exactly today's | firm |
| D2 | The tool is not `Find` with `mode=search` (so: `Grep`, `Glob`, `Read`, `Find mode=refs`, `docsel`, the prompt's environment block, every other tool) | never call | firm |
| D3 | `Find` search written by the large model, and none of D4 to D12 forbids it | **call**: reorder the pool by meaning and mark promoted candidates. This is the main case | firm as design; gain unmeasured |
| D4 | Identifier query, or the query names a symbol that exists in the symbol table | do not call. `Grep` and refs are exact; a meaning ranking can only demote the exact hit, and it costs time | firm |
| D5 | Vocabulary gap `G >= P-GAP` (provisional 0.5), e.g. a Portuguese description over English code | call, with the widest pool (`N_POOL` at its maximum). This is where words cannot help (21% of gold files) | firm that this is the target; the gain is the open question of the whole design |
| D6 | Covered top, and `G = 0`, and relative margin between first and second candidate `>= P-MARGIN` (provisional 0.3) | do not call | uncertain: see note 1 |
| D7 | `G = 0` but not covered top (terms spread over several candidates, or the top entered by an edge) | call only if everything needed is already cached (cost = one query vector) | uncertain |
| D8 | Pool has fewer than `P-NMIN` (provisional 3) candidates, or all of them will be shown anyway (`pool <= limit`) | do not reorder. If `G >= P-GAP`, D13 may still add candidates | firm |
| D9 | Pool larger than `N_POOL` (provisional 24) | reorder only the best `N_POOL` by lexical order; the rest keep their lexical order below | firm |
| D10 | Small folder (fewer chunks than `P-SMALL`, provisional 400) | call under the same rules, and embed missing chunks of the pool on demand. A small folder becomes fully cached after a few queries without any sweep | estimate |
| D11 | Large folder, cold cache: the uncached part of the pool does not fit the time budget (`uncached tokens / P-SPEED > T_MAX`) | embed the best uncached candidates that fit, in lexical order; reorder only the candidates that have a vector; never wait past `T_MAX` | firm |
| D12 | Time budget of the request already spent (`P-TREQ`), or two calls in this session ended by timeout | do not call for the rest of the request (or session, for the timeout case); say so once on stderr, never to the model | firm |
| D13 | `G >= P-GAP` and the folder has vectors in cache for chunks outside the pool | also score those cached vectors against the query (a scan, no new embedding) and let at most `P-ADD` (provisional 2) of them enter as extra candidates, marked as in section 4 | uncertain: this is the only path that can reach a file the words never reached, and it only sees what is cached |
| D14 | The whole project is not embedded | never sweep the project on the engine's initiative. A full index is built only by an explicit owner command (section 3.4) | firm |
| D15 | Neighbour note (`fg_neighbor_note`) | do not call. See note 2 | firm for now |
| D16 | Overview request, `docsel`, the environment block | do not call. Situating is fact | firm |
| D17 | `ToolSearch` keyword query | do not call now. If ever: tool descriptions are few and fixed, so their vectors are cached once and the cost is one query vector; only when the keyword search returns nothing | low priority |
| D18 | The large model is generating or doing prefill | nothing to decide inside `Find` (the large model is waiting for the tool result, so the embedding does not compete with it). Background work must stop: see D20 | firm |
| D19 | GPU or CPU for the embedding | CPU. See note 3 | firm as default; measure |
| D20 | Warming in the background | off by default. If the owner turns it on: only while the agent waits for the user's next message, only chunks of files already touched in this session (read, edited, or returned by `Find`), stops at the next chunk boundary when the user sends a message | uncertain |
| D21 | Plan mode | same rules as normal mode. `Find` is read-only and already writes its index to `.katana/cache` in plan mode; the vector cache goes to the same folder | firm |
| D22 | Headless session (`-p`) | same rules; background warming never runs (there is no idle time); the model is loaded synchronously at the first qualifying call if `P-LOAD <= P-LOADMAX`, otherwise the session runs without embeddings | uncertain until `P-LOAD` is measured |
| D23 | First qualifying `Find` of an interactive session, embedding model not loaded yet | start loading in a background thread and answer this call **without** embeddings unless the load finishes inside `T_MAX`. Later calls use it | uncertain until `P-LOAD` is measured |
| D24 | Later qualifying calls of the session | call; query vectors of this session are kept in memory (a repeated query costs nothing), chunk vectors come from the cache | firm |
| D25 | A `Grep` whose pattern is a natural-language phrase is redirected to `Find` (rule R1 of `docs/investigation-engine.md` section 5, if it is ported) | the `Find` behind it follows this same table | firm |
| D26 | The user's request text, before the model has asked for anything | never embed it to produce candidates. That is the automatic seed | firm |

**Note 1 (agreement between signals, D6).** The intent is "do not pay when words already answered". But the
research measured that the margin does not identify correct answers on investigation questions (top-1 right about
4% of the time, and the most confident 10% of queries had 0% top-1 precision; `docs/investigation-engine.md`
3.6), and that the agreement between BM25 and the graph ranking carries no information because the graph is
seeded from BM25. So D6 is a **cost rule, not a confidence rule**: it skips the call where a reorder is least
likely to change the top (every word found, in a symbol or path), and it is not evidence that the top is right.
The firm skip rules are D4 (identifier) and D8 (nothing to reorder). If the measurement of section 7 shows that
reordering under D6's condition changes the right answer as often as elsewhere, drop D6.

**Note 2 (neighbour note, D15).** Cost and benefit:

- The note runs after every successful `Read`, `Grep` and `Find` (up to 8 times per request). The neighbours are
  whole files that are usually not in the vector cache, so each note would embed new chunks while the large
  model waits.
- The note has no query written by the model. Ranking neighbours by meaning needs a text to compare with, and
  the only one available is the user's request: that is inference nobody asked for (D26).
- The note's content is a fact ("linked via symbol X"). Mixing a meaning order into it blurs fact and
  suggestion.

One cheap variant is left for later and only if measured useful: when more neighbours pass `FG_NEIGH_MIN` than
the 3 that fit, order them by similarity to the **last `Find` query the model wrote in this request**, using
only vectors already in the cache (zero new embedding), and leave the wording of the note unchanged.

**Note 3 (CPU or GPU, D19).** The 4 GB card is full by design: what the dense part and the state do not use
becomes expert slots, 19 of 256 per layer. Arithmetic from the documented sizes (an estimate that ignores work
buffers): the 639 MB file is the size of about 326 experts of 1.96 MB, that is 8 of the 19 slots in each of the
40 layers. Putting the embedding model on the GPU would cut the expert cache by about 40% for every generated
token in order to speed up an occasional search. The embedding call happens while the large model waits, so the
CPU cores are free at that moment. Default: CPU, all cores. `KATANA_EMBED_GPU=1` exists only to measure the
alternative.

## 3. Budget and cache

### 3.1 What is embedded

- **Unit:** the chunk the `Find` index already has (40 lines, stride 25). One vector per chunk, keyed by content.
- **Text of a chunk:** `path`, then the names it defines (the `defines:` / `sections:` list), then the chunk's
  lines, cut at `L_MAX` tokens (provisional 256). A 40-line chunk is about 400 tokens by the research's own cost
  rule (characters / 3.5; an estimate), so many chunks will be cut. Putting path and symbols first makes the cut
  lose body lines, not names. A cut chunk is flagged in its cache record.
- **Query:** the model's `query` string exactly as written (no translation, no expansion), with one fixed English
  task instruction in front. The instruction text is a constant in the binary and is part of the cache key.
- **Per file, one chunk:** the pool has one best chunk per file (that is what `Find` shows). Only that chunk is
  embedded, not every chunk of the file.

### 3.2 Per-call budget

| Parameter | Provisional value | Meaning |
|---|---|---|
| `N_POOL` (`KATANA_EMBED_POOL`) | 24 | most chunks reordered per call (4 x the default `limit` of 6, 2 x the maximum of 12) |
| `L_MAX` (`KATANA_EMBED_TOKENS`) | 256 tokens | cap per chunk text |
| `T_MAX` (`KATANA_EMBED_MS`) | 1500 ms | wall-clock cap per `Find` call for all embedding work, query included |
| `P-TREQ` | 4000 ms | cap per user request, summed over its `Find` calls |
| `P-ADD` | 2 | most candidates that may enter from outside the pool (D13) |

Why `T_MAX` is in this range (reasoning, not a measurement): a `Find` answer that sends the model to the wrong
file costs one extra round. A round is one generated tool call (a few tens of tokens at 15 to 27 tokens/s, so
roughly 2 to 4 s) plus the prefill of its result. An embedding call is worth its time only if it costs clearly
less than the round it may save, and it will not save one every time. 1.5 s is below one round; the per-request
cap of 4 s is about one round. Both must be revised when `P-SPEED` and the real benefit are known.

What the cap means in chunks depends entirely on the unmeasured speed `P-SPEED` (tokens/s of the embedding
model on this CPU). Arithmetic only, for a full cold pool of 24 x 256 = 6144 tokens:

| If `P-SPEED` were | cold pool of 24 chunks takes | chunks that fit in `T_MAX` = 1.5 s |
|---|---|---|
| 500 tokens/s | 12.3 s | 2 |
| 2000 tokens/s | 3.1 s | 11 |
| 6000 tokens/s | 1.0 s | 35 |

These three speeds are placeholders chosen to show the sensitivity; none is measured or predicted. The table
says: below a few thousand tokens/s, a cold call cannot reorder a useful pool inside the cap, and the feature
lives or dies by the cache. Above that, on-demand embedding alone is enough.

**On overflow** (the clock passes `T_MAX`, checked between chunks and between batches):

1. Stop embedding. Keep the vectors already computed and write them to the cache (the work is not lost: the next
   call starts warmer).
2. Reorder only the candidates that have a vector; the others keep their lexical order. If the query vector
   itself is not ready, return the lexical ranking unchanged.
3. Never block: the `Find` result is returned at `T_MAX` plus at most one batch.
4. Count the timeout (D12). Nothing about it is written into the tool result.

### 3.3 Cache on disk

- **File:** `<working folder>/.katana/cache/embed-<fp>.vec`, next to `find-<hash>.idx`, same fallback rule
  (`~/.katana/cache`, `KATANA_CACHE_DIR`). `<fp>` is a fingerprint of the model (file size, tensor count and a
  hash of the GGUF header and metadata) plus the cache format version. Another model, another quantization or a
  format change gives another file name, so an old vector can never be compared with a new one.
- **Key of a chunk record:** `sha1(chunk text as embedded)`. The text includes path and symbol names, so a moved
  file is a new key. The file header stores: model fingerprint, vector dimension, `L_MAX`, pooling rule, the
  sha1 of the query instruction, storage type. A header that does not match exactly: the file is ignored and
  rebuilt on demand, without an error.
- **Record:** 20-byte key, token count, flags (truncated), vector. Append-only; a torn last record is dropped on
  load.
- **Invalidation:** none is needed for content changes, because the key is the content: an edited chunk misses
  and the old record becomes garbage. Garbage is collected when the file passes `KATANA_EMBED_CACHE_MB`
  (provisional 64): keep the records whose key is a current chunk of the index, then the most recently used.
- **Query vectors:** in memory only, the last 64 of the session, keyed by `sha1(instruction + query)`. Not
  written to disk (queries are cheap and rarely repeat across sessions).
- **Size on disk** (arithmetic): a record is about 4.1 KB with 1024 float32 values, about 2.1 KB with float16,
  about 1.05 KB with int8. Per 10,000 cached chunks: 41 MB, 21 MB or 10.5 MB. For scale, the `Find` index is
  5.3 MB for the katana tree and 18.5 MB for the 5349-file test repository (`docs/tools.md`). Provisional
  choice: int8 with the vector normalised first (`P-STORE`), to be confirmed by measuring the ranking change
  against float32. A shorter vector (256 dimensions) would divide the size by 4; it is not assumed.
- **Scan cost** (D13): scoring 10,000 cached int8 vectors of 1024 dimensions against one query is about 10
  million multiply-adds, well under the `Find` query's own milliseconds on one core (an estimate).

### 3.4 On demand, in the background, or by command

- **On demand (default when on):** vectors are computed only for the pool of a qualifying `Find` call, inside
  `T_MAX`. A folder the model never searches gets no vectors.
- **Background warming (`KATANA_EMBED_WARM=1`, off by default):** D20. It exists because the vocabulary-gap case
  is exactly where the pool may not contain the right file, and only cached vectors outside the pool (D13) can
  bring it in.
- **Full index by command:** `katana embed index [path]` (name proposed; the owner runs it, the agent does not).
  This is the only way the whole project is embedded. It prints progress and can be interrupted; what it wrote
  stays valid. Arithmetic for the size of the job: the 4.7k-file research repository is 35 MB of text
  (`docs/investigation-engine.md` 7.1), about 10 million tokens at 3.5 characters per token. Chunks overlap (40
  lines every 25) and are cut at `L_MAX`, which roughly cancel, so the job is in the order of 10 million tokens
  (an estimate; the chunk count was not measured). At the placeholder speeds of 3.2 that is about 5.5 hours,
  1.4 hours or 28 minutes. A full index of a large repository is therefore an explicit, long job, never
  something the agent starts.

### 3.5 Concurrency and memory

- Inside a `Find` call the large model is idle by construction, so the embedding may use every core.
- Background warming and background loading must never run while the large model generates or does prefill:
  with 19 of 256 expert slots on the GPU, most expert work is on the CPU, and a second model on the same cores
  slows every token. The warm thread checks a "large model busy" flag between chunks.
- The embedding model is opened lazily (D23) with `mmap`, so its 639 MB are page cache, not private memory, and
  the system can drop them under pressure. Private memory is its work buffers and its own KV state for `L_MAX`
  tokens (`P-RSS`, not measured).
- On the minimum machine (24 GB RAM, `docs/handoff.md`), the fit rule "file plus 2 GB must fit in RAM" has to
  count the embedding file as well. If it does not fit, the engine behaves as if the model were absent (D1).
- After `P-IDLE` (provisional 10 minutes) without a qualifying call, the engine frees the work buffers and
  unmaps the file.

### 3.6 All provisional parameters

| Name | Provisional | What it is |
|---|---|---|
| `P-SPEED` | unknown | embedding tokens/s on this CPU, batch of chunks, large model loaded but idle |
| `P-QUERY` | unknown | time for one query vector (about 20 to 40 tokens) |
| `P-LOAD` | unknown | time from "not mapped" to "first vector", cold and warm page cache |
| `P-LOADMAX` | 3 s | longest synchronous load accepted in a headless session |
| `P-RSS` | unknown | private memory of the embedding model while in use |
| `N_POOL` | 24 | chunks reordered per call |
| `L_MAX` | 256 tokens | cap per chunk |
| `T_MAX` | 1500 ms | cap per call |
| `P-TREQ` | 4000 ms | cap per user request |
| `P-GAP` | 0.5 | vocabulary gap that forces a call |
| `P-MARGIN` | 0.3 | relative margin for the skip of D6 |
| `P-NMIN` | 3 | smallest pool worth reordering |
| `P-SMALL` | 400 chunks | "small folder" |
| `P-ADD` | 2 | candidates admitted from outside the pool |
| `P-KEEP` | 3 | lexical candidates that are always shown (section 4) |
| `P-PROMO` | half of `limit` | most candidates shown because of meaning (section 4) |
| `P-RRFK` | 60 | constant of the rank fusion |
| `P-WSEM` | 1.0 | weight of the meaning rank in the fusion |
| `P-STORE` | int8 | vector storage type |
| `P-IDLE` | 10 min | idle time before unloading |
| cache cap | 64 MB | `KATANA_EMBED_CACHE_MB` |

## 4. Fusion and evidence

### 4.1 Combining the two rankings

Use **reciprocal rank fusion** over the pool, not a weighted sum of scores:

```
lex_rank(c)  = position of candidate c in today's order (BM25 + request-word filter + activation)
sem_rank(c)  = position of c by cosine(query vector, chunk vector), among candidates that have a vector
score(c)     = 1 / (P-RRFK + lex_rank(c)) + P-WSEM / (P-RRFK + sem_rank(c))
               (a candidate without a vector gets only the first term)
```

Why ranks and not scores:

- BM25 scores have no fixed scale, activation values are normalised per query, and cosines of one embedding
  model sit in a narrow band that shifts with the query. A weighted sum needs a weight tuned per repository.
- The research already tried `bm25_norm + mu * cos_norm` (with the folder embeddings): mu 0.25 tied BM25, mu 0.5
  and 1.0 hurt (`docs/investigation-engine.md` section 4). And it measured that thresholds calibrated on one
  kind of query do not transfer to another (3.6). Rank fusion has no scale to calibrate.
- Uncertain: `P-RRFK` = 60 is the conventional value, not one measured here; with pools of 24 the constant
  matters. `P-WSEM` should rise with the vocabulary gap `G` (the lexical order is least reliable there), but
  the shape of that dependence is a guess until measured.

Two guards keep the embedding a suggestion and never an exclusion:

1. **Lexical floor.** The first `P-KEEP` (3) candidates of today's order are always among the shown ones,
   whatever their meaning rank. Their order may change.
2. **Promotion cap.** At most `P-PROMO` (half of `limit`: 3 of the default 6) of the shown candidates may be
   there because of meaning (candidates that today's order would have left below `limit`, plus D13 additions).

Everything not shown is still counted in the existing `(+N more candidates; raise limit or narrow path)` line,
so the model can ask for it. Nothing is removed from the pool.

### 4.2 The evidence line

Only a candidate whose position improved because of meaning gets the line. It uses the existing `   why:` slot
of `fd_show`, so the neighbour-note parser and the result format are unchanged. Exact formats:

```
   why: semantically close to your description (similarity 0.71; rank 9 by words). Unverified: read it before relying on it.
```

```
   why: semantically close to your description (similarity 0.68; no query word matches). Unverified: read it before relying on it.
```

```
   why: via src/x.c (symbol foo_bar); also semantically close to your description (similarity 0.66)
```

Rules for the line:

- First form: the candidate was in the pool and moved up. `rank 9 by words` is its position in today's order.
- Second form: the candidate entered from outside the pool (D13). It says plainly that no word matched.
- Third form: the candidate entered through a graph edge and was also promoted; the factual evidence comes
  first.
- `similarity` is the cosine with two decimals. Never a percentage, never the word "confidence", never
  "match" or "relevant". A percentage reads as a probability, and the research showed no score here behaves
  like one.
- The line names no model and no method beyond "semantically close". The reader of the line is the large model;
  it must not learn a product name from it (section 8).
- A candidate that did not move gets no semantic text, even if its similarity is high.
- The line costs about 110 characters of the 1500-character `Find` budget; with the promotion cap that is at
  most about 330 characters (3 lines) in a default call. The budget is not raised.
- The closing line of `Find` is unchanged.

### 4.3 What is forbidden

- No candidate list before the model asks (D26). No note prepended to the user turn built from embeddings.
- No intercept that replaces what the model asked for with what the embedding prefers.
- No exclusion: the embedding cannot make a lexical candidate disappear (lexical floor, counted remainder).
- No statement of fact derived from similarity in any `[katana: ...]` note. The `[katana: ...]` notes of
  `src/agent/loop.c` (plan, repetition, neighbours, todo) stay fact-based.
- No use of similarity as an abstention or confidence test.

## 5. Failure modes and safeguards

| Failure | Detection | Degradation |
|---|---|---|
| Model file absent, unreadable, or not the expected architecture | checked once per process at the first qualifying call (path, GGUF magic, architecture, vector dimension) | embeddings off for the process; `Find` is today's `Find`; one line on stderr, nothing in the tool result. Every other feature is untouched |
| Vectors from another model or cache version | file name fingerprint and header fields (3.3) | file ignored; rebuilt on demand |
| Corrupt cache file | header check, record length check, torn tail | bad records dropped; never an error to the model |
| Truncated chunk (text longer than `L_MAX`) | token count at embedding time; flag in the record | the vector is used, but a truncated candidate cannot enter from outside the pool (D13); path and symbols are first in the text so they survive the cut |
| Mixed language (Portuguese query, English code, Portuguese comments) | none needed for the model, which is multilingual by its description; the risk is the opposite: Portuguese comments or docs outrank the code | documentation files keep today's treatment; a promoted candidate that is a `.md` file counts against the promotion cap like any other; measure before trusting (section 7) |
| Confident false positive (high similarity, wrong chunk; typical when the thing asked about is absent) | cannot be detected: no score separates right from wrong (3.6 of the research) | structural limits instead: lexical floor, promotion cap, the "Unverified: read it" wording, and the curriculum teaching the model to read before affirming or editing |
| Pressure on the large model's context | the `Find` answer stays under `KATANA_FIND_CHARS` (1500); evidence lines are counted inside it | when the budget is short the evidence line is dropped before the matched lines are |
| Latency the owner notices | wall clock per call and per request; timeout counter | `T_MAX`, `P-TREQ`, D12 (two timeouts turn it off for the session); with `KATANA_FIND_TIME=1` the stderr line reports embedding time, chunks embedded, cache hits |
| Memory | fit rule at load (3.5); `mmap` so the file is reclaimable | does not load if it does not fit; unloads after `P-IDLE` |
| Slower generation because of background work | "large model busy" flag | warming and loading pause; warming is off by default |
| Different result for the same query on two runs (cold then warm cache, or a timeout) | inherent | accepted for interactive use; for tests and for the curriculum the feature must be deterministic or off (section 8) |
| The model stops reading and trusts the first line | only measurable in agent runs | wording of the line; curriculum cases where the promoted candidate is wrong |
| Windows | `Find` does not exist there yet (`docs/tools.md`) | nothing to do until `Find` exists |

## 6. Configuration

Default: **off**. Nothing below changes behaviour until the owner sets `KATANA_EMBED=1`.

| Variable | Default | Meaning |
|---|---|---|
| `KATANA_EMBED` | `0` | `1` turns the filter on for `Find` search. Anything else: off |
| `KATANA_EMBED_MODEL` | unset | path of the GGUF file. Unset: look for an embedding model in `<models folder>/embed/`, where the models folder is the existing rule (`KATANA_MODELS`, `./models`, `~/.katana/models`; `docs/handoff.md`) |
| `KATANA_EMBED_MS` | `1500` | `T_MAX`, per call |
| `KATANA_EMBED_REQ_MS` | `4000` | `P-TREQ`, per request |
| `KATANA_EMBED_POOL` | `24` | `N_POOL` |
| `KATANA_EMBED_TOKENS` | `256` | `L_MAX` |
| `KATANA_EMBED_WARM` | `0` | `1` allows background warming while waiting for the user (D20) |
| `KATANA_EMBED_GPU` | `0` | `1` runs the embedding model on the GPU. For measurement only (note 3) |
| `KATANA_EMBED_THREADS` | all cores | threads for the embedding model |
| `KATANA_EMBED_CACHE_MB` | `64` | cap of the vector cache per folder |
| `KATANA_EMBED_TUNE` | unset | research string: `gap=0.5,margin=0.3,k=60,wsem=1,keep=3,promo=3,add=2` |
| `KATANA_EMBED_WHY` | `1` | `0` reorders without printing the evidence line. For A/B measurement only; not for use |

How the owner turns it on today: the file is outside the models folder, so
`KATANA_EMBED=1 KATANA_EMBED_MODEL=/mnt/dados/embed-models/qwen3-embedding-0.6b/Qwen3-Embedding-0.6B-Q8_0.gguf katana ...`.

Open point: the catalog (`katana pull`) holds only the two large models by the owner's decision. Whether the
embedding model becomes a catalog item is the owner's call; this design does not need it.

## 7. What to measure to decide (list only)

No campaign is proposed. For each provisional parameter, the smallest measurement that would fix it:

| Parameter | Minimal measurement |
|---|---|
| `P-SPEED` | `katana embed` over about 100 real chunks of one repository at 256 tokens, CPU, with the large model loaded and idle; tokens/s for batch sizes 1, 8, 24 |
| `P-QUERY` | the same command on 20 short queries; median time |
| `P-LOAD`, `P-LOADMAX` | time to the first vector after dropping the page cache, and again warm |
| `P-RSS` | peak private memory of that run |
| CPU or GPU (D19) | large model `bench` tokens/s with and without the embedding model on the GPU, alternating in the same hour; and `P-SPEED` on both |
| Slowdown from concurrency (D20) | large model tokens/s while one embedding batch runs on the CPU, alternating |
| **Whether to build this at all** | on the existing 46-question sample (`tools/findbench`), offline: recall of gold files at 5 and 10 for (a) today's order, (b) rank fusion over the pool of 24, and the same on the hop files only. If the hop slice does not move, stop |
| `N_POOL` | in the same run: fraction of gold files present in the pool at 12, 24 and 40. This bounds what any reorder can gain |
| D13 and background warming | in the same run: recall when every chunk has a vector and the pool is ignored (pure dense retrieval), on the hop files. If this is far above the reorder, the full index matters and on-demand is not enough |
| `L_MAX` | the same recall at 128, 256 and 512 tokens |
| `P-RRFK`, `P-WSEM`, fusion against weighted sum | the same recall for k in {10, 60} and weight in {0.5, 1, 2}; one weighted-sum row for comparison |
| `P-GAP`, D6 (`P-MARGIN`) | split the same questions by `G` and by "covered top": how often the reorder changes the top 5, and in which direction, in each group |
| `P-KEEP`, `P-PROMO` | count, in the same run, the gold files lost by the floor and by the cap |
| `P-STORE` | top-10 overlap of the order with int8 vectors against float32 on the same pool |
| `T_MAX`, `P-TREQ` | not measurable offline: they follow from `P-SPEED` and from how many rounds a better first candidate saves in agent runs with the large model |
| Evidence wording | agent runs only: does the model read a promoted candidate before using it |
| Query instruction text | the same recall with two or three instruction wordings, Portuguese and English queries |

The 46-question run needs only the embedding model and the Python bench; it does not run the large model.

## 8. Impact on the training curriculum

The large model must see the new evidence line before it meets it in use, and it must learn the treatment, not
the tool behind it. Nothing the large model reads may name the embedding model or any method: the line says
"semantically close to your description", the prompt already says "Find searches the project by meaning"
(`src/agent/prompt.c`), and that is all. If the embedding model is replaced, no training text changes.

New situations, as an extension of the `x-find6` family (`docs/curriculum.md`, version 6):

| Situation | What the conversation shows |
|---|---|
| `sem-right` | the request describes a function in other words (half the time in Portuguese over English code); `Find` returns a candidate with the semantic `why:` line; the model reads the pointed lines with `offset`/`limit`, confirms, answers with `file:line`. It never affirms from the `Find` result alone |
| `sem-wrong` | the promoted candidate is the wrong one; the model reads it, says it is not what was asked, then reads the next candidate or uses `Grep` with a code term, and answers with the right `file:line`. Same shape as today's `confuse` |
| `sem-edit` | a change request ("change the function that ..."): `Find`, read the whole definition, `Find mode=refs` for the uses, `Edit`, run the tests. The read happens before the edit even when the similarity is high |
| `sem-absent` | the thing asked about does not exist in the project; `Find` still returns a semantically close candidate; the model reads it, finds it is something else, checks with `Grep`, and says the project has no such thing. This is the confident false positive |
| `sem-two` | a promoted candidate and a lexical first candidate point to different files; the model reads both in the same turn (as in `x-batch`) and decides from what it read |
| `sem-none` | the same requests with no semantic line in the result (filter off or skipped); the model behaves the same. This keeps the model independent of the feature |
| unchanged | `exact`: a known name goes to `Grep`, never to `Find`, with or without the filter |

Rules for the answers in these conversations:

- The final answer never quotes the similarity and never says the engine "found" something by meaning; it cites
  what was read, with `file:line`.
- The similarity number is never used to choose between two candidates in the visible reasoning; reading is.

Practical problem to solve before generating: the curriculum's `check` replays every conversation through the
engine and compares byte for byte. A result that depends on an optional model, on cache state and on a time
budget is not reproducible. Proposal (uncertain, the owner decides): a test switch that makes the vector source
deterministic and model-free for generation and `check` (fixed vectors per chunk text, from a table inside the
generator), so the evidence line is still produced by the engine's real formatting code and never written by
hand. The time budget is disabled under that switch. The real model is not needed to generate or to check.

Mix: `sem-none` and the existing `x-find6` situations must stay the majority while the filter is off by
default; the semantic situations are a minority that grows only if the owner turns the filter on for good.

## 9. Main uncertainties

1. **Whether it helps at all.** The only embedding evidence in the repository is negative, and it concerns
   static vectors (folder co-occurrence, the large model's input table), not a trained retrieval model. The
   expectation that this model bridges Portuguese questions and English code is an expectation. One offline run
   decides (section 7).
2. **A reorder cannot reach what the words never reached.** With 21% of gold files sharing no informative term
   with the question, and R@10 of 44.9% for the pool's source, many right files are outside any pool. Reordering
   fixes order, not recall. D13 and a full index are the only paths for those files, and both need vectors that
   on-demand embedding does not produce.
3. **Cost.** Every time figure depends on `P-SPEED`, `P-LOAD` and `P-RSS`, all unmeasured here.
4. **D6.** The skip by agreement is a cost rule with weak support; the research found that agreement and margin
   do not predict correctness.
5. **Behaviour of the large model.** Whether it reads a promoted candidate before using it, and whether the
   "Unverified" wording is enough, is unknown until agent runs.
6. **Determinism** for tests and for the curriculum needs the switch of section 8 or the feature stays out of
   generated data.
