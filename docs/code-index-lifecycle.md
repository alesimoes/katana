# Code index lifecycle

How the coder index (`src/coder/`) is placed, built, kept fresh, bounded, queried and discarded, and what the
model is told about it. This is the state of the code, in nine parts (the nine build phases map to the sections
below). Commands and environment keys: `docs/coder.md`. Nothing here is a measurement; limits are the defaults in
the code.

## 1. Principles

- The index is a cache derived from the working tree: never wrong, never half-written. Immutable segments plus an
  atomically swapped manifest; one writer, lock-free readers.
- Queries never walk the tree. Something else (crawler, session watch, tool hints) keeps the index current.
- Breadth before depth: the whole tree is cataloged before any text is read, and text before symbols.
- Nothing is silently left out: ignored content is recorded with its reason and size.
- No migration: a damaged or older-format index is rebuilt (the source of truth is the tree).

## 2. Index root (phase 2, `roots.c`)

A root is a folder with the marker `<folder>/.katana/coder/ROOT`, created by `katana init`. `.git` and `KATANA.md` do not
define roots. The index of a root is `<root>/.katana/coder/store` and covers the subtree, except:

- subfolders with their own marker (a nested init is an independent index; the parent does not descend);
- git linked worktrees (a `.git` that is a file pointing into `worktrees/`): recorded as `separate-checkout`. Submodules
  and nested clones are real code and are indexed.

Resolution walks up from the real path: the first ancestor with a marker is the root and the query is restricted to the
path prefix inside it. Without any marker, the highest registered home root that is the path or an ancestor absorbs it;
otherwise the visited folder becomes a home root (`~/.katana/coder/<key>/store`, one store per visited root, registered in
`~/.katana/coder/roots/`). Only visited folders are indexed (working folder, cited folder, `path` of `Find` or
`Symbols`); the whole home is never indexed, and a folder without init never receives a `.katana/`.

`katana init` moves the home store of the folder inside it (rename; if it crosses devices, the home store is dropped and
rebuilt). When a root absorbs home roots below it, their stores are deleted once the ancestor store is written. `init` in
the home or `/` needs `--force`. Moving a project with init moves its index with it.

## 3. Levels and the crawler (phases 3 and 5, `crawl.c`)

One crawler process per root, elected by `flock` on `<store>/CRAWL.LOCK`, started on demand by the session or query that
needs it (`autoindex_dir` in the agent loop). It is not a daemon: it exits when every level is covered and the settle time
has passed. A second crawler hands its request to the first through the socket
(`PING`, `PRIO`, `TOUCH`, `SWEEP`, `SYNC`, `STATUS`, `STOP`; one line, one reply). It runs at low CPU and I/O priority and
parses with a few threads (`KATANA_INDEX_THREADS`).

| Level | Content | Published as |
|---|---|---|
| N0 | Catalog: name, size, date, language, folder records with a summary | `STS_DOCS` segment with hash 0, plus `STS_DIRS` |
| N1 | Text search: lexicon and postings (names, subwords, BM25, trigrams) | `STS_DOCS` (level 2) plus search sections; masks the N0 record |
| N2 | Symbols and references | produced by the same parse as N3 |
| N3 | Links (text keys, imports, graph) | N2 and N3 advance together (level value 4) |

The level is stored in bits 8 to 11 of `StoreDoc.flags` (0 = complete, from the synchronous path; 1 = N0; 2 = N1;
4 = N2+N3). Each batch (4096 files or 16 MiB) is a new segment, published by the manifest swap together with an
`STS_CRAWL` progress record, so progress and data are always consistent.

Order: the frontier, queues and bands are recomputed from the store at each batch. Bands: 0 hot (files requested by
`--prio`, touched by `--changed`, or changed during the run: all levels at once); 1 recent code (30 days), code under
`src/ lib/ app/ cmd/ pkg/ include/ internal/`, README and `KATANA.md`; 2 other code; 3 docs and configuration; 4 large
data, lock files, `*.min.*`, `*.map`, folders over 20 thousand entries. Inside a band, newest first. Segments are
compacted only between stages (after N0 above 24 segments; at the end above 8) and only the tail of the manifest.

Limits: depth 32, 20000 entries per folder, 500000 files, 200000 text files, binary, large and unreadable files are
cataloged by name only. Generated and minified detection exists and is off by default (`KATANA_INDEX_GENERATED=1`)
because it changes what search finds. Symbolic links to folders are recorded as `link` and not followed. Folder states:
`discovered, listed, ignored, nested-root, link, too-deep, too-many-entries, unreadable, gone, separate-checkout`.

The synchronous path (`katana coder index`) is the same scan and the same ignore decision function; it exists for
scripts and for the first query when no crawler is running.

## 4. Freshness (phase 4, `watch.c`, `fresh.c`, `indexctl.c`)

`index_ensure` is the single point that decides whether a query revalidates. In an agent session with an existing index
the session watch decides:

- one inotify descriptor per process, read without blocking before a query, after a tool that changes files, and while
  the prompt is idle (about every 3 s); no thread;
- one watch per indexed folder (same rules as the scan, shallower first); budget one quarter of
  `fs.inotify.max_user_watches` (`KATANA_INDEX_WATCH_MAX`). Beyond the budget or on `ENOSPC` the root is partially
  covered and the session requests a periodic sweep;
- changes go to the crawler as `TOUCH` (socket) or through a short `crawl --changed` process; more than 200 changes with
  no crawler, queue overflow, or more than 2000 events under one folder become a `SWEEP` (stat scan);
- the first visit of a session to a root with an index requests a `SWEEP` (changes made while no session was open);
- `.git/HEAD` or `.git/index` changing requests a sweep (branch switch); git is only a hint;
- the agent's own writes (`Edit`, `Write`, `MultiEdit`, `NotebookEdit`) and `Bash` call `index_touch`;
- a query waits for the crawler to drain at most 300 ms (2 s on a cold index) and then answers;
- without inotify (other systems, `KATANA_INDEX_WATCH=poll`, network or FUSE filesystems) or with
  `KATANA_INDEX_WATCH=off`: periodic sweep (`KATANA_INDEX_SWEEP_S`, reserve 600 s with full coverage).

Every file a reply cites is read again from disk for the evidence, and compared with the catalog (`fresh.c`): size and
mtime equal means as indexed; only mtime differs with the same hash means unchanged; otherwise it changed since indexed,
its path goes to the watch (becomes `TOUCH`) and the reply gets `[index: N files changed since indexed; shown from the files]`.
A symbol line that came from the index then reads "line NOT verified".

## 5. Ignore rules and transparency (phase 8, `ignore.c`, `maint.c`)

One decision function (`st_scan_decide`) serves the scan and the crawler. Precedence, highest first:

1. `overrides` file (`include`, `exclude`, `scoped`; written by `katana coder include/exclude`; a path covered by several
   lines is decided by the last one);
2. `.katanaignore`;
3. `.gitignore` (deepest rule, last line wins; `!pattern` re-includes what the built-in list would ignore);
4. the single built-in list of heavy folders;
5. nothing.

Built-in heavy folders, by group and with the reason shown to the user: version control and katana data (`.git .hg .svn
.katana`; never included, not even by override); agent work areas (`.claude`); installed dependencies (`node_modules
.venv venv site-packages bower_components .gradle .m2 .cargo`); build outputs (`build dist out target obj .next .nuxt
__pycache__ .pytest_cache .mypy_cache .tox coverage`); tool caches (`.cache .idea .vs .parcel-cache .terraform`);
credential folders (`.ssh .gnupg .aws`; never included). `vendor`, `ref`, `models`, `bin` and `lib` are not in the list.
Folders that are never in the list by default (`docs src lib include test tests config scripts tools .github
migrations ...`) are hidden only by a user rule, and `ignored` marks them "usually worth indexing".

Secrets: file names such as `.env*` (not `.example`, `.sample`, `.template`), `*.pem *.key *.p12 *.pfx *.jks *.keystore
*.kdbx *.tfstate *.tfvars *.secret`, `id_rsa* id_dsa* id_ecdsa* id_ed25519*` (not `.pub`), `.netrc .npmrc .pypirc
.htpasswd credentials secrets.*`, anything under `.ssh/ .gnupg/ .aws/`, and files whose first 200 bytes hold a private key
header. They are cataloged by name only; their content is never read and `include` refuses them.

Ignore reasons recorded: `gitignore, katanaignore, heavy-folder, excluded, binary, too-big, secret, nested-root,
separate-checkout`. The `ignored` registry (a text file next to the store) is produced by a counting pass that is the same
scan without listing files, plus a `readdir`/`lstat` inside each ignored folder (nothing is read; cap 200 thousand
entries or 1 s per folder). It runs at the end of `katana coder index` and of a crawler round, and is redone per folder
only when the folder mtime changed. Until then `status` says "not counted yet".

What the model sees: `Symbols action=tree` lists folders with their index depth and ignored folders with reason and
size; a NOT FOUND answer says when the missing name could be inside an ignored folder, and says "so far" while the scope
is partial; a `path` inside an ignored folder is indexed for that query (scoped include) with the note
`[index: <dir>/ was ignored (<why>); it is now indexed for queries whose path is inside it]`, or
`... has more than N files, so it was not included; set path to a smaller folder inside it` above
`KATANA_INDEX_INCLUDE_MAX_FILES`.

## 6. One index for `Find` (phase 7, `findstore.c`, `graph.c`)

`Find` has no index of its own: no `find-*.idx` cache and no second walk of the tree. Without a coder index `Find` answers with an error. Candidates come from the store lexicon and postings (about 100 files, `KATANA_FIND_CAND`)
plus the files that the name and text engine suggested and the files that define a name in the question. The snippet
index (40-line chunks, BM25 per field, symbols by rule) is built per query over those files only, read from disk; the
document frequencies come from the store lexicon. The file graph is the section `STS_GRAPH`, derived from N2/N3 and the
N1 lexicon. Consequence to keep in mind: a file outside the candidates never gets a snippet score.

## 7. What the model sees (phase 6, `statemap.c`, `symtool.c`)

The state map is aggregated per query from the live documents and the `STS_DIRS`/`STS_CRAWL` sections; no project file
is opened. For each folder (to the depth asked, up to 4 below the base) it counts files per level and languages; files
that are binary, large, secret or generated have no level to wait for.

`Find` and `Symbols` print at most one status line, first, and only when the queried scope is incomplete or stale (for
example the crawler announcements `[index <root>: N files found]`, `[index <root>: text ready]`,
`[index <root>: symbols and links ready (S s)]`, or `[index was damaged|of an older format and is being rebuilt]`). For a
small project that is complete and fresh the output has no extra line.

## 8. Storage health and maintenance (phase 8, `maint.c`)

- Format version in the manifest (currently 2): a store of a different format is deleted and rebuilt without a notice; a newer one is left alone.
- Damage (bad checksum): the store is moved to `store.bad-<time>` (one copy kept) and rebuilt; `[index was damaged and
  is being rebuilt]`. The crawler checks every checksum about every 7 days and rebuilds on damage.
- Full disk: writing pauses and retries every 60 s up to five times; `KATANA_INDEX_MIN_FREE_MB` sets the margin.
- `gc` runs by itself once a day from the crawler; manual `gc` and `forget` are in `docs/coder.md`. An initialized root's
  index, and its quarantine copies, are never touched by `gc`. The home registry entry of an initialized folder gone for
  30 days is dropped; the store is never touched.
- Concurrency: one writer per store (`LOCK`), one crawler per root (`CRAWL.LOCK`, released by the kernel when the process
  dies); readers take no lock and remap when the manifest changes (a `stat`, up to 4 cached views per process).

## 9. Storage format

Layout of `store/`: `MANIFEST` (the only mutable file, replaced whole by rename), `LOCK`, `CRAWL.LOCK`, `seg/NNNNNN.cbs`
(immutable segments), `tmp/` (leftovers after a crash are garbage), `.gitignore` containing `*`. Format version 2, all
little-endian, fields read and written one by one (never `fwrite` of a struct).

- Segment: 64-byte header (magic `KSEG`, format, segment id, section count, total size, section-table checksum, header
  checksum), a section table of 40 bytes per section (type, section version, offset, size, data checksum, record count),
  then the sections, 8-byte aligned.
- Manifest: magic `KMAN`, format, generation, next segment id, segment list (id, size, table checksum, section count,
  count), root, and a trailing checksum. Segments are listed oldest to newest; the newest segment that cites a key (as a
  record or a tombstone) decides it; older records stay masked until compaction.
- Section types: `STS_DOCS` 1 (documents, 40 bytes per record: path, size, mtime in ns, content hash, kind, flags, sorted by
  path, with a path pool), `STS_TOMBS` 2 (removed paths), `STS_DIRS` 3 (folder records: state, reason, rule, counts,
  summary, languages; 72-byte records), `STS_CRAWL` 4 (progress per segment); `0x100..0x1FF` lexicon and postings;
  `0x200..0x2FF` symbols, references, relations; `0x300..0x3FF` knowledge base; `STS_GRAPH` for the file graph.
- 32-bit ids are local to a segment; identities that must survive use the stable 64-bit content hash.

Design goal, not measured here: a catalog of about 40 bytes per file plus the path, so a 100 thousand file tree has a
catalog of a few MB.

## 10. Decisions that still explain the code

| Decision | Why |
|---|---|
| Root defined by `katana init`, not `.git` | A repository is not the unit the user cares about (monorepos, nested clones, a folder of repositories); the user says where an index lives, and nothing is written in a folder without that choice |
| One home store per visited root | Independent lifetimes and cheap `forget`; absorption folds a store into its ancestor when one appears |
| Crawler on demand, exits when done | No daemon to supervise; the session watch covers changes while a session is open; the settle time (2 s) groups a burst of changes into one run |
| Git linked worktrees are not descended | With `.git` not a boundary, a worktree inside the project would duplicate every symbol |
| `vendor` is not in the built-in list | The model may need to read vendored code; it is indexed, last in priority |
| Generated-file detection off by default | It changes what search finds relative to the synchronous path |
| Ignored content transparent, not forbidden | The model can reach into an ignored folder with `path` instead of concluding a name does not exist |
