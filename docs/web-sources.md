# Web sources: registry and router (design)

Status: implemented 2026-11-25 (steps 1 to 4 of section 7, except the local index of section 5, which is pending). It compiles; nothing was measured (the engine was only run, without a model, on a handful of searches to see that each reader parses its source). Code: `src/agent/websrc.c` (registry, router, readers, verifier, delivery), `src/agent/websources.inc` (built-in registry, a data file), `src/uioe/conn_web.c` (the `web.search` connector), `katana sources [question]` (lists the registry; with a question, prints the classification and the route). This file is documentation for people; the engine does not read it. What the engine reads is the **source registry** (section 3), a data file.

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

A tenth field, optional, is the **map**: space-separated `key=value` pairs the readers need (an extension that reality asked for: a JSON API cannot be read without knowing its fields):

- `json`: `items=<path to the array>` (`.` is the root, dotted paths, `*` = first member), `title=`, `url=`, `snip=`, `date=`. A value is a path, a template with `{path}` (for example `https://example.org/q/{question_id}`), alternatives separated by `,` (first non-empty) or, in `snip`, parts joined by `+` (shown as `name: value`).
- `html`: `parser=result-a | b-algo | title-s | links` (markup shapes; `links` is the generic reader: anchors with 12+ characters of text pointing to other sites).
- WebFetch twins: `page=<address with {1} {2}>` and `fetch=<address with the same captures>`, optionally `fetchpath=<json path of the text>` (section 4).

Dates are understood as ISO 8601, RFC 822 or seconds since 1970. A feed declaring ISO-8859-1 or windows-1252 is converted to UTF-8. `KATANA_SOURCES` points at another owner file; `KATANA_SEARX=<base>` adds a JSON metasearch of the owner's (`own-metasearch`) as the first general source. Invalid lines are skipped and logged to the agent log.

The built-in file was checked address by address with curl on 2026-11-25 (section 8).

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

## 8. Implementation notes (2026-11-25)

What exists, and where it differs from the design above:

- **Classifier**: lives in `websrc.c` (`ws_classify`), not in `uioe/classify.c`: the web layer is compiled before the pre-model layer and the UIOE classifier works on a whole request with its context, not on a search query. Word lists in Portuguese and English decide topics, language (stopwords and accents), country (place names; Portuguese defaults to `br`), recency (words like today/latest, or the current or previous year) and entities (`owner/name`, names with versions, quoted text, capitalized words in the middle of a sentence; `owner/name` adds the code and ai topics).
- **Router**: score = 10 per matching topic + 2 x trust, +4 language match (-6 mismatch), +4 country match (-8 mismatch), recency bonus for `hours` sources; at most 4 specific sources, at most 2 query-less feeds, and nothing more than 12 points below the best. Feeds without `%s` are used only for news questions.
- **Parallel query**: every source is a `curl` child started at once (8 s each); a whole `WebSearch` keeps a 14 s budget for the route changes. The pipes are read in order, so a slow early source can stall a large later one (no `poll` yet).
- **Rate limit**: per process, a one-minute window per source; an HTTP 429 or 403 pauses the source for 120 s. A source skipped by its limit is named as failed (`limit of N requests per minute reached`).
- **Verifier**: sponsored links are dropped (`/y.js`, `ad_provider=`, `ad_domain=`, `/aclk?`, `/aclick?`, `&adurl=`). A result with none of the query words (title, snippet or address, accent-folded, word-start match) is dropped only if its cosine against the query is under 0.30; without the embedding service, only general-search results with no query word are dropped (a specific source's API already matched the query). Score = (word fraction + 0.3 x position + 0.8 x cosine) x (0.5 + 0.1 x trust) x freshness (only when the question asks for recent facts: 0.3 + 0.7/(1 + age in hours/48); unknown date 0.5). The 0.30 threshold is a guess.
- **Ladder**: when nothing survives, the next unused general source runs, then the first general source with the query reduced to its 4 main words, inside the budget.
- **Delivery**: the first line of `WebSearch` is `[search: ok|partial|empty, sources: a, b (c failed: reason; d failed: reason), n results]` (one line per query when `queries` is used, followed by `for "query"`); each result ends with `(sources: id[, id]; date YYYY-MM-DD)`.
- **WebFetch**: before the plain request, an address matching a source's `page=` is read from its `fetch=` twin (README of a repository or model card as markdown, encyclopedia article as plain text, paper abstract from the papers feed); the address of a registered feed is read with the feed reader. The route shows as `via src:<id>`. Less than 150 bytes falls back to the normal ladder. The "another source" step of the ladder (`[next: ...]`) runs the router on the page subject when at least 8 s are left and names up to 3 addresses from other sites.
- **UIOE**: `web.search` is bound to `cw_search` (`src/uioe/conn_web.c`); results become dated low-confidence `UN_FACT` nodes (20 + 8 x trust, at most 60, tier 4, never verified); the state line goes to `limits`. The policy is unchanged: the connector runs only with `KATANA_UIOE_WEB=1`.
- **Pending**: local content index for slow sources (section 5, second item); a `docs` source (no open documentation search API was chosen); `poll` on the source pipes.

### Address check (curl, 2026-11-25)

| Source id | Works | Notes |
|---|---|---|
| code-hosting (repository search API) | yes, JSON | 10 searches/min without a key (header); registry limit 8 |
| q-and-a (Q&A excerpts API) | yes, JSON | 300 requests/day without a key (`quota_max`); registry limit 4/min |
| packages-js, packages-rust (package registries) | yes, JSON | no key |
| tech-news (tech news search API) | yes, JSON | no key; Ask posts have no url, the item page is used |
| models-hub (models API) | yes, JSON | 500 requests per 5 min window (header) |
| papers (preprint API) | yes, Atom | the site asks for one request every 3 s |
| encyclopedia-pt, encyclopedia-en | yes, JSON | extract API used for WebFetch |
| news-br-1, news-br-2, news-br-3 | yes, RSS | news-br-3 is ISO-8859-1 (converted) |
| news-br-search, news-search-en (news search feed) | yes, RSS | links are redirect addresses of the aggregator; WebFetch of them may need the browser |
| news-world-1, news-world-2, news-world-3, news-world-pt | yes, RSS | |
| general-1, general-2 (HTML result pages) | yes | ads appear as `y.js` links in general-1, filtered |
| a third general HTML engine (`title-s` reader) | **no** | answers a captcha/challenge page; not embedded, reader kept |
| a UK newspaper world feed | **no** | HTTP 403 to this client |
| a scholarly search API | **no** | HTTP 429 without a key |
| an open scholarly catalog API | works, **not embedded** | now meters usage in dollars per key/day; papers come from the preprint feed instead |
