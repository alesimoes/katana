# The coder module: entry point

`src/coder/` is katana's code intelligence: a per-root index of a folder tree (catalog, text search, symbols,
references, links) and the commands and tools that query it. This page is the single entry to the module: what
it is, the `katana coder ...` commands, where the data lives, and which document holds the details.

| Document | Holds |
|---|---|
| `docs/coder.md` (this page) | Overview, command reference, environment keys, source map |
| `docs/code-index-lifecycle.md` | How the index is built, kept fresh, bounded and shown to the model: roots, crawler, levels, ignore rules, transparency, storage format |
| `docs/code-intelligence.md` | The query side: parsers, search channels, symbol queries, links by text key, knowledge base |
| `docs/code-intelligence-architecture.md` | Why: requirements, alternatives considered |
| `docs/tools.md` | The agent tools that sit on top (`Find`, `Symbols`) |

## What it does

- **Index roots.** A root is a folder where the user ran `katana init`. Its index lives inside it
  (`<root>/.katana/coder/`) and covers the subtree. A folder with no initialized ancestor is indexed in the home
  (`~/.katana/coder/<key>/`) only when it is visited, and never gets a `.katana/` inside.
- **Levels.** The tree is indexed breadth first, in levels, by a crawler that runs on demand and exits when it is done:
  N0 catalog, N1 text, N2 symbols and references, N3 links. Each level is queryable as soon as its batch is published.
- **Freshness.** A session watches the folders it uses (inotify; periodic stat sweep as fallback) and the agent tools
  report their own writes. Queries do not walk the tree; every file a reply cites is re-read from disk.
- **One index.** `Find` reads the same store as `Symbols` (no second cache). The file graph is a section of the store.
- **Transparent ignoring.** Whatever the index leaves out is recorded with its reason and size, shown by
  `katana coder ignored` and by the `tree` action, and can be included or excluded by the user.

## Commands

`katana init [folder] [--force] [--remove]` marks a folder as an index root (the agent's `/init` does the same):
creates `.katana/coder/ROOT` and a `.gitignore` inside it (the marker is not versioned), moves an existing home
index of that folder inside it, and writes `KATANA.md`. It refuses the home folder and `/` without `--force`.
`--remove` deletes the marker and the local index and keeps `KATANA.md`.

`katana coder <sub> [folder]` (folder defaults to the current one; it is resolved to its root and the query is
restricted to the folder's prefix inside it):

| Sub | What it does |
|---|---|
| `status [folder]` | Where the index comes from (local init, ancestor init, home), segments, live files, per-level progress, crawler and watch state, ignored summary |
| `crawl <folder> [--prio <prefix>] [--changed <path>...] [--sweep] [--stop] [--quiet]` | The crawler. One per root (file lock); a second call hands its request to the running one. Exits after the settle time (`KATANA_INDEX_SETTLE_MS`, 2000) when every level is covered |
| `index [folder]` | Synchronous incremental scan and write of the catalog and the search and symbol sections; ends with the ignored count |
| `tree [folder] [--depth N] [--limit N] [--json]` | Folders of the project with the index depth of each, a summary and top symbols. Reads only the index (N0) |
| `list [--json]` | Known roots (init and home): size, files, age, last use, crawler state |
| `forget <folder>` | Deletes the home index of a root. An initialized root is refused: use `katana init --remove` |
| `gc [--dry-run] [--max-mb N]` | Cleans the home: roots whose folder is gone for 30 days, roots absorbed by an init or by a higher home root, orphan store folders, quarantine copies older than 7 days, then least recently used roots (idle 14 days or more) until the home fits the limit (`KATANA_INDEX_MAX_MB`, 2048). Never touches an initialized root |
| `ignored [folder] [--files] [--json]` | What the index leaves out, why, how many files and bytes, and the command that changes it |
| `include <path>` / `exclude <path>` | Writes the `overrides` file next to the index. Secret files can never be included |
| `verify [folder]` | Checks the checksums of every section and the manifest |
| `compact [folder]` | Merges all segments into one |

Query commands over the same index: `search`, `find-name`, `def`, `refs`, `callers`, `callees`, `uses`, `outline`, `at`,
`deps`, `impact`, `flow`, `symbols`, `tokens`, `kb` (see `docs/code-intelligence.md`).

## Where the data lives

| Path | Content |
|---|---|
| `<root>/.katana/coder/ROOT` | Init marker (not versioned) |
| `<root>/.katana/coder/store/` or `~/.katana/coder/<key>/store/` | The store: `MANIFEST`, `seg/*.cbs`, `LOCK`, `CRAWL.LOCK`, `tmp/` |
| `.../overrides`, `.../ignored` | User rules (`include`, `exclude`, `scoped` lines) and the ignored registry, next to the store |
| `~/.katana/coder/roots/<key>.root`, `~/.katana/coder/inits/<key>.root` | Registry of home roots and of initialized roots (what `list` reads) |
| `~/.katana/run/coder-<key>.{sock,state}`, `watch-<pid>-<key>.state` | Crawler socket and state; per-session watch state |

`<key>` is a hash of the real path.

## Environment keys

Limits and behavior: `KATANA_INDEX_MAX_DEPTH` (32), `KATANA_INDEX_MAX_FILES` (500000), `KATANA_INDEX_MAX_TEXT_FILES`
(200000), `KATANA_INDEX_THREADS`, `KATANA_INDEX_BATCH_MB`, `KATANA_INDEX_SETTLE_MS` (2000), `KATANA_INDEX_GENERATED=1`
(detect generated and minified files; off), `KATANA_INDEX_MAX_MB` (home size limit, 2048), `KATANA_INDEX_MIN_FREE_MB`
(pauses writing on a full disk), `KATANA_INDEX_INCLUDE_MAX_FILES` (cap for a scoped include inside an ignored
folder). Freshness: `KATANA_INDEX_WATCH` (`off`, `poll`), `KATANA_INDEX_WATCH_MAX` (watch budget),
`KATANA_INDEX_SWEEP_S` (reserve sweep period, 600 s with full watch coverage), `KATANA_INDEX_WAIT_MS` and
`KATANA_INDEX_FRESH_MS` (how long a query waits for the crawler). Diagnostics: `KATANA_INDEX_TRACE`, `KATANA_FIND_TIME`.

## Source map

`src/coder/`: `roots.c` (root resolution, init, absorption), `store.c` (segment store), `storedocs.c` (catalog, scan,
`index`/`status`/`verify`/`compact`), `ignore.c` (ignore tables and overrides), `crawl.c` (crawler), `watch.c` (session
watch), `fresh.c` (cited-file check), `indexctl.c` (when a query revalidates; cached views), `maint.c` (`list`,
`forget`, `gc`, `ignored`, `include`, `exclude`, store health), `statemap.c` (per-folder level map for the status
line and `tree`), `findstore.c` and `graph.c` (`Find` over the store; the file graph), `text.c`, `search.c`, `keys.c`,
`parse.c`/`parse2.c`, `symindex.c`, `symquery.c`, `symtool.c`, `flow.c`, `kb.c`; `coder.c` dispatches `katana coder`.
