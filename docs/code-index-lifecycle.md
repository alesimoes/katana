# Code index lifecycle (design)

Status: design only, 2026-11-23, revised the same day with the owner's answers (index roots are defined by
`katana init`; a new Symbols action `tree`; ignored content made visible and controllable; the crawler runs on demand and exits when done; all nine phases are to be built, in waves; the
unified Find becomes the default). No code was changed and nothing was measured for this document. Every number
marked "observed" comes from single runs reported by earlier agents; every number marked "target" is a design
goal, not a result. The design covers when, where, how and by whom the coder index is built, kept fresh,
queried and discarded, and what the model is told about it.

Related documents: `docs/code-intelligence.md` (2.4 storage, 2.6 incremental update, 8.5/8.6),
`docs/tools.md` (Find, Symbols), `docs/curriculum.md` and `data/curriculum/v7/README.md` (what training assumes).

## 0. The decision in ten lines

1. **The index is built by a crawler, on demand.** One crawler process per index root (`katana coder crawl
   <root>`), elected by a file lock, started by the session or query that needs it. It walks the tree breadth
   first, level by level, never runs inside the agent process, and **exits as soon as every requested folder
   has reached the last level**. It is not a daemon.
2. **Everything is indexed, at several depths of information.** N0 tree (directories, file names, size, date,
   language, a summary per directory), N1 text search, N2 symbols and references, N3 links, N4 derived summaries
   (optional). Each level is published and queryable as soon as a batch of it is ready.
3. **Breadth before depth.** The whole tree reaches N0, then every file reaches N1, then N2, then N3. The one
   exception is the hot band (files the user or the model is touching, and files that just changed): those go
   through all levels at once.
4. **One index, one walk.** The Find cache (`find-<hash>.idx`) disappears; Find reads the coder store, by
   default once phase 7 is built (`KATANA_FIND_INDEX=legacy` is the escape). The file graph and sparse activation
   are derived from N2/N3 sections instead of a second read of every file.
5. **An index root is a folder where the user ran `katana init`,** not a folder with `.git`. An initialized
   folder owns `<folder>/.katana/coder/` and covers its subtree, except subfolders that were initialized
   themselves (nested, independent indexes). A folder without init is absorbed by its nearest initialized
   ancestor, as a prefix.
6. **Folders with no initialized ancestor go to the home index** (`~/.katana/coder/`): one store per visited
   folder plus a registry. Only what the user visits is indexed (working folder, cited folder, `path` of Find or
   Symbols), never the whole home. A folder without init never gets a `.katana/` inside.
7. **Queries never walk the tree.** Each session watches the folders it uses (inotify, polling fallback) and the
   tools report their own writes; a pending change or an incomplete scope starts the crawler again. A query
   waits at most 300 ms for changes or 2 s for a cold index and checks each file it prints.
8. **The store stays what it is:** immutable segments, atomic manifest, one writer, lock-free readers. Levels are
   sections; crawl progress is published in two new section types in the same atomic swap. Corruption or a
   format change means rebuild, never migrate.
9. **The model sees at most one status line,** only when the queried scope is not complete or not fresh. For a
   small project the output is byte-identical to today, including `[symbol index created in N s]`. The tree
   and its per-directory summaries are asked with a new Symbols action, `tree`. What is ignored is always
   recorded with its reason and its size, shown by `katana coder ignored`, and can be included.
10. **All nine phases are to be built, in five waves** (about 23.5 agent-days of work, about 17 with the
    parallel waves), with a file list per wave so that agents do not collide.

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
| Init | `src/agent/extend.c` `init_cli` (`katana init`), `ext_slash` (`/init`) | `katana init [folder]` writes `KATANA.md` and then runs `coder_cmd_index`; it refuses when `KATANA.md` already exists unless `--force`. The slash command `/init` writes `KATANA.md` only and does not index. Neither leaves a marker that the folder was initialized. |
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
12. **Init is not a recognizable event.** `katana init` and `/init` do different things (table above), the
    first refuses to run where `KATANA.md` exists, and a folder that merely was a launch folder has the same
    `.katana/coder/store` as one that was initialized. Several other pieces also create `.katana/` in the
    working folder without any init (`fd_cache_path` in `find.c`, the prefix cache in
    `src/agent/prefix_cache.c`).

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

### A. Index root

**Decision (owner, 2026-11-23).** A root is defined by **init**, not by `.git` or `KATANA.md`: the user declares
"this folder is in the index" by running `katana init` (or `/init`) there. `.git` cannot be trusted as the
boundary of a project.

**A.1 The marker.** Init creates `<folder>/.katana/coder/ROOT` (a small text file: format version, date). The
store is `<folder>/.katana/coder/store`. The marker, not the presence of `.katana/` or of a store, is what makes
a folder a root: launch folders of earlier versions already contain `.katana/coder/store` without ever having
been initialized (1.3 item 12).

Changes to init that this requires (problems found in the code, see 1.1):

- `katana init` and `/init` do the same thing: write `KATANA.md` when it does not exist, create the marker,
  start the crawler.
- When `KATANA.md` already exists, init no longer stops: it keeps the file (unless `--force`), and still creates
  the marker and the index. Otherwise a folder that has a `KATANA.md` could never be initialized.
- If the folder has an older local store without marker, init adopts it (the crawler re-validates it). If the
  folder has a store in the home index, init moves it into the folder when both are on the same filesystem
  (`rename`), otherwise the folder is crawled again and the home store is removed.
- `katana init --remove` (new) deletes the marker and the local store; the folder falls back to the rules below.

**A.2 Resolution.** `index_resolve(path, &root, &prefix, &store)` (new file `src/coder/roots.c`):

1. `realpath` the path; a file resolves through its directory.
2. Walk up from the path, the path itself included. The first ancestor with a marker is the root; the query is
   restricted to the prefix of the path inside it. The walk does not stop at filesystem boundaries or at `.git`;
   it ends at `/`.
3. No initialized ancestor: the folder belongs to the **home index**. In the registry (F.2), the nearest
   registered home root that is the path or an ancestor of it absorbs the path (prefix). If there is none, the
   visited folder itself becomes a new home root, with its store in `~/.katana/coder/<key>/store`.
4. The home folder and `/` are never crawled as roots (already the rule in `autoindex_dir`, `ct_findtool`,
   `ct_symbols`): the home index is the collection of the folders visited, not an index of the home tree. A
   session whose working folder is the home (the desktop) indexes nothing until a folder is visited.
   `katana init` in the home folder is refused without `--force`.

"Visited" means exactly: the working folder at open and after `/cd`, a folder cited in a request (`place_note`),
and the `path` of a Find or Symbols call. Nothing else starts a crawl.

So, in the owner's examples: opening the desktop in `~` and asking about `greymod/bridge` creates the home root
`~/source/greymod/bridge` (only that subtree is crawled); doing `cd greymod` and `katana init` puts the index in
`greymod/.katana/coder/` and from then on `greymod/bridge` is a prefix of it; a second init in
`greymod/bridge` gives that subfolder its own `.katana/` and its own index.

**A.3 What a root covers.**

| Case | Rule | Why |
|---|---|---|
| Initialized subfolder inside a root | The parent records the directory at N0 as `nested-root` and does not descend; the two indexes are independent and nothing is duplicated. When a subfolder is initialized later, the parent's next crawl pass tombstones that prefix | Owner's rule: a second init is a second index |
| Home root visited above existing home roots (the user visits `greymod/bridge`, later `greymod`) | The ancestor becomes a home root and covers the whole subtree; the descendant home stores are retired (registry entries removed, stores deleted) once the ancestor has reached N1 for their prefix | One owner per file; without this the home index would hold the same files twice |
| Home root with an initialized folder inside | `nested-root`, not descended | Same rule as for initialized parents |
| Monorepo with several projects | One init at the top covers all of them (links across projects are kept); an init per project separates them. The user decides by where init is run | Replaces every marker heuristic |
| Subdirectory with its own `.git` (nested clone, submodule, git worktree) inside a root | Descended like any directory, since `.git` is not a boundary; `.claude` stays in the heavy-directory list (D.8). Git worktrees are the open decision in section 7 | Consequence of not trusting `.git`; worktrees are whole copies of the code and bring the inflation problem back under any name other than `.claude` |
| Symbolic links | Never followed. A link to a directory is recorded at N0 as `link` with its target; a root is always a real path | Cycles and double indexing. Same as the current `lstat` walk |
| Mount point inside a root | Recorded at N0 as `other-filesystem`, not descended | A mounted disk or network share inside a project is not project code |

**A.4 Consequences the owner should know.**

- Without init, the index ends at the edge of the visited folder: asking about `greymod/bridge` from the desktop
  finds nothing in `greymod/common`. Connections across the project need an init at the project's top (or a
  visit to the top folder).
- The desktop's home index grows with every folder visited. It is bounded by `katana coder gc` (F.5), which
  applies only to the home index.
- `proj_is_launch` and `g_launch_dir` (`src/tools/common.c`) are removed: where a store lives no longer depends
  on where the process was launched, so two sessions always agree (defect 7 of 1.3 disappears).
- The same rule has to be applied to the other creators of `<folder>/.katana/` (the prefix cache in
  `prefix_cache.c` and, until it is deleted, `fd_cache_path`): inside the folder only when it is initialized,
  otherwise under `~/.katana/cache`. Project skills (`.katana/skills`, created by an explicit command) are not
  touched by this design.

**Alternatives rejected.** Root by `.git`/`KATANA.md` markers (the first version of this design): rejected by
the owner; a stray `.git` high in the tree, or a project without one, gives the wrong root. Always the folder
named, with its own store: overlapping indexes for a folder and its subfolder. One index of the whole home:
unbounded, and most of it is never asked about.

### B. Who indexes

**Decision.** One **crawler process per index root**, **on demand**: `katana coder crawl <root>` (alias
`katana coder index --watch <root>` keeps it in the foreground). It is the only process that walks, reads,
parses and writes for that root. Sessions (console, desktop, `katana serve`, subagents) read the store in
process, watch the folders they use, and start the crawler when something has to be done.

Owner's rule for its life (2026-11-23): the crawler starts crawling when it is needed, stops when it reaches the
last level, and when it has covered every level of every requested folder it exits and closes. It does not wait
idle and it is not a daemon.

Lifecycle:

- **Start.** A session calls `index_ensure(root, ...)` (new `src/coder/indexctl.c`) when it opens in a folder,
  on `/cd`, when a request cites a folder (`place_note`), on init, and before a Find or Symbols query. If there
  is work (no index, a level missing for the scope, changes reported by the session's watcher or by a tool) and
  no crawler answers on the socket, it spawns one detached (`posix_spawn`, new session with `setsid`, stdio to a
  log file), as `autoindex_dir` spawns `coder index` today. The work is passed on the command line (`--prio
  <prefix>`, `--changed <path>...`, `--sweep`) or, when a crawler is already running, over its socket.
- **Election.** The crawler takes `flock(LOCK_EX|LOCK_NB)` on `CRAWL.LOCK` in the store directory and holds it
  while it lives. A second crawler for the same root fails the lock, forwards its work to the first over the
  socket and exits 0. The kernel releases the lock when the process dies, however it dies.
- **Writing.** The store's `LOCK` is taken per commit only (`store_writer_begin` ... `store_writer_end`), so a
  manual `katana coder compact` or `verify` can interleave between batches. `store_writer_begin` gains a blocking
  mode with timeout for the crawler.
- **End.** The crawler exits when the change queue, the frontier and every level queue are empty, the derived
  sections (graph) are written and compaction is done, and no request arrived during a short settle time
  (`KATANA_INDEX_SETTLE_MS`, default 2000: the same quiet period the graph rebuild waits for, so that a burst of
  edits or a checkout is one run and not hundreds). It finishes its crawl even if the session that started it
  has closed; a large repository does not have to be kept open to be indexed.
- **Restart.** Whoever needs it next: a session whose watcher saw changes, a tool that wrote a file, a query
  whose scope is incomplete, a new visit. With an explicit list of changed paths the crawler processes only
  those; on the first start of a session for a root, or when events may have been missed (overflow, polling
  mode, no session was watching), it starts with one stat sweep of the catalog (D.6).
- **Priority.** `nice 10`, idle I/O priority, a small thread pool (I.3): the session beside it is running a
  model.
- **If it dies.** Readers are unaffected (immutable segments, published manifest). The next start cleans `tmp/`
  and orphan segments (already done by `store_writer_begin`) and rebuilds its queues from the store. At most one
  unpublished batch is lost.
- **If it cannot start** (spawn fails, no writable home): the tools fall back to the current in-process
  `store_catalog_update`, capped by the wait budget and by `KATANA_INDEX_INLINE_MAX_FILES` (default 5000); above
  the cap they answer from Grep-style scanning and say that no index is available.
- **Limit.** One session starts at most 4 crawlers at a time; further roots wait in the session's own queue.

Socket protocol while a crawler is alive (one line request, one line reply, `SOCK_STREAM`, path
`~/.katana/run/coder-<key>.sock`; the run directory is on the home filesystem because socket files cannot be
assumed to work on ntfs3 or network mounts):

| Request | Reply | Use |
|---|---|---|
| `PING` | `OK <pid> <generation> <state>` | Is a crawler running, and how far is it |
| `PRIO <relative path>` | `OK` | Move a file or directory to the hot band (the model reads, edits or asks about it) |
| `TOUCH <relative path>` | `OK` | This file changed (a tool wrote it, or a session's watcher saw it) |
| `SWEEP` | `OK` | Re-validate the whole catalog by stat (events may have been missed) |
| `SYNC <level> <prefix> <timeout ms>` | `OK <generation> complete\|partial <done> <total>` | Block until the change queue is empty and `<prefix>` reached `<level>`, or the timeout |
| `STATUS` | one line of counters (same content as the state file, H.1) | Status command, console line, desktop |
| `STOP` | `OK` | `katana coder forget`, `init --remove` |

**Alternatives and their cost.**

| Option | For | Against | Verdict |
|---|---|---|---|
| Detached `coder index` per event (today) | Simple, no lifecycle | Every query must sweep because nothing knows what changed; unbounded cold build; no levels, no priority | Replaced |
| Thread inside the agent | No IPC; shares memory | Global state and `dup2` in the query path (1.3 item 6); dies with the session, so a 100-thousand-file crawl restarts with every short session; N sessions still need an election; competes with inference inside the same process | Rejected |
| One daemon per user (`katana indexd`) holding all roots and serving queries | One process, one inotify instance, hot roots in memory | Queries through IPC lose the main property of the store (mapped, in-process, lock-free reads shared by the page cache); a single failure point; a permanent process, which the owner does not want | Rejected |
| A crawler per root that stays alive to watch (first version of this design: idle exit after 10 minutes) | Changes are indexed even when no session asks | A background process outliving its work | Rejected by the owner |
| A crawler per root that runs to completion; the watcher lives in the sessions | No process without work; election is one `flock`; failure isolated per root | Changes made while no session is open are found only at the next visit (by the sweep); each session spends its own inotify watches | **Chosen** |

What the examples say: git has no daemon either: every command revalidates what it needs, and the optional
fsmonitor only answers "what changed since". Here the session's watcher is that fsmonitor, and the crawler is
the command that runs and ends. LMDB and SQLite show that readers need no coordination when the writer publishes
snapshots.

### C. Freshness

**Decision.** Three mechanisms, none of which walks the tree in the query path.

**C.1 The session watches (push).** The crawler exits when its work is done (B), so the watcher lives in each
session, inside `index_ensure`'s module, with no thread: one inotify descriptor per session, read without
blocking at three moments (before a Find or Symbols query, after each tool call, when the session is idle at the
prompt). For each root it uses, the session adds a watch per listed directory, taken from `STS_DIRS`
(`IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_CLOSE_WRITE|IN_ATTRIB|IN_DELETE_SELF|IN_MOVE_SELF`), and adds
the watches of new directories when a new generation lists them.

Events are coalesced by path in the session and handed to the crawler: `TOUCH` over the socket when one is
running, otherwise a spawn with `--changed`. A changed file goes through all its levels at once in one small
segment (the observed cost of one file is 0.013 s). A new directory is crawled as a branch (D.7).
`IN_Q_OVERFLOW`, or more than 2000 events under one directory between two reads, collapses to `SWEEP`.

Watch budget: at most a quarter of `fs.inotify.max_user_watches` per session (read from `/proc`; several
sessions each hold their own watches), in priority order (hot band first, shallow first). Directories beyond the
budget, and every directory when the filesystem is network or FUSE (detected with `statfs`), are covered by
**polling**: when the session reads its watcher and the last sweep of the root is older than
`max(2 s, 20 × the cost of the previous sweep)`, it asks for a `SWEEP`. Inside a sweep, directory mtime is used
only to skip `readdir` of unchanged directories (git's untracked-cache idea); file content changes are found by
the file `stat`, because a directory mtime does not change when a file is rewritten in place. A change of
`.git/HEAD` or `.git/index` mtime (two `stat` calls, when the root has a `.git`) asks for a sweep at once: a
branch switch changes many files together. This uses git only as a hint, never as the boundary of anything.

Changes made while no session was open are not watched by anyone: the first `index_ensure` of a session for a
root always asks for a sweep.

**C.2 The session tells (hint).** Edit, Write, MultiEdit and NotebookEdit call `index_touch(path)` after writing:
the path enters an in-process dirty set and goes to the crawler (`TOUCH`, or a spawn with `--changed`). Bash sets a "tree may have changed"
flag (the watcher sees the actual changes).

**C.3 The query waits, briefly (pull).** Before a Find or Symbols query, `index_ensure(root, level, prefix)`:

| Situation | Action | Budget (env) |
|---|---|---|
| Dirty set empty, flag clear, no watcher events, scope complete (from the cached state map) | Nothing: one non-blocking read of the watcher and one `stat` of the manifest (`store_stale`) | 0 |
| Dirty set non-empty, flag set or watcher events | Start or reach the crawler; `SYNC` until the change queue drains | 300 ms (`KATANA_INDEX_FRESH_MS`) |
| Scope not yet at the needed level (cold or still crawling) | Start or reach the crawler; `PRIO <prefix>` then `SYNC <level> <prefix>` | 2000 ms (`KATANA_INDEX_WAIT_MS`) |
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
nothing for folders without git. Trusting inotify alone: silently wrong on network mounts and after overflow. A crawler kept alive only to
watch: rejected by the owner (B).

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
| state | `discovered` (seen in the parent's listing, not listed yet), `listed`, `ignored`, `nested-root` (has its own init), `link`, `other-filesystem`, `too-deep`, `too-many-entries`, `unreadable` |
| reason, rule | For `ignored`: the reason class and the rule that decided (D.10) |
| skipped files, skipped bytes, counted | For `ignored`: how much was left out of the index, and whether the count is complete (D.10) |
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
| Change queue | Paths from the sessions' watchers and tools (`TOUCH`, `--changed`) and from sweeps | First in, first out, coalesced by path, 50 ms debounce |
| Frontier | Directories in state `discovered` | Heap by (band, depth, sequence): breadth first, hot branches first |
| Level queues Q1, Q2, Q3 (Q4) | Files that are at level k-1 and eligible for level k | Heap by (band, newest mtime first) |

#### D.4 Order: breadth first, then priority inside a level

Service order of the main loop, re-evaluated after every batch:

1. Change queue (changed files go N0→N3 at once; they are what the user is working on).
2. Hot band of any queue (band 0, all levels at once).
3. Frontier until empty: the whole tree reaches N0. (Target: seconds even for 100 thousand files.)
4. Q1 until empty: every eligible file reaches N1, band by band.
5. Q2 until empty, then Q3, then Q4 if enabled.
6. The file graph (`STS_GRAPH`), compaction, verification scrub, garbage collection. Then the crawler exits (B).

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
- change queue = the paths it was started with (`--changed`), or one stat sweep of the catalog (`--sweep`, the
  same sweep polling uses) when the session cannot vouch that it saw every change.

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
| Built-in heavy folders (the list and the justification of each entry are in D.10) | Directory recorded at N0 as `ignored`, with the rule name and the count of what is inside; not descended | "`node_modules/` (ignored: built-in heavy folder, 12,340 files)" |
| Ignored by `.gitignore`, `.katanaignore`, `.git/info/exclude` (to be added) | Ignored directory: recorded with the rule (`file:line` and pattern) and counted, not descended. Ignored file inside an indexed directory: cataloged at N0 by name with the rule, never read | "`out/` (ignored: .gitignore:4 `out/`, 512 files)" |
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

#### D.10 Ignored content: visible, explained, controllable

Owner's requirement (2026-11-23): folders such as `node_modules` do pollute and must be left out, but the user
and the model must be able to see what is being left out, why, and change it.

**Every ignored thing is recorded at N0 with its reason.** Nothing is dropped silently by the walk
(`st_scan_walk` today only increments a counter).

| Reason class | Recorded detail | Example of how it is shown |
|---|---|---|
| `gitignore` | File and line of the rule, and the pattern | `.gitignore:4 "build/"` |
| `katanaignore` | File and line, pattern | `.katanaignore:2 "fixtures/big/"` |
| `git-exclude` | `.git/info/exclude` line, pattern | `.git/info/exclude:7 "*.log"` |
| `heavy-folder` | Name of the built-in rule | `built-in heavy folder "node_modules"` |
| `excluded` | The entry of the per-root overrides file (below) | `excluded with katana coder exclude` |
| `binary` | Extension, or "NUL byte in the first 8000 bytes" | `binary (.png)` |
| `too-big` | Size and the limit | `larger than 2 MiB` |
| `secret` | Name of the secret rule; the content is never read | `secret file rule ".env*"` |
| `generated` | What matched (lock file, minified, marker) | `generated (line longer than 20000 bytes)` |
| `too-deep`, `too-many-entries`, `total-limit` | The limit that was hit | `more than 20000 entries` |
| `nested-root`, `link`, `other-filesystem`, `unreadable` | Target or error | `has its own index` |

Storage: for directories, the reason class, a rule reference and the pattern text in the `STS_DIRS` record. For
files, bits 12 to 15 of `StoreDoc.flags` hold the reason class and bits 16 to 31 an index into a small new
section `STS_RULES` of the segment (rule source file, line, pattern). The classes `binary`, `too-big`, `secret`,
`generated` and `unreadable` are the existing `SDF_SKIP_*` flags extended.

**What is counted.** An ignored directory is not descended by the crawl, so its size is not known for free. A
separate **counting pass** runs at the lowest priority (after N3, before the crawler exits): it walks each
ignored directory with `readdir` and `lstat` only, reads nothing, and stores the number of files and bytes that
were left out. It is capped per directory (200 thousand entries or 1 s; beyond that the record says "more than
200,000 files") and is redone only when the directory's mtime changed. Until it ran, outputs say "not counted
yet". `.git`, `.hg`, `.svn` and `.katana` are never counted (they are not project content).

**The built-in heavy list**, each entry with its reason for being there. This replaces the three different
lists of today (`st_ignored_dir` and the no-`.gitignore` list in `st_scan_walk`, `fd_skip_rel`). A built-in
rule applies only to a directory with exactly that name; `.katanaignore` or `katana coder include` overrides
any of them except the first group.

| Group | Names | Why ignored | Can be included |
|---|---|---|---|
| Version control and katana's own data | `.git`, `.hg`, `.svn`, `.katana` | Not project content; object stores and indexes; `.katana` holds the index itself | No |
| Agent work areas | `.claude` | Worktrees: whole copies of the project, every symbol duplicated (the inflation that already happened) | Yes |
| Installed dependencies | `node_modules`, `.venv`, `venv`, `site-packages`, `bower_components`, `.gradle`, `.m2`, `.cargo` | Third-party code fetched by a tool, typically 10 to 1000 times the size of the project; it floods search results and the rare-identifier edges | Yes (per package, see below) |
| Build outputs | `build`, `dist`, `out`, `target`, `obj`, `.next`, `.nuxt`, `__pycache__`, `.pytest_cache`, `.mypy_cache`, `.tox`, `coverage` | Derived from the sources that are already indexed; duplicates and minified text | Yes |
| Tool caches and editor state | `.cache`, `.idea`, `.vs`, `.parcel-cache`, `.terraform` | Machine-generated state, no code | Yes |

Changes against today's lists, all deliberate:

- `vendor` **leaves** the built-in list. In Go, PHP and many C projects `vendor/` is committed code that the
  project calls and that the model must be able to follow. It is indexed, in band 4 (last to deepen). A user who
  does not want it writes `vendor/` in `.katanaignore`.
- `ref`, `models`, `modelos` and the "more than 5000 files" heuristic tied to those names leave the code: they
  are katana's own layout, not general rules. The katana repository gets them through its `.gitignore` (which
  already lists them) or a `.katanaignore`. The general protection against a huge unlisted folder is the fan-out
  and total limits of D.8, which lower the depth (N0 only) instead of hiding the folder.
- `bin` and `lib` are **not** in the list: too often they hold sources.

**Never ignored by default**, whatever their size (they can only be left out by an explicit rule of the user):
`docs`, `doc`, `test`, `tests`, `spec`, `examples`, `samples`, `scripts`, `tools`, `src`, `lib`, `include`,
`config`, `.github`, `.gitlab`, `migrations`; and the files `README*`, `KATANA.md`, `CHANGELOG*`, `LICENSE*`,
`Makefile`, `CMakeLists.txt`, `Dockerfile`, build manifests (`package.json`, `Cargo.toml`, `pyproject.toml`,
`go.mod`, `*.csproj`). A `.gitignore` rule that hides one of these directories is still obeyed, but the
directory is then listed first by `katana coder ignored` with the note "usually worth indexing".

**Control.**

| Who | How | Effect |
|---|---|---|
| User, in the project | `.katanaignore`: a pattern ignores, `!pattern` includes what a `.gitignore` or a built-in rule ignores | Versionable with the project; highest precedence among rule files |
| User, without touching the project | `katana coder include <path>` and `katana coder exclude <path>` | Written to the per-root overrides file `overrides` next to the store (`<root>/.katana/coder/overrides` for an initialized folder, `~/.katana/coder/<key>/overrides` for a home root), so a folder without init gets no file inside it. Overrides win over every rule file. `include` starts the crawler for that branch |
| Model | The `path` argument of Find, Symbols or `tree` pointing INTO an ignored folder (`path=node_modules/express`) | A **scoped include** of exactly that folder: it is crawled like any visited folder (hot band), recorded in the overrides file as `scoped`, and searched **only** by queries whose `path` is inside it. Default searches of the project do not see it, so nothing is polluted. No new argument and no schema change |
| Anyone | Read, Grep, Glob | Unaffected by all of this: they read and scan the files directly. Ignoring limits the index and the ranked search, never access |

Limits of a scoped include: `KATANA_INDEX_INCLUDE_MAX_FILES` (default 50000) per request; secret files stay
unread; the first group of the list cannot be included.

**Commands.**

`katana coder ignored [folder] [--files] [--json]`:

```
ignored under /home/u/source/app (root: init at /home/u/source/app)
  12,340 files  310 MiB  node_modules/        built-in heavy folder "node_modules"
     512 files   48 MiB  build/               .gitignore:4 "build/"
      37 files  1.2 MiB  docs/api/generated/  .gitignore:9 "generated/"     usually worth indexing: under docs/
      14 files  9.1 MiB  (files)              binary (.png 9, .ico 3, .woff2 2)
       2 files    1 KiB  (files)              secret file rule ".env*"
       1 file   3.4 MiB  data/cities.json     larger than 2 MiB
total: 12,906 files, 372 MiB not in the index; 1,420 files indexed
to index something that is ignored:   katana coder include node_modules/express
                                      or a line  !docs/api/generated/  in .katanaignore
to ignore something that is indexed:  katana coder exclude <path>   or a line in .katanaignore
```

Sorted by file count; `--files` lists the individual ignored files with their rule. `katana coder status` gains
one block with the totals per reason class (files and bytes), and the hint to run `katana coder ignored`.

**In the answers to the model.** `tree` marks ignored folders with their reason (J.5). A search that finds
nothing says where it did not look (J.3).

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

**Migration.** None; the two formats are caches and nothing is converted. Owner's decision (2026-11-23): the
unified path is the **default as soon as phase 7 is built** (`KATANA_FIND_INDEX=store`). `KATANA_FIND_INDEX=legacy`
stays as an escape, which means `fd_save`, `fd_load`, `fd_cache_path`, `fd_build_file`, `fd_invert`, `fg_build`,
`fg_save` and `fg_load` stay in the binary for now; in legacy mode `fd_cache_path` follows the init rule (cache
inside the folder only when it is initialized). Removing the legacy code and letting `katana coder gc` delete the
old `find-*.idx` files is a later cleanup, on the owner's word.

**Memory in the agent process.** Today: the whole Find index in the heap (terms, per-file blobs, inverted lists,
graph) plus per-query views. Unified: mapped segments (resident only where touched, shared with the crawler and
other sessions through the page cache) plus one cached view per root: about 32 bytes per file for the search
view and about 72 bytes per file for the symbol view's live table, so on the order of 12 MB for 116 thousand
files and well under 1 MB for katana (estimates from the struct sizes, not measured). At most 4 roots are kept
open per process (least recently used closes).

**Risk.** Ranking changes for Find (different tokenizer and candidate source), with no measurement campaign
allowed now. The owner accepted this by making the unified path the default; the legacy switch is the way back.
See section 6.

### F. Storage and place

**F.1 The rule** (owner, 2026-11-23; replaces "only the launch folder has `.katana/` inside").

- An **initialized** folder keeps its index inside itself: `<folder>/.katana/coder/store`, with the marker
  `<folder>/.katana/coder/ROOT` (A.1) and the existing `.gitignore` containing `*`.
- Everything else is in the **home index**: `~/.katana/coder/<key>/store`, one store per home root (a visited
  folder with no initialized ancestor, A.2).
- A folder without init **never** gets a `.katana/` created inside it by the index.
- One store per root, whoever asks and from wherever the process was launched.
- If an initialized folder is on a network or FUSE filesystem, or is not writable, its marker stays in the folder
  but the store is placed in the home index (locks and mapping are only trusted on local filesystems);
  `katana coder status` says so.

**One store per home root, not one multi-root store.** Considered: a single store under `~/.katana/coder/`
holding every visited folder, with paths relative to the home. Rejected:

| Aspect | One store per visited root (chosen) | One multi-root store |
|---|---|---|
| Writer | One crawler per root; two folders are crawled at the same time by two processes | A single writer lock for everything the user visits: a large crawl blocks the index of every other folder |
| Compaction | Proportional to the root | Rewrites data of unrelated folders; the big folder's segments are merged with the small one's |
| Forgetting a folder, `gc` | Delete a directory | Tombstone a prefix and wait for compaction to reclaim the space |
| Corruption | Costs one folder | Costs the index of everything |
| Search view per query | Sized by the root | Sized by everything ever visited, filtered by prefix on every query |
| Ranking statistics (document frequencies, average lengths) | Per project | Mixed across unrelated projects |
| Cost | A registry (F.2) and the absorption rule of A.3 | None of those |

The home index is therefore a directory of stores with a registry; "the index of everything" that the desktop
uses is the sum of them.

**F.2 Registry.** `~/.katana/coder/roots/<key>.root`, one small text file per root, written by tmp and rename:
real path, kind (`init` or `home`), device and inode of the root directory, store directory, format version,
created, bytes on disk. Initialized roots are registered too (so `katana coder list` shows them and absorption
can be decided without walking the disk), but for them the marker in the folder is the authority and the
registry entry is only a cache of it. Its
mtime is the last-use time (touched at most once per hour per process). `<key>` is the existing
`store_hash64(realpath)` in hex.

- Lookup by path: the walk up of A.2 does one `stat` per ancestor for the marker; the home-root lookup is one
  `stat` of `<key>.root` per ancestor (a few dozen `stat` calls in total, cached per session and per path).
- **Moved folder**: when a path has no registry entry, the registry is scanned for an entry whose (device, inode)
  equals the folder's and whose recorded path no longer exists; if found, the home store directory is renamed
  to the new key and the entry rewritten. An initialized folder's store travels with the folder.
- **Root recorded in the manifest differs from the located root** (moved local store): the reader uses the
  located root for all file paths, never the recorded one, and the crawler rewrites it
  (`store_writer_set_root`) and re-validates the catalog. Fixes defect 8.
- **Copied folder**: a new root, built from zero. Identity by git remote was considered and rejected: two clones
  of one remote are different trees, and the saving is one crawl.

**F.3 Permissions.** Store directories `0700`, every file `0600` (segments, manifest, locks, state, socket,
registry). On filesystems that ignore modes (the owner's `/mnt/dados` is ntfs3 mounted with fixed uid and gid)
this is not enforceable; `katana coder status` says so. An index stays out of such a disk simply by not running
init there: without init the store is in the home.

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
| `katana coder forget <dir>` | `STOP` to the crawler, delete the store and the registry entry of a home root. For an initialized folder it points to `katana init --remove` |
| `katana coder gc [--dry-run]` | Home index only: remove stores whose root has not existed for 30 days; remove home roots absorbed by an ancestor or by an init (A.3); remove least recently used stores while the total is above `KATANA_INDEX_MAX_MB` (default 2048) and the store was unused for 14 days; remove orphan directories without a registry entry. Lists, without deleting, local stores that have no marker (launch folders of earlier versions). Never removes the store of an initialized folder |
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
| Several sessions on one root | At most one crawler at a time (`CRAWL.LOCK`); all sessions read; each session has its own watcher and may be the one that starts the next crawler |
| Two crawlers spawned at once | One wins the lock; the other forwards its work over the socket and exits 0 |
| Readers during a commit | They hold the previous generation's segments mapped; `store_stale` (one `stat`) tells them to reopen on their next query. Dropped segments are unlinked and stay readable while mapped (POSIX). When mapping fails the reader copies the file into memory (`store_seg_map` fallback), which is also safe |
| A segment vanishes between reading the manifest and mapping | `store_open` already retries up to 6 times |
| Crawler killed mid-batch | `tmp/` files and orphan segments are removed by the next `store_writer_begin`; the batch is redone |
| Session killed | Nothing to clean: its watcher dies with it; a running crawler finishes and exits |
| A request arrives exactly as the crawler exits | The client's connect fails or the socket is gone: `index_ensure` spawns a new crawler. The exiting crawler removes its socket before releasing `CRAWL.LOCK`, and a new crawler waits up to 200 ms for the lock |
| Disk full | `st_write_file` fails, the commit returns -1 and the previous generation stays. The crawler checks `statvfs` before a batch (needs twice the estimated segment size plus 64 MiB free), enters state `paused: disk full`, retries every 60 s, and reports it in the state file |
| Manual `index`, `compact`, `verify` while crawling | They take the store `LOCK` between batches (the crawler holds it only per commit) or become clients |
| Network or FUSE filesystem under the root | Store in the home index; sweeps instead of events; `status` says so |
| ntfs3 (`/mnt/dados`) | Treated as local: it is an in-kernel filesystem, so `flock`, inotify and `rename` are expected to work; `mmap` has the read fallback. Not verified in this design (section 6) |
| Read-only project | Init fails with a clear message (it cannot create the marker); without init the store is in the home anyway |
| Read-only or missing home | No crawler; inline fallback is impossible too; tools answer through scanning and say `index unavailable` |
| Corrupt store | Any reader error other than "no manifest" is sent to the crawler as a rebuild request. The crawler runs the full checksum pass (`store_seg_verify` plus the section verifiers); if it fails the store directory is renamed `store.bad-<time>` (one kept) and rebuilt. The tool output carries one line: `[index was damaged and is being rebuilt]`. A scrub of all checksums also runs at the end of a crawler run when the last scrub is older than 7 days |
| Clock jumps, files with future mtime | Flagged `SDF_RACY`, re-hashed instead of trusted |
| inotify limit reached | Remaining directories covered by sweeps; `katana coder status` run inside a session says how many are watched |

### H. Observability and control

**H.1 State file.** `~/.katana/run/coder-<key>.state`, rewritten by tmp and rename at most twice a second while
something changes: pid, root, store, generation, crawl epoch, per-level counts (done / eligible / total), frontier
size, change-queue size, current activity, limits hit, last error, time of the last complete pass per level, and
`finished` with the end time once the crawler has exited (the file stays). Any process can show progress without
connecting.

**H.2 Commands.**

| Command | Output |
|---|---|
| `katana init [dir] [--force] [--remove]` | A.1: marker, `KATANA.md` when missing, start of the crawl; `--remove` undoes the marker and the local store |
| `katana coder status [dir] [--json]` | Root and how it was resolved (`init at <folder>` or `home index, visited <folder>`), store path and size, format, generation, a table of levels (files done, eligible, total, percent, age of last complete pass), directories by state, skipped and ignored totals by reason class (files and bytes, D.10), crawler (pid, activity, queue, watch mode) or "not running", last error |
| `katana coder crawl <dir> [--prio p] [--changed f...] [--sweep]` | The crawler (detached use); exits when done |
| `katana coder index <dir> --watch` | The crawler in the foreground with a progress line, plus its own watcher: it does not exit, for users who want an index kept current without a session |
| `katana coder index <dir>` | One synchronous pass, or a client of the live crawler (D.9) |
| `katana coder list`, `forget`, `gc` | F.5 |
| `katana coder ignored [dir] [--files] [--json]`, `include <path>`, `exclude <path>` | D.10: what is left out of the index, why, how much, and how to change it |
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
| `KATANA_AUTOINDEX` | 1 | 0: nothing is indexed on open, `/cd` or mention; Find and Symbols still start the crawler on first use (existing name, same intent) |
| `KATANA_INDEX_WAIT_MS` | 2000 | Budget per tool call for a cold or incomplete scope |
| `KATANA_INDEX_FRESH_MS` | 300 | Budget per tool call for pending changes |
| `KATANA_INDEX_THREADS` | min(4, cores/4), at least 1 | Analysis threads in the crawler |
| `KATANA_INDEX_WATCH` | auto | `inotify`, `poll`, `off` |
| `KATANA_INDEX_SETTLE_MS` | 2000 | Quiet time with empty queues before the crawler writes the graph and exits |
| `KATANA_INDEX_MAX_FILES`, `KATANA_INDEX_MAX_TEXT_FILES`, `KATANA_INDEX_MAX_DEPTH` | 500000, 200000, 32 | D.8 |
| `KATANA_INDEX_INCLUDE_MAX_FILES` | 50000 | Cap of one scoped include requested through `path` (D.10) |
| `KATANA_INDEX_MAX_MB` | 2048 | Total size of the home index before `gc` removes old stores |
| `KATANA_INDEX_INLINE_MAX_FILES` | 5000 | Cap of the inline fallback |
| `KATANA_INDEX_LEVELS` | 3 | Deepest level built (4 enables summaries) |
| `KATANA_FIND_INDEX` | store (from phase 7 on) | `legacy`: Find's own cache, kept as an escape (E) |
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
directory, about 40 bytes per watched directory, held by each session, not by the crawler); the crawler exits
when its work is done.

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
| A folder with its own init inside the queried prefix | `[index: third_party/lib/ has its own index; search it with path=third_party/lib]` |
| A `path` outside the root of the working folder, first visit | no extra line: it is resolved to its own root (A.2) and the lines above apply to that root |
| Damaged, unavailable | `[index was damaged and is being rebuilt]` / `[index unavailable: cannot write to ~/.katana; searched by scanning files]` |

Only the most important applicable line is printed (order of the table from the third row down: damaged, limits,
prefix behind, still being built, nested root, changed files).

**J.3 Absence.** `NOT FOUND` is an exact claim, so it is only printed bare when the whole queried scope reached
the level the query needs. Otherwise the existing "what was indexed" sentence (`sy_limits` in `symquery.c`)
carries the limit:

```
NOT FOUND in the part indexed so far (symbols: 3100 of 98000 files). This is not proof that it does not exist; try Grep, or ask again shortly.
```

When the index is complete but folders under the queried scope are ignored, the absence is exact only for the
indexed set, and the answer names where it did not look (the up to three largest ignored folders under the
scope; version-control folders are never named):

```
NOT FOUND in the indexed set; 3 ignored folders could hold it: node_modules/, build/, ref/. To search one, set `path` to it.
```

"Could hold it" is a statement about where the index did not look, not a guess that the name is there: nothing
in an ignored folder is read to produce the line. Following the hint (`path=node_modules/express`) is the
scoped include of D.10.

**J.4 Certainty vocabulary.** Unchanged: the cascade (exact, derived, probable) with confidence 0 to 100
describes how a result was found, and a partial index does not lower the confidence of a hit that was found and
verified on the file. What a partial index limits is completeness: "all callers", `impact`, `refs` counts and
`NOT FOUND`. Those outputs say "so far" when the scope is partial: `3 callers so far (index partial)`.

**J.5 Top-down navigation: the new Symbols action `tree`** (owner's decision, 2026-11-23: a new action, not
`outline` on a folder).

Arguments (all existing parameters of Symbols; only the `action` enum gains a value and the descriptions of
`file` and `depth` gain a few words):

| Argument | Meaning for `tree` | Default, limits |
|---|---|---|
| `action` | `"tree"` | — |
| `file` | The folder to show, relative to the project (`file_path` accepted as today) | The project folder |
| `path` | The project folder, as for every Symbols action; a `path` inside an ignored folder is a scoped include (D.10) | The working folder |
| `depth` | Levels of subfolders listed under the folder | 2, maximum 4 |
| `limit` | Maximum number of folder lines | 40, maximum 100 |

It needs only N0 and is answered from the cached state map and the `STS_DIRS` records: no file is read at query
time. One line per folder, breadth first, larger folders first inside a level; each line carries what the folder
holds, how deep its index is, and its main symbols when they exist:

```
greymod/  373 files (C 210, shell 40, markdown 23)  [indexed: symbols and links]
  doc: Game modding toolkit: bridge to the phone, packer, desktop session.
  files here: KATANA.md, Makefile, README.md (+4)
  bridge/         14 files (C 9, shell 3)   [symbols]     doc: Bridge between the phone app and the desktop session.
                  main: bridge_open, bridge_send, session_list, wire_decode
  bridge/proto/    6 files (C)              [symbols]     main: proto_encode, proto_decode
  bridge/tests/   11 files (shell)          [text]        symbols not ready
  packer/         88 files (C 80)           [symbols]     main: pack_open, pack_entry_add, pack_write
  docs/           23 files (markdown)       [text]        doc: User manual and file formats.
  data/        9,200 files (json)           [names only]  limit: more than 20000 entries below
  node_modules/   (ignored: built-in heavy folder, 12,340 files)
  build/          (ignored: .gitignore:4 "build/", 512 files)
  third_party/zlib/  (has its own index; use path=third_party/zlib)
(6 more folders not shown; raise `limit` or call tree on a subfolder)
ignored: 2 folders, 12,852 files not in the index. To search inside one, set `path` to it.
```

Rules of the output:

- The index depth of a folder is the lowest level reached by all eligible files under it, in words the model can
  act on: `[names only]` (N0), `[text]` (N1), `[symbols]` (N2), `[symbols and links]` (N3). When a branch is
  behind, the reason or the consequence follows on the same line (`symbols not ready`, `limit: ...`).
- `main:` lists up to 4 symbols: the definitions under the folder most referenced from outside it (from N2;
  absent below N2). `doc:` is the README or `KATANA.md` line stored at N0, when the folder has one.
- Ignored folders are always listed (they do not count against `limit`), each with its reason and count in one
  line; without the counting pass yet, `(ignored: ..., not counted yet)`.
- Files are not listed, except a short `files here:` for the folder asked; the file list of a folder is what
  Glob is for, and the symbols of a file are `outline`.
- The whole output respects the same size cap as the other Symbols actions (about 6500 characters).
- The status line of J.2, when one applies, is still the first line.

`tree` on a folder that reached only N0 is complete as a tree; it never waits for deeper levels (its
`index_ensure` asks for N0 of the prefix only).

**Training.** `tree` does not exist in v7: the model has never called it and has never read its output. Until
the next curriculum (v8) teaches it, the only prompt for it is the tool description. v8 needs a family for it
(phase 9 records the requirement): asking what a project contains, descending two levels with `tree` before any
Read, reacting to `[names only]` and to an ignored folder (setting `path` to it when the answer must be there),
and not calling `tree` when the request already names a file or symbol.

Init changes nothing in what the model reads: a root is described by the folder path the model already uses, and
the status lines never mention init, the home index or where a store is. The one visible effect is scope:
without an init above it, a query on `greymod/bridge` covers only that folder, and `NOT FOUND` there keeps the
existing "what was indexed" sentence, which now names the folder searched.

**J.6 Training data (v7).** `data/curriculum/v7` contains 175 occurrences of `symbol index created in N s`,
normalized to `0.0 s` (`docs/curriculum.md`, line 471), produced by the real engine on small generated projects.
Under this design those scenarios produce the same bytes: the project is small, the first Symbols call waits
inside the budget, the index completes, and the same line is printed under the same condition. When the crawler
was already started at session open the line is absent, which is also what happens today with `autoindex`.
The new lines (J.2 rows 3 to 9), the ignored-folder sentence of J.3 and the `tree` action do not exist in v7; the model has never seen them.
They follow the bracket convention it knows, and they appear only on large or changing trees. The next
curriculum version (v8) should add a family for them (a partial index, a `NOT FOUND ... so far`, a `NOT FOUND
in the indexed set; ... ignored folders`, the `tree` action); until then this is a risk listed in section 6. The generator must set `KATANA_AUTOINDEX=0` or wait for
the crawler so that scenario outputs stay deterministic. Two more things for the generator: its scenario
projects are not initialized, so their indexes go to the home index of the generating user (it should point
`HOME` or `KATANA_CACHE_DIR`-style overrides at a scratch folder and clean it); and with the unified Find as the
default, new Find outputs will differ in ranking from the v7 ones, so v7-style families regenerated after phase 7
are not comparable line by line with the stored v7 data.

## 4. Timelines

Times are targets from I.1, not measurements.

### 4.1 Working directly in a project: `cd greymod`, `katana init` (373 files)

```
t=0        user runs `katana init` in ~/source/greymod (or /init inside a session there)
           init: KATANA.md written if missing; marker greymod/.katana/coder/ROOT created
           a home store of greymod or of folders under it, if any, is adopted or retired (A.1, A.3)
           spawn `katana coder crawl greymod` (detached, nice 10)
t=5 ms     crawler takes CRAWL.LOCK, opens the store, frontier = [root]
t=30 ms    N0 published (gen 1): 373 files, 21 directories       stderr: [index greymod: 373 files found]
t=0.25 s   N1 published (gen 2)                                   stderr: [index greymod: text ready]
t=0.45 s   N2+N3 published by augmentation (gen 3, 4)             stderr: [index greymod: symbols and links ready (0.4 s)]
t=0.5 s    queues empty; settle 2 s
t=2.5 s    graph section written; crawler EXITS

later      user opens katana in greymod: index_ensure -> marker found -> root greymod, store inside the folder
           session adds 21 inotify watches; first visit of the session -> spawn crawler --sweep
           sweep finds nothing changed (ms) -> crawler exits after the settle time
           model calls Find: no events, scope complete -> 1 read of the watcher, 1 stat of MANIFEST; no status line
```

If a request arrives while the first crawl is still running, the Find call sends `PRIO`/`SYNC`, waits inside its
2 s budget and answers from a complete index; a Symbols call in that position prints `[symbol index created in
0.4 s]`.

### 4.2 The desktop in the home folder, citing a folder that was never initialized

```
t=0        desktop session, working folder ~         index_ensure(~): home is never a root -> nothing started
t=20 s     user: "in greymod/bridge, why does the session list arrive empty?"
           locate.c place_note resolves ~/source/greymod/bridge
           index_resolve: no marker in bridge, greymod, source, ~ ; registry: no home root at or above it
             -> new home root ~/source/greymod/bridge, store ~/.katana/coder/<key>/store, registry entry written
           spawn crawler for that root; nothing is created inside greymod
t=20.03 s  N0 of bridge/ only (the rest of greymod is not visited, not crawled)
t=20.1 s   model calls Find path=greymod/bridge -> same root, prefix ""
           SYNC N1 returns complete in ms; output: results, no status line
t=20.4 s   bridge at N3; settle; crawler exits at t=22.4 s

t=5 min    user: "and who calls it from greymod/common?"
           index_resolve(greymod/common): no marker; no home root at or above it -> a second home root
           (the two home roots do not see each other: links between bridge/ and common/ are not in any index)

t=9 min    user cites ~/source/greymod itself
           index_resolve: no marker; new home root greymod, ancestor of the two existing ones
           crawler for greymod covers the whole subtree; when it reaches N1 for bridge/ and common/ their home
           stores are retired (A.3); from now on both are prefixes of greymod

another day  user does `cd greymod && katana init`
           the home store of greymod is moved into greymod/.katana/coder/store (same filesystem) or rebuilt;
           the home index no longer holds greymod
```

### 4.3 A folder of 100 thousand files

```
t=0        visit (open in it, or init); crawler starts; frontier breadth first, 4 workers listing directories
t=3 s      N0 complete: 116000 files, 9000 directories
           the session adds 9000 inotify watches (limit 65536, its budget 16384)
           stderr: [index kernel: 116000 files found, indexing in the background]
t=3..60 s  Q1 by band: recently modified source first; a batch (4096 files) is published about every second
t=10 s     model: Find "where the scheduler picks the next task"
           index_ensure: PRIO "" ; SYNC N1 waits 2 s; text 31000 of 116000
           output line 1: [index is still being built: text 31000 of 116000 files; results are partial]
           results come from band 1 and 2 files, which are the likeliest places anyway
t=11 s     model: Symbols tree (depth 1)          -> answered from N0 at once: the top folders, their sizes,
           `[names only]` or `[text]` per branch, the ignored folders with reason and "not counted yet"
t=12 s     model: Symbols callers pick_next_task, path unchanged -> PRIO: the files that mention it jump to
           band 0; Symbols tree kernel/sched/ a moment later shows that branch at `[symbols]`
t=60 s     N1 complete      stderr: [index kernel: text ready]
t=60..180 s Q2 then Q3 (augmentation of each batch segment), then the graph, then compaction to 8 segments
t=180 s    stderr: [index kernel: symbols and links ready]      t=182 s: crawler EXITS
           user closes katana at t=40 s: the crawler keeps going to the last level, then exits
           crawler killed at t=40 s: the next visit starts a crawler that rebuilds its queues from the store
           and continues at about text 80000, not at zero
```

### 4.4 Two sessions at once on the same root

```
session A (console, opened in katana/)            session B (desktop, in ~, cites katana/)
index_resolve -> marker in katana/ -> same root   index_resolve -> the same marker -> same root, same store
first visit: spawn crawler C1 --sweep             first visit: spawn C2 -> lock held by C1 -> forwards, exits
C1: nothing changed -> settle -> exits            .
A watches 60 directories                          B watches the same 60 directories (its own watches)
A: Find -> reads gen 12 in process                B: Symbols -> reads gen 12 in process (same mapped pages)
A: Edit src/tools/find.c -> dirty set; no crawler running -> spawn C3 --changed src/tools/find.c
C3: find.c N0..N3 in one small segment -> gen 13  B's watcher also saw the write
A: Find -> SYNC returns at gen 13 -> fresh        B: next query reads its watcher -> sends TOUCH find.c to C3
                                                     (already done: hash unchanged, nothing written)
C3: queues empty, settle 2 s, graph rows, EXITS   B: store_stale sees gen 13 -> view rebuilt once
```

### 4.5 The folder changes while the model works

```
t=0      index complete at gen 40, no crawler running; model has just received Find results
t=1 s    model: Edit session.c (adds a function)         index_touch -> dirty set {session.c}
                                                          spawn crawler --changed session.c
t=1.03 s crawler: session.c N0..N3 in one small segment -> gen 41; settle timer starts
t=1.5 s  model: Bash "git checkout other-branch"         flag "tree may have changed"
                                                          after the tool call the session reads its watcher:
                                                          300 events -> TOUCH x300 to the running crawler
t=1.55 s crawler: batches publish gen 42, 43
t=1.6 s  model: Symbols callers session_list
         index_ensure: flag set -> SYNC with 300 ms budget
           drained in time  -> query on gen 43, no status line
           not drained      -> query on what is published; every printed file is checked (C.4):
                               changed files are re-parsed in process, vanished ones dropped;
                               line 1: [index: 4 files changed since indexed; shown from the files]
t=3.6 s  2 s without requests: graph rows of the changed files rebuilt; crawler EXITS
t=40 s   the user saves a file in an editor; nothing runs
t=55 s   next tool call or query: the session reads its watcher, finds the event, spawns crawler --changed
```

On a network mount the same sequence has no events: the tools' own writes are still reported (`index_touch`),
the checkout is caught by the `.git/HEAD` check or by the next sweep, and C.4 protects the printed lines
meanwhile.

## 5. Implementation: nine phases in five waves

The owner ordered all nine phases. They are grouped in waves; inside a wave the agents run in parallel, each in
its own worktree, and **an agent edits only the files listed for it**. Agent-days are estimates for a Sonnet
agent with this document. Every phase must compile with the project's build and, per the owner's rule for this
period, is reported as "done, compiled, nothing measured". "Done when" lists observable behavior, checked by
running the commands once, not by a test campaign. Source edits follow the project rule (Python replacement
scripts with count asserts). Each wave is merged into `main` before the next starts.

### 5.1 The phases

| Phase | Content | Done when | Days |
|---|---|---|---|
| 1. Cheap queries | Knowledge base loaded once per process; `CsView` and `SyView` cached per root and generation (max 4 roots); `indexctl.c` with `index_ensure(root, level, prefix, budget)`, whose first version spawns the existing `katana coder index` when no manifest exists or the dirty flag is set and waits up to the budget; `coder_lookup` and `sym_ensure_index` call it instead of `store_catalog_update`; `index_touch` from Edit, Write, MultiEdit, NotebookEdit; "may have changed" flag from Bash | With `KATANA_FIND_TIME` set, a second Find on an unchanged indexed folder reports no walk and no knowledge-base load; an Edit followed by Symbols shows the new function; Symbols on a small unindexed folder still prints `[symbol index created in N s]` | 2 |
| 2. Roots by init, registry, place | Marker `.katana/coder/ROOT`; `katana init` and `/init` unified (marker, `KATANA.md` only when missing, crawl; `--remove`); `index_resolve` (walk up to the marker, else home root through the registry, absorption of A.3); `store_locate` takes the resolved root and store; `proj_is_launch` and `g_launch_dir` removed; the prefix cache and `fd_cache_path` follow the init rule; located root overrides the manifest's root in readers; moved-folder detection by (device, inode); `0700`/`0600`; nested-init, link and mount detection in `st_scan_walk`; secret file table and `SDF_SKIP_SECRET`; `.git/info/exclude`; Find and Symbols resolve `path` through `index_resolve` | After `katana init` in a folder, `katana coder status sub/dir` reports `init at <folder>` and the store inside it; in a folder with no init anywhere above, no `.katana/` appears inside it and `katana coder list` shows a home root; a subfolder initialized inside an initialized folder is listed as `nested-root` by the parent and its files are absent from the parent's catalog; a `.env` file is cataloged as skipped and none of its words are found by `katana coder search` | 3 |
| 3. The crawler | `katana coder crawl` with `--prio`, `--changed`, `--sweep`; `CRAWL.LOCK`; frontier, level queues, bands; `STS_DIRS`, `STS_CRAWL`; level bits in `StoreDoc.flags`; batches; N0 by append, N1 by supersession, N2 by augmentation; same-level compaction with the 24/8 thresholds; reconstruction of queues from the store; state file; socket (`PING`, `PRIO`, `TOUCH`, `SWEEP`, `SYNC`, `STATUS`, `STOP`); blocking writer lock; exit when all levels are covered and the settle time passed; `index_ensure` switches from "spawn index" to "spawn or reach the crawler"; `autoindex_dir` and init call it; limits of D.8; generated/minified detection; every ignored directory and file recorded with reason class and rule (`STS_DIRS` fields, `STS_RULES`, flag bits), the single built-in heavy list of D.10 replacing the three lists of today, the per-root `overrides` file read by the walk, the counting pass, scoped includes. **Also creates, as compiling stubs with their includes in `katana.c` and their call sites already placed, every new file of the later phases** (see 5.3) | On a large folder `katana coder status` shows N0 complete within seconds while text is still rising; the crawler process is gone a few seconds after the last level completes; `kill -9` of the crawler followed by a new visit continues from the reported counts instead of zero; two crawlers started together leave one process; a small folder ends with the same sections as `katana coder index` produces today (`katana coder verify` ok) | 5 |
| 4. Watching and freshness | Session watcher (inotify descriptor read without blocking at the three moments of C.1, watch budget, overflow → sweep); polling period and the `.git/HEAD` hint; coalescing and hand-over to the crawler (`TOUCH` or spawn `--changed`); branch re-entry (D.7); `SDF_RACY`; print-time check with single-file re-parse and the "changed since indexed" count; network/FUSE detection by `statfs` | With a session open, saving a file in another terminal and then issuing any tool call changes the generation shown by `katana coder status`; `KATANA_INDEX_WATCH=poll` gives the same result after the polling period; deleting a file and querying it immediately yields no line for it | 3 |
| 5. Threads | Worker pool in the crawler; indexing state passed as a context; audit and removal of static state in `text.c`, `parse.c`, `parse2.c`, `search.c`, `symindex.c`; `KATANA_INDEX_THREADS` (1 = the single-thread path, kept as the default until the owner allows a comparison) | With 4 threads the store of a folder passes `verify` and `katana coder status` shows the same file, term, symbol and reference counts as with 1 thread | 2.5 |
| 6. What the model and the user see | Status lines of J.2 and the `NOT FOUND ... so far` form; "so far" on completeness outputs; state map (files per level per directory) in `statemap.c`; the new Symbols action `tree` (J.5: schema in `symbols.c`, output from `STS_DIRS` and the state map); the ignored-folder sentence on `NOT FOUND`; a `path` inside an ignored folder becomes a scoped include; console lines of H.3; desktop status item; `katana coder status` full output and `--json` | On a folder still crawling, Find's first line is the "still being built" line and disappears when the crawl ends; on a complete small folder the Symbols outputs are byte-identical to the previous build's; `Symbols` with `action=tree` prints the folders with their index depth and the ignored ones with reason and count; Find with `path=node_modules/<package>` answers from that package and a later Find without `path` does not return files from it | 2 |
| 7. Unified Find, as the default | File list from the view, candidates from `cs_search_core`, chunk ranking at query time over the candidate files, symbols from N2, `STS_GRAPH` built by the crawler through the `graph_on_quiet` hook (sym, path, rare, optional co), activation and neighbor notes reading it, request state keyed by path hash; `KATANA_FIND_INDEX` defaults to `store`, `legacy` keeps today's path working | Find answers in a folder that has no `find-*.idx` and creates none; with `KATANA_FIND_INDEX=legacy` the old path still answers | 4 |
| 8. Housekeeping and failure paths | `katana coder ignored`, `include`, `exclude` and the ignored block of `status` (D.10); `list`, `forget`, `gc` (home index only) and the daily automatic `gc`; retiring absorbed home roots; version mismatch → rebuild, newer store → leave and report; corruption → quarantine and rebuild; scrub at the end of a run; disk-full pause; inline fallback caps; read-only home path | Truncating a segment file by hand leads, on the next query, to the "damaged" line and a complete index shortly after; `katana coder forget` leaves no file for a home root; `gc --dry-run` lists what it would remove and never names an initialized folder's store; `katana coder ignored` in a project with `node_modules` lists it with rule, file count and bytes, and after `katana coder include node_modules/x` that folder is searchable | 1.5 |
| 9. Documents and training note | Update `docs/tools.md`, `docs/code-intelligence.md` (2.4, 2.6, 8.6), `docs/extending.md` (init), `docs/handoff.md`, `docs/plan.md`, README (init, environment table); record in `docs/curriculum.md` the requirements for the v8 curriculum: a family for the `tree` action (J.5), the status lines (J.2), the two `NOT FOUND` forms (J.3) and the scoped include through `path`; `docs/tools.md` gets `tree` and the ignore rules; N4 summaries left as a described, switched-off option | Documents describe the behavior that exists | 1 |

**Phase 1 status (2026-11-23): built and compiled; nothing measured.** `src/coder/indexctl.c` holds `index_ensure`, `index_due`/`index_validated`
(a second consumer, the Find cache, uses the same state), `index_touch` (Edit, Write, MultiEdit and NotebookEdit through `ct_f_save`; Bash sets a
"tree may have changed" flag unless the command is a simple read), the per-process cache of `CsView`/`SyView` (4 slots, valid while the MANIFEST
does not change) and the counters shown by `KATANA_FIND_TIME`. Implementer decisions: (1) `index_ensure` updates in process instead of spawning
`katana coder index`, because a child that overruns the budget leaves the query with no index at all (the segment is published only at the end);
the child pays off with the crawler of phase 3. (2) Until the session watcher exists (phase 4), a root is also revalidated when more than
`KATANA_INDEX_SWEEP_S` seconds (default 10; 0 = every call, the old behavior) passed since its last walk; files printed in an answer are still re-read
from disk. (3) The Find cache (`find-*.idx`) skips its own list-and-stat walk under the same rule. (4) The knowledge base is loaded once per process
(`cs_kb_get`). (5) Store files are created 0600 and folders 0700; stores of older versions are fixed at the next write (`st_harden`); the Find cache
too. (6) `autoindex_dir` spawns `katana coder index` once per folder per process. Not done in this phase: `agent/devtools.c` and `agent/tools.c`
(older tool set) do not call `index_touch`.

**Phase 2 status (2026-11-23): the part about roots, place and init is built and compiled; nothing measured.** New `src/coder/roots.c` (`index_resolve`,
`index_init_root`, `index_remove_root`, `index_retire_below`, `index_marker_root`, `rt_skip_subdir`); `store_locate` now asks it, so every reader and writer
follows one rule. `katana init` and `/init` are one path (`ex_init_run` in `src/agent/extend.c`): `KATANA.md` is written only when missing (or `--force`),
the marker `.katana/coder/ROOT` and the index are always created, `katana init --remove` undoes the marker and the local store. `proj_is_launch` and
`g_launch_dir` are gone; `fd_cache_path` and the prefix cache write inside a folder only under an initialized root. Queries resolve `path` through the
resolver: a prefix of an absorbing root restricts the view (`store_live_rebase`, paths stay relative to the visited folder), views are cached per store and
prefix, and the located root replaces the manifest's recorded root (a moved folder reads its own files). `katana coder status` prints the resolved root, where the
index comes from (local, ancestor, home), whether there is an init, and the file count under the prefix. Implementer decisions (also at the head of
`roots.c`): the home registry holds only home roots; absorption retires descendant home stores right after the absorbing store is written, not "at N1";
init moves a home store with `rename` (otherwise deletes it and rebuilds); an old local store without marker is neither read nor deleted, `status` warns and `init`
adopts it; the marker is git-ignored; init in the home folder or `/` needs `--force`; the walk (and the legacy Find list) does not descend into a subfolder with its own
marker or into a linked git worktree (`.git` file pointing into `worktrees/`), while submodules and nested clones are still descended. Not done in this phase:
the secret file table, `.git/info/exclude`, link and mount detection, moved-folder detection of home roots by (device, inode), `katana coder list/forget/gc`
(phase 8), and Grep, which has no index, still lists nested folders.

### 5.2 The waves

| Wave | Agents (in parallel) | Files each agent may edit | Days of the wave |
|---|---|---|---|
| 1 | **1a** = phase 1, then **1b** = phase 2. Serial, one agent after the other: both change `search.c` and `symquery.c` | 1a: `src/coder/search.c`, `symquery.c`, `symtool.c`, `kb.c`, new `src/coder/indexctl.c`, `src/katana.c`, `src/tools/edit.c`, `write.c`, `multiedit.c`, `notebookedit.c`, `bash.c`. 1b: new `src/coder/roots.c`, `src/coder/store.c`, `storedocs.c`, `search.c` (`cs_read_doc`, `cs_junk` only), `symquery.c` (root of file paths only), `src/tools/find.c` (`ct_findtool` root and prefix, `fd_cache_path`), `symbols.c`, `common.c`, `src/agent/extend.c` (`init_cli`, `ext_slash`), `loop.c`, `prefix_cache.c`, `src/katana.c` | 5 |
| 2 | **2a** = phase 3, alone. Nobody else edits `src/` during this wave | new `src/coder/crawl.c`; stubs: new `src/coder/watch.c`, `fresh.c`, `statemap.c`, `graph.c`, `maint.c`; `src/coder/store.c`, `storedocs.c`, `search.c` and `symindex.c` (hooks, mergers for the new sections), `indexctl.c`, `coder.c`, `src/agent/loop.c`, `extend.c` (init starts the crawler), `src/katana.c` | 5 |
| 3 | **3a** = phase 4, **3b** = phase 7, **3c** = phase 8 | 3a: `src/coder/watch.c`, `fresh.c`, `indexctl.c`, `crawl.c` (change queue and branch re-entry functions), `symtool.c`. 3b: `src/tools/find.c`, `findgraph.c`, `src/coder/graph.c`. 3c: `src/coder/maint.c`, `roots.c`, `coder.c`, `storedocs.c` (subcommands only) | 4 |
| 4 | **4a** = phase 5, **4b** = phase 6 | 4a: `src/coder/crawl.c`, `search.c`, `symindex.c`, `text.c`, `parse.c`, `parse2.c`. 4b: `src/coder/statemap.c`, `symquery.c` (`sy_limits`, `sy_emit`, the new `coder_tree_main`), `symtool.c`, `src/tools/find.c` (output only), `symbols.c`, `src/agent/loop.c`, `storedocs.c` (`coder_cmd_status`), `desktop-qt/` | 2.5 |
| 5 | **5a** = phase 9 | `docs/`, `README.md`, `desktop-qt/README.md` | 1 |

About 23.5 agent-days of work in about 17.5 days of waves.

### 5.3 What makes the parallel waves safe

- **Stubs placed in wave 2.** Phase 3 creates `watch.c`, `fresh.c`, `statemap.c`, `graph.c` and `maint.c` with
  empty functions, includes them in `src/katana.c` in their final order, and puts the calls where they belong:
  `watch_poll()` at the three moments of C.1 (in `indexctl.c` and `loop.c`), `fresh_check(root, path, ...)` in
  the three places that re-read a file for evidence (`cs_read_doc` in `search.c`, `sy_file_load` in
  `symquery.c`, `fd_text` in `find.c`), `graph_on_quiet(reader, writer)` and `maint_on_crawl_start()` /
  `maint_on_crawl_end()` in `crawl.c`, `statemap_get(view, prefix)` after the view is built. Later agents fill
  the bodies in their own file and do not need to touch the call sites or `katana.c`.
- **Wave 3.** 3a is the only one in `crawl.c`, `indexctl.c` and `symtool.c`; 3b the only one in the Find files
  (its work in the crawler goes through the `graph_on_quiet` hook in its own `graph.c`); 3c the only one in
  `maint.c`, `roots.c`, `coder.c` and the subcommand functions at the end of `storedocs.c`.
- **Wave 4.** 4a owns the indexing side (`crawl.c`, `search.c`, `symindex.c`, parsers); 4b owns the output side
  and must not edit `search.c`: the state map it needs lives in `statemap.c`. 4b edits `find.c` after 3b is
  merged (wave order guarantees it) and touches only the functions that print.
- **The links work in progress** (text keys, another agent) must be merged before wave 2 starts or wait for wave
  4: it changes `symindex.c` and the hook chain that phase 3 reworks. Its sections need a per-document hook with
  a context argument and reserved numbers in the `STS_SYM_BASE` range; `STS_GRAPH` and `STS_SUMMARY` take the
  next free numbers there, and `STS_DIRS`/`STS_CRAWL` take 3 and 4 in the catalog range.
- **Any other work on `find.c`, `findgraph.c`, `search.c`, `symquery.c`, `store.c` or `storedocs.c`** has to be
  scheduled between waves, not during one.

## 6. Risks and honest uncertainties

1. **Nothing in this design was measured.** The targets in I.1 extrapolate single runs made by other agents on
   one machine. The split of cold-build time between reading, tokenizing and parsing is inferred from two data
   points (0.29 s of about 1.1 s; 35.7 s at 116 thousand files with no total given).
2. **ntfs3 behavior is assumed, not verified**: `flock`, inotify events, `mmap`, reading an unlinked mapped
   segment, and `fsync` cost on `/mnt/dados`. The store already falls back from `mmap` to reading. If `flock` or
   events misbehave there, the escape is `KATANA_INDEX_WATCH=poll` and not initializing folders on that disk (the
   store is then in the home); phase 3 should check these four things by hand on that mount before relying on
   them. Note that the katana repository itself is on that disk and will be initialized there.
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
   campaign there is no evidence either way. The owner decided to make it the default anyway; if answers get
   worse, `KATANA_FIND_INDEX=legacy` restores the old path.
7. **The model has never seen the new status lines** or the output of `tree`. The lines are short and in the
   bracket form it knows, but a local model may ignore "results are partial" and treat a partial `NOT FOUND` as
   final. The `tree` action is in the same position, more so: it is a new call the model must choose to make. The wording in J.3 is the mitigation; the real fix is a curriculum family.
8. **Secret detection is heuristic.** A password in a file named `config.yaml`, or a short token, enters the
   lexicon. The store permissions and keeping the store in the home are the real protection; on ntfs3 the permissions do
   not exist.
9. **Without init the index is as narrow as the visit.** A user who never runs init and always names
   subfolders gets many small home roots with no links between them, and may read that as "the index does not
   connect files". The absorption rule (visiting the parent merges them) and a hint in `katana coder status`
   ("3 folders under greymod are indexed separately; run `katana init` in greymod to join them") are the
   mitigation. The reverse also exists: an init high in the tree (for example in `~/source`) makes one very
   large root.
10. **inotify limits.** 65536 watches for the user, shared with editors and other tools. Because the crawler
    exits, every session holds its own watches: a quarter of the limit each, sweeps for the rest. Four sessions
    on large trees exhaust it.
11. **Starting a process per change.** With no resident crawler, an edit outside a burst costs one process
    start of the katana binary plus the settle time before it exits. The cost of starting the binary was not
    measured; if it is high, the settle time is the knob (a longer settle means fewer starts and a process that
    lingers longer, which is the trade the owner decided against by default).
12. **Changes made while no session is open** are found only by the sweep at the next visit. For a root of 100
    thousand files that sweep is the walk-and-stat cost again (about 0.3 s by the observed rate), once per
    session and root, in the background.
13. **Existing local stores without a marker.** Every folder katana was launched in before this change has a
    `.katana/coder/store` that no longer counts. Those folders are re-indexed into the home index on the next
    visit; the old stores are listed by `gc` and adopted if the user runs init there.
14. **The counting pass walks what the crawl avoids.** Counting `node_modules` is a `readdir`/`lstat` walk of
    tens of thousands of entries, capped and at the lowest priority, but it delays the crawler's exit. If it
    proves too slow it can be made lazy (only when `tree` or `katana coder ignored` asks).
15. **`vendor` is indexed by default now.** Correct for Go and PHP projects that commit their dependencies, noisy
    for projects that use the name for downloaded code. It deepens last (band 4) and one line in
    `.katanaignore` removes it, but the default may be wrong for a given project.
16. **What I remember versus what I know about the examples** is marked in 3.0. The design does not depend on
    any detail marked "recollection".
17. **The desktop integration was not read.** H.3 assumes the desktop can show a status item fed by the agent
    process.

## 7. Decisions

### 7.1 Taken by the owner on 2026-11-23

| Question | Decision | Where |
|---|---|---|
| What defines an index root | `katana init`. Not `.git`, not `KATANA.md`. Nested inits are independent indexes; a folder without init is absorbed by the nearest initialized ancestor, else by the home index; no `.katana/` in a folder without init | A, F.1 |
| What the home index is | Everything visited that has no init, indexed only as visited; the desktop relies on it | A.2, F.1 (one store per visited root, justified there) |
| Phases | All nine, in waves | 5 |
| Life of the crawler | On demand; exits when every level of every requested folder is covered; no idle wait, no daemon | B, C.1 |
| Unified Find | Default as soon as phase 7 is built; `legacy` is the escape | E |
| Directory summary for the model | A new Symbols action `tree` (not `outline` on a folder); examples required in the v8 curriculum | J.5, phase 6, phase 9 |
| Ignored content | Still ignored, but transparent and controllable: every ignored folder and file recorded with its reason and size, `katana coder ignored`, `include`/`exclude`, ignored folders shown by `tree`, searches say when ignored folders were not looked at, the model can reach into one with `path` | D.10, J.3 |

### 7.2 Still open

1. **Git worktrees and other whole copies of the code inside a root.** With `.git` no longer a boundary, a
   worktree of the project placed inside the project (the agents' worktrees are the known case) is indexed as
   ordinary files, doubling or multiplying every symbol. `.claude` stays in the built-in heavy list, which covers
   the case that happened; any other location is not covered. Recommendation: do not descend into a subdirectory
   whose `.git` is a FILE pointing into a `worktrees/` directory (that is exactly a linked worktree; record it
   as `separate-checkout`), and keep descending into submodules and nested clones, which are distinct code. This
   uses `.git` only to recognize a duplicate, not to define a root. Alternative: nothing automatic; the user
   lists such folders in `.katanaignore`.
2. **Settle time before the crawler exits** (2 s by default). It is what turns a burst of 300 changed files into
   one crawler run instead of many, and it means the process is visible for about 2 s after its last write.
   Recommendation: keep 2 s. Alternative: 0 (exit at once; every isolated change starts and ends a process, and
   the graph is rewritten on each).
3. **`vendor` out of the built-in heavy list** (D.10). The owner named `vendor/` as a folder the model may need to
   read, so this design indexes it by default, last in priority. Recommendation: keep it indexed. Alternative:
   keep it in the built-in list and rely on `katana coder include vendor` or on the scoped include through
   `path`; then project searches do not see vendored code unless asked.
