# Code index lifecycle (design)

Status: design only, 2026-11-23. No code was changed and nothing was measured for this document. Every number
marked "observed" comes from single runs reported by earlier agents; every number marked "target" is a design
goal, not a result. The design covers when, where, how and by whom the coder index is built, kept fresh,
queried and discarded, and what the model is told about it.

Related documents: `docs/code-intelligence.md` (2.4 storage, 2.6 incremental update, 8.5/8.6),
`docs/tools.md` (Find, Symbols), `docs/curriculum.md` and `data/curriculum/v7/README.md` (what training assumes).

## 0. The decision in ten lines

1. **The index is built by a crawler.** One crawler process per index root (`katana coder crawl <root>`), elected
   by a file lock, started by whichever session touches the root first and shared by all of them. It walks the
   tree breadth first, level by level, and never runs inside the agent process.
2. **Everything is indexed, at several depths of information.** N0 tree (directories, file names, size, date,
   language, a summary per directory), N1 text search, N2 symbols and references, N3 links, N4 derived summaries
   (optional). Each level is published and queryable as soon as a batch of it is ready.
3. **Breadth before depth.** The whole tree reaches N0, then every file reaches N1, then N2, then N3. The one
   exception is the hot band (files the user or the model is touching, and files that just changed): those go
   through all levels at once.
4. **One index, one walk.** The Find cache (`find-<hash>.idx`) disappears; Find reads the coder store. The file
   graph and sparse activation are derived from N2/N3 sections instead of a second read of every file.
5. **Index root = project root, not the folder named.** A subfolder of a repository uses the index of the
   repository root with a path prefix; nested repositories, git worktrees and submodules are their own roots and
   are not descended from the parent.
6. **Queries never walk the tree.** Freshness comes from the crawler's file watcher (inotify, polling fallback),
   a bounded wait before the query (300 ms for changes, 2 s for a cold index) and a check of each file cited in
   the output. A query costs one `stat` of the manifest plus one `stat` per file it prints.
7. **Storage rule kept:** only the launch folder has `.katana/` inside; every other root lives in
   `~/.katana/coder/<key>/store`, with a registry that maps roots to stores and follows a moved folder by inode.
8. **The store stays what it is:** immutable segments, atomic manifest, one writer, lock-free readers. Levels are
   sections; crawl progress is published in two new section types in the same atomic swap. Corruption or a
   format change means rebuild, never migrate.
9. **The model sees at most one status line,** only when the queried scope is not complete or not fresh. For a
   small project the output is byte-identical to today, including `[symbol index created in N s]`.
10. **Nine phases, about 23.5 agent-days,** each compiling and useful alone; the risky parts (unified Find,
    threads) enter behind switches that default to the current behavior.

## 1. Current state and where the time goes

### 1.1 What exists (read in the code)

| Piece | File and function | What it does today |
|---|---|---|
| Store | `src/coder/store.c` | Immutable segments (`seg/NNNNNN.cbs`), `MANIFEST` swapped by rename, `LOCK` with `flock(LOCK_EX\|LOCK_NB)` in `store_writer_begin`, readers never lock (`store_open`, retry when a segment vanishes), `store_stale` = one `stat` of the manifest, tiered compaction in `store_writer_compact`. Files are created `0644`. Writing on Windows is not implemented. |
| Catalog | `src/coder/storedocs.c` `store_catalog_update` | Walks the whole tree (`st_scan_walk`, single thread, `lstat` per entry, ignore rules from `.gitignore`/`.katanaignore`/built-in lists), compares each file with the catalog by (size, mtime), reads and hashes changed files, calls the `doc_fn` hooks, writes ONE new segment, compacts above 8 segments. The walk happens before the writer lock is taken. |
| Search sections | `src/coder/search.c` | `cs_hook_doc` / `cs_hook_commit` build lexicon, postings, lengths and lexicon trigrams; state in the global `g_csix` and a `static char line[4100]`. `cs_view_open` rebuilds the live view (hash table of all documents, path arrays, lengths) on every query. `cs_search_core` loads the knowledge base on every query (`coder_kb_load`, line 839). |
| Symbol sections | `src/coder/symindex.c` | `sy_doc_hook` parses each file (`coder_parse`) and `sy_commit_hook` writes seven sections; state in the global `g_sy`. |
| Tool entry, Find | `src/coder/search.c` `coder_lookup` | Runs `store_catalog_update` (full walk and stat) synchronously on EVERY call, then the search. |
| Tool entry, Symbols | `src/coder/symtool.c` `sym_ensure_index`, `sym_tool_run` | Same full update on every call. Prints `[symbol index created\|updated in N s]` when the index was created or the update took more than 700 ms. `sym_capture` redirects the process's stdout and stderr with `dup2` while the query runs. |
| Find's own index | `src/tools/find.c` `fd_update`, `fd_cache_path`, `fd_save`; `src/tools/findgraph.c` `fg_build`, `fg_ensure` | A second index: file list from `git ls-files` (one child process, `ct_grep_collect` in `src/tools/grep.c`) plus a `stat` of every file on every call, 40-line chunks, its own tokenizer and stemmer, all loaded into the heap, saved whole to `find-<hash>.idx`. The file graph is rebuilt entirely (every file read again, plus `git log`) whenever anything changed. |
| Where | `store.c` `store_locate`, `src/tools/common.c` `proj_is_launch` | Launch folder: `<folder>/.katana/coder/store`. Others: `~/.katana/coder/<hash of realpath>/store`. |
| When | `src/agent/loop.c` `autoindex_dir`, `autoindex_start`; `src/agent/locate.c` `place_note` | A detached `katana coder index <folder>` on open, on `/cd`, and for each folder a request cites (spawned again on every request that cites it). Not in the home folder or `/`. `KATANA_AUTOINDEX=0` turns it off. |

### 1.2 Observed numbers (single runs, reported by other agents)

| Case | Observed |
|---|---|
| katana, about 1.4 thousand files, cold | 1.0 to 1.3 s total, of which symbol parsing 0.29 s; 6.7 MiB search + 5.5 MB symbols |
| katana, one file changed | 0.013 s |
| greymod, 373 files, cold | 0.3 to 0.5 s |
| 116 thousand files | symbol parsing alone 35.7 s; 2.2 million symbols |
| catalog of 25 thousand files | 1.9 s cold; 0.07 s on the second `index` (the pure walk-and-stat cost) |
| one search query | 11 to 15 ms, of which 10 to 14 ms is loading the knowledge base |

### 1.3 Defects and bottlenecks found while reading

1. **Two or three full tree sweeps per tool call.** A Find call runs `fd_update` (`git ls-files` child process and
   a `stat` per file) and then `coder_lookup` → `store_catalog_update` (a second walk with `lstat` per entry). A
   Symbols call runs the walk again. At the observed 0.07 s per 25 thousand files this is invisible in katana and
   roughly 0.3 s per call, per sweep, at 116 thousand files, on every call, forever.
2. **Cold build is synchronous and unbounded.** The first Find or Symbols on a 116-thousand-file folder blocks the
   model's turn for the whole build (tens of seconds) and holds everything for one segment in memory.
3. **Single thread everywhere**: walk, read, tokenize, parse. The machine has 16 hardware threads.
4. **The knowledge base is parsed from text on every query** (10 to 14 ms of an 11 to 15 ms query).
5. **The live view is rebuilt on every query** (`cs_view_open`, `sy_view_open`): linear in the number of files.
6. **Global state blocks threads and in-process indexing**: `g_csix`, `g_sy`, the static line buffer in
   `cs_hook_doc`, and `sym_capture`'s `dup2` of stdout (any other thread printing during a Symbols call would
   write into the capture file).
7. **Two stores for one root are possible.** `store_locate` decides local or central by `proj_is_launch`, which is
   per process. A session launched in X writes `X/.katana/coder/store`; a session launched elsewhere that cites X
   writes `~/.katana/coder/<hash>/store`. Both get maintained separately.
8. **A moved folder breaks the local store silently.** Readers build file paths from the root recorded in the
   manifest (`cs_read_doc` uses `v->r->man.root`); after `mv`, evidence would be read from the old location.
9. **Index files are world readable** (`open(..., 0644)` in `st_write_file`, `LOCK` too) although the lexicon and
   the symbol signatures contain text from the user's files.
10. **Nothing is told to the model about a partial index**, because today there is no partial index: the call
    blocks until the build ends.
11. **Ignore handling differs between the two indexes**: Find trusts `git ls-files`, the coder store its own
    rules; the 15 agent worktrees under `.claude/` inflated the index before `.claude` entered `st_ignored_dir`.
    A nested repository with any other name still inflates it.

## 2. Principles

1. **The index is a cache of the working tree.** It may be late; it may never be wrong or half written. Anything
   doubtful is rebuilt. (Already the store's principle.)
2. **Crawl, do not batch.** Discover breadth first, publish each level as it becomes ready, keep deepening in the
   background. A reader must always be able to answer "how far has this part of the tree been indexed".
3. **Index everything, to the depth each thing deserves.** Every directory and file name is known at N0, including
   the ones that will never be read (binary, huge, secret, ignored directory): they are recorded with the reason.
4. **One index per project, one walk per change.** No second cache, no second tokenizer pass, no walk in the
   query path.
5. **Readers are in process and lock free; the writer is elsewhere and alone.** Queries are function calls over
   mapped segments. Writing, watching and parsing are the crawler's job.
6. **Bounded waits.** A tool call waits at most a fixed budget for the index, then answers with what exists and
   says so in one line.
7. **Say what was not looked at.** Absence is only claimed for the part of the tree that reached the needed
   level; the status line carries the limit. (Owner's decisions 4 and 5.)
8. **Nothing external.** No embeddings, no service, no third-party library; `git` is never required (it is an
   optional source for the co-change edges only).
9. **Risky changes enter switched off.** Compile is mandatory; no measurement campaign in this phase.

## 3. The design, question by question

### 3.0 What the known systems teach

"Sure" = widely documented behavior I am confident about. "Recollection" = remembered, may be dated or wrong in
detail; do not build on the detail without checking.

| System | What it does | Certainty | Take | Reject |
|---|---|---|---|---|
| git index | One file with a stat cache per path (size, mtime, ctime, inode...) so `status` re-hashes only files whose stat changed; "racily clean" entries (mtime equal to the index's own timestamp) are re-hashed; content addressed objects | Sure | (size, mtime) then content hash, exactly what the catalog does; the racy rule (see C.5) | Trusting ctime/inode (breaks across copies and on ntfs3) |
| git untracked cache | Remembers directory mtimes to skip `readdir` of unchanged directories | Sure | Directory mtime to skip re-listing in polling mode | Using it for content changes: a directory mtime does not change when a file is modified in place |
| git fsmonitor | A watcher (Watchman hook or built-in daemon) tells git which paths changed since a token, so `status` skips the stat sweep | Sure for the idea; built-in daemon availability on Linux is recollection | A long-lived watcher plus a "changed since generation G" answer | Depending on an external watcher program |
| ripgrep, fd | No index; parallel directory walk honoring `.gitignore`, `.ignore`; fast enough for interactive grep | Sure | Parallel walk per directory; ignore semantics; Grep stays index-free and is the fallback when no index can exist | "No index" for ranked or structural queries: no ranking, no symbols, no links |
| codesearch, Zoekt | Trigram index gives candidate files, the real match is verified on the file; Zoekt keeps immutable shards per repository, memory mapped | Sure for codesearch and the shard idea; Zoekt delta-shard details are recollection | Immutable shards plus verification on the live file; candidates from the index, truth from the file | Indexing per commit: the agent works on a dirty tree that changes while it works |
| livegrep | Suffix array over the corpus, rebuilt in bulk | Recollection | Nothing | Bulk rebuild |
| clangd | Background index in `.cache/clangd/index/`, one shard per file keyed by name and content digest, skipped when the digest matches; a dynamic index of open files overrides the background one; progress shown by the editor | Sure in outline; shard naming is recollection | Per-file content hash to skip work; "fresh overlay wins over background index" (C.4); progress as a status item; index in a dot-folder with ignore rules | One shard file per source file (100 thousand small files; bad on ntfs3) |
| rust-analyzer | Salsa: in-memory incremental query database, recomputes only what depends on a changed input; essentially no persistent cache | Recollection | Derivations declared by level so only the affected level is recomputed | Everything in memory per process: several katana sessions would each pay it, and each already holds a model |
| TypeScript server | A project per `tsconfig.json`, found by walking up from the opened file; inferred project when none; watches files and directories | Sure in outline | Root by walking up to a marker (A) | Build-file markers as roots: they split a monorepo |
| VS Code, Cursor | File watcher service; text search is ripgrep without an index; Cursor syncs a Merkle tree of file hashes to know which chunks changed and indexes on folder open with a progress indicator | VS Code sure; Cursor recollection from public descriptions | Index on open, discreet progress, watcher with rescan on overflow | Embeddings and a remote service (owner's decision 3); the Merkle tree itself: with one local consumer the catalog's (size, mtime, hash) per file gives the same answer without hashing directories |
| ctags | One sorted `tags` file, usually regenerated whole | Recollection for the incremental modes | Nothing | Whole-file regeneration |
| SCIP, LSIF | Per-language indexers emit an index offline (per commit), a server consumes it | Sure in outline | A stable section format that other producers could fill later | Offline, per-commit production |
| Bazel, Nix | Results cached under a key that is the hash of every input | Sure | Derived sections are keyed by content hash and by the version of the code that derives them: a parser version bump invalidates only its sections | A content-addressed store on disk for index pieces: too many small files |
| SQLite (WAL), LMDB | One writer, many readers that never block; LMDB maps the file and keeps a reader table; both are fragile on network filesystems | Sure | One writer, readers on immutable snapshots (already the store); central store on a local filesystem when the project is on a network one | A shared mutable file; a reader table |
| inotify | Watch per directory, not recursive, limit `fs.inotify.max_user_watches` (65536 on the owner's machine), events lost on queue overflow (`IN_Q_OVERFLOW`) | Sure | The Linux watcher, with overflow → rescan | Assuming it sees changes made on the other side of NFS/CIFS/FUSE |
| fanotify | Mount- or filesystem-wide marks; historically privileged, unprivileged use is limited | Recollection for the unprivileged limits | Nothing now | Requiring privileges |
| FSEvents, ReadDirectoryChangesW | Recursive watch per path/handle, coalesced events, overflow means rescan | Sure in outline | The same watcher interface for macOS and Windows later | — |
| mmap of indexes | Zero-copy readers, resident memory proportional to what is touched, shared between processes through the page cache | Sure | Already used (`store_seg_map`, with read fallback) | `mmap` of structs: the store decodes field by field, keep that |

### A. Project root

**Decision.** Every path the agent names is resolved to an **index root** plus a **prefix** inside it. A new
function `index_resolve(path, &root, &prefix)` (new file `src/coder/roots.c`) applies, in order:

1. `realpath` the path; a file resolves through its directory.
2. Walk up from the path. The first ancestor (the path itself included) that satisfies one of these is the root:
   a. it is registered as a root in the registry (F.2) — an index already exists above;
   b. it contains `.git` (directory or file), `.hg` or `.svn`;
   c. it contains `KATANA.md` or `.katana/coder/store/MANIFEST`.
3. The walk stops, without accepting them, at the home folder, at `/` and at a filesystem boundary (`st_dev`
   changes). If nothing was accepted, the root is the path itself (a folder without git is its own root).
4. The home folder and `/` are never roots for automatic indexing (already the rule in `autoindex_dir`,
   `ct_findtool`, `ct_symbols`). `katana coder index ~` is refused without `--force`.

So `greymod/bridge` uses the index of `greymod` with prefix `bridge/`. This is what `ct_findtool` already does
for subfolders of the working folder (`root` plus `pre`); the rule becomes general and Symbols gets the same.

Special cases:

| Case | Rule | Why |
|---|---|---|
| Monorepo with several projects | One root at the repository; projects are prefixes; the per-directory summaries (D.2) give each project its own top-down view | Cross-project links are the point of the index (owner's decision 6). Splitting at `package.json`/`Cargo.toml` (the TypeScript-server way) would cut them. |
| Git worktree (`.git` is a file) | Its own root: different checkout, different content | A worktree shares history, not files. |
| Nested repository, worktree or submodule INSIDE a root | The crawler records the directory at N0 with state `nested-root` and does not descend. It becomes its own root when something names it | This is the general fix for the `.claude/worktrees` inflation, for any directory name. A submodule's code is still reachable: one line tells the model it is a separate index. |
| Symbolic links | Never followed. A link to a directory is recorded at N0 as `link` with its target; the root itself is always a real path | Cycles and double indexing. Same as the current `lstat` walk. |
| Mount point inside a root | Recorded at N0 as `other-filesystem`, not descended | A mounted disk or network share inside a project is not project code; avoids crawling a NAS. |
| Folder without markers | Root is the folder itself | Nothing better is knowable. |
| Folder named is above existing roots (for example `~/source`) | It becomes a root if explicitly asked; the roots below are `nested-root` when they have their own VCS marker | Keeps one owner per file. |

**Alternatives rejected.** (1) Always index exactly the folder named: produces overlapping indexes
(`greymod` and `greymod/bridge`), double work and links that stop at the folder edge. (2) Always the launch
folder: wrong when the request cites another project (decision 1 demands indexing it). (3) Build-file markers:
see monorepo row.

### B. Who indexes

**Decision.** One **crawler process per index root**: `katana coder crawl <root>` (alias `katana coder index
--watch <root>` in the foreground). It is the only process that walks, reads, parses, watches and writes for
that root. Sessions (console, desktop, `katana serve`, subagents) are clients: they read the store in process
and talk to the crawler through a small local socket.

Lifecycle:

- **Start.** Any session calls `index_ensure(root)` (new `src/coder/indexctl.c`) when it opens in a folder, on
  `/cd`, when a request cites a folder (`place_note`), and when Find or Symbols is asked for a root with no live
  crawler. `index_ensure` tries to connect to the crawler's socket; if nobody answers it spawns the crawler
  detached (`posix_spawn`, new session with `setsid`, stdio to a log file) exactly as `autoindex_dir` spawns
  `coder index` today. Spawning twice is harmless.
- **Election.** The crawler takes `flock(LOCK_EX|LOCK_NB)` on `CRAWL.LOCK` in the store directory and holds it
  for its whole life. A second crawler fails the lock and exits 0 silently. The kernel releases the lock when the
  process dies, however it dies.
- **Writing.** The store's `LOCK` is taken per commit only (`store_writer_begin` ... `store_writer_end`), so a
  manual `katana coder compact` or `verify` can interleave between batches. `store_writer_begin` gains a blocking
  mode with timeout for the crawler.
- **Leases.** Each session keeps its socket connection open while it uses the root. With zero connections and an
  empty queue for `KATANA_INDEX_IDLE_EXIT_S` (default 600) the crawler exits. Its next start resumes from the
  store (D.6).
- **Priority.** The crawler runs at `nice 10` with idle I/O priority and a small thread pool (I.3): the session
  beside it is running a model.
- **If it dies.** Readers are unaffected (immutable segments, published manifest). The next `index_ensure` finds
  the socket dead, removes it and spawns a new crawler, which cleans `tmp/` and orphan segments (already done by
  `store_writer_begin`) and rebuilds its queue from the store. At most one unpublished batch is lost.
- **If it cannot start** (spawn fails, no writable home): the tools fall back to the current in-process
  `store_catalog_update`, capped by the wait budget and by `KATANA_INDEX_INLINE_MAX_FILES` (default 5000); above
  the cap they answer from Grep-style scanning and say that no index is available.

Socket protocol (one line request, one line reply, `SOCK_STREAM`, path `~/.katana/run/coder-<key>.sock`; the run
directory is on the home filesystem because socket files cannot be assumed to work on ntfs3 or network mounts):

| Request | Reply | Use |
|---|---|---|
| `PING` | `OK <pid> <generation> <state>` | Liveness, lease |
| `PRIO <relative path>` | `OK` | Move a file or directory to the hot band (the model reads, edits or asks about it) |
| `TOUCH <relative path>` | `OK` | A session wrote this file; enqueue it now, do not wait for the watcher |
| `SYNC <level> <prefix> <timeout ms>` | `OK <generation> complete\|partial <done> <total>` | Block until the change queue is empty and `<prefix>` reached `<level>`, or the timeout |
| `STATUS` | one line of counters (same content as the state file, H.1) | Status command, console line, desktop |
| `STOP` | `OK` | `katana coder forget`, tests |

**Alternatives and their cost.**

| Option | For | Against | Verdict |
|---|---|---|---|
| Detached `coder index` per event (today) | Simple, no lifecycle | No watcher, so every query must sweep; unbounded cold build; spawned again and again | Replaced |
| Thread inside the agent | No IPC; shares memory | Global state and `dup2` in the query path (1.3 item 6); dies with the session, so a 100-thousand-file crawl restarts with every short session; N sessions still need an election; competes with inference inside the same process and address space | Rejected |
| One daemon per user (`katana indexd`) holding all roots and SERVING queries | One process, one inotify instance, hot roots in memory | Queries through IPC lose the main property of the store (mapped, in-process, lock-free reads shared by the page cache); a single failure point for every session; it must manage many roots' lifecycles; language servers need this shape because their index is live heap state, ours is immutable files | Rejected now |
| One crawler per root, readers in process | Election is one `flock`; failure is isolated per root; nothing to serve; idle roots cost nothing | One process and one inotify instance per active root | **Chosen** |

The socket protocol is the same one a per-user daemon would speak, so merging crawlers into one process later is
a packaging change, not a redesign.

What the examples say: git's fsmonitor and clangd's background indexer are exactly "a long-lived helper that
watches and writes, while the readers use files"; LMDB and SQLite show that readers need no coordination when the
writer publishes snapshots.

### C. Freshness

**Decision.** Three mechanisms, none of which walks the tree in the query path.

**C.1 The crawler watches (push).** After listing a directory the crawler adds an inotify watch on it
(`IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_CLOSE_WRITE|IN_ATTRIB|IN_DELETE_SELF|IN_MOVE_SELF`). Events
enter a coalescing change queue keyed by path, debounced 50 ms. A changed file goes through all its levels at
once in one small segment (the observed cost of one file is 0.013 s). A new directory is crawled as a branch
(D.7). `IN_Q_OVERFLOW`, or more than 2000 events under one directory in a second, collapses to "re-list this
branch".

Watch budget: at most half of `fs.inotify.max_user_watches` (read from `/proc`), in priority order (hot band
first, shallow first). Directories beyond the budget, and every directory when the filesystem is network or FUSE
(detected with `statfs`), are covered by **polling**: a background stat sweep of the catalog every
`max(2 s, 20 × the cost of the previous sweep)`. Directory mtime is used only to skip `readdir` of unchanged
directories (git's untracked-cache idea); file content changes are found by the file `stat`, because a directory
mtime does not change when a file is rewritten in place. In polling mode a change of `.git/HEAD` or `.git/index`
mtime (two `stat` calls) triggers an immediate sweep: a branch switch changes many files at once.

**C.2 The session tells (hint).** Edit, Write, MultiEdit and NotebookEdit call `index_touch(path)` after writing:
the path enters an in-process dirty set and a `TOUCH` goes to the crawler. Bash sets a "tree may have changed"
flag (the watcher sees the actual changes).

**C.3 The query waits, briefly (pull).** Before a Find or Symbols query, `index_ensure(root, level, prefix)`:

| Situation | Action | Budget (env) |
|---|---|---|
| Dirty set empty, flag clear, crawler queue empty (from the last `PING`) | Nothing: one `stat` of the manifest (`store_stale`) | 0 |
| Dirty set non-empty or flag set | `SYNC` until the change queue drains | 300 ms (`KATANA_INDEX_FRESH_MS`) |
| Scope not yet at the needed level (cold or still crawling) | `PRIO <prefix>` then `SYNC <level> <prefix>` | 2000 ms (`KATANA_INDEX_WAIT_MS`) |
| Budget expires | Query what is published; one status line (J) | — |

With the observed numbers, a project of up to about 2 thousand files completes all levels inside the 2 s budget,
so small projects behave synchronously as today; larger ones answer partially and keep crawling.

**C.4 The output is checked against the live files (verify).** Every file cited in a result is already re-read
for evidence (`cs_read_doc`, `sy_verify`, `fd_text`). That read now compares (size, mtime) with the catalog:

- equal → the line is printed as is;
- different → for text hits the evidence is recomputed from the live file (already the behavior) and the hit is
  dropped if the text is gone; for symbol hits the single file is re-parsed in process (`coder_parse` on one
  file, the "dynamic index overrides the background index" idea of clangd) and line numbers come from the fresh
  parse; the file is sent as `TOUCH`;
- missing → the hit is dropped and counted.

One line reports it when it happened: `[index: 2 files changed since indexed; shown from the files]`.

**C.5 Racy timestamps.** If a file's mtime is within the filesystem's timestamp granularity of the moment it was
cataloged, a later change of the same size could go unseen (git's "racily clean"). Such entries get the flag
`SDF_RACY` and are re-hashed on the next sweep or event instead of trusted. The granularity is taken as 2 s on
FAT-like filesystems, 1 s when the filesystem reports whole seconds, 10 ms otherwise.

**Freshness contract.** A result reflects every change that happened before the call started, except (a) changes
the wait budget could not absorb, reported in the status line, and (b) changes on filesystems without events,
which are bounded by the polling period and always caught for the files actually printed (C.4).

**Alternatives rejected.** Stat everything per call (today): linear cost on every call. A validity window of N
ms without events ("trust the index for 2 s"): returns stale data right after the agent's own edit, the most
common sequence there is. `git status` as the change detector: needs git, a child process per query, and does
nothing for folders without git. Trusting inotify alone: silently wrong on network mounts and after overflow.

### D. The crawler: order, levels and limits

#### D.1 Levels

| Level | Content | Reads file content? | Sections | Queries it enables |
|---|---|---|---|---|
| N0 tree | Every directory and file: name, size, mtime, language by extension, skip reason; per directory: counts, languages, the title line of its README/KATANA.md, state | No (one small README per directory, see D.2) | `STS_DOCS` (records with hash 0 and level 0), `STS_DIRS` (new, type 3), `STS_CRAWL` (new, type 4) | "What is in `bridge/`", path and file-name search (the path and name fields of Find), Glob-like listing, directory summaries |
| N1 text | Content hash; lexicon, postings by field, lengths, lexicon trigrams; generated/minified/secret detection | Yes | `STS_DOCS` (hash, level 1) + `CS_T_*` (existing) | Find by words, identifiers and quoted text; literal verification |
| N2 symbols | Definitions, references, intervals | Yes (again, hash checked) | `SY_T_*` (existing) | Symbols `def`, `refs`, `callers`, `callees`, `outline`, `at`, `impact`; Find `mode=refs`; symbols in Find candidates |
| N3 links | Imports resolved to files, text keys shared between files (routes, event names, config keys, table names), call edges by key | Derived from N2 output plus a scan for key shapes | Link sections in the `STS_SYM_BASE` range (being defined by the links work in progress; this document fixes only their lifecycle) + `STS_GRAPH` (new: the file graph, D.8/E) | `deps`, cross-language connections, graph neighbors, sparse activation |
| N4 summaries (optional, off by default) | Per file: one line built mechanically from its top symbols and header comment; per directory: main symbols by reference count | No new reads | `STS_SUMMARY` (new) | Richer directory summaries. No model is run to produce them. |

A level is useful alone: with only N0 the model can navigate top down; with N1 it can search; N2 and N3 add
exactness and connection.

#### D.2 The directory record and the directory summary

`STS_DIRS` is a table sorted by path, newest segment wins, like documents. One record per directory:

| Field | Meaning |
|---|---|
| path, depth | Relative to the root |
| state | `discovered` (seen in the parent's listing, not listed yet), `listed`, `ignored`, `nested-root`, `link`, `other-filesystem`, `too-deep`, `too-many-entries`, `unreadable` |
| reason | For `ignored`: `gitignore`, `katanaignore`, `heavy-dir`, `vcs` |
| mtime | Directory mtime when listed |
| nfiles, ndirs, nignored | Direct children |
| lang[3] | Top three languages by file count (from the extension table `kind_fn`) |
| doc | Up to 160 bytes: first heading or first sentence of `KATANA.md`, `README*` or `index.md` in the directory, when one exists, is under 256 KB and is not a secret |
| band | Priority band the directory was assigned (D.4) |

The recursive totals (files under this directory at each level) are NOT stored: the reader aggregates them from
the live documents once per generation (one pass, cached with the view, I.4). That is the **state map**: for any
prefix the reader knows how many files are at N0, N1, N2, N3, and therefore what it can and cannot claim there.

`STS_CRAWL` is one small record in the newest segment: crawl epoch, format and derivation versions, counters per
level, frontier size, watch mode, limits that were hit, time of the last complete pass per level.

On "progress markers in the manifest": the markers are part of the published state and change atomically with
the manifest swap, but physically they live in these two section types. The manifest is generic (store.c knows
nothing about code) and is rewritten whole on every commit; putting a per-directory table in it would make every
commit proportional to the number of directories.

#### D.3 Queues

All in the crawler's memory; nothing is persisted separately because all of it is reconstructible from the
store (D.6).

| Queue | Items | Order |
|---|---|---|
| Change queue | Paths from the watcher, `TOUCH` and polling | First in, first out, coalesced by path, 50 ms debounce |
| Frontier | Directories in state `discovered` | Heap by (band, depth, sequence): breadth first, hot branches first |
| Level queues Q1, Q2, Q3 (Q4) | Files that are at level k-1 and eligible for level k | Heap by (band, newest mtime first) |

#### D.4 Order: breadth first, then priority inside a level

Service order of the main loop, re-evaluated after every batch:

1. Change queue (changed files go N0→N3 at once; they are what the user is working on).
2. Hot band of any queue (band 0, all levels at once).
3. Frontier until empty: the whole tree reaches N0. (Target: seconds even for 100 thousand files.)
4. Q1 until empty: every eligible file reaches N1, band by band.
5. Q2 until empty, then Q3, then Q4 if enabled.
6. The file graph (`STS_GRAPH`), compaction, verification scrub, garbage collection.

Bands:

| Band | What |
|---|---|
| 0 hot | Paths given by `PRIO`/`TOUCH` (files the model read, edited or was asked about, the prefix of the current query), files changed during this crawl, the directory of each of those |
| 1 | Source code modified in the last 30 days; files at the root; `KATANA.md`, `README*`; source under directories named like `src`, `lib`, `app`, `cmd`, `pkg`, `include`, `internal` |
| 2 | Other source code |
| 3 | Documentation, configuration, plain text |
| 4 | Data-like and generated: `json`/`csv`/`xml` above 256 KB, lock files, `*.min.*`, `*.map`, files flagged generated, directories flagged `too-many-entries` |

Why strict level order instead of finishing one folder before the next: after the cheap N0 pass, the model can
already see the whole tree; after N1 it can search everywhere; a folder-by-folder build leaves most of a large
repository invisible for most of the build. Why the hot band is the exception: for the files in front of the
user the full depth costs milliseconds and is what the next question needs.

Honest note: on the observed numbers, N1 (read and tokenize) is the larger share of a cold build and N2 (parse)
is roughly a quarter to a third, so splitting N1 from N2 moves less time than splitting N0 from the rest. The
split is kept because it is what makes partial states simple to describe and to resume.

#### D.5 Batches, and how a level becomes sections

A batch is the unit of publication: up to 4096 files, 64 MiB of source or 1 s of work, whichever comes first
(hot and change batches are published immediately, whatever their size). One batch = one commit = one new
generation.

| Transition | Mechanism in the store | Why this mechanism |
|---|---|---|
| Frontier → N0 | New segment with `STS_DOCS` records (hash 0, level 0) for the files listed and `STS_DIRS` records for the directories listed and discovered | Plain append |
| N0 → N1 | New segment with `STS_DOCS` records (hash set, level 1) and the search sections for the batch. The older N0 records of the same paths are masked by the existing rule "the newest segment that cites a path decides" | No rewrite of the big N0 segments; reuses the vigency rule unchanged |
| N1 → N2, N2 → N3 | **Augmentation**: the crawler re-reads the batch's files (skipping any whose hash changed), builds the new sections, and writes a segment that carries the old segment's sections copied byte for byte plus the new ones, with the same document order; commit adds it and drops the old segment (`store_writer_add_section`, `store_writer_drop`) | Every section of a segment is indexed by that segment's document table, so a separate "symbols only" segment would mask the text sections of the same paths. Augmentation keeps the invariant, needs no id remapping, and its cost is bounded by the batch size |
| A changed file | New small segment with all levels for that file (today's behavior) | Hot path, 13 ms observed |

The level of a document is recorded in bits 8 to 11 of `StoreDoc.flags` (today only bits 0 to 2 are used), so the
reader does not have to infer it from the sections present.

Compaction merges only segments of the same level (tiers per level), and during a crawl tolerates up to 24
segments (8 again once the crawl is complete). Mergers are unchanged.

#### D.6 Resumable by construction

There is no queue file. On start the crawler opens the store and rebuilds:

- frontier = directories whose newest record is `discovered`;
- Qk = live documents at level k-1 that are eligible for level k;
- change queue = one stat sweep of the catalog (the same sweep polling uses), because events were missed while
  no crawler was alive.

If the store is empty, the frontier is the root. Killing the crawler at any point loses at most the batch in
flight. `STS_CRAWL.epoch` increases at each full restart from zero.

#### D.7 Re-entering one branch

A change reported for a directory (new entry, removal, rename, or a mtime difference found by polling) re-lists
only that directory: new files enter Q1 (or the change queue), vanished files get tombstones, new
subdirectories enter the frontier as `discovered`, and a removed directory tombstones its whole prefix. A change
to `.gitignore` or `.katanaignore` re-lists the subtree under the file that changed, since rules apply downward.

#### D.8 Limits and explosion policy

| Thing | Policy | State the model can be told |
|---|---|---|
| Heavy directories by name (`.git`, `.hg`, `.svn`, `.katana`, `.claude`, `node_modules`, `vendor`, `build`, `dist`, `target`, `__pycache__`, `.venv`, `venv`, `.cache`, `.gradle`, `.idea`; union of the lists in `st_ignored_dir`, `st_scan_walk` and `fd_skip_rel`) | Directory recorded at N0 as `ignored: heavy-dir`; not descended. `.katanaignore` can re-include with `!` | "`node_modules/` exists and is not indexed" |
| Ignored by `.gitignore`, `.katanaignore`, and `.git/info/exclude` (to be added) | Ignored directory: record only. Ignored file: not cataloged, counted in the parent's `nignored` | "12 ignored entries in `out/`" |
| Depth | Beyond `KATANA_INDEX_MAX_DEPTH` (default 32): `too-deep`, not descended | yes |
| Fan-out | A directory with more than 20 thousand direct entries: `too-many-entries`; its files are cataloged at N0 and stay in band 4 | yes |
| Total | `KATANA_INDEX_MAX_FILES` (default 500 thousand at N0) and `KATANA_INDEX_MAX_TEXT_FILES` (default 200 thousand at N1 and above); when reached, lower bands stay at the level they have; `STS_CRAWL` records the limit | "index limited to 200000 files; `data/` is at catalog level" |
| Binary (extension list, or a NUL byte in the first 8000 bytes) | N0 only, `SDF_SKIP_BINARY` (existing) | counted |
| Larger than `--max-size` (2 MiB) | N0 only, `SDF_SKIP_BIG` (existing). Text beyond `KATANA_CODER_INDEX_KB` (128 KB) of a file is not in the lexicon (existing) | counted |
| Generated or minified (lock files, `*.min.*`, `*.map`, a marker such as "DO NOT EDIT" or "@generated" in the first 1000 bytes, average line longer than 500 bytes, any line longer than 20 thousand) | N1 with path and name fields only, body not tokenized; no N2; flag `SDF_GENERATED` | counted |
| Secrets (F.4) | N0 only, `SDF_SKIP_SECRET`; content never read | counted, never shown |
| Unreadable | N0, `SDF_SKIP_UNREADABLE` (existing) | counted |

"Index everything" therefore means: everything is known at N0 with a state and a reason; depth of information is
spent where it is useful.

#### D.9 The existing synchronous path

`store_catalog_update` stays as the engine of the manual `katana coder index <dir>` (no `--watch`), of `katana
init`, and of the inline fallback (B). When a crawler is alive for the root, `katana coder index` becomes a
client: it sends `SYNC` for all levels and prints progress.

### E. One index: unifying Find with the coder store

**Decision.** `find-<hash>.idx` and the in-heap `FdIndex` are removed; Find becomes a consumer of the store.

| Find needs | Today | Unified |
|---|---|---|
| File list, kinds | `git ls-files` + `stat` per call (`fd_update`) | Live documents of the view (N0) |
| Candidate files for a query | Its own chunk postings, own tokenizer and stemmer | The store's field postings and trigram channel through `cs_search_core` (already called by Find via `coder_lookup`); the top `FG_K_FILE` (40) files |
| Best chunk inside a file, lines to show | Chunk postings (40 lines, stride 25) stored in the cache | Computed at query time on the candidate files only: they are read anyway for verification (C.4). Chunking and the per-chunk BM25 of `fd_rank` run over 40 files instead of being stored for all |
| Symbols defined in a chunk | Regex rules (`fd_sym`) | N2 intervals (`SY_T_IVAL`); the regex rules remain only for files still below N2 |
| `mode=refs` | `fd_refs_sym` (symbol index) then `fd_refs` (own postings) | Symbol index only; below N2 the answer says so and offers Grep |
| File graph: `sym`, `path`, `rare`, `co` edges; sparse activation; neighbor notes | `fg_build`: reads every file again and runs `git log`, whole rebuild on any change | Derived by the crawler into `STS_GRAPH`: `sym` = join of references and definitions by name (`SY_T_BYREF` ⋈ `SY_T_BYDEF`) with the same hub cut-offs; `path` = import links (N3); `rare` = lexicon terms with document frequency 2 to 4 and identifier shape; `co` = `git log` when git exists (optional, `KATANA_FIND_CO`). Nodes are keyed by the 64-bit path hash, since document ids are local to a segment. Rebuilt after the change queue has been quiet for 2 s, incrementally for the changed files' rows when the change set is small |
| Request state (already seen, near files) | In-process arrays keyed by file index | Same, keyed by path hash |

What stays in Find: the query classification, the ranking adjustments (`FD_W_*`), the output shape and budget,
the decision layer over `CoderHits`. What it stops owning: storage, listing, tokenizing at index time, the graph
build.

**Migration.** None. The unified path enters behind `KATANA_FIND_INDEX=store` with `legacy` as the default; when
the owner flips the default, `fd_save`, `fd_load`, `fd_cache_path`, `fd_build_file`, `fd_invert`, `fg_build`,
`fg_save` and `fg_load` are deleted and `katana coder gc` removes old `find-*.idx` files. The two formats are
caches; nothing is converted.

**Memory in the agent process.** Today: the whole Find index in the heap (terms, per-file blobs, inverted lists,
graph) plus per-query views. Unified: mapped segments (resident only where touched, shared with the crawler and
other sessions through the page cache) plus one cached view per root: about 32 bytes per file for the search
view and about 72 bytes per file for the symbol view's live table, so on the order of 12 MB for 116 thousand
files and well under 1 MB for katana (estimates from the struct sizes, not measured). At most 4 roots are kept
open per process (least recently used closes).

**Risk.** Ranking changes for Find (different tokenizer and candidate source), with no measurement campaign
allowed now. That is why the switch defaults to legacy. See section 6.

### F. Storage and place

**F.1 The rule.** Unchanged: only the launch folder has `.katana/` inside. Stated precisely with roots:

- If the launch folder IS an index root, its store is `<root>/.katana/coder/store`.
- Every other root (cited folders, `path` arguments, and the repository root when katana was launched in a
  subfolder of it) is in `~/.katana/coder/<key>/store`.
- **One store per root, whoever asks.** If a root already has a local store, every process uses it, launched
  there or not; a central store is never created beside it. This removes defect 7 of section 1.3 and needs the
  registry, because the choice can no longer be made per process.
- If the root is on a network or FUSE filesystem, or is not writable, the store is central even for the launch
  folder (locks and mapping are only trusted on local filesystems).

**F.2 Registry.** `~/.katana/coder/roots/<key>.root`, one small text file per root, written by tmp and rename:
real path, device and inode of the root directory, store directory, format version, created, bytes on disk. Its
mtime is the last-use time (touched at most once per hour per process). `<key>` is the existing
`store_hash64(realpath)` in hex.

- Lookup by path: one `stat` of `<key>.root`.
- **Moved folder**: when a path has no registry entry, the registry is scanned for an entry whose (device, inode)
  equals the folder's and whose recorded path no longer exists; if found, the central store directory is renamed
  to the new key and the entry rewritten. A local store travels with its folder.
- **Root recorded in the manifest differs from the located root** (moved local store): the reader uses the
  located root for all file paths, never the recorded one, and the crawler rewrites it
  (`store_writer_set_root`) and re-validates the catalog. Fixes defect 8.
- **Copied folder**: a new root, built from zero. Identity by git remote was considered and rejected: two clones
  of one remote are different trees, and the saving is one crawl.

**F.3 Permissions.** Store directories `0700`, every file `0600` (segments, manifest, locks, state, socket,
registry). On filesystems that ignore modes (the owner's `/mnt/dados` is ntfs3 mounted with fixed uid and gid)
this is not enforceable; `katana coder status` says so, and `KATANA_INDEX_LOCAL=0` forces the central store even
for the launch folder.

**F.4 Secrets.** The index holds text of the files (lexicon terms, signatures, the README line). Three layers:

1. **Never read** (cataloged at N0 by name only, `SDF_SKIP_SECRET`): `.env`, `.env.*` except `*.example`,
   `*.sample`, `*.template`; `*.pem`, `*.key`, `*.p12`, `*.pfx`, `*.jks`, `*.keystore`, `*.kdbx`, `*.tfstate`,
   `*.tfvars`; `id_rsa*`, `id_dsa*`, `id_ecdsa*`, `id_ed25519*`; `.netrc`, `.npmrc`, `.pypirc`, `.htpasswd`,
   `credentials`, `credentials.*`, `secrets.*`, `*.secret`; anything under `.ssh/`, `.gnupg/`, `.aws/`. The list
   is a table in `storedocs.c`; `.katanaignore` adds to it and can re-include with `!`.
2. **Sniffed at N1**: a file whose first 200 bytes contain `-----BEGIN` followed by `PRIVATE KEY` is treated as
   layer 1.
3. **Tokens that never enter the lexicon** (extension of `cs_junk` in `search.c`): tokens with a known secret
   prefix (`AKIA`, `ghp_`, `gho_`, `github_pat_`, `sk-`, `xoxb-`, `xoxp-`, `eyJ` followed by 20 or more base64
   characters) and any token of 24 or more characters drawn from base64 or hex alphabets with both letters and
   digits and no dictionary shape.

These are heuristics; section 6 says what they miss.

**F.5 Cleaning.**

| Command or event | Effect |
|---|---|
| `katana coder list` | Roots in the registry: path, store, size, levels, last use, crawler alive or not |
| `katana coder forget <dir>` | `STOP` to the crawler, delete the store and the registry entry (local or central) |
| `katana coder gc [--dry-run]` | Remove central stores whose root has not existed for 30 days; remove least recently used central stores while the total is above `KATANA_INDEX_MAX_MB` (default 2048) and the store was unused for 14 days; remove orphan directories without a registry entry; remove `find-*.idx` once the legacy Find is gone. Never removes a local store |
| Automatic | A crawler runs `gc` at start when the stamp `~/.katana/coder/gc.stamp` is older than a day |

A root that vanished may be an unmounted disk, hence the 30-day grace.

**F.6 Format version.** Rebuild, never migrate. A store whose manifest, segment or section versions do not match
what the binary writes is treated by readers as "no index" and by the crawler as garbage: it is deleted and the
crawl starts from zero. A store NEWER than the binary is left alone and reported (an older binary must not
destroy a newer one's work); the tools then use the inline fallback. Each derived section carries the version of
its deriver (tokenizer, each parser family, link rules); bumping one invalidates only that level for the files
it applies to, which simply re-enter the corresponding queue (the Bazel idea: the deriver is part of the cache
key).

### G. Concurrency and robustness

| Situation | Behavior |
|---|---|
| Several sessions on one root | One crawler (`CRAWL.LOCK`); all sessions read; each holds a lease on the socket |
| Two crawlers spawned at once | One wins the lock; the other exits 0 |
| Readers during a commit | They hold the previous generation's segments mapped; `store_stale` (one `stat`) tells them to reopen on their next query. Dropped segments are unlinked and stay readable while mapped (POSIX). When mapping fails the reader copies the file into memory (`store_seg_map` fallback), which is also safe |
| A segment vanishes between reading the manifest and mapping | `store_open` already retries up to 6 times |
| Crawler killed mid-batch | `tmp/` files and orphan segments are removed by the next `store_writer_begin`; the batch is redone |
| Session killed | Its socket closes; the lease ends |
| Disk full | `st_write_file` fails, the commit returns -1 and the previous generation stays. The crawler checks `statvfs` before a batch (needs twice the estimated segment size plus 64 MiB free), enters state `paused: disk full`, retries every 60 s, and reports it in the state file |
| Manual `index`, `compact`, `verify` while crawling | They take the store `LOCK` between batches (the crawler holds it only per commit) or become clients |
| Network or FUSE filesystem under the root | Store central; polling instead of events; `status` says "polling every N s" |
| ntfs3 (`/mnt/dados`) | Treated as local: it is an in-kernel filesystem, so `flock`, inotify and `rename` are expected to work; `mmap` has the read fallback. Not verified in this design (section 6) |
| Read-only project | Central store (the access check exists in `store_locate`) |
| Read-only or missing home | No crawler; inline fallback is impossible too; tools answer through scanning and say `index unavailable` |
| Corrupt store | Any reader error other than "no manifest" is sent to the crawler as a rebuild request. The crawler runs the full checksum pass (`store_seg_verify` plus the section verifiers); if it fails the store directory is renamed `store.bad-<time>` (one kept) and rebuilt. The tool output carries one line: `[index was damaged and is being rebuilt]`. A scrub of all checksums also runs when the crawler is idle and the last scrub is older than 7 days |
| Clock jumps, files with future mtime | Flagged `SDF_RACY`, re-hashed instead of trusted |
| inotify limit reached | Remaining directories polled; state file says how many are watched |

### H. Observability and control

**H.1 State file.** `~/.katana/run/coder-<key>.state`, rewritten by tmp and rename at most twice a second while
something changes: pid, root, store, generation, crawl epoch, per-level counts (done / eligible / total), frontier
size, change-queue size, current activity, watch mode and watch count, limits hit, last error, time of the last
complete pass per level. Any process can show progress without connecting.

**H.2 Commands.**

| Command | Output |
|---|---|
| `katana coder status [dir] [--json]` | Root and how it was resolved, store path and size, format, generation, a table of levels (files done, eligible, total, percent, age of last complete pass), directories by state, skipped files by reason, crawler (pid, activity, queue, watch mode) or "not running", last error |
| `katana coder crawl <dir>` | The crawler (detached use) |
| `katana coder index <dir> --watch` | The crawler in the foreground with a progress line |
| `katana coder index <dir>` | One synchronous pass, or a client of the live crawler (D.9) |
| `katana coder list`, `forget`, `gc` | F.5 |
| `katana coder verify`, `compact` | Existing |

**H.3 Progress for the user.** Console: one line on stderr when indexing starts, and one when each level
completes for the root; no spinner, nothing between prompts while the model is writing:

```
[index greymod: 373 files found]
[index greymod: text ready]
[index greymod: symbols and links ready (0.4 s)]
```

For a large root the first line is `[index kernel: 116000 files found, indexing in the background]` and
`/status` shows the percentages. The current message in `autoindex_dir` is replaced by these. Desktop: a status
bar item with the same three states and a percentage, fed by the state file through the agent process (the
desktop's own protocol was not read for this design; phase 6 must check `desktop-qt/README.md`).

**H.4 Environment.**

| Variable | Default | Meaning |
|---|---|---|
| `KATANA_AUTOINDEX` | 1 | 0: no crawler is started automatically; Find and Symbols use the inline fallback on first use (existing name, same intent) |
| `KATANA_INDEX_WAIT_MS` | 2000 | Budget per tool call for a cold or incomplete scope |
| `KATANA_INDEX_FRESH_MS` | 300 | Budget per tool call for pending changes |
| `KATANA_INDEX_THREADS` | min(4, cores/4), at least 1 | Analysis threads in the crawler |
| `KATANA_INDEX_WATCH` | auto | `inotify`, `poll`, `off` |
| `KATANA_INDEX_IDLE_EXIT_S` | 600 | Crawler exits after this long without leases or work |
| `KATANA_INDEX_MAX_FILES`, `KATANA_INDEX_MAX_TEXT_FILES`, `KATANA_INDEX_MAX_DEPTH` | 500000, 200000, 32 | D.8 |
| `KATANA_INDEX_MAX_MB` | 2048 | Total size of central stores before `gc` removes old ones |
| `KATANA_INDEX_LOCAL` | 1 | 0: never create a store inside a project |
| `KATANA_INDEX_INLINE_MAX_FILES` | 5000 | Cap of the inline fallback |
| `KATANA_INDEX_LEVELS` | 3 | Deepest level built (4 enables summaries) |
| `KATANA_FIND_INDEX` | legacy | `store`: unified Find (E) |
| `KATANA_CODER_INDEX_KB`, `KATANA_CACHE_DIR`, `KATANA_STORE_STALL` | existing | Unchanged |

### I. Performance

**I.1 Targets** (design goals extrapolated from the single observed runs in 1.2; nothing here was measured):

| Root size | N0 complete | N1 complete | N2 and N3 complete | One changed file, write to published | Query overhead for freshness |
|---|---|---|---|---|---|
| up to 2 thousand files | 50 ms | 0.6 s | 1.0 s (inside the 2 s wait: the call is effectively synchronous) | 50 ms | 1 `stat` + 1 per printed file |
| 25 thousand | 0.5 s | 10 s | 25 s | 50 ms | same |
| 116 thousand | 3 s | 60 s | 120 s | 50 ms | same |

If the measured values, when measurement is allowed again, miss these by a wide margin, the order of work in
section 5 still holds; only the wait budgets need retuning.

**I.2 Where the time goes today** (from reading the code, in expected order of weight for a cold build):

1. Reading and tokenizing every text file on one thread (`store_catalog_update` → `cs_hook_doc`: a `memcpy` per
   line into a static buffer, a hash-table lookup and possibly a `malloc` per term occurrence).
2. Parsing on one thread (`sy_doc_hook` → `coder_parse`): 0.29 s of 1.0 to 1.3 s in katana, 35.7 s at 116
   thousand files; it also copies each file once more.
3. Building one segment for the whole tree in memory, then four `fsync` calls (segment, `seg/`, manifest, store
   directory). At 116 thousand files the in-memory structures for 2.2 million symbols are large; `fsync` cost on
   ntfs3 is unknown.
4. The walk: `opendir`/`readdir`/`lstat` per entry by full path, plus the ignore-rule match (a recursive glob
   per rule per entry, across the stack of rule sets). 0.07 s per 25 thousand files warm.

And per query: knowledge-base load (10 to 14 ms), view rebuild (linear), the full update (linear), twice or
three times per turn (1.3 items 1, 4, 5).

**I.3 Parallelism.** In the crawler only. A pool of `KATANA_INDEX_THREADS` workers takes jobs:

- list one directory (`openat`, `readdir`, `fstatat` relative to the directory descriptor, ignore match) →
  entries;
- N1 for one file (read, hash, sniff, tokenize into a per-thread term buffer);
- N2/N3 for one file (parse into a per-thread `CoderFile`).

One coordinator thread owns the queues, assembles each batch's sections in document order from the workers'
per-file results, and is the only one that touches the store writer. This needs the indexing state to be passed
as a context instead of living in globals: `g_csix` and the static line buffer in `search.c`, `g_sy` in
`symindex.c`, and a check that `coder_parse` (parse.c, parse2.c) and the tokenizer (text.c) keep no static
state. The default of 4 threads at most, at low priority, leaves the rest of the machine to inference.

**I.4 Once per process, not once per query.**

| Thing | Today | Design |
|---|---|---|
| Knowledge base | Parsed from text in `cs_search_core` on every call | Loaded once per process and per project overlay; reloaded when the mtime of its directory or files changes (checked at most once per second) |
| Search view (`CsView`) and symbol view (`SyView`) | Built per call | Cached per root, tagged with the manifest identity that `store_stale` already compares; rebuilt only when the generation changes; up to 4 roots |
| State map (files per level per directory) | Does not exist | Aggregated with the view |
| Index update | Per call | Never in the query path |

Expected effect on the observed 11 to 15 ms query: the 10 to 14 ms of knowledge-base load disappear after the
first call (not measured).

**I.5 Memory.** Agent process: section E (mapped segments plus about 100 bytes per file of cached views,
estimate). Crawler: one batch in flight (bounded by 64 MiB of source and its derived sections), the queues
(about 24 bytes per pending file plus paths taken from the mapped catalog), the watch table (path hash to
directory, about 40 bytes per watched directory); it exits when idle.

### J. What the model sees

**J.1 Rule.** At most **one** status line, the first line of the tool output, and only when the scope of the
query is not complete at the level the query needs, or not fresh. In the steady state (index complete, no
pending change) the output has no status line at all: no cost in the prompt. Tool descriptions and the system
prompt do not change (the Symbols description already says "built on first use, then kept up to date").

**J.2 The lines.** Bracketed, like every existing engine note.

| Situation | Line |
|---|---|
| The call waited for a cold index that became complete within the budget | `[symbol index created in 0.4 s]` — **exactly today's text and conditions** (`sym_tool_run`: created, or more than 700 ms) |
| The call waited for an update that took more than 700 ms | `[symbol index updated in 1.2 s]` (today's text) |
| Root still crawling, text level incomplete | `[index is still being built: text 41200 of 116000 files; results are partial]` |
| Text complete, symbols not | `[index is still being built: symbols 3100 of 98000 files; results are partial]` |
| The queried prefix is behind | `[index: src/foo/ is at catalog level (names only); text and symbols are not ready]` |
| Files changed and were handled at print time | `[index: 2 files changed since indexed; shown from the files]` |
| Limits | `[index limited to 200000 files; data/ is at catalog level]` |
| Nested root inside the queried prefix | `[index: third_party/lib/ is a separate project; search it with path=third_party/lib]` |
| Damaged, unavailable | `[index was damaged and is being rebuilt]` / `[index unavailable: cannot write to ~/.katana; searched by scanning files]` |

Only the most important applicable line is printed (order of the table from the third row down: damaged, limits,
prefix behind, still being built, nested root, changed files).

**J.3 Absence.** `NOT FOUND` is an exact claim, so it is only printed bare when the whole queried scope reached
the level the query needs. Otherwise the existing "what was indexed" sentence (`sy_limits` in `symquery.c`)
carries the limit:

```
NOT FOUND in the part indexed so far (symbols: 3100 of 98000 files). This is not proof that it does not exist; try Grep, or ask again shortly.
```

**J.4 Certainty vocabulary.** Unchanged: the cascade (exact, derived, probable) with confidence 0 to 100
describes how a result was found, and a partial index does not lower the confidence of a hit that was found and
verified on the file. What a partial index limits is completeness: "all callers", `impact`, `refs` counts and
`NOT FOUND`. Those outputs say "so far" when the scope is partial: `3 callers so far (index partial)`.

**J.5 Top-down navigation.** The directory summary is exposed through the existing Symbols `outline` action
when `file` is a directory (open decision 1 in section 7):

```
bridge/  (14 files, 3 folders; C 9, shell 3, markdown 2; indexed: symbols)
  doc: Bridge between the phone app and the desktop session.
  folders: proto/ (6 files, C)  tests/ (11 files, shell)  vendor/ (not indexed: heavy folder)
  files: bridge.c (1204 lines)  session.c (611)  wire.h (140)  ...
  main symbols: bridge_open, bridge_send, session_list, wire_decode
```

"main symbols" appears only when the directory is at N2; before that the line reads `indexed: names only`.

**J.6 Training data (v7).** `data/curriculum/v7` contains 175 occurrences of `symbol index created in N s`,
normalized to `0.0 s` (`docs/curriculum.md`, line 471), produced by the real engine on small generated projects.
Under this design those scenarios produce the same bytes: the project is small, the first Symbols call waits
inside the budget, the index completes, and the same line is printed under the same condition. When the crawler
was already started at session open the line is absent, which is also what happens today with `autoindex`.
The new lines (J.2 rows 3 to 9) and the directory `outline` do not exist in v7; the model has never seen them.
They follow the bracket convention it knows, and they appear only on large or changing trees. The next
curriculum version should add a family for them (a partial index, a `NOT FOUND ... so far`, a directory
outline); until then this is a risk listed in section 6. The generator must set `KATANA_AUTOINDEX=0` or wait for
the crawler so that scenario outputs stay deterministic.

## 4. Timelines

Times are targets from I.1, not measurements.

### 4.1 Opening in a project folder (greymod, 373 files)

```
t=0        user runs katana in ~/source/greymod
           loop.c: index_ensure(cwd) -> index_resolve: root = greymod (has .git), prefix ""
           registry: no entry -> store = greymod/.katana/coder/store (launch folder is the root)
           socket dead -> spawn `katana coder crawl greymod` (detached, nice 10)
t=5 ms     crawler takes CRAWL.LOCK, opens empty store, frontier = [root]
t=30 ms    N0 published (gen 1): 373 files, 21 directories       stderr: [index greymod: 373 files found]
t=0.25 s   N1 published (gen 2)                                   stderr: [index greymod: text ready]
t=0.45 s   N2+N3 published by augmentation (gen 3, 4)             stderr: [index greymod: symbols and links ready (0.4 s)]
t=0.5 s    graph section written; crawler idle, watching 21 directories
t=8 s      user's first request; model calls Find
           index_ensure: dirty set empty, queue empty -> 1 stat of MANIFEST; view built once and cached
           output: no status line
```

If the user's request arrives at t=0.1 s, the Find call sends `PRIO`/`SYNC`, waits about 0.35 s inside its 2 s
budget, and answers from a complete index; a Symbols call in that position prints `[symbol index created in
0.4 s]`.

### 4.2 Opening in the home folder and citing another folder

```
t=0        user runs katana in ~            index_ensure(~): home is never a root -> nothing started
t=20 s     user: "in greymod/bridge, why does the session list arrive empty?"
           locate.c place_note resolves ~/source/greymod/bridge
           index_ensure(that path): walk up -> ~/source/greymod has .git -> root = greymod, prefix = bridge/
           registry lookup: greymod has a local store (from 4.1)? yes -> use it. no -> ~/.katana/coder/<key>/store
           crawler spawned; PRIO bridge/ puts that branch in band 0
t=20.03 s  N0 of the whole tree; bridge/ files go N1..N3 first (hot band)
t=20.1 s   model calls Find path=greymod/bridge -> root greymod, prefix bridge/
           SYNC N1 bridge/ returns complete in ms; the rest of the root is still at N0/N1
           output: results for bridge/, no status line (the queried prefix is complete)
t=20.5 s   whole root at N3
```

No `.katana/` is created in greymod by this session unless one already existed.

### 4.3 A repository of 100 thousand files

```
t=0        open; crawler starts; frontier breadth first, 4 workers listing directories
t=3 s      N0 complete: 116000 files, 9000 directories; 9000 inotify watches (limit 65536, budget 32768)
           stderr: [index kernel: 116000 files found, indexing in the background]
t=3..60 s  Q1 by band: recently modified source first; a batch (4096 files) is published about every second
t=10 s     model: Find "where the scheduler picks the next task"
           index_ensure: PRIO "" ; SYNC N1 waits 2 s; text 31000 of 116000
           output line 1: [index is still being built: text 31000 of 116000 files; results are partial]
           results come from band 1 and 2 files, which are the likeliest places anyway
t=12 s     model: Symbols outline kernel/sched/   -> PRIO kernel/sched/ : that directory's files jump to band 0,
           reach N2 in well under the 2 s budget; output is complete for that prefix, no status line
t=60 s     N1 complete      stderr: [index kernel: text ready]
t=60..180 s Q2 then Q3 (augmentation of each batch segment), then the graph, then compaction to 8 segments
t=180 s    stderr: [index kernel: symbols and links ready]
           user closes katana at any point: the crawler finishes the crawl, then exits after 10 idle minutes
           crawler killed at t=40 s: next session rebuilds the queues from the store and continues at ~text 80000
```

### 4.4 Two sessions at once on the same root

```
session A (console, launched in katana/)          session B (desktop, launched in ~, cites katana/)
index_ensure -> spawns crawler C                  index_ensure -> resolves the same root -> registry -> same store
C holds CRAWL.LOCK                                connect to C's socket (a spawn race would lose the lock and exit)
A: Find -> reads gen 12 in process                B: Symbols -> reads gen 12 in process (same mapped pages)
A: Edit src/tools/find.c -> TOUCH                 .
C: batch for find.c -> gen 13 (LOCK held ~10 ms)  .
A: Find -> SYNC returns at gen 13 -> fresh        B: next query: store_stale sees gen 13 -> view rebuilt once
A exits (lease closed)                            B still holds a lease -> C keeps running
```

### 4.5 The folder changes while the model works

```
t=0      index complete at gen 40; model has just received Find results
t=1 s    model: Edit session.c (adds a function)         tool calls index_touch -> dirty set {session.c}; TOUCH
t=1.02 s crawler: change queue -> session.c N0..N3 in one small segment -> gen 41
t=1.5 s  model: Bash "git checkout other-branch"         flag "tree may have changed"; inotify: 300 events
t=1.55 s crawler: debounce; 300 paths in the change queue; batches publish gen 42, 43
t=1.6 s  model: Symbols callers session_list
         index_ensure: flag set -> SYNC with 300 ms budget
           drained in time  -> query on gen 43, no status line
           not drained      -> query on what is published; every printed file is checked (C.4):
                               changed files are re-parsed in process, vanished ones dropped;
                               line 1: [index: 4 files changed since indexed; shown from the files]
t=1.9 s  crawler quiet for 2 s later -> graph rows of the changed files rebuilt
```

On a network mount the same sequence has no events at steps 1.02 and 1.55: `TOUCH` still covers the agent's own
edit, the checkout is caught by the `.git/HEAD` check on the next polling tick, and C.4 protects the printed
lines meanwhile.

## 5. Implementation phases

Agent-days are estimates for a Sonnet agent with this document. Every phase must compile with `build.bat`
(Linux build equivalent) and, per the owner's rule for this period, is reported as "done, compiled, nothing
measured". "Done when" lists observable behavior, checked by running the commands once, not by a test campaign.
Source edits follow the project rule (Python replacement scripts with count asserts).

| Phase | Content | Files | Done when | Days | Depends on |
|---|---|---|---|---|---|
| 1. Cheap queries | Knowledge base loaded once per process; `CsView` and `SyView` cached per root and generation (max 4); new `src/coder/indexctl.c` with `index_ensure(root, level, prefix, budget)` whose first version spawns the existing `katana coder index` when no manifest exists or the dirty flag is set and waits up to the budget; `coder_lookup` and `sym_ensure_index` call it instead of `store_catalog_update`; `index_touch` from Edit, Write, MultiEdit, NotebookEdit; "may have changed" flag from Bash | `search.c`, `symquery.c`, `symtool.c`, `kb.c`, new `indexctl.c`, `katana.c` (include), `src/tools/edit.c`, `write.c`, `multiedit.c`, `notebookedit.c`, `bash.c` | With `KATANA_FIND_TIME` set, a second Find on an unchanged indexed folder reports no walk and no knowledge-base load; an Edit followed by Symbols shows the new function; Symbols on a small unindexed folder still prints `[symbol index created in N s]` | 2 | — |
| 2. Roots, registry, place | `index_resolve`; registry files; `store_locate` takes the resolved root and the registry (one store per root); located root overrides the manifest's root in readers; moved-folder detection by (device, inode); `0700`/`0600`; nested-root, link and mount detection in `st_scan_walk`; secret file table and `SDF_SKIP_SECRET`; `.git/info/exclude`; Symbols and Find resolve `path` through `index_resolve` | new `src/coder/roots.c`, `store.c`, `storedocs.c`, `search.c` (`cs_read_doc`), `symquery.c`, `src/tools/find.c` (`ct_findtool` root and prefix), `symbols.c`, `common.c`, `src/agent/loop.c` | `katana coder status greymod/bridge` reports root `greymod`; a `.env` file appears in the catalog as skipped and none of its words are found by `katana coder search`; a nested clone inside a root is listed as `nested-root` and its files are absent from the parent's catalog; `mv` of a centrally stored folder keeps its index | 2.5 | 1 |
| 3. The crawler | `katana coder crawl`; `CRAWL.LOCK`; frontier, level queues, bands; `STS_DIRS`, `STS_CRAWL`; level bits in `StoreDoc.flags`; batches; N0 by append, N1 by supersession, N2 by augmentation; same-level compaction with the 24/8 thresholds; reconstruction of queues from the store; state file; socket with `PING`, `PRIO`, `TOUCH`, `SYNC`, `STATUS`, `STOP`; blocking writer lock; `index_ensure` switches from "spawn index" to "spawn or reach the crawler"; `autoindex_dir` calls it; idle exit; limits of D.8; generated/minified detection | new `src/coder/crawl.c`, `store.c`, `storedocs.c`, `search.c` and `symindex.c` (hooks take a context; mergers for the new sections), `indexctl.c`, `coder.c` (subcommands), `src/agent/loop.c` | On a large folder `katana coder status` shows N0 complete within seconds while text is still rising; `kill -9` of the crawler followed by a restart continues from the reported counts instead of zero; two crawlers started together leave one process; a small folder ends with the same sections as `katana coder index` produces today (`katana coder verify` ok) | 5 | 1, 2 |
| 4. Watching and freshness | inotify watcher with budget and overflow handling; polling sweep and its period; `.git/HEAD` check; change queue with debounce; branch re-entry (D.7); `SDF_RACY`; print-time check with single-file re-parse and the "changed since indexed" count; network/FUSE detection by `statfs` | `crawl.c`, new `src/coder/watch.c`, `search.c`, `symquery.c`, `symtool.c`, `src/tools/find.c` | Editing a file in another terminal changes `katana coder status` generation within a second with no query issued; `KATANA_INDEX_WATCH=poll` gives the same result within the polling period; deleting a file and querying it immediately yields no line for it | 3 | 3 |
| 5. Threads | Worker pool; context-passed indexing state; audit and removal of static state in `text.c`, `parse.c`, `parse2.c`, `search.c`, `symindex.c`; `KATANA_INDEX_THREADS` (1 = today's single-thread path, kept as the default until the owner allows a comparison) | `crawl.c`, `search.c`, `symindex.c`, `text.c`, `parse.c`, `parse2.c` | With 4 threads the store of a folder passes `verify` and `katana coder status` shows the same file, term, symbol and reference counts as with 1 thread | 2.5 | 3 |
| 6. What the model and the user see | Status lines of J.2 and the `NOT FOUND ... so far` form; "so far" on completeness outputs; state map in the cached view; directory summary and `outline` on a directory (after open decision 1); console lines of H.3; desktop status item; `katana coder status` full output and `--json` | `symquery.c` (`sy_limits`, `sy_emit`), `symtool.c`, `src/tools/find.c`, `symbols.c`, `src/agent/loop.c`, `storedocs.c` (`coder_cmd_status`), `desktop-qt/` | On a folder still crawling, Find's first line is the "still being built" line and disappears when the crawl ends; on a complete small folder the Find and Symbols outputs are byte-identical to the previous build's | 2 | 3 |
| 7. Unified Find (switch, default legacy) | `KATANA_FIND_INDEX=store`: file list from the view, candidates from `cs_search_core`, chunk ranking at query time over the candidate files, symbols from N2, `STS_GRAPH` built by the crawler (sym, path, rare, optional co), activation and neighbor notes reading it, request state keyed by path hash | `src/tools/find.c`, `findgraph.c`, `crawl.c`, new `src/coder/graph.c`, `search.c` | With the switch on, Find answers in a folder that has no `find-*.idx` and creates none; with the switch off nothing changes. Flipping the default and deleting the legacy code is a separate step taken by the owner | 4 | 3, 6 |
| 8. Housekeeping and failure paths | `list`, `forget`, `gc` and the daily automatic `gc`; version mismatch → rebuild, newer store → leave and report; corruption → quarantine and rebuild; scrub when idle; disk-full pause; inline fallback caps; read-only home path | `storedocs.c`, `roots.c`, `crawl.c`, `indexctl.c`, `coder.c` | Truncating a segment file by hand leads, on the next query, to the "damaged" line and a complete index shortly after; `katana coder forget` leaves no file for the root; `gc --dry-run` lists what it would remove | 1.5 | 3 |
| 9. Documents and training note | Update `docs/tools.md`, `docs/code-intelligence.md` (2.4, 2.6, 8.6), `docs/handoff.md`, `docs/plan.md`, README environment table; add the new output lines to the curriculum backlog in `docs/curriculum.md`; N4 summaries left as a described, switched-off option | `docs/` | Documents describe the behavior that exists | 1 | all |

Total: about 23.5 agent-days; phases 1 and 2 alone remove the per-call sweeps and the duplicate stores and can
ship first. Phases 4, 5, 6 and 8 are independent of each other after phase 3 and can be done in parallel by
separate agents (they touch `crawl.c` in different functions; phase 5 must merge last). Phase 7 conflicts with
any other work in `find.c` and `findgraph.c`.

Coordination with work in progress: the link sections (text keys) being written by another agent must expose a
per-document hook with a context argument, like the search and symbol hooks after phase 5, and reserve their
section numbers in the `STS_SYM_BASE` range; `STS_GRAPH` and `STS_SUMMARY` take the next free numbers there, and
`STS_DIRS`/`STS_CRAWL` take 3 and 4 in the catalog range.

## 6. Risks and honest uncertainties

1. **Nothing in this design was measured.** The targets in I.1 extrapolate single runs made by other agents on
   one machine. The split of cold-build time between reading, tokenizing and parsing is inferred from two data
   points (0.29 s of about 1.1 s; 35.7 s at 116 thousand files with no total given).
2. **ntfs3 behavior is assumed, not verified**: `flock`, inotify events, `mmap`, reading an unlinked mapped
   segment, and `fsync` cost on `/mnt/dados`. The store already falls back from `mmap` to reading. If `flock` or
   events misbehave there, `KATANA_INDEX_LOCAL=0` and `KATANA_INDEX_WATCH=poll` are the escape; phase 3 should
   check these four things by hand on that mount before relying on them.
3. **Augmentation (N1 → N2) rewrites each batch segment once per level.** Total write volume for a cold crawl is
   roughly the final index size times the number of levels. It is bounded per batch, but on a slow disk it may
   cost more than writing all levels per batch at once. If it does, the fallback is to make N1+N2+N3 a single
   step for bands 1 and 2 as well and keep the split only for bands 3 and 4.
4. **Many segments during a large crawl** (one per batch until compaction): every term lookup visits every
   segment. The 24-segment tolerance during a crawl may make queries slower exactly when the user is waiting.
5. **Thread safety of the parsers is unknown.** `parse.c` and `parse2.c` (4400 lines) were not audited for
   static state in this design. Phase 5 may be larger than estimated; it defaults to one thread for that reason.
6. **Unified Find changes ranking.** The candidate set comes from a different tokenizer and field model than
   Find's chunk index, and the v7 training outputs were produced with the legacy ranking. Without a measurement
   campaign there is no evidence either way, so the switch stays off until the owner allows a comparison.
7. **The model has never seen the new status lines** or a directory outline. The lines are short and in the
   bracket form it knows, but a local model may ignore "results are partial" and treat a partial `NOT FOUND` as
   final. The wording in J.3 is the mitigation; the real fix is a curriculum family.
8. **Secret detection is heuristic.** A password in a file named `config.yaml`, or a short token, enters the
   lexicon. The store permissions and the central location are the real protection; on ntfs3 the permissions do
   not exist.
9. **Root resolution can surprise.** A stray `.git` or `KATANA.md` high in the tree makes a large root; a project
   kept inside another repository without its own marker is indexed as part of the outer one. `katana coder
   status` shows how the root was chosen, and `.katanaignore` plus a `KATANA.md` in the inner folder fix both.
10. **inotify limits.** 65536 watches for the user, shared with editors and other tools; the crawler takes at
    most half and polls the rest, but another program may already have used the budget.
11. **Process count.** Several active roots mean several crawlers, each with threads. They are idle-exiting and
    low priority, but a session that cites many folders can start many at once; `index_ensure` should refuse to
    have more than 4 crawlers started by one session and queue the rest.
12. **What I remember versus what I know about the examples** is marked in 3.0. The design does not depend on
    any detail marked "recollection".
13. **The desktop integration was not read.** H.3 assumes the desktop can show a status item fed by the agent
    process.

## 7. Open decisions for the owner

1. **How the model asks for a directory summary.** Recommendation: the existing Symbols action `outline` with a
   directory in `file` (no new tool, no schema change, the description gains a few words). Alternative: a new
   action `tree`. Either one is new to the trained model and needs curriculum examples.
2. **katana launched in a subfolder of a repository.** With the rule "index root = repository root" the launch
   folder is not the root, so by the storage rule the store is central and no `.katana/coder/` appears in the
   launch folder. Recommendation: keep it that way (one store per root). Alternative: put the root's store in
   the launch subfolder, which breaks "one store per root" when two sessions start in different subfolders.
3. **Ignored files at N0.** Recommendation: ignored directories are recorded (so the model can be told they
   exist) and ignored files are only counted. Alternative: catalog ignored files by name as well, which makes
   "index everything" literal at the cost of large catalogs where build outputs live outside the heavy-folder
   list.
4. **When to flip Find to the unified index.** Recommendation: after phase 7 compiles, the owner uses it with
   `KATANA_FIND_INDEX=store` for a while; the default changes and the legacy code is deleted only on the
   owner's word, since no comparison is allowed in this phase.
5. **Crawler lifetime after the last session closes.** Recommendation: finish the crawl in progress, then exit
   after 10 idle minutes. Alternative: exit immediately with the last session (less background activity; a large
   repository then advances only while katana is open).
