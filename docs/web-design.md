# Web reading: design (WebSearch, WebFetch)

Status: design, 2026-11-25. Nothing here is implemented yet except where marked. Nothing was measured.

## 1. Why

A session on 2026-11-25 opened one documentation page 7 times and always got 13 bytes. Causes found in the code:

- `WebSearch` reads result pages in parallel **without running JavaScript** and wrote the empty shell into the disk cache; `WebFetch` then read that cache entry and never called the browser. (Fixed: a cached page with less than 200 bytes of text no longer counts; the address key drops the fragment and the trailing slash.)
- The same page was asked with a different `prompt`/`render`/`offset`, so the repeated-call detector saw a different call each time. (Fixed: for `WebFetch` only the address and `offset` identify the call.)
- The result was plain text. The model could not tell "this is the whole page" from "this is an empty shell", and nothing told it where to go next.

The model is the consumer. What it needs from a web tool is **the answer and where it came from; when the source fails, the engine changes route before the model sees the failure.**

## 2. What common practice looks like

Web-reading services and agent tools converge on these points (read from their public documentation on 2026-11-25):

- Output is markdown of the main content, with a size limit.
- Reading is guided by a question: only the passages that answer it come back, ranked, a few per page.
- Cache with a freshness control and a way to bypass it.
- Failures are structured per address (status, typed error), never empty text.
- JavaScript pages are handled by escalating the reader: plain request, then a browser, then wait/selector, then a better route. None of them documents a detector of "empty shell" pages; the user is told to raise a timeout. A cheap detector in the engine is therefore more than they do.
- Against loops, the documented mechanism is a limit that counts failures, plus the cache bypass.
- Research on corrective retrieval: a light evaluator scores what was retrieved and, when it is poor, triggers another source and keeps only the relevant parts.
- Research on retrieval quality: word search plus embeddings plus reranking beats either alone; giving each chunk its heading path as context reduces misses.
- Sites that publish machine-readable twins: a `llms.txt` index, a `.md` version of a page, `rel="alternate" type="text/markdown"` or a `Link` header.

## 3. Design

### 3.1 Contract: the result always says its state

`WebFetch` returns, before the text, one line:

`[web: ok|partial|shell|empty|blocked|login|error, via <route>, <n> of <m> bytes of text, quality <q>]`

and, when not `ok`, a **next route computed by the engine** with real addresses (the same page as markdown, the repository of the project, a search for another source), not the generic "try another source".

### 3.2 Quality verifier (deterministic, in C)

Runs on every result before the model sees it. Signals: bytes of text, ratio text/HTML, empty application root (`id=app|root` with no children), `<noscript>` asking for JavaScript, title only, repeated boilerplate. Output: `ok`, `shell` or `empty`. It is the evaluator of the corrective loop: its verdict picks the next route.

### 3.3 Ladder of routes (engine, automatic)

Each step runs only if the previous one fails the verifier:

1. Plain request, main content to markdown.
2. Browser without window (exists: `wn_render`), also called automatically when the verifier says `shell` (today it depends on a size test inside `wn_ingest`).
3. Machine-readable twins: `<address>.md`, `index.md`, `/llms.txt`, alternate link, `Link` header. Cheap; try before searching.
4. If the page links to its own source repository ("edit this page", repository link), read the source file raw.
5. A web search for the same subject on another site (corrective step), reading with the same ladder.

The result says which route worked. Site-specific adapters (code hosting, papers, PDF, forums) are **not** in the first phase: no source was found that describes them and no real case asks for them yet.

### 3.4 `ask`: reading guided by a question

`WebFetch(url, ask)`: `ask` is what the model needs to know. The engine returns the best passages instead of the first 4 KB. This already exists in part (`tools/webfetch.c`: blocks of up to 1500 characters, word score x4, the 12 best candidates reranked by the embedding service, cosine x20, a block with no word in common needs cosine >= 0.60; the question comes from `find=` or from the last `WebSearch`). Changes:

- `ask` becomes an explicit argument (falls back to `find` and to the last search).
- Each block gets its **heading path** ("Installation > Linux > Step 2") as the title passed to `es_rank` (today `NULL`).
- Code blocks, commands and tables stay verbatim and are marked; the heading outline and the page date/version come first.

### 3.5 Role of the embedding service

Unchanged rule (`docs/embedding-usage.md`): the embedding only reorders and suggests; it never decides and never excludes. In this design it has two uses: ranking the blocks of a page (3.4) and, as one extra signal for the verifier, a **low best-cosine against `ask`** meaning "this page does not answer it": that only triggers the next route, it never drops content. Without a model, without time or with `KATANA_EMBED=0`, only words and the deterministic verifier remain. Cost is unmeasured (first use loads the model; the embedding of 12 blocks on a busy CPU can take seconds; the content cache helps repeats).

### 3.6 Ledger of what the model already received

As for `Read` (`KNF_READ_SEEN`): per address and per section, the engine remembers what was delivered in this request. A repeat returns only what is new ("you already have sections 1-2; here are 3-4") or, when nothing is new, says so and gives the next route from 3.3. This replaces the repeated-call heuristic added on 2026-11-25 for `WebFetch`.

### 3.7 One reader for search and fetch

`WebSearch` reads the best results with the same ladder (browser allowed, bounded by the time budget), ranks passages against the query and returns passages with their address. Today the parallel reader has no JavaScript and the page it caches is worse than the one `WebFetch` would get. The cache keeps one entry per address with a quality mark; a `shell` entry is never served and is replaced by a better read.

### 3.8 Context discipline

Default answer 3 to 4 KB. The rest stays in the cache, addressable by section ("section 4 of the previous result"). Failures count against a per-request limit of fetches, as the repeat cut does today.

## 4. Order of work

1. Contract (3.1), verifier (3.2), ladder steps 1-3 (3.3), cache rule and shared reader (3.7). Fixes today's case deterministically.
2. `ask` explicit, heading path as chunk title, verbatim code (3.4), cosine as verifier signal (3.5).
3. Ledger (3.6) and context discipline (3.8).
4. Ladder steps 4-5 and, only if a real case asks, site adapters.

## 5. Not confirmed

- Thresholds for the verifier and the 0.60 cosine on web pages (copied from the memory ranking by analogy).
- Whether services re-read automatically when a result is empty (their documentation does not say).
- Cost of the embedding step on pages with many blocks.
- No measurement exists for any of this; it follows the project rule (compile only) until the owner asks to measure.
