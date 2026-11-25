# Web sources: registry and router (design)

Status: design, 2026-11-25. Not implemented. Nothing was measured. This file is documentation for people; the engine does not read it. What the engine reads is the **source registry** (section 3), a data file.

## 1. Why

The quality of what the model answers is capped by the quality of what the web tools give it. `WebSearch` today scrapes one general search page; when its format changes or it blocks, one source is left, and its results are mixed with off-topic pages (a telecom operator's login pages for a query about a developer tool). A general search engine is the worst front door: noisy, fragile, and impossible to tell from a good result.

The design: a **module shared by `WebSearch` and `WebFetch`** that knows many specific sources, routes each question to the best ones, queries them in parallel, verifies and ranks what comes back, and uses the general search only as a last resort. It follows `docs/web-design.md` (state line, verifier, ladder of routes) and plugs into the pre-model layer (`src/uioe/`: classify, route, connectors).

## 2. Responsibilities

| Piece | Does | Where |
|---|---|---|
| Registry | says WHERE: which sources exist and how to query each | data file (section 3) |
| Router | decides which 2 to 4 sources fit a question | C, a web connector in the pre-model layer |
| Readers | know HOW to read each kind of answer (JSON API, feed, HTML result page, sitemap) | C, one reader per kind |
| Verifier + ranking | drops off-topic and spam, orders by words + embedding + source trust | C (shared with web-design.md 3.2 and 3.4) |
| Delivery | state line, origin of each result, size limit | C (web-design.md 3.1) |

The embedding service only reorders and suggests (`docs/embedding-usage.md`); it never decides and never excludes.

## 3. Registry format

Two files, merged at start: the built-in defaults compiled into the binary, and `~/.katana/sources.txt` (the owner's own; same format; a line with the same `id` replaces the built-in one; `-id` removes it). One source per line, fields separated by ` | `, `#` starts a comment:

```
id | topics | country | lang | kind | query template | freshness | trust | limit/min
```

- `id`: short stable name (`code-hosting`, `models-hub`, `news-br-1`, `encyclopedia-en`).
- `topics`: comma list from a fixed vocabulary: `code, ai, news, world-news, encyclopedia, papers, q-and-a, docs, general`.
- `country`, `lang`: `br`, `us`, `*`; `pt`, `en`, `*`.
- `kind`: how to read the answer: `json`, `feed` (RSS/Atom), `html`, `sitemap`.
- `query template`: address with `%s` for the encoded query (feeds use no query: the router filters the entries).
- `freshness`: `hours`, `days`, `years` (decides whether a cached answer is acceptable and whether "latest" questions use it).
- `trust`: 1 to 5. It weighs in the ranking.
- `limit/min`: requests per minute the engine may spend on that source.

The built-in file is checked address by address before it ships (none of the addresses was verified when this was written).

## 4. Routing

1. **Classify the question** (rules and lists, no model; extends `uioe/classify.c`): topic (code, AI, news, general knowledge, papers, how-to), language, country, whether it asks for recent facts, which entities it names (repository, model, package, person, company).
2. **Pick sources**: topics match, then country/language, then trust; 2 to 4 sources; the general search is added only when fewer than 2 specific sources fit or all of them came back empty.
3. **Query in parallel** with a time budget and each source's rate limit.
4. **Read** with the reader of the source's `kind`. For `feed` sources the router filters entries by the query words and date.
5. **Verify and rank**: drop results with no query word in title or snippet and a low embedding match; drop sponsored results; order by relevance x trust x freshness (when the question asks for recent facts).
6. **Deliver**: `[search: ok|partial|empty, sources: a, b (c failed), n results]` and each result with its source. A failed source is named, never hidden.
7. **Change route**: if the verifier rejects everything, the next ladder step runs (another source of the same topic, the reformulated query, the general search) before the model sees the failure.

`WebFetch` uses the same registry in two ways: an address that belongs to a registered source gets that source's reader (for example a feed or an API instead of its HTML page), and the "search for another source" step of the ladder calls the router.

## 5. Two kinds of "index"

- **Registry of live query addresses** (this design, first). Nothing is crawled; the cost is the request.
- **Local content index** for sources that change slowly (documentation sites by sitemap or the machine-readable index some sites publish): crawled on demand, stored under `~/.katana/`, searched with words + embedding as `Find` does for code. Second phase.
- For news the best "index" is the feed: open, stable, no HTML scraping.

## 6. What this does not do

- It does not use a general search engine that needs an API key or a paid plan (project rule: nothing proprietary in the engine). If the owner wants one, it enters as a `kind` plus a key in the owner's file, as an explicit decision.
- It does not automate pages that forbid it. Sources are chosen among those with a public API, a feed or a plain result page.
- No measurement exists. The first thing to check after implementing is how often the router picks the right source on real questions, and what a source costs per request.

## 7. Order of work

1. Registry loader (built-in + owner's file), router on the classifier, readers `json` and `feed`, the verifier on search results (words + embedding, sponsored results), the state line for `WebSearch`.
2. First built-in sources: code hosting, Q&A, AI models hub, encyclopedia, a few news feeds (Brazil, world), papers.
3. `html` reader for sources without an API, general search as the last step.
4. `WebFetch` using registered readers; local index for slow sources.
