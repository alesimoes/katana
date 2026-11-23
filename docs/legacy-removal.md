# Legacy removal: inventory and plan (2026-11-23)

Owner's decisions (2026-11-23), in order: "remove the legacy code, the project is new, we must have nothing legacy";
"we do not need to keep compatibility"; "we do not need the old functions; remove the legacy and train only the new one;
we never use anything of katana: no memory, no log, nothing". This document is the inventory and the removal plan, for a
wave of agents to execute later. **No code was changed, nothing was run except compiler warnings and a throw-away copy of the
sources in the scratchpad** (no tests, no measurements, owner's rule for this phase). Every number below was counted from the
tree at commit `5a1e6b6` plus the merge of `main`; where something is an opinion it is marked **(opinion)**.

Rules this plan follows (from the owner's last two messages):

1. The old tool set in `src/agent/` goes **in full**, including the single-function tools. No migration to the table set. A
   function with no equivalent is listed in section 2.3 so the owner can see what is lost; the list does not block anything.
2. No reader of old data survives: no migration reader, no warning, no alias. Old data is simply not read. An index that does
   not validate is rebuilt, nothing more.
3. The only exception is memory (`src/agent/memory.c`: `remember`, `recall`, `katana memory`), kept by the owner's decision.
   Its old line format (`DATE<TAB>fact`, no ids) and `katana memory export` (old format) go. Section 2.5 reports a fact about
   why memory was said to be needed (the UIOE), which the owner should read.
4. The curriculum is regenerated from zero with the new tools: no scenario has to keep teaching an old name (section 2.7).
5. Environment keys that only restore old behaviour go; keys of real operation (limits, switching a feature off for diagnosis,
   folders) stay (section 3.3).

## Status after the final stage (2026-11-23)

Done in the final stage, on top of fronts A (old tool set) and B (legacy Find index, store format 2, research keys): dead code reported by
`-Wall` removed everywhere except `scenario*`, `curriculum.c` and `importer.c` (still listed in section 3.7: `cu_rv_free`, `xc_tab_con`,
`g_x7_note`, `g_x7_scn`, `im_bs`, `code_good`); `glob.c` renamed `filewalk.c`; `-Wmisleading-indentation`, `-Wparentheses` and
`-Wint-in-bool-context` warnings fixed without changing behaviour; the escape keys `REPEAT_NOTE`, `READ_CURSOR`, `READ_DELTA`, `PLACE_NOTE`,
`READ_OUTLINE` (the off switch), `G4_PLAIN_RESULT`, `G4_ENVLAST`, `MOE_OLD`, `SESS_RESUMO` and `RETOMAR*` removed (the `/model` reload keeps its
session through the internal key `KATANA_RESUME_OTHER`); the memory file has one format (`#fact ID DATE fact`) and `katana memory export` is gone;
`nkeyold`, the `recall` hint in `Find` and the `kb:` message outside `katana coder kb check` are gone; Portuguese command aliases (`/novo`,
`/sistema`...), the `modelos` alias and the `shell_id` argument alias are gone. What stays on the `grep -i 'legado|legacy|compat|antigo'`
list is the MCP old SSE transport (`mcp.c`, `mcp_extra.c`, `mcpx_legacy_*`), `PRE_LEGACY`/`encode_legacy` (tokenizer family), the
`Mozilla/5.0 (compatible; ...)` user agent, the word list in `text.c`, the K&R remark in `parse.c`, the `.kspec` layout notes in `train.c` and
`train_methods.c` (files that older readers stop reading, by design), `train_vk.c` (the fp32 kernel path behind `KATANA_TRAIN_COOPMAT_MOE=0`),
`convert.c` (the external HF expert layout) and the many "mais antigo" meaning oldest.

## 0. Summary in numbers

| what | number | where |
|---|---|---|
| old tool names still accepted by the dispatcher | 24 | `src/agent/dispatch.c:67-202` (one `strcmp` chain) |
| lines of `src/agent/` that die once those branches go (measured, see 2.4) | about 1450 non-blank lines, 46 functions, in 5 rounds of "delete what the compiler calls unused" | `devtools.c` 569 to 117, `logs.c` 196 to 5, `jobs.c` 108 to 41, `todo.c` 74 to 24, `code.c` 172 to 63, `locate.c` 304 to 194, `terminal.c` 452 to 260, `tools.c` 553 to 411, `sudo.c` 139 to 93, `dispatch.c` 296 to 131 |
| functions already dead today (`-Wunused-function`, with `-Wall`, AVX2 flags) | 32 (31 reported plus 1 found on the second round) | section 3.7 |
| legacy Find index: lines | `fg_build` 210 + `fd_update` 62 + `fd_load` 50 + `fd_save` 26 + `fd_cache_path` 28 + `fg_save/load` 25 + `fg_index_for_loop` 13 + the non-store half of `fg_neighbor_note` about 40 | section 1.2 |
| `KATANA_*` names read by the code | 291 (`getenv("KATANA_...")` distinct names) | section 3.3 lists the ones that restore old behaviour |
| matches of `grep -rn -i 'legado\|legacy\|compat\|antigo' src/` | 132 lines in 49 files; about 30 of them are real legacy, the rest are not (section 3.6) | |
| files in the repository root that are not source | 2 tracked junk items (`compare.sh`, `attic/ia.c`); 6 untracked build leftovers (`ia.exe`, `ia.obj`, `modelo.bin`, `katana.exe`, `katana.obj`, `katana.cfg`) plus an empty `desktop/` | section 3.1 |

Method, so the wave can repeat it: (a) `gcc -O1 -c -Wall -Wunused-function -mavx2 -mfma -mf16c src/katana.c` for dead code (the
normal `build.sh` passes `-Wno-unused`, which hides all of it); (b) a scratch copy of `src/` where the old branches of
`agent_tool_run` were deleted, then a script that deletes every function the compiler calls unused and recompiles until no new
warning appears (this is how the cascade in 2.4 was measured); (c) `grep -w` for each name over `src`, `desktop-qt`, `tools`,
`plugins`, `docs`; (d) reading `docs/tools-review.md` entirely (655 lines) and checking its claims against the code.

## 1. The legacy Find index

### 1.1 State today

The Find is unified over the coder's store by default (`fd_store_mode()`, `src/tools/find.c:82`:
`KATANA_FIND_INDEX=legacy` is the only way out). In the default path `fs_begin` (`src/coder/findstore.c`) gives the file list and
candidates and the "mini index" of chunks is built per query. The legacy path is still compiled and still runs in three
situations:

1. `KATANA_FIND_INDEX=legacy` (`find.c:82`).
2. Fallback: `fs_begin` returned NULL because the coder index could not be created (`find.c:1733-1737`, then `1738-1742`).
3. **`Rename`**, which does not use the store: `src/tools/multiedit.c:339` calls `fd_update(&g_fd, root, ...)` and reads
   `X->f`, `X->sf`, `X->sn`, `X->poff[2]`, `X->cf`, `X->pch` (lines 340-374). `fd_update` writes `<folder>/.katana/cache/find-<hash>.idx`
   (`fd_cache_path` `find.c:639`, `fd_save` `find.c:667`). `Replace` and `Patch` do **not** call it (the only `fd_update` call in
   `multiedit.c` is line 339, inside the Rename case); the coordinator's brief listed all three, the grep says one.
4. The neighbour notes of the loop: `fg_neighbor_note` (`findgraph.c:831-898`) uses the store when `fd_store_mode()` and otherwise
   `fg_index_for_loop` (`findgraph.c:709-721`), which calls `fd_update` and loads the cache of the working folder.

Evidence that the files exist on disk: `/mnt/dados/source/katana/.katana/cache/` holds only `pfx-*.kv` today (no `find-*.idx` in
the repository folder), so this machine no longer has any; the code path still can create them.

### 1.2 What goes and what stays (function, lines, dependents)

| item | lines | dependents | action |
|---|---|---|---|
| `fd_cache_path` | `find.c:639-666` | `fd_update` (788, 790) | remove |
| `fd_save` | `find.c:667-692` | `fg_ensure` (`findgraph.c:392`), `fd_update` | remove |
| `fd_load` | `find.c:700-749` | `fd_update` | remove |
| `fd_update` | `find.c:783-844` | `find.c:1740`, `findgraph.c:719`, `multiedit.c:339` | remove; Rename gets a new source (1.3) |
| `fd_in_nested` | `find.c:766-775` | only `fd_update` (805) | remove |
| `fd_free_files` | `find.c:693-699` | `findstore.c:36`, `fd_load`, `fd_update` | keep only if `findstore.c:36` still needs it ("the index came from the old mode"); that line is itself a legacy guard: remove it and the function |
| `fd_skip_rel` | `find.c:752-764` | `findstore.c:69, 216, 295, 399` | **keep** (used by the unified Find; its comment in `coder/ignore.c:3` says the list was unified) |
| `fd_store_mode` | `find.c:82` | `find.c:1732, 1736`, `findgraph.c:833` | remove; every branch takes the store side |
| `fg_build` | `findgraph.c:176-385` | `fg_ensure` (391) | remove |
| `fg_ensure` non-store half | `findgraph.c:387-394` | `find.c:1151, 1755` | reduce to `return fs_graph(X->fs) != NULL` |
| `fg_save`, `fg_load` | `findgraph.c:397-421`, prototypes `find.c:85-86` | `fd_save`, `fd_load` | remove |
| `fg_index_for_loop` | `findgraph.c:709-721` | `findgraph.c:836` | remove |
| non-store body of `fg_neighbor_note` | `findgraph.c:834-898` | none once `fd_store_mode` is gone | remove, keep the call to `fgs_neighbor_note` |
| non-store branch of `ct_findtool` | `find.c:1737-1742` | | remove: if the coder index cannot be created, the answer is an error, not a second index |
| comments naming the old index | `find.c:8` ("find-<hash>.idx"), `find.c:67`, `find.c:1729`, `findgraph.c:15`, `findstore.c:2, 362`, `coder/indexctl.c:13` ("a second consumer, the Find's own index, until phase 7 removes it") | | rewrite |
| `index_due`/`index_validated` with `IDX_FIND` | `coder/indexctl.c:68, 78, 194-195` | `find.c:1740` only | remove the second consumer (the `cons` argument) once `find.c:1740` goes; `IDX_CODER` stays |

After this, `FdIndex`, `fd_build_file`, `fd_invert`, `fd_rank` and the other `fd_*` functions of `find.c` remain: the unified Find still
uses them for the mini-index (`fs_prepare`, `find.c:1779`). So this is **not** "delete find.c"; it is about 400 lines out of
1836 + 902.

### 1.3 What Rename needs

Rename needs, per file: its text, its symbol definitions (collision check, `multiedit.c:345-347, 363-365`) and the list of files
that contain the identifier (`poff[2]`, `pch`, lines 356-362). The unified Find already produces exactly this for `mode=refs`:
`fs_prepare_refs(FS, X, name)` followed by `fd_refs` (`find.c:1767`). **(opinion)** Rename should call
`fs_begin(root, pre, &g_fd, ...)` + `fs_prepare_refs(FS, &g_fd, sym)` + `fs_defs(...)` (declared `find.c:77`) and then keep its own
text replacement code unchanged. That is the only non-mechanical change in the whole removal; it needs a tooltest
(no tooltest mentions `Rename` today: `grep -l Rename tools/tooltest/* tools/traintest/*` is empty; add one).

### 1.4 Other cache folders

| path | what | action |
|---|---|---|
| `<folder>/.katana/cache/find-*.idx` | legacy Find cache | the code stops writing it; nothing deletes existing ones (owner instruction in section 4.7) |
| `~/.katana/cache` (804 MB on this machine) | `web/` (webfetch cache), `pfx-*.kv` (prefix cache), vision cache | **not legacy**: `KATANA_CACHE_DIR` is a real operation key (prefix cache `agent/prefix_cache.c:99-104`, web `tools/webfetch.c:71-91`, vision `vision/image.c:51`, tests). Keep |
| `/mnt/dados/source/katana/.katana/` (2.4 MB, repository root: `cache/pfx-*.kv` and `coder/`) | made by running katana inside the repository; `.gitignore` has no `.katana` entry, the coder store's own `.katana/coder/.gitignore` keeps it out | not legacy, junk: delete by hand (4.7) |

## 2. The old tool set

### 2.1 Where the old names live (verified with `grep -w`)

The 24 names: `run_command`, `read_file`, `write_file`, `edit_file`, `list_dir`, `find_file`, `search_files`, `glob`, `web_search`,
`web_fetch`, `read_document`, `ask_user`, `todo`, `delegate`, `job`, `terminal`, `terminal_screen`, `git`, `install_package`,
`download_file`, `read_log`, `remember`, `recall`, `howto` (verified against the `strcmp` chain, `dispatch.c` lines 38-49, 67-201).

| place | what | lines |
|---|---|---|
| `src/agent/dispatch.c` | `old_guard()` (the old names go through `brief_guard_subject`) and the 24-branch `agent_tool_run` chain; `notes_neutralize` lists `run_command`, `terminal`, `web_fetch`, `web_search` | 30-55, 66-202, 215-216 |
| `src/agent/prompt.c` | `ct_set2()` and `KATANA_TOOLS`; the table branch `if (ct_set2())` (146-194) and the old branch with `TOOLDEF` macros (195-238); the "Also:" line that names 8 old tools (170-171); Internet line (130-131) | 130-131, 136-139, 144-146, 170-171, 195-238 |
| `src/agent/prompt.c` `g_code` | `katana code` mode (3.4) | 144, 172, 174, 211-212, 234 |
| `src/agent/loop.c` | `run_command` in the program-key function (383); `loop_plan_readonly` list with `recall`, `howto`, `read_log`, `read_file`, `list_dir`, `find_file`, `search_files`, `glob`, `web_search`, `web_fetch`, `read_document`, `ask_user` (416-417); `loop_plan_note` `todo` (426, 439, 441, 516); `rr_is_plan` `todo` (501); `rl_writes` `write_file`, `edit_file` (541); banner text "Internet (web_search, web_fetch) ... remember and recall" (575); "write_file ... append=true" hints (859, 914); `ct_set2()` ternaries (439, 441, 516, 858, 913, 992) | listed |
| `src/agent/planmode.c` | `plan_allows()`: ok-list of old names (50-51), `run_command` (55), `git` (56) | 50-56 |
| `src/agent/webnav.c`, `howto.c` | `ct_set2()` ternaries (`webnav.c:154, 160, 192`; `howto.c:3-4, 112`) | |
| `src/agent/diagnose.c` | hints that tell the model to use `install_package`, `terminal action=open`, `list_dir`, `glob` (lines 29, 33, 38, 47, 70) | rewrite to table names (Bash) or drop |
| `src/agent/undo.c` | comment only (`write_file`, `edit_file`); the undo logic itself is called from table tools too (`undo_note`: `tools/edit.c`, `write.c`, `multiedit.c`) | comment |
| `src/agent/teacher.c` | `TCH_EXTRA[][2]`: declarations of `todo`, `git`, `terminal`, `glob`, `recall`, `download_file`, `install_package`, `read_log`, `job`, `howto` for the teacher (92-101) | remove the table |
| `src/agent/callgrammar.c` | `install_package`, `download_file`, `read_log` named (7 matches); line 651 comment about `KATANA_TOOLS=1` | rewrite |
| `src/agent/mcp_serve.c` | exposes only table tools (`Read`, `Glob`, `Grep`, `Write`, `Edit`, `NotebookEdit`, `Bash`); no old name (the `"path"` key at line 115 is a generic argument key) | nothing to remove |
| `src/agent/mcp_extra.c` | `ask_user` in a comment (221: the CHOICE marker) | the marker stays (2.6), rename the comment |
| `src/app/launch.c` | permission prompt list `Q[]` (622): `ask_user`, `delegate`, `terminal`, `run_command`; plan card test `todo` (702) | 622, 702 |
| `src/app/importer.c` | `katana import` converts old names to new ones: map and per-tool key translation (217-330), message at 937 | 2.7 |
| `src/engine/forward.c`, `src/app/main.c` | self-test strings use `web_search` and `list_dir` as arbitrary tool names (forward.c 1182-1197; main.c 928) | they are fixtures for the grammar self-test: rename to table names |
| `src/katana.c` | include-line comments naming old tools (39, 45, 50, 53-56, 65, 116) | |

### 2.2 The table set already covers most of it

The table (`src/tools/table.c`, `g_ct[]`, 46 rows) is tried first in `agent_tool_run` (`dispatch.c:60-65`); the old names are reached
only when `ct_readonly(name) < 0`. **A fact that changes the shape of the removal:** several "old" implementation functions are shared engines of
table tools, so the files are not removed whole, only their old-name entry points:

| old function | still called by a table tool | evidence (`grep` over `src`) |
|---|---|---|
| `tool_ask_user` (`agent/ask.c`) | `AskUserQuestion` | `tools/askuserquestion.c:50` |
| `tool_delegate` (`agent/delegate.c`) | `Agent` | `tools/agent.c:32` |
| `tool_howto` (`agent/howto.c`) | `Skill` (recipes as skills) | `tools/skill.c:373` |
| `tool_run_command` (`agent/terminal.c:3`) | `PowerShell` | `tools/powershell.c:51` |
| `tool_web_fetch`, `tool_web_search*` (`agent/web.c`, `webnav.c`) | `WebFetch`, `WebSearch` | `tools/webfetch.c:366`, `tools/websearch.c:205-208` |
| `run_cmd_ex`, `cmdlog_add`, `sudo_prepare` | `Bash` | `tools/bash.c` |
| `agent_act`, `agent_write_path`, `agent_read_access`, `undo_note`, `code_check`, `json_str/json_has` | `Read`, `Write`, `Edit`, `Rename` and others | many |
| `todo_ui`, `todo_count` | `TodoWrite`, `Task*`, `ReportFindings` | `tools/tasks.c`, `tools/reportfindings.c` |

So the work is: delete the entry points and let the compiler list what becomes dead (2.4), instead of deleting files by name.

### 2.3 Tool by tool: equivalent in the table, or lost

(a) = has an equivalent, removed with nothing to do. (b) = no equivalent: **removed anyway** (owner's decision); the right column says what is lost,
for the owner to read. "Argument map" is for the wave: nothing keeps the old keys.

| old tool | implementation (file, function) | equivalent | what is lost, if anything |
|---|---|---|---|
| `run_command` | `terminal.c:3` `tool_run_command`; `jobs.c` for `background` | (a) `Bash` (`command`, `timeout`, `run_in_background`) | nothing; `tool_run_command` stays because `PowerShell` uses it |
| `read_file` | `code.c:62` `tool_read_file`, `rd_*` (27-62) | (a) `Read` (`file_path`, `offset`, `limit`) | HTML as text for a local `.html` (docs/tools-review.md 4.2: `Read` reads it raw); `rd_get`, `rd_notes`, `rd_all_notes` die with it (they become unused in round 1) |
| `write_file` | `dispatch.c:94-118` (inline) | (a) `Write` | `append=true` (long file in parts) and `sudo=true` (write as root). `Write` says "use Bash with sudo" |
| `edit_file` | `devtools.c:154` `tool_edit_file` | (a) `Edit` (`file_path`, `old_string`, `new_string`, `replace_all`, `edits`) | `sudo=true` |
| `list_dir` | `locate.c:18` `tool_list_dir` | (a) `Bash ls` | the sorted listing with sizes and dates in one call; 148 calls in the old log, 100 repeats (tools-review 4.1.6) |
| `find_file` | `locate.c:123` `tool_find_file`, `ff_*` | (b) none (`Find` refuses the home folder; `Glob` needs a path) | **machine-wide location by part of a name** (tools-review 4.1.5: the only fix for the 2026-11-23 wrong-path failure). `Bash find` or `locate` is the manual substitute; `place_note` (`locate.c:251`, used by `loop.c:707`, `main.c:1011`, UIOE) is a separate engine feature and stays |
| `search_files` | `tools.c:509` `tool_search_files`, `sf_*` | (a) `Grep` | nothing |
| `glob` | `devtools.c:282` `tool_glob`, `glob_*` | (a) `Glob` (`pattern`, `path`; the old key was `root`) | nothing |
| `web_search`, `web_fetch` | `web.c`, `webnav.c` | (a) `WebSearch`, `WebFetch` | nothing; engines stay |
| `read_document` | `devtools.c:520` | (a) `Read` handles PDFs (`pages`) | local HTML to text only (same as `read_file`) |
| `ask_user` | `ask.c:7` | (a) `AskUserQuestion` | nothing; engine stays, and the desktop CHOICE marker stays (2.6) |
| `todo` | `todo.c:27` `tool_todo` | (a) `TodoWrite` (`todos`, `done`) | nothing; `todo_ui/count/clear` stay (used by `TodoWrite`) |
| `delegate` | `delegate.c:24` | (a) `Agent` | nothing; engine stays |
| `job` | `jobs.c:82` `tool_job`, `tool_job_start` | (a) `Bash run_in_background` + `TaskStop`/`Monitor` | the numbered job list; `jobs.c` shrinks 108 to 41 lines (`job_all_kill` and friends stay if `Bash` background uses them: the compiler decides) |
| `terminal` | `terminal.c:311` `tool_terminal`, `term_*` (59-310) | (b) none | **a shell that keeps its state** (open/send/read/close/list: interactive programs, REPLs, `ssh`). The only stateful shell; 73 calls in the old log (tools-review 4.3). `Bash` keeps only the cwd |
| `terminal_screen` | `logs.c:162` | (b) none | reading the terminal screen of the above; goes with `terminal` |
| `git` | `devtools.c:436-494` `tool_git`, `tool_git_in`, `git_*` | (a) `Bash git ...` | the structured `git(args)` (no shell, refuses dangerous forms: `KATANA_GIT_UNSAFE`); `Bash` now gets the `never:`/`ask:` guards of `KATANA.md` instead |
| `install_package` | `devtools.c:363` | (a) `Bash pip/npm/apt/cargo install` | the `manager` allow-list and the sudo wiring (`sudo_prepare`, via `Bash` it is still there) |
| `download_file` | `devtools.c:320` | (a) `Bash curl -o` / `WebFetch` | the size limit `KATANA_DL_MAX_MB` (4 uses) and the check of the destination path (`agent_write_path`) |
| `read_log` | `logs.c:80` `tool_read_log`, `log_emit` | (b) none | reading the katana's own logs by name (`source`: agent, commands, journal, dmesg, syslog) with `filter`. `Bash tail/journalctl/grep` is the manual substitute; `logs.c` is **dead as a whole** after removal (196 to 5 lines) |
| `howto` | `howto.c:59` `tool_howto` | (a) `Skill` | the recipes are already reached through `Skill` (`skill.c:373`); `howto.c` keeps the recipe table |
| `remember`, `recall` | `memory.c:302, 385` | **kept** (owner's exception) | see 2.5 |

Summary of what is truly lost (for the owner): `terminal` + `terminal_screen` (stateful shell), `find_file` (machine-wide
name search), `read_log`, `sudo=true` on `write_file`/`edit_file`, `append=true`, directory listing in one call, structured
`git`. `docs/tools-review.md` section 8 listed the 8 old names the curriculum still teaches (55 calls, 1.6% of v7): all go with the curriculum regeneration.

### 2.4 Measured cascade (what the compiler leaves dead)

Experiment, in a scratchpad copy only: delete `dispatch.c:66-202` (the `old_guard` call and the 24 branches, turning
`} else if (mcp_owns...` into `if`) and `old_guard`, then run the delete-unused loop. Result: 78 functions removed over 5 rounds,
of which 32 were already dead (the baseline of 3.7) and **46 are new**:

```
dispatch.c: old_guard
code.c:     tool_read_file, rd_all_notes, rd_get, rd_notes
devtools.c: tool_download_file, tool_edit_file, tool_git, tool_git_in, tool_glob, tool_install_package, tool_read_document,
            diff_lines, git_has, git_tokens, glob_cmp, glob_match, glob_walk, sha2_file, tx_emit
jobs.c:     tool_job, tool_job_start, job_status
locate.c:   tool_find_file, tool_list_dir, ld_cmp, loc_size
logs.c:     tool_read_log, tool_terminal_screen, ci_has, log_emit, read_tail_file
memory.c:   tool_recall
sudo.c:     root_read_file, root_write_file, sudo_prefix, sudo_ready
terminal.c: tool_terminal, sudo_path, term_extract, term_find
todo.c:     tool_todo
tools.c:    tool_search_files, sf_file, sf_match, sf_walk
```

Non-blank lines of `src/agent/*.c`: 13285 before, 11832 after: **1453 lines**, before counting the old prompt branch (about 45
lines), the `ct_set2` ternaries, static data that only the dead functions used (job table, terminal sessions, `g_rd_*` read
cache; GCC does not warn for those with the flags used), and the desktop/tests/curriculum. Note the compiler cannot see **non-static** functions
(this is a unity build, so most are non-static or `static` in one file); a second pass with `-Wunused-function` only catches `static`. The wave
must also grep each removed function name afterwards.

Two surprises worth knowing: `tool_recall` becomes dead (it is only reached by the dispatcher; the model would no longer
have a recall tool) while `tool_remember` stays alive only through `katana memory add` (`memory.c:434`); and `tool_howto` stays alive through `Skill`.

### 2.5 Memory, and why the owner said the UIOE needs it

Facts from the code, for the owner to decide (nothing here blocks the removal):

- `memory.c` is 500 lines. Its header says `memory_search` is "used by recall and by the UIOE". **`memory_search` has no caller
  at all today** (it is in the dead-function list, `memory.c:284`); `tool_recall` uses `mem_rank` directly.
- The UIOE connector `memory.recall` (`src/uioe/registry.c:84`) has no `run` function: `uioe_av_memory` (registry.c:27-32) only
  checks that `~/.katana/memoria.txt` (or `KATANA_MEM`) exists; `budget.c:59` prints "exists in the registry but is not built in
  this phase (memory, history and web arrive in U8...)". So today the UIOE reads **nothing** from memory.
- The system prompt injects the last 20 facts (`prompt.c:144` `mem_tail(20)`, header "Saved memory"), unless `g_code`.
- The curriculum teaches `remember`/`recall` (`scenario_more.c:277-283`, `scenario_rare.c`) and `recall` is in the plan-mode ok-list.

Decision implied by the owner's words ("memory stays because the UIOE uses memory.search"): keep `memory.c`, **but** the
tools `remember` and `recall` are old-name tools in `dispatch.c`. Two coherent options **(opinion: A)**:

- A. Keep `memory.c` as the engine behind `katana memory` and `mem_tail`/`memory_search` (for the UIOE when it gets a `run`), and remove the model-facing tools
  `remember` and `recall` with the rest of the old set (the model then has no memory tool; the saved facts still reach the prompt through `mem_tail`, written by `katana memory add`).
- B. Keep the two tools, by adding them to `table.c` as `Remember`/`Recall` (the same function bodies, the table's shape). This is the only
  migration in the whole plan, and the owner said none.

What goes in either case: the old line format reader (`mem_load` branch for `DATE<TAB>fact` without id, `memory.c:52-95`, header
comment lines 2-4, 8, 11-12), `katana memory export` (`memory.c:491`, usage text 440), `mem_tail`'s "identical to before" promise.
`mem_load` will accept only the new line kinds (`#sup`, `#del`, `#pin`, and a new live-fact line that carries its id).
**Behaviour change to accept:** an existing `~/.katana/memoria.txt` is then not read (owner: no data worth keeping).

### 2.6 Desktop, MCP server, tests

**Desktop** (`desktop-qt/`):

| file | old names | change |
|---|---|---|
| `qml/Verbs.js:5-9` | verbs for `list_dir`, `read_file`, `write_file`, `edit_file`, `run_command`, `search_files`, `glob`, `web_search`, `web_fetch`, `download_file`, `install_package`, `git`, `read_document`, `terminal`, `read_log`, `terminal_screen`, `remember`, `recall`... | delete the old keys (`Find`/`Symbols` verbs exist: 7 matches in the file). Fallback `verbo(n) = verbos[n] || n` stays |
| `src/backend.cpp:1915` (`isPlanTool`) | `todo` | remove `todo`; keep `Task*`, `TodoWrite` |
| `src/backend.cpp:1793` | `CHOICE` marker comment "(ask_user)" | marker stays (`AskUserQuestion` still emits it via `tool_ask_user`); rename the comment |
| `src/backend.cpp:2131` | comment `edit_file replace_all ... Gemma 4` | rename |
| `src/backend.h:233, 240, 415` | comments naming `todo`, `ask_user` | rename |
| `qml/Main.qml:556` | comment `ask_user` | rename |
| `src/selftest.cpp:177, 278, 285, 464`; `tools/standin-engine.sh:124` | the stand-in sends `TOOL list_dir {"path":"."}` and the selftest checks "cartao de ferramenta list_dir" | change the fixture to `Read`/`Bash` (any table name) and the checks |
| `README.md:39, 59-65, 73, 86` | `run_command`, `write_file`, `edit_file`, `ask_user`, `list_dir` in prose | edit |
| old sessions | `~/.katana/desktop/sessoes/*.json` replay tool cards by name; with the verbs gone an old session shows the raw name (the fallback) | no warning needed (owner: no data to keep) |

**MCP server** (`katana mcp serve`, `agent/mcp_serve.c`): nothing to remove. tools-review 7.3 suggested adding `Find`/`Symbols`:
that is a feature, out of scope here.

**Tests** (counts are `grep -cw` of the old names over each file; `terminal`/`job` also match prose):

| file | count | action |
|---|---|---|
| `tools/testar_ferramentas.sh` | 106 | tests the old tools with `katana tool run_command ...`; **delete the file** (the table tools have `tools/tooltest/*.sh`). Its header says it needs git, pdftotext |
| `tools/tooltest/meta.sh` | 14 | has the old-set cases and line 165 `KATANA_TOOLS=1 katana tool howto`; remove old-name cases, keep table cases |
| `tools/tooltest/files.sh` | 6, `find.sh` 3, `search.sh` 1, `bash.sh` 1, `web.sh` 1, `tasks.sh` 2, `extend.sh` 1, `mcpx.sh` 1, `launch.sh` 1 (`unset KATANA_TOOLS`), `reap.sh` 4 | replace each old call with the table call, or drop the case |
| `tools/tooltest/channel_test.c`, `console_pty.py` | 4 and 2 | `setenv("KATANA_TOOLS","2")` lines: drop the variable; `mock_teacher*.py` `MOCK_TOOLS` |
| `tools/servertest/test_server.py:174` | `KATANA_TOOLS="2"` | drop |
| `tools/traintest/run.sh` | 11; line 31 `export KATANA_TOOLS=1`; lines 203-255 pass `=2` | **line 31 makes every old step run on the old set**: those steps need rewriting to table names (this is the biggest test rewrite; ask the wave to read the file whole) |
| `tools/traintest/gemma4_part.sh`, `specialize_part.sh`, `specialize_oneshot.sh` | `KATANA_TOOLS=1/2` on dozens of lines (`specialize_oneshot.sh` 12 uses) | delete the variable (a sed, count with `grep -c KATANA_TOOLS`: 35 lines in `tools/`) |
| `tools/traintest/mock_teacher.py`, `mock_teacher_prog.py`, `import_calls.c`, `plugins/teacher-anthropic/test/mock_api.py` | old names in mock replies | switch the mock to table names |
| `tools/codebench/README.md` | `web_search`/`web_fetch` | edit |

Criterion for "tooltests passing": **the wave must run them**; this inventory did not (rule: no tests here).

### 2.7 Curriculum and scenarios (to be regenerated, so rewrite or drop)

`data/curriculum/v7` is regenerated by `src/app/curriculum.c` + `scenario*.c` (`generate.sh`). Dependencies on old names:

| where | what | action |
|---|---|---|
| `src/app/scenario.c:11, 86-109` `XF_OLD` | "tool of the first set: `agent_tool_run`" flag; it also marks MCP calls (`scenario_conv.c:109`, `x_mc`) | the old-tool meaning goes; the MCP meaning needs another name (`XF_DIRECT`) |
| `src/app/scenario_more.c:145-156, 264-289, 345-351` | `fam_x_tools` kinds `K_GIT`, `K_MEM`, `K_DL`, `K_TERM` (calls `git`, `remember`, `recall`, `download_file`, `terminal`) | drop those kinds (4 of 14); `K_MEM` only if memory tools go (option A) |
| `src/app/scenario_more.c:431` `cu_is_old_tool` | list of the 8 old tools; used by `curriculum.c:2364, 2459` (validation skips the table check for them) | delete the function and both uses |
| `src/app/scenario_rare.c` (`x-rare`, 152 conversations = 14.6% of v7 per tools-review) | **44 table rows** whose first field is `git`, `terminal`, `recall`, `remember`, `download_file`, `install_package`, `read_log`, `howto` (e.g. lines 110-129 for `terminal`, `install_package`, `read_log`, `howto`; 336-342 for `git`); `x_rare_declared`, `old` flag (`e->c[i].old`, line 659) | delete the 44 rows and the `old` flag |
| `src/app/curriculum.c:2531, 2593` | `putenv_dup("KATANA_TOOLS=2")` | delete |
| `src/app/scenario_concept.c`, `scenario_tasks.c`, `scenario_v7.c`, `scenario.c` | 3, 1, 2, 1 matches of old names (read each; `scenario_tasks.c:566, 619` use `%s_legacy` as a *generated function name* for a scenario, **not** a legacy of the code) | read and fix |
| `src/app/importer.c` | the old-to-new map and key translation (217-330) | delete: with no old data, `katana import --keep-tool-names` and the conversion have no input |
| `data/curriculum/v6/` | old curriculum files: 23 MB `v6.kspec` + `v6-*.jsonl.gz`, tracked in git | **(opinion)** delete the directory (it is the previous curriculum; `docs/curriculum.md` says v7 regenerates everything and "nothing is copied from the v6 files") |
| `data/curriculum/v7/*.jsonl.gz`, `family-counts.txt`, `check-*.txt` | contain 55 old-name calls (terminal 9, git 12, remember 7, install_package 6, download_file 6, read_log 5, recall 5, howto 5, tools-review.md section 2) | regenerated; replace the files in the same commit as the generator change |

## 3. Other legacy

### 3.1 Files in the repository root

| path | tracked in git? | size | what it is | action |
|---|---|---|---|---|
| `attic/ia.c` | **yes** | 12 KB | 2026-11-14 prototype ("ia.c, transformer in pure C"); not built, not referenced by `build.sh`, `build.bat` or any doc (`grep -rn attic` finds only its own commit message) | delete (git history keeps it) |
| `compare.sh` | **yes** | 1.3 KB | alternates katana and the reference engine with `katana.exe` and `llama-completion.exe` (Windows names, `llama.cpp` spelled out in the script and in 6 doc lines: `README.md:326`, `docs/knowledge.md:65, 284, 310, 423`, `docs/optimization-roadmap.md:5`, `docs/comparisons.md:56`, `docs/handoff.md:1321, 1329`) | **(opinion)** delete: it cannot run on Linux as written (`$here/katana.exe`), cites another engine by name (CLAUDE.md: "the code does not cite other inference engines"); `katana bench` and `katana codebench compare` exist. The doc lines must stop calling it a tool |
| `build.bat` | yes | 0.3 KB | MSVC build, used by the project's own CLAUDE.md ("Compilar: cmd //c ...") | keep (not legacy) |
| `build.sh` | yes | 1 KB | Linux build; passes `-Wno-unused` | keep; the wave changes it (4.5) |
| `ia.exe`, `ia.obj` (2026-11-14), `modelo.bin` (2026-11-14, 388 KB) | **no** (ignored by `.gitignore`: `*.exe`, `*.obj`, `*.bin`) | 192 KB, 32 KB, 388 KB | build products of `attic/ia.c` | junk, delete by hand |
| `katana.exe`, `katana.obj` (2026-11-15) | no (ignored) | 1.2 MB, 1.4 MB | old Windows build products | junk |
| `katana.cfg` (root) | no (`.gitignore`: `katana.cfg`) | 68 bytes (`gpu=`, `seg=`, `rows=`, `rowsb=`) | per-folder cfg written by `katana tune`; `~/.katana/katana.cfg` is the real one (read in `launch.c`, `update.c:70`) | local data, ignore |
| `desktop/` | no (empty directory, no tracked file) | 0 | the old desktop folder, empty since `desktop-qt/` replaced it | `rmdir` |
| `katana-desktop` | no (symlink, ignored) | | link to `desktop-qt/build/katana-desktop-qt` | keep (a local convenience) |
| `katana` (7 MB binary) | no (ignored) | | build output | keep |
| `.katana/` (root) | no | 2.4 MB | a store and prefix caches made by running katana in the repo | delete by hand |
| `.github/workflows/pacotes.yml` | yes | | AppImage workflow, manual trigger; runs `./build.sh` and `desktop-qt/packaging/appimage.sh` | keep; name it in English if the owner wants (opinion, low value) |

### 3.2 Old file formats the code still reads (all marked for removal; no migration, no warning)

| format | reader (file, function, lines) | what to do |
|---|---|---|
| `memoria.txt` live-fact line `DATE<TAB>fact` (no id) | `memory.c:52-95` `mem_load`; header 2-12 | 2.5: only the new line kinds |
| stores without the `ROOT` marker | `coder/roots.c:187` `index_old_local_store` (+ stub at 198), used only by `coder/storedocs.c:611` (`coder status` prints "there is an old local index without marker... `katana init` adopts it"); the doc comment `roots.c:18-19` | delete the function, the stub, the message and the comment; a store without a marker is simply not an index (the next `init`/query rebuilds) |
| store format version `< STORE_FORMAT` | `store.c:36` (`STORE_FORMAT 1`), `store.c:271` ("older... will be rebuilt"), `coder/maint.c:75-104` `mt_store_class` class 1 "delete and rebuild", `maint.c:23` | **(opinion)** keep the *newer than this binary* and *damaged* classes (real safety: the code must not destroy data from a newer build) and reduce "older" to the same rebuild path as damaged; `STORE_FORMAT` is 1 today, so nothing older exists: the "older" branch is already dead code |
| index sections without the new bits | `coder/symindex.c:16-23`: "old segments only have the seven sections", "old stores only have roles 1 and 2 and sub 0", `KDOCS` bit `0x40000000` "(without it an old contract has no new keys)"; `crawl.c:14, 648` ("old segment leaves the manifest in the same swap" is normal compaction, **not** legacy) | remove the reader tolerance for absent sections/bit; the rebuild covers old stores. Needs one read of `symindex.c` by the wave (the exact branches were not traced here) |
| `find-*.idx` | section 1 | |
| old `.ksess` sessions | `agent/loop.c:177-181` block `KSX1` appended at the end ("who reads the session to resume stops before this block, so old and new files stay valid"); `loop.c:176` `KATANA_SESS_RESUMO`; `270-282` `KATANA_RETOMAR*` | the `.ksess` format is a feature (resume, training-data extraction). Remove only "old and new valid" tolerance: files without the `KSX1` block are rejected by `learn`/`importer` instead of tolerated (**opinion**, small gain; see readers: `app/learn.c`, `app/importer.c`, `app/specialize.c`, `app/codebench.c`, `agent/teacher.c`) |
| `agente.log` line format | `agent/tools.c:149`: "the old readers (katana agentlog, learn, export, dashboard, desktop) keep reading the same line format" | nothing to remove: it describes that one format is kept; delete the sentence |
| `.kspec` end marks | `app/train.c:1868-1872` (`KSXF` + marks; "old files have none and mean 0"), `train.c:2006` `.kopt`, `train_methods.c:442, 545` ("old readers stop at the tensors") | these are LoRA/DoRA file layout, not katana-version legacy; **keep** (opinion) |
| `~/.katana/cache` | `KATANA_CACHE_DIR` (real key) | keep; see 1.4 |
| manifests of an earlier format | none besides the store `MANIFEST` above | |
| Portuguese command aliases | `app/console.c:82` ("names of commands in English; the old Portuguese names are still accepted as aliases", rewrites the line) | remove the alias table (3 matches in `console.c`) |
| Portuguese flag/CLI spellings | `models`/`modelos` (`main.c:1274`), `calibrar` (`main.c:1092`), `--sessao` (used in tests: `specialize_part.sh:119-121`), env `KATANA_RETOMAR*`, `KATANA_SESS_DIAS`, `KATANA_SESS_RESUMO`, `KATANA_TEMA`, `KATANA_FONTE` | CLAUDE.md rule: names in English; **(opinion)** rename or drop the Portuguese twin; each one needs its test updated |

### 3.3 `KATANA_*` environment keys

291 distinct names are read. Classification here is only for the ones tied to the old behaviour, the code-agent layer and the ones the
brief named; the engine's performance knobs (`KATANA_VK_*`, `KATANA_QN_*`, `KATANA_Q4_*`, `KATANA_TRAIN_*`, `KATANA_EXPERT_*`...) were
not reviewed one by one and are **not** claimed legacy. Default = what the code does when the variable is unset.

| key | default | what it does | class | recommendation |
|---|---|---|---|---|
| `KATANA_TOOLS` (`prompt.c:139`) | table set | `=1` declares the old set (old prompt branch) | **legacy** | remove |
| `KATANA_FIND_INDEX` (`find.c:82`) | `store` | `=legacy` uses the Find's own index and `find-*.idx` | **legacy** | remove |
| `KATANA_FIND_REFS` (`find.c:1628`) | symbol index | `=text` skips the symbol index for `mode=refs` ("0 = follows the old path") | **legacy** (the "text" path is the old way) | remove, with the text fallback of `fd_refs_sym` only if `fd_refs` is still used by the store path (it is: `find.c:1767`; keep `fd_refs`, remove the switch) |
| `KATANA_READ_OUTLINE` (`read.c:219`), `_LINES` (214) | on | `=0` turns the symbol map of a large file off | escape | **(opinion)** keep `_LINES` (limit), remove `=0` switch |
| `KATANA_REPEAT_NOTE` (`loop.c:477`) | on | `=0` turns the "same call twice" note off | escape | remove (feature stays on) |
| `KATANA_READ_CURSOR` (`common.c:166`) | on | `=0` turns the read-cursor continuation off | escape | remove |
| `KATANA_READ_DELTA` (`facts.c:96`) | on (depth 1) | forces the delta-read on or off | escape | remove |
| `KATANA_PLACE_NOTE` (`loop.c:707`) | on | `=0` turns the "[katana: you mention X, it is at ...]" note off | escape | remove |
| `KATANA_CODER` (`find.c:1506`) | `2` (full) | `off\|0` no coder engine in Find; `names\|exact` names only | escape / research | **(opinion)** keep (a way to run the Find without the coder engine for diagnosis); it is not tied to old data |
| `KATANA_AUTOINDEX` (`loop.c:454`) | on | `=0` stops the agent from starting an index of the working folder | operation | keep |
| `KATANA_INDEX_WATCH` (`coder/watch.c:59, 87`) | `1` (on) | `off\|0`, `poll`, `on` | operation | keep |
| `KATANA_NEIGHBORS` (`findgraph.c:832`) | on | `=0` turns the neighbour notes off | escape | remove |
| `KATANA_FIND_ACT` (`findgraph.c:44`) | on | `=0` turns sparse activation off | research | remove (as tuning switch) |
| `KATANA_FIND_GRAPH` (`coder/graph.c:77`), `_LEX` (`findstore.c:385`) | on | `=0` | research | remove |
| `KATANA_FIND_NEXT` (`find.c:1641`) | on | `=0` turns the "next:" lines off | escape | remove |
| `KATANA_FIND_CO` (`findstore.c:618`, `findgraph.c:318`) | on | `=0` turns the git co-change edges off | research | remove |
| `KATANA_FIND_TUNE`, `KATANA_FIND_REPEAT`, `KATANA_FIND_TIME`, `KATANA_FIND_CAND`, `KATANA_FIND_CHARS`, `KATANA_FIND_MAX_FILES`, `KATANA_FIND_GRAPH_MAX`, `KATANA_FIND_DEBUG` | | ranking weights, repeat count, timing, candidate cap, output chars, file cap | research (`FIND_TUNE`, `FIND_REPEAT`, `FIND_TIME`); limits (`FIND_MAX_FILES`, `FIND_GRAPH_MAX`, `FIND_CAND`, `FIND_CHARS`) | keep limits and `FIND_TIME`; drop `FIND_TUNE` and `FIND_REPEAT` (benchmarks) **(opinion)** |
| `KATANA_CODER_BM25`, `_RRF`, `_CONCEPT`, `_RANK` (`coder/search.c:425-428`) | none | override ranking constants / `RANK=sum` | research | remove |
| `KATANA_TOOL_DECL` (`prompt.c:185`) | by the model (`g_spec_tools`) | `0` never, `1` always declare tools | operation (needed for base models) | keep |
| `KATANA_CALL_GRAMMAR` (`callgrammar.c:509, 525`) | on | switch the constrained call grammar | operation | keep |
| `KATANA_G4_PLAIN_RESULT` (`loop.c:968`) | off | "`=1` goes back to the old" tool-result wrapper for Gemma 4 | **legacy** | remove |
| `KATANA_G4_ENVLAST` (`fmt_gemma4.c:172`) | off | environment block placement | escape | remove |
| `KATANA_MOE_OLD` (`main.c:1346`, `cpu_kernels.c:687`) | off | "previous path of the CPU experts (comparison)" | **legacy** | remove with its branches (`cpu_kernels.c:701`, `vk_dense.c:1334`) |
| `KATANA_COMPACT` (`loop.c:562`) | on | `=0` turns auto compaction off | operation | keep |
| `KATANA_SESS_RESUMO` (`loop.c:176`) | exit only | `troca` also summarizes on session switch | escape | remove (Portuguese name too) |
| `KATANA_RETOMAR`, `_FRIO`, `_OUTRO` (`loop.c:270-282`) | | resume modes, "compare" cold resume | research | remove (Portuguese names) |
| `KATANA_BRIEF_NOTE` (`tools/table.c:225`) | off | prints the end-of-request note in `katana tool` | research | keep or drop, nobody calls it |
| `KATANA_MEM`, `KATANA_LOG`, `KATANA_CACHE_DIR`, `KATANA_MODELS`, `KATANA_WEB`, `KATANA_WEB_CACHE`, `KATANA_CONFIRM`, `KATANA_ROUNDS`, `KATANA_CTX`, `KATANA_MAXTOK`, limits `KATANA_INDEX_MAX_*` | | paths, limits | operation | keep |

Count of keys proposed for removal in this table: 20 (`TOOLS`, `FIND_INDEX`, `FIND_REFS`, `READ_OUTLINE` switch, `REPEAT_NOTE`, `READ_CURSOR`, `READ_DELTA`,
`PLACE_NOTE`, `NEIGHBORS`, `FIND_ACT`, `FIND_GRAPH`, `FIND_GRAPH_LEX`, `FIND_NEXT`, `FIND_CO`, `FIND_TUNE`, `FIND_REPEAT`, `CODER_BM25/RRF/CONCEPT/RANK`, `G4_PLAIN_RESULT`, `G4_ENVLAST`, `MOE_OLD`, `SESS_RESUMO`, `RETOMAR*`). Removing a switch means
keeping the on-behaviour and deleting the off branch. **(opinion)** the first wave should only remove the three the owner named as
legacy plus `G4_PLAIN_RESULT` and `MOE_OLD`; the others are "escape" switches that are cheap and the owner said keep operation keys,
so leave the call to the wave leader after the loop/find agents finish.

### 3.4 Old modes and commands

- **`katana code`** (`main.c:1099-1100`: `argv[1]=="code"` sets `g_code`; `agentsys code` at `main.c:1068-1071`; `codebench --mode agent|code`, `codebench.c:1037`). `g_code` has 16 uses and only changes the prompt text ("a programming agent", "Write the code into files, never only describe it", no memory facts, no `remember` declaration in the old branch). README marks it "older". **Recommendation: remove** (merge: the same agent already codes; the extra prompt sentence is two lines that can move to the one prompt). Touches: `prompt.c:144, 172, 174, 211, 212, 234`, `tools.c:113`, `main.c:1068-1071, 1099-1100`, `scenario.c:784` and `curriculum.c:875` (`g_code = 0` assignments), `launch.c:1364` list `"code"`, `codebench.c:1037`, `README.md:56, 213`, `docs` (`grep -c 'katana code\b'`: `README.md` 2, `docs/handoff.md` 2, `docs/plan.md` 1, `docs/uioe-code-audit.md` 1; `katana codebench` and `katana coder` are different commands).
- **`lc_known_cmd` list** (`launch.c:1363-1371`) vs the subcommands handled in `main.c`: the list is **missing** `coder`, `docsel`, `kerneltest`, `lookuptest`, `placenote`, `uioe` (handled at `main.c:1250, 1251, 1202, 1161, 1011, 1249`), and **contains** `skills` (no handler: only `skill`). Fix = delete the list and dispatch by the same table as `main.c`, or make it complete (it also contains `install`, `uninstall`, `adopt`, handled in `launch.c:1525-1526`, correct).
- Other CLI names: `calibrar` (Portuguese, `main.c:1092`), `modelos` alias of `models`, `compare` (two meanings: `main.c:1209` and `codebench compare`), `tiersim`, `iobench`, `kerneltest`, `lookuptest`, `placenote`, `gradcheck`, `q4check/q4bench/q4gpu`: research/test subcommands, not legacy by the owner's rule (**opinion**: they are measurement tools; the owner said no measurement in this phase, not delete them).
- `--sessao` flag (Portuguese) exists next to `--session-id`; check with the owner.

### 3.5 Scripts in `tools/` that do not work any more

Checked: `tools/testar_ferramentas.sh` (old set, 106 old-name matches, **dead**), `tools/traintest/run.sh` line 31 (old set), `compare.sh` (Windows names).
Not checked: the many tools under `tools/` (889 tracked files) were not executed. Candidates the wave should read before deleting (none proven broken):
`tools/agulha.py` (long-context needle test, referenced by `docs/design/70-inference.md` and `docs/handoff.md`), `tools/sim_vagas.c` (expert-slot simulator, same docs),
`tools/investigate/`, `tools/mcp-teste/servidor.py`, `tools/web-mock/`, `tools/dashboard/` (referenced by the agent log readers: `katana export`, `dashboard`).
No references to `katana.exe`, `ia.exe`, `modelo.bin` or `attic` exist in `tools/`, `desktop-qt/`, `plugins/`, `data/`, `site/`.

### 3.6 `grep -i 'legado\|legacy\|compat\|antigo' src/`: 132 lines, classified

Real legacy to remove or rewrite (about 30 lines): `dispatch.c:30, 66` (old set guard), `prompt.c:138`, `webnav.c:154`, `memory.c:2-12`, `findstore.c:2, 36`, `find.c:8, 67, 1615, 1729`, `findgraph.c:15`, `roots.c:18-19, 187`, `maint.c:23, 75, 92`, `symindex.c:16-23`, `loop.c:180, 967`, `agent/tools.c:149`, `console.c:82`, `importer.c:217-220, 276, 937`, `howto.c:3`, `callgrammar.c:517` ("the old path of forward.c is left out": check what it refers to), `indexctl.c:13`, `main.c:1346` (MOE_OLD).

Not legacy, keep (about 100 lines): `mcp.c`/`mcp_extra.c` `mcpx_legacy_*` and "old SSE transport" (14+7 matches: this is the **MCP protocol's own** old SSE transport, still spoken by some servers; opinion: keep, the owner may drop it), `tokenizer.c` `PRE_LEGACY`/`encode_legacy` (a pre-tokenizer family of some models), `web.c:57` "Mozilla/5.0 (compatible; ...)" (HTTP user agent string), `train.c`, `train_methods.c`, `train_vk.c` (file layouts and "old path" of the Vulkan kernel in training, 9 lines: `train_vk.c:13, 297, 374` describe a fallback kernel; check if the fallback is reachable), `crawl.c` (compaction), `scenario_tasks.c:566, 619` (`%s_legacy` generated names), `webfetch.c` ("compatible"), `context.c` ("anterior" = previous turn), `codebench.c:7` ("the old run.py": the old script no longer exists, edit the comment).

Plan-time rule: after the wave, this grep must return only the "not legacy" list above, each line justified in a comment or reworded so the grep is clean.

### 3.7 Dead code (compiler, not opinion)

Command: `gcc -O1 -c -Wall -Wunused-function -Wno-unused-variable ... -mavx2 -mfma -mf16c -I . -I src -I vk/include src/katana.c`. Result: 32 unused static
functions, after running the same cascade script on the untouched tree (the 31 first-round + `store_table_lower` second round), plus 1 unused label.

| file | dead functions |
|---|---|
| `app/launch.c` | `lc_exe_path` (133) |
| `app/scenario_report.c` | `cu_rv_free` (10) |
| `app/scenario_concept.c` | `xc_tab_con` (602) |
| `app/train_methods.c` | `tm_finitef` (51) |
| `agent/extend.c` | `ex_trimcpy` (71) |
| `agent/memory.c` | `memory_search` (284): **the one the header says the UIOE uses** |
| `agent/fmt_gemma4.c` | `g4_cmp_str` (38) |
| `agent/tools.c` | `path_ok` (63) |
| `uioe/verify.c` | `uioe_range_text` (107) |
| `uioe/evgraph.c` | `ug_edge_between` (58), `ug_reach_tier` (54), `ug_same_file` (14) |
| `coder/text.c` | `coder_detect_lang` (366), `coder_split_ident` (202) |
| `coder/kb.c` | `kb_open` (411) |
| `coder/parse2.c` | `c2_vlang_ok` (2397), `c2_refrange` (614), `c2_symrange` (335) |
| `coder/parse.c` | `cp_heritage` (741), `cp_closer` (269) |
| `coder/maint.c` | `ign_scoped_add` (380), `ign_reg_largest` (166), `ign_reg_find_dir` (164) |
| `coder/storedocs.c` | `st_cmp_str` (434), `store_live_hide_scoped` (132), `store_docs_find` (53) |
| `coder/store.c` | `store_next_section` (350), `store_table_find` (135), `st_get_deltas` (90), `st_put_deltas` (84), then `store_table_lower` |
| `engine/qwen4exp.c` | `q4_kern_ok` (505) (reported in the first, flag-less run; absent after fixing the flags: re-check) |
| `engine/cpu_kernels.c` | `hsum128` (reported with the AVX2 flags) |
| `uioe/budget.c` | label `retry` (91), `-Wunused-label` |

Limits of this list: only `static` functions are found; in a unity build a non-static unused function is invisible to the compiler. The wave should
add `-Wunused-function` without `-Wno-unused` to `build.sh` (3.8) and a second pass counting names with `grep -w` (a name that
appears once in `src/` is dead, as for `store_*`).
`#if 0`: 0. TODO/FIXME/HACK: 0 real (the `XXXXXX` hits are `mkstemp` templates). Large commented-out blocks: not found by the keyword greps used; not searched exhaustively.

### 3.8 Build

`build.sh` line 14 passes `-Wall -Wno-unused`: the second flag is why 32 dead functions survive. Done criterion needs `-Wno-unused` removed
(or replaced by `-Wno-unused-variable` only if the wave cannot clean variables in the same pass; **opinion**: clean both).

## 4. The plan

### 4.1 Order, by risk (first what has no dependent)

Wave 0 and 1 can run **now**, in parallel with the agents that work on `loop.c`, `find.c` and `symtool.c` (plan.md T7 says to wait for them
for waves 2 to 4; wave 0 touches none of those three files).

| wave | content | files | risk | depends on |
|---|---|---|---|---|
| **0: junk and dead code** | delete 32 dead functions (3.7); delete `attic/`, `compare.sh`, `desktop/` (empty), `data/curriculum/v6/`; fix the root of the repository; remove `-Wno-unused` from `build.sh` and fix what appears | files in 3.7 (none is `loop.c`/`find.c`/`symtool.c`), `build.sh`, root, `docs/*` that cite `compare.sh` (6 lines) | very low: nothing calls them | nothing |
| **1: old formats and old names with no dependents** | the 6 formats of 3.2 except memory; Portuguese aliases (`console.c:82`); `index_old_local_store`; `STORE_FORMAT` "older" branch; `symindex.c` tolerance; `KATANA_G4_PLAIN_RESULT`, `KATANA_MOE_OLD`; `katana code` mode (3.4) | `coder/roots.c`, `storedocs.c`, `store.c`, `maint.c`, `symindex.c`, `app/console.c`, `fmt_gemma4.c`, `cpu_kernels.c`, `vk_dense.c`, `main.c`, `prompt.c`, `tools.c`, `launch.c`, `codebench.c` | low | nothing; `prompt.c` is also edited in wave 3: do `g_code` after wave 3 or in the same pass |
| **2: legacy Find index** | section 1.2 in full; new source for Rename (1.3); drop `KATANA_FIND_INDEX`, `KATANA_FIND_REFS` | `tools/find.c`, `findgraph.c`, `multiedit.c`, `coder/findstore.c`, `coder/indexctl.c`, comments | medium: `Rename` | the `find.c` agent finishes |
| **3: old tool set** | delete the 24 branches of `dispatch.c`, `old_guard`, the old branch and `KATANA_TOOLS` of `prompt.c`, the lists in `loop.c`, `planmode.c`, `launch.c:622, 702`, `teacher.c TCH_EXTRA`, `diagnose.c` hints, then loop "compile, delete the unused, repeat" (2.4) until clean; remove `remember`/`recall` per 2.5 | `agent/dispatch.c`, `prompt.c`, `loop.c`, `planmode.c`, `devtools.c`, `locate.c`, `code.c`, `jobs.c`, `logs.c`, `terminal.c`, `todo.c`, `tools.c`, `sudo.c`, `memory.c`, `teacher.c`, `diagnose.c`, `webnav.c`, `howto.c`, `callgrammar.c`, `app/launch.c`, `katana.c` (includes: `logs.c` goes), `forward.c`/`main.c` fixtures | **high**: the biggest change, many files | the `loop.c` agent finishes; wave 4 must land with it, or the build of the curriculum breaks |
| **4: curriculum, tests, desktop, docs** | 2.6, 2.7, docs (4.4) | `src/app/scenario*.c`, `curriculum.c`, `importer.c`, `tools/*`, `desktop-qt/*`, `docs/*`, `README.md`, `data/curriculum/v7/` | medium: tests | wave 3 (same commit or immediately after) |
| **5: environment keys** | the escape switches of 3.3 | `loop.c`, `read.c`, `common.c`, `facts.c`, `findgraph.c`, `find.c`, `coder/*`, `fmt_gemma4.c` | low each, many files | waves 2 and 3 |
| **6: owner's machine** | 4.7: instructions only | | | last |

### 4.2 What each removal does to tests, docs and curriculum

| removal | tests | docs | curriculum |
|---|---|---|---|
| old formats, `index_old_local_store` | none (no test covers them; `tools/tooltest/*` has none) | `docs/code-index-lifecycle.md` (8 matches), `docs/code-intelligence.md` (5) | none |
| `katana code` | `codebench --mode code` in `tools/traintest/codebench_part.sh` | README 56, 213; `docs/specialization.md`; `desktop-qt/README` | none (`g_code = 0` in `scenario.c:784`, `curriculum.c:875`) |
| legacy Find index | add a Rename tooltest in `tools/tooltest/find.sh` or `files.sh` (it has none today: `grep -n Rename tools/tooltest/*.sh` is empty); remove `KATANA_FIND_INDEX` mentions | `docs/code-index-lifecycle.md`, `docs/code-intelligence.md`, `README.md`, `docs/handoff.md` (62 old-name matches, the largest) | none |
| old tool set | section 2.6 | section 4.4 | section 2.7 |
| env keys | `tools/*` that set them (grep each) | `README.md:308` table and `docs/handoff.md` | none |

### 4.3 What the owner's old sessions and logs lose

Per the owner's last message nothing in `~/.katana` has value, so: **no warning is needed and none should be coded.** For the record,
what stops working after waves 1 to 3: `~/.katana/desktop/sessoes/*.json` and `*.ksess` that call old tools replay as cards with the raw
tool name (the desktop falls back to the name); `agente.log` lines with old names stay in the file (the readers `agentlog`, `learn`, `export`
read names as strings, so they are unaffected); `memoria.txt` is no longer read (wave 3, 2.5); `find-*.idx`
caches are ignored. **(opinion)** `katana --resume` of a session that contains an old tool call would send that history to the model with names the model no longer
knows; it will not break, it will confuse the model. Delete the sessions (4.7).

### 4.4 Docs to change

Counted with `grep -cw` of the old names, `KATANA_TOOLS`, `KATANA_FIND_INDEX`, `find-*.idx`: `docs/handoff.md` 62, `docs/tools-review.md` 68 (a dated review: leave
its text, add one line at the top saying the old set was removed by this plan, **opinion**), `docs/tools.md` 10, `docs/code-index-lifecycle.md` 8,
`docs/katana-notes.md` 7, `docs/uioe-code-audit.md` 7, `README.md` 5 (line 148 says the 8 old names are "still available by name": delete
that row), `docs/code-intelligence.md` 5, `docs/curriculum.md` 4, `docs/teacher-protocol.md` 4, `docs/new-models.md` 3, `desktop-qt/README.md` 3,
`docs/call-formats.md`, `docs/console.md`, `docs/mcp.md`, `docs/plan.md` (T7 row), `docs/specialization.md`, `docs/training-data.md` 1 each.
`docs/uioe.md:900` says `memory.recall` uses `tool_recall`/`mem_read`: see 2.5 (wrong today). The `CLAUDE.md` "Prática" section is unaffected.

### 4.5 Criterion of done

1. `build.sh` without `-Wno-unused`: **0 warnings of unused function, variable or label** (3.7 list empty), and no new warning class.
2. `grep -rnw 'run_command\|read_file\|write_file\|edit_file\|list_dir\|find_file\|search_files\|web_search\|web_fetch\|read_document\|ask_user\|terminal_screen\|install_package\|download_file\|read_log\|KATANA_TOOLS\|KATANA_FIND_INDEX\|KATANA_FIND_REFS' src desktop-qt plugins tools data docs README.md` is empty, except historical sentences in `docs/handoff.md` and
   `docs/tools-review.md` that are dated (the wave lists them).
3. `grep -rn -i 'legado\|legacy\|compat\|antigo' src/` reviewed line by line, only the "not legacy" list of 3.6 remains, each justified.
4. `grep -rn 'find-.*\.idx\|fd_update\|fg_build\|fd_cache_path\|index_old_local_store\|g_code\|ct_set2' src` empty.
5. The table tools pass their tests: `tools/tooltest/*.sh` (rewritten per 2.6) and `tools/traintest/run.sh` (rewritten, line 31 gone), run by the wave (not run here).
6. `katana curriculum` generates v8 from zero with no scenario naming an old tool (`cu_is_old_tool` gone, `XF_OLD` gone or renamed) and `katana curriculum check` passes.
7. Repository root: no `attic/`, `compare.sh`, `desktop/`; `git status --ignored` in the root shows no `ia.*`, `modelo.bin`, `katana.exe/.obj`.
8. `README.md` and `docs/` describe one tool set.

### 4.6 What not to remove (looks old, is not)

`mcpx_legacy_*` (protocol transport), `encode_legacy`/`PRE_LEGACY` (tokenizer family), `KATANA_CACHE_DIR`, `build.bat`,
`katana-desktop` symlink, `.github/workflows/pacotes.yml`, `%s_legacy` in `scenario_tasks.c` (a generated name), `.kspec`
file layouts (`train.c:1868-2006`), `tool_ask_user`/`tool_delegate`/`tool_howto`/`tool_run_command`/`tool_web_*` (shared engines, 2.2), `todo_ui`/`todo_count`,
`place_note`, the `CHOICE` marker of the desktop, `fd_skip_rel` and the `fd_*` ranking functions (the unified Find uses them), `KATANA_AUTOINDEX`,
`KATANA_INDEX_WATCH`, `KATANA_TOOL_DECL`, `KATANA_CALL_GRAMMAR`, `KATANA_COMPACT`.

### 4.7 Final step, instructions to the owner only (nothing here is executed by the wave)

After the removal is merged, the owner may delete by hand, on this machine:

- `~/.katana/curriculum-backup`: **it does not exist** here (the listing of `~/.katana` has: `agente.log archive askpass.sh bench cache coder context dashboard desktop jobs katana.before-2026-11-22 katana.cfg knowledge mail mcp-trust mcp.log memoria.txt models run sessions specs training treino undo versions`); if it exists on another machine, it goes.
- Data katana made in `~/.katana` (sizes measured with `du -sh`): `archive` 936 MB, `cache` 804 MB (this one is regenerated; `web/` and `pfx-*.kv`), `training` 271 MB, `bench` 28 MB,
  `context` 26 MB, `coder` 14 MB, `knowledge` 9.0 MB, `versions` 5.2 MB, `katana.before-2026-11-22` 4.5 MB (an old binary), `desktop` 4.4 MB (sessions),
  `agente.log` 2.0 MB, `sessions` 736 KB, `undo` 664 KB, `jobs` 288 KB, `memoria.txt`, `treino`. Keep `models`, `katana.cfg`, `skills` if it exists and anything holding credentials (`mcp-trust`, `askpass.sh`: not reviewed).
- In the repository root: `ia.exe`, `ia.obj`, `modelo.bin`, `katana.exe`, `katana.obj`, `katana.cfg`, `.katana/`, and `desktop/` (empty).
- `<project>/.katana/cache/find-*.idx` in any folder where Find or Rename ran with the old index.

## 5. Open points for the owner (not blocking)

1. Memory (2.5): option A (engine and `katana memory` stay, no model-facing `remember`/`recall`) or B (two table tools)? The stated reason (the UIOE uses `memory_search`) is not in the code today.
2. `terminal`/`terminal_screen` (stateful shell) and `find_file` (machine-wide search) are the two losses with a real use in the old log (73 and 5 calls; the wrong-path failure of 2026-11-23): confirm they go.
3. `data/curriculum/v6` (23 MB in git) and `compare.sh`: delete? (opinion: yes)
4. `katana code`: delete (opinion: yes) or merge?
5. Portuguese names (`modelos`, `calibrar`, `--sessao`, `KATANA_RETOMAR*`, `KATANA_SESS_*`, `KATANA_TEMA`, `KATANA_FONTE`): rename in the same wave?
6. The MCP client's old SSE transport (`mcpx_legacy_*`, about 20 lines plus a test `tools/tooltest/ssefake.c`): kept by this plan as protocol support.
