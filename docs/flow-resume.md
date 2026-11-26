# Flow resume: durable work state, reconciliation and re-entry

Written 2026-11-26. **Built the same day (section 12 lists where the build differs from this text). Compiled; nothing was run with a model and nothing was measured.**
Every number is a design value. It is the build specification for the work described in `docs/flows.md` section 7
item 6 ("flow state saved with the session") and it extends that item to every way the agent comes back to a job.
Line numbers refer to the tree at commit `f99b8b5`.

Read first: `docs/flows.md` (playbooks, the `next` line), `docs/research/beyond-rag.md` (sections 1, 3, 4, 7, 8).

## 1. The problem, in the code

The engine knows where a job stands only while the process lives. Each item below is a place where that knowledge
is dropped today.

| # | Way back | What is lost | Where |
|---|---|---|---|
| 1 | Session resumed (`/resume`, `--retomar`) | the whole flow state `g_fl` (stack, task, path, error, counters, seen set, call ring); the todo list `g_todo`; the facts notebook; plan mode | `sess_save` writes only messages, ids and heat (`src/agent/loop.c:212-244`); `sess_resume` restores only `hist` and `full` (`loop.c:247-308`); `g_todo` is a process global (`src/agent/todo.c:6`); `facts_reset` (`src/agent/facts.c:11`) |
| 2 | `/resume` inside a running process | worse than lost: `g_fl` of the previous conversation stays, because the handler calls `ct_session_reset()` but not `flow_reset()` | `loop.c:618` (compare `loop.c:674`) |
| 3 | Agent restarted or killed | all of 1, and every background task: `job_all_kill` at exit also deletes the output files (`src/agent/jobs.c:48`), the watchdog kills the groups on any death (`src/agent/reaper.c:1-8`) | the model's context still says "task 3 is running" |
| 4 | Agent reloads itself (`agent_regrow`, `execv`) | the job table `g_jobs` (process memory) while the commands survive (`reap_keep_exec`, `reaper.c:69`); the terminal of each task, because the master side is close-on-exec (`src/tools/bash.c:155`); task ids restart at 1 (`g_job_seq`) | `loop.c:28-70` does not check for running tasks before `execv` |
| 5 | Crash before the exit save | the model-written summary: only the exit path may summarise (`allow_sum`, `loop.c:1058`); the per-turn save keeps an old summary or none and masks tool results (`loop.c:154-170`) | the resumed context has the last two requests and no account of the work |
| 6 | Context compacted with a summary | names, numbers, what failed: the summary is written by the model; the engine adds the facts notebook and the todo list (`src/agent/context.c:581`, `:613`) but not the flow state, the attempts or the tasks | `context.c:523` |
| 7 | KV lost (`g_ctx_lost`: Render, sub-agent, probe) | nothing textual: the conversation is recomputed from `hist` (`loop.c:780-783`; set in `src/tools/render.c:67`, `src/agent/delegate.c:131`, `:226`, `src/agent/flowmem.c:135`). State is lost only if that recompute also summarises (item 6) | no separate mechanism is needed |
| 8 | User interrupts, or changes subject and returns | the flow of the interrupted job: a new intent sets `g_fl.depth = 0` (`src/agent/flow.c:472`, `:476`, `:480`) | nothing remembers the suspended job |
| 9 | A background task ends later | `g_fl.task` is one integer (`flow.c:215`): a second task is invisible; a task that ends while the user is away is noticed only if the next request happens to poll it (`flow.c:466`) | no line ever says "task N ended" unless it was the tracked one |
| 10 | What was tried and failed | repetition memory is hashes in process memory (`g_fl.ring`, `g_fl.seen`, `flow.c:220-222`; `rp_`, `rr_` per request, `loop.c:311`, `:423`) | after any of 1 to 6 the model repeats the failed call with no warning |

Evidence, `~/.katana/agente.log`, 2026-11-25 (also `docs/flows.md` section 1): `xtool setup` started and stopped
six times (lines 10284-10295), an image made and never shown (10307-10311), the same search for hours
(10171-10190). Those happened inside one process; every one of them becomes unbounded across a restart, because
the counters that eventually stop them are gone.

## 2. The idea

**The work state is the trunk; the conversation is a cache of it.** Today the conversation is the only record and
the engine's state is a by-product. This design inverts that:

1. One structure, `RzState`, holds what the job is, where it stands, what was won, what failed and what runs.
2. It is **event-sourced**: every change is one line appended to a file before it takes effect in memory
   (write-ahead). Live operation and recovery run the same function, `rz_apply`. A checkpoint is the same file
   rewritten as the shortest event list that reproduces the state. One format, one parser, one code path.
3. What is read back is a **hypothesis**. `rz_reconcile` checks every item against the world without the model and
   produces the state as it is now plus the list of what changed.
4. The model's context is a **projection** of the state at three sizes: the `next` line (each turn), the `resume`
   block (on re-entry), the capsule (inside a summary, or instead of old turns).
5. The model declares nothing. All transitions come from tool results, as in `flow.c`. No new tool, no new
   instruction in the system prompt.

What goes beyond the usual session restore: memory items carry what they were verified against and are re-verified
on return (beyond-rag 3.1); the state, not a model summary, anchors compaction and survives a crash; failed
attempts are an engine table that outlives the process; orientation after re-entry is scored from the calls the
model makes, with no judge.

## 3. Files, names and switches

New files (comments in Portuguese without accents, names in English):

- `src/agent/resume.c`: types, the event log, apply, load, checkpoint, lock, mirrors, reconciliation.
- `src/agent/resume_note.c`: the three projections, rehydration, the orientation score, `katana flow state`.

Both are included in `src/katana.c` after `agent/flowmem.c` (line 178) and before `agent/loop.c` (line 179).
Functions called from files included earlier (`flow.c`, `context.c`, `jobs.c`, `tools/table.c`) get a `static`
prototype in the caller's file, the pattern `flow.c:453-456` already uses. Prefix: `rz_` / `Rz` / `RZ_` (unused in
the tree).

| Variable | Default | Effect | Why off |
|---|---|---|---|
| `KATANA_RESUME=0` | on | turns the whole feature off: no file, no block | - |
| `KATANA_RESUME=strict` | off | a call that repeats a dead attempt from before the re-entry is refused before running | a wrong match would block a valid call |
| `KATANA_RESUME_MAX=N` | 1200 | byte cap of the `resume` block (clamped to 400..3000) | - |
| `KATANA_RESUME_IDLE_MIN=N` | 30 | minutes of silence after which the next request gets the short block | - |
| `KATANA_RESUME_REHYDRATE=1` | off | on session resume, old turns are replaced by the capsule | the model gets less text; not measured |
| `KATANA_RESUME_PROBE=1` | off | one logit probe after re-entry checks the model's next tool | runs the model; parks the context on disk |
| `KATANA_RESUME_WAKE=1` | off | a task that ends while the agent is idle starts a turn | a turn starts without the user typing |
| `KATANA_RESUME_RECALL=1` | off | a suspended job is restored when the user returns to it | a wrong match resurrects an old flow |
| `KATANA_RESUME_FILE=path` | unset | state file path when there is no session file (tests, `katana flow sim`) | test only |

Not in this design, by decision: **rollback of a failed attempt** (beyond-rag P1, rank 4). It makes the model's
context diverge from the visible transcript and stays an owner decision. This design only supplies what a rollback
would need: the attempt table is the source of the "epitaph" line (beyond-rag section 8, first risk). Nothing here
removes a message the user saw.

## 4. The durable state

### 4.1 Structures (`src/agent/resume.c`)

```c
#define RZ_VERSION 1
#define RZ_ATT 24            /* tentativas falhas */
#define RZ_EVID 16           /* evidencias ganhas (anel) */
#define RZ_FILE 24           /* arquivos tocados ou produzidos */
#define RZ_DEC 8             /* decisoes do usuario (anel) */
#define RZ_WORK 3            /* trabalhos suspensos */
enum { RZJ_RUN = 1, RZJ_WAIT, RZJ_DONE, RZJ_STOPPED, RZJ_LOST, RZJ_DETACHED };
enum { RZF_WRITE = 1, RZF_MADE };                     /* editado pelo agente; artefato produzido (imagem, arquivo a entregar) */
enum { RZR_NONE, RZR_SESSION, RZR_SUMMARY, RZR_INTERRUPT, RZR_IDLE, RZR_TASK, RZR_RECALL };

typedef struct { int id; long pid, agent_pid; long long start, t0; unsigned char status, told; int rc; long log_sz;
                 char cmd[200], log[1300], asked[240]; } RzJob;
typedef struct { char path[400]; uint64_t hash; long long size, mtime; unsigned char op, shown, gone, changed; int req; } RzFile;
typedef struct { uint64_t key, call; char tool[24], subj[80], cause[120], fix[120]; int n, req, epoch; long long t; unsigned char dead; } RzAttempt;
typedef struct { unsigned char kind; uint64_t key, hash; char text[160], path[400]; int req; unsigned char stale; } RzEvid;
typedef struct { char q[160], a[160]; int req; } RzDecision;
typedef struct { char goal[400]; FlowPos stk[FL_DEPTH]; int depth; long long t; } RzWork;
typedef struct {
    char sess[64], host[64], cwd[1024]; long agent_pid; long long t_event, t_clean;
    char goal[400], last_req[240]; int req;
    FlowPos stk[FL_DEPTH]; int depth, task, cycles, webcalls, badfetch;       /* espelho de g_fl */
    char path[400], err[170], errfile[330];
    char todo[3000]; int plan_mode, worktree;
    char open_tool[24], open_subj[120]; uint64_t open_sig;                    /* chamada iniciada e sem resultado */
    char ask[240]; int ask_task;                                              /* pergunta pendente ao usuario */
    RzJob job[JOB_N]; RzFile file[RZ_FILE]; int nfile;
    RzAttempt att[RZ_ATT]; int natt; RzEvid ev[RZ_EVID]; int nev, ev_w; RzDecision dec[RZ_DEC]; int ndec, dec_w;
    RzWork work[RZ_WORK]; int nwork;
    int epoch;                                                                /* sobe a cada reentrada */
} RzState;
static struct {
    RzState s; FILE *f; int lockfd, readonly; long seq, since_ckpt; char file[1100];
    int reason;                                                               /* reentrada pendente (RZR_*) */
    int grace, score_left, sc_on, sc_dead, sc_dup, sc_reason; char delta[6][200]; int ndelta;
} g_rz;
```

`JOB_N` is 8 (`jobs.c:7`); on Windows, where `jobs.c` compiles to nothing, `resume.c` defines `RZ_NJOB 8` and uses
it instead, and the job functions are empty.

### 4.2 What each field means and who writes it

| Item | Meaning | Source (never the model's word) |
|---|---|---|
| goal | the job in the user's words | the request text that opened the bottom flow of the stack or the first request that led to a tool call when no flow matched; replaced only when a request classifies as a different flow |
| flow stack, step | `g_fl.stk`, plus `task`, `cycles`, `webcalls`, `badfetch`, `path`, `err`, `errfile` | mirror of `g_fl` after `flow_request` and `flow_after` |
| plan | `g_todo` | mirror after every tool result |
| evidence won | one line per step that gained (the rule of `fl_gain`, `flow.c:441`) | tool result |
| failed attempts | (tool, subject, error signature) -> cause, count, last failing call signature, the call that later worked | tool result; key built with `fm_subject` and `fm_errsig` (`flowmem.c:15`, `:33`) |
| background tasks | id, pid, process start time, owner pid, command, output file, status, exit code, output size last seen, question waiting, whether the model was told of the end | mirror of `g_jobs` |
| files | path (absolute), hash, size, mtime, edited or produced, shown to the user | `Write`/`Edit`/`NotebookEdit` results; `g_fl.path`; `Render` and `SendUserFile` results |
| user decisions | question and answer | `AskUserQuestion` result ("the user answered: ...", `src/agent/ask.c:49`) |
| pending question | what a task is waiting for | `Job.asked_label` (`jobs.c:14`) |
| open call | a call that started and has no result yet | written before dispatch, cleared by the result |

### 4.3 On disk

Path: the session path with `.ksess` removed and `.kstate` appended
(`~/.katana/sessions/<folder hash>/<id>.kstate`, next to the session, `src/app/launch.c:240`). Without a session
path the state lives in memory only, unless `KATANA_RESUME_FILE` is set. File mode 0600.

Format: text, one event per line. First line is exactly `katana-state\t1`. A file whose first line differs is
renamed to `<file>.bad` and the state starts empty: **no migration** (project rule); a new version rebuilds.

    <seq> TAB <epoch seconds> TAB <kind> TAB <field> ... TAB <crc>

Fields escape `\` as `\\`, TAB as `\t`, newline as `\n`; other bytes under 0x20 become a space. `crc` is the low
32 bits (8 hex digits) of FNV-1a 64 (`fl_hash`, `flow.c:228`) over the line up to, not including, the last TAB.
Flow and state are written by **name** (`image`, `show`), not by number: a reordered table cannot corrupt a saved
state, and a name that no longer exists drops that stack entry.

| Kind | Fields | Effect in `rz_apply` |
|---|---|---|
| `H` | session id, agent pid, host, cwd | header; a different pid or host marks "new process" |
| `U` | request number, request text (240 bytes) | `last_req`, `req` |
| `G` | goal text | `goal` |
| `F` | depth, `flow:state` x depth, task, cycles, webcalls, badfetch | the stack mirror |
| `X` | `path` or `err` or `errfile`, text | the flow variable |
| `B` | tool, subject, call signature | open call set |
| `E` | tool, ok (0/1), call signature, result hash, event name, gain (0/1) | open call cleared |
| `A` | key, call signature, tool, subject, cause, count, dead (0/1), fix | attempt inserted or updated |
| `W` | kind (`read`, `cmd`, `web`, `plan`, `sub`, `user`), key, text, path, file hash | evidence appended (ring) |
| `J` | id, pid, start, agent pid, t0, status name, rc, log size, told, command, log path, question | job inserted or updated by (agent pid, id) |
| `P` | path, op, hash, size, mtime, shown, request | file inserted or updated by path |
| `D` | question, answer | decision appended (ring) |
| `Q` | task id, question text (empty clears) | pending question |
| `T` | todo text | plan |
| `M` | plan mode (0/1), worktree (0/1) | modes |
| `S` | up to 32 hashes | `g_fl.seen` (checkpoint only) |
| `R` | `sig:res:n:req` x k | `g_fl.ring` (checkpoint only) |
| `O` | slot, goal, depth, `flow:state` x depth, time | suspended job |
| `C` | history length | clean point: the model answered the user; `t_clean` |
| `N` | reason name, bytes of the block | re-entry happened; `epoch++` |

An unknown kind inside version 1 is skipped. A line whose crc fails ends the replay: it and everything after it are
ignored (a torn tail after a crash).

Writing (`rz_emit`): format the line, `fputs` to the append stream, `fflush`, then `rz_apply`. `fsync` only on a `B`
of a tool that is not read-only (`ct_readonly(name) == 0`, `tools/table.c:199`) and at a checkpoint: a process
kill never loses a flushed line; a power cut can lose only read-only bookkeeping.

Checkpoint (`rz_checkpoint`): write `<file>.tmp` with the header line and the shortest event list for the current
state (`H`, `G`, `U`, `F`, `X` x3, `T`, `M`, `Q`, one `J` per job, `P` per file, `A` per attempt, `W` per evidence,
`D` per decision, `O` per suspended job, `S`, `R`, `B` if a call is open, `C`), `fsync`, `rename`, reopen for
append. Done on a `C` event when 50 events passed since the last checkpoint, at 200 events in any case, and on
close.

Size: the caps of 4.1 bound a checkpoint to about 30 KB. `sess_purge` (`loop.c:109-141`) removes `<base>.kstate`,
`.kstate.lock`, `.kstate.bad` with the session, and task output files older than the session age limit.

### 4.4 Secrets and privacy

Every text field goes through `rz_clean` before it is formatted: `secret_mask` (`src/agent/tools.c:228`), one line,
byte cap, UTF-8 safe cut (`fc_cut`, `facts.c:12`). Additional rules:

- `SendInput`: the subject stored is `task N`, never the `text` argument (the rule of `job_redact`, `jobs.c:71`).
- Tool **results are never stored**: only their hash, and for a failure the error signature of `fm_errsig`
  (lower-case first error line, digits replaced), masked.
- A decision whose question contains `password`, `senha`, `token`, `secret`, `passphrase`, `key`, `chave` or `pin`
  stores the answer as `(given)`. Secrets typed through `SendInput(ask_user=true)` never reach the model or this
  file.
- The request text (goal, 400 bytes) is stored. The session file beside it already stores the whole conversation;
  the state file is purged with it. `KATANA_RESUME=0` writes nothing.

## 5. Recording: the reasoning loop

Each turn is `orient -> act -> record -> verify -> next`. The model does one thing, the call; the engine does the
rest.

| Phase | Engine, alone | Asked of the model |
|---|---|---|
| orient | at the request: reconcile, pick the projection, write it at the tail of the user message | read it |
| act | `rz_before`: write `B` before dispatch; match the call against dead attempts and running tasks | one tool call |
| record | `rz_after`: `E`; attempt, evidence, file, decision; mirrors of flow, tasks, plan, modes | nothing |
| verify | the checks `flow_after` already does (file exists, task alive); task ends found by the task mirror | nothing |
| next | `fl_line` (unchanged) | follow the line |

Rules of `rz_after(name, js, end, result, failed, ev, gain)`:

1. `E` always.
2. Failed: key = hash(tool, subject, error signature). Insert or `n++`; `call = g_fl.cur_sig`; `cause` = the
   signature; `dead = n >= 2`; `epoch = g_rz.s.epoch`. Table full: replace the oldest entry that is not dead, else
   the oldest.
3. Succeeded and an attempt with the same (tool, subject) exists: those attempts get `dead = 0` and
   `fix` = the call in one line (shell command, or the argument shape as `fm_shape` gives it, `flowmem.c:235`).
4. Succeeded with gain: one `W`. Text by event: `FE_READ` "read PATH" or "searched PATTERN" (with path and file
   hash when `file_path` is present); `FE_CMD_OK` "`COMMAND` passed"; `FE_FETCH_OK` "read URL"; `FE_SEARCH_OK`
   "searched QUERY"; plan count rose: "plan step K done: SUBJECT"; `FE_SUB_OK` "sub-agent answered: PROMPT";
   `FE_ASKED`: a `D` instead.
5. `FE_WRITE`: `P` with `RZF_WRITE`, hash, size and mtime read after the write. `FE_RENDER_OK`: `P` `RZF_MADE`,
   shown. `FE_IMG_CMD`: `P` `RZF_MADE` for `g_fl.path`, not shown. `FE_SENT_FILE`: that file's `shown = 1`.
6. `rz_sync_all()`: compare `g_fl` (stack and variables), `g_jobs`, `g_todo`, `g_plan_mode`, `g_wt_active` with
   their mirrors in `g_rz.s`; emit `F`, `X`, `J`, `T`, `M`, `Q` only for what differs. No hook is needed inside
   `bash.c`, `jobs.c`, `tasks.c`: the mirrors are compared after every result and at every request.

Only the main agent records: every entry point returns at once when `g_ct_depth > 1` (the test `facts.c:98` uses),
so a sub-agent's calls never touch the state.

## 6. Reconciliation

`rz_reconcile()` runs at every request (`rz_request_begin`) and once right after a load. It uses `stat`, `kill(pid,
0)`, `/proc`, file hashes and the live tables. It never runs the model. Output: the corrected state (through
ordinary events, so the correction is durable) and up to six delta sentences in `g_rz.delta`, in the priority
order below.

Environment first:

- `new_proc` = saved agent pid differs from `getpid()`, or saved host differs from `gethostname`.
- `moved` = saved cwd differs from `getcwd`. `foreign` = the saved cwd does not exist, or the host differs.
- Paths are stored absolute, so `moved` alone changes nothing in the checks. Delta: "the working folder was A when
  this work stopped; it is B now".
- `foreign`: files and tasks are not checked. Every file gets `changed = 1`, every task `RZJ_LOST`. Delta: "this
  work was saved on another machine or folder: file and task state below is not verified".

| Item | Check | Result |
|---|---|---|
| open call (`B` without `E`) | exists after a load | delta: "the call TOOL SUBJECT was started and never returned: katana stopped during it. Its effect is unknown; check before you repeat it". For a write tool the file is then checked like any file. Cleared. |
| task, same process, in `g_jobs` | `job_poll` | status and rc from the table |
| task, same pid, not in `g_jobs` (after `agent_regrow`) | `kill(pid, 0)` and the start time in `/proc/<pid>/stat` field 22 equal to the saved one | alive: `RZJ_DETACHED`, re-adopted into a free `g_jobs` slot with the same id and `mfd = 0` so `Read`, `Monitor` and `TaskStop` keep working; delta "task N is still running but its terminal was lost: it can be read and stopped, not answered". Dead: `RZJ_LOST` |
| task, other process | same liveness test | alive (it left the group on purpose): `RZJ_DETACHED`, not adopted, delta names the pid and the output file. Dead: `RZJ_LOST`, delta "task N (`CMD`) ended when katana restarted; exit code unknown" |
| any task not running | output file exists, size against `log_sz` | "its output (K new bytes) is in LOG" or "its output file is gone" |
| task ids | after a load in a new process | `g_job_seq = max(saved id)`, so a new task never reuses a number the model has in its context |
| flow `terminal` on the stack | its task is not running | popped (the rule of `flow.c:466`), `g_fl.task = 0` |
| flow `image` at `wait` | task ended | file exists: to `deliver`; missing: to `failed`, delta "the image command ended but PATH was not written" |
| flow `image` at `deliver` or `show` | `g_fl.path` exists | missing: to `failed`, same delta |
| flow `planmode` at `explore` | new process | `g_plan_mode = 1`, `plan_ui()`: read-only is a safety property and is restored, not dropped |
| flow `worktree` | new process | popped; delta "the worktree session did not survive the restart" |
| file, `RZF_WRITE` | `stat`; if size or mtime differ, hash (files over 8 MB: size and mtime only) | missing: `gone`, delta "PATH, which you edited, no longer exists". Hash differs: `changed`, delta "PATH changed outside this session after your edit: read it again before editing". Equal: nothing |
| file, `RZF_MADE` | exists | missing: delta. Present and `shown == 0`: kept for the block line `done:` as "made, not shown yet" |
| evidence with a path | file hash against the saved one | differs or missing: `stale = 1`; a stale item is not shown as done |
| attempt | age and environment | older than 24 h, or `moved`, or `foreign`: `dead = 0` (it becomes "failed before", not "do not repeat"). The user saying something is already done (`says_already_done`, `loop.c:461`) does the same for all attempts |
| plan | `g_todo` empty and the saved list is not | restored: `g_todo`, `ct_task_sync()`, `todo_ui()` |

Delta priority: open call, tasks, artefacts, edited files, folder, plan and modes. Items past the sixth are counted
("and N more changes").

## 7. Re-entry

### 7.1 When

A re-entry is an event after which the model can no longer be assumed to know where the job stands. `g_rz.reason`
is set by:

| Reason | Set where | Block form |
|---|---|---|
| `RZR_SESSION` | after `rz_load` on `/resume`, `--retomar`, `agent_regrow` | full |
| `RZR_SUMMARY` | a compaction that wrote a summary | capsule inside the summary (7.4); the next `next` line is forced |
| `RZR_INTERRUPT` | the request ended at `if (g_stop) break` (`loop.c:855`) | short |
| `RZR_IDLE` | at a request, `now - t_clean` over the idle limit | short |
| `RZR_TASK` | at a request or a tool result, a task ended and `told == 0` | one clause |
| `RZR_RECALL` | a suspended job was restored (off by default) | full |

KV loss alone (`g_ctx_lost`) is not a re-entry: the text is intact.

**No work, no block.** A block is written only if at least one holds: the stack is not empty; the plan has open
steps; a task has `told == 0`; a delta exists; a call was open. A session that was only conversation resumes
exactly as today.

**Grace.** The first request after `RZR_SESSION` must not end the open job just because it is not a "continue"
word ("and the image?" is not in the `go` list of `flow.c:462`). `g_rz.grace = 1` after a load; `flow_request`
skips its ending branch (`flow.c:478-481`) while `rz_grace()` is true; a request that classifies as a different
flow still replaces the job. Cleared at the end of that request.

### 7.2 The block

New note `KN_RESUME` in `src/agent/knote.c` (class ORDER, site REQUEST, no budget flag), listed as F4 in
`docs/katana-notes.md` section 10:

    #define KNF_RESUME "\n[katana: resume (%s)\n%s]"

First argument, the reason: `session reopened after 2h10m`, `you were interrupted`, `back after 45m`,
`back to earlier work`. Second argument, the body: fixed labels in a fixed order, one line each, a line is absent
when it has nothing to say.

    [katana: resume (session reopened after 2h10m)
    goal: set up xtool and build the app for the phone
    was: terminal 1/3 answer, over debug 2/6 locate; plan step 3 of 5: log in to xtool
    done: `xtool --version` passed; read /home/u/app/Package.swift; plan step 2 done: install the SDK
    changed: task 2 (`xtool setup`) ended when katana restarted; exit code unknown; its output (412 new bytes) is in /home/u/.katana/jobs/bash-615605-17.log
    running: none
    do not repeat: `xtool setup` then TaskStop -> the command waits for an answer, stopping it loses the question (x5); WebFetch xtool.sh/anisette -> page gave no text (x6)
    asked: "Which account e-mail?" -> the user answered: the work one
    ]
    [katana: next debug 2/6 locate: ...]

| Line | Content | Cap | Dropped when over the byte cap |
|---|---|---|---|
| `goal:` | `goal` | 200 | never |
| `was:` | stack from top to bottom as `flow i/n state`, joined by ", over "; then the open plan step | 240 | never |
| `done:` | newest three evidence items that are not stale; then `made, not shown yet: PATH` | 300 | first (oldest item first) |
| `changed:` | the deltas of section 6, joined by "; " | 400 | after the third delta |
| `running:` | each task running or waiting: `task N (`CMD`) running, output in LOG` / `waits at "QUESTION"`; `none` when tasks existed and none runs | 240 | never |
| `do not repeat:` | dead attempts, most repeated first: `SUBJECT -> CAUSE (xN)`; at most three | 300 | third, then second |
| `asked:` | the pending question, else the last decision | 200 | second |

The short form (`RZR_INTERRUPT`, `RZR_IDLE`) has only `was:`, `changed:` and `running:` and a 500-byte cap. The
task form (`RZR_TASK`) is not a block: it is one clause in front of the `next` line (`fl_line`'s `pre`): "task N
(`CMD`) ended with exit code R; its output is in LOG. ".

**Composition with `next`.** The block never states the next step when a flow is open: the existing line follows it
and stays the last thing the model reads (the rule of `docs/flows.md` section 4). With no flow, the block ends with
its own line `next: STEP`, where STEP is the open plan step, else "tell the user in two sentences where this work
stands and ask how to go on". So there is always exactly one next step, and it is last.

### 7.3 Position and the prefix cache

The block is appended to the **end of the user message** of the request that follows the re-entry, in front of the
flow line, at `loop.c:732-736`. Nothing already in the context is rewritten: the resumed conversation keeps the
token ids `sess_resume` loaded (`loop.c:274-299`), so the KV file found by `pfx_key` (`loop.c:288`) is still
valid, the system prompt does not grow, no tool declaration moves. A stale block is never edited; the next one
supersedes it. Cost: at most `KATANA_RESUME_MAX` bytes (about 300 tokens) once per re-entry.

### 7.4 Compaction: the state anchors, the summary narrates

`agent_compact` rebuilds the context from the start, so text placed there costs no extra recompute. The capsule
(projection L3) is appended to the summary message after the facts notebook and the todo list, in the three places
a summary message is built: `context.c:581`, `context.c:613`, `loop.c:158`.

    #define KNF_RESUME_CAPSULE "\n\n[katana: state of the work, kept by the engine; where it differs from the summary above, this is right]\n"

followed by the same body lines with a 2500-byte cap and no `changed:` line. The capsule holds only the **stable**
part of the state (goal, plan position, evidence, files, attempts, decisions): volatile facts (elapsed time,
deltas, live tasks) go in the tail block. Stable-first ordering keeps the recomputed prefix reusable (beyond-rag
4.2 d).

`context.c` sets `g_cp_summed = 1` beside each `summ = 1` in `agent_compact`. After a compaction returns, the loop
(`loop.c:794-802` and `:638-641`) calls `rz_after_compact()`: if `g_cp_summed`, clear it, set
`g_fl.emitted_st = -1` (the `fresh` test of `flow.c:588` then re-emits the `next` line on the following result) and
write `N summary`. The summary instruction text (`KNF_COMPACT`) does not change.

### 7.5 Rehydration (off: `KATANA_RESUME_REHYDRATE=1`)

Today a resumed context is "old summary if one exists + the last two requests, tool results masked"
(`sess_compact`, `loop.c:143-173`). After a crash there is no summary at all. Rehydration builds the context from
the state instead of from old turns, with no model call:

`rz_rehydrate(m, hist, sys)`, called in `sess_resume` right after `hist` is filled (`loop.c:272`) and before the
ids are encoded (`loop.c:274`). It applies only when the state has work (7.1) and the messages before the last
user request hold more than 600 tokens. It replaces everything before the last user request by two messages: user
= `KNF_RESUME_CAPSULE` body (the capsule), assistant = `Understood. I continue from this state.`; the last request
and what followed stay whole, masked with `ah_mask` as `loop.c:165` does if over 3000 tokens. The full transcript
(`full`, the visible conversation) is untouched: this is the same class of change as compaction, not a rollback.
Because the capsule is deterministic and stable, the second resume of the same session finds its KV on disk under
the same key (`pfx_save`, `loop.c:298`).

## 8. Using the state after re-entry

### 8.1 Dead attempts across the boundary

`rz_before(name, js, end)` runs inside `flow_before` after the call signature is computed (`flow.c:496-499`). If
`g_fl.dev` is still empty and the signature equals `call` of a dead attempt whose `epoch` is lower than the
current one, it writes the deviation: "you made this call before SESSION-OR-SUMMARY and it failed N times: CAUSE.
It will fail the same way; use what you have or tell the user what blocks you. ". It is soft (`dev_soft = 1`)
unless `KATANA_RESUME=strict`. Attempts of the current epoch are left to the existing controls (`rp_`, the ring),
so no call gets two notes. Second rule, once per epoch and subject: three or more dead attempts share (tool,
subject) with the call: "N different calls around SUBJECT failed before: CAUSE1; CAUSE2. ".

### 8.2 All tasks, not one

`rz_job_clause(out, cap)` is called in `flow_after` after the memory note (`flow.c:580`): after `rz_sync_all`,
every task that reached `RZJ_DONE` or `RZJ_STOPPED` with `told == 0` and is not `g_fl.task` adds the task clause of
7.2 and gets `told = 1`. The tracked task keeps its flow transition (`FE_TASK_ENDED`), which also sets `told`.

### 8.3 Idle wake (off: `KATANA_RESUME_WAKE=1`)

`rz_wake_line(line, cap)` is called first in `ct_idle_line` (`tools/table.c:239`). When a task ended with
`told == 0` while the agent waits for input, it returns the line "[a background task ended; the user did not type
this]" and sets `RZR_TASK`, so the turn that starts carries the clause.

### 8.4 Suspended jobs (off: `KATANA_RESUME_RECALL=1`)

`flow_request` calls `rz_suspend()` before each of its three stack resets (`flow.c:472`, `:476`, `:480`) when the
job being dropped is not at an answer state: the goal and stack go to `work[]` (`O` event; oldest slot reused).
`rz_try_recall(folded_request)` runs when the request classifies as no flow and the stack is empty: a suspended
job is restored if the request shares at least half of the goal's words of four letters or more, or is a `go`
word and exactly one job is suspended. Restore sets the stack, the goal and `RZR_RECALL`.

### 8.5 `agent_regrow` and running tasks

`agent_regrow` (`loop.c:28-70`) gets one more early return: if any `g_jobs` entry is running, do not reload. A
reload would hang up every interactive task (the terminal master is close-on-exec). With no task running the
reload proceeds and the state file carries the job over like any restart.

## 9. Checking that the model re-oriented (designed, not run)

No measurement is taken now. The engine records what a later comparison needs.

**Passive score, always on, no model.** When a block is written, `g_rz.score_left = 3`. Each of the next three
calls is classified in `rz_after`: on the flow (the tool is in the state's `expect` list, `flow.c:29`, or the list
is empty); a dead attempt from before the re-entry (8.1 matched); a duplicate of a running task
(`fl_job_same_cmd`, `flow.c:310`). After the third call, or when the request ends earlier, one line goes to
`~/.katana/flow.log` through `fl_log`:

    resume	score	<reason>	calls=3	on_flow=2	dead=0	dup=0	answered=0

and `resume enter <reason> <bytes>` when the block is written. `katana flow resume-report [--days N]` sums these
lines per reason. This is the same idea as the stall detector: orientation is read from behaviour, not asked.

**Active probe (off: `KATANA_RESUME_PROBE=1`).** In the first `flow_after` after a re-entry (where `g_agent_m` and
`g_agent_pos` are valid, `loop.c:999`), `rz_probe()` calls `fm_probe_rank` (`flowmem.c:102`): question "Which tool
comes next?", candidates = up to four tools of the state's `expect` list (or "answer the user"), plus up to three
distractors (the tools of the dead attempts and of the off list). Top candidate expected: `oriented`; a
distractor: `drift`, and a clause naming the state's step goes in front of that result's `next` line; margin under
the probe's threshold: `nosignal`, nothing changes. Logged as `resume probe <verdict>`. It uses the disk park of
the probe as built and costs what `docs/flows.md` section 4 states.

**The comparison to run when measuring returns** (Qwen3.6-35B-A3B only): scripted jobs cut at a fixed point by a
restart, by `/compact`, and by an interruption; `KATANA_RESUME=0` against the default, alternated; read from the
log: calls until the first on-flow call, dead-attempt repeats, duplicate task starts, whether the request reached
an answer. Falsified if dead-attempt repeats after re-entry are not lower with the block than without it.

Without a model, `katana flow sim` (`flow.c:651`) gets one more script line, `R` (restart): checkpoint, clear
`g_fl` and `g_rz.s`, load, reconcile, set `RZR_SESSION`; the next `U` line prints the block. `katana flow state
[file]` prints a state file as the block and as the capsule.

## 10. Edge cases

| Case | Behaviour |
|---|---|
| File header wrong or version differs | renamed to `.bad`, state starts empty, one dim line on stderr. No migration |
| Torn or corrupt line | replay stops there; what came before is used; the next checkpoint rewrites the file |
| Flow or state name unknown | that stack entry and those above it are dropped |
| Same session in two windows | `flock(LOCK_EX | LOCK_NB)` on `<file>.lock`, held while the session is open, with `FD_CLOEXEC` so `agent_regrow` releases and retakes it. The loser loads the state, reconciles, shows the block, and writes nothing (`readonly = 1`); stderr says so. Windows: no lock |
| Resumed in another folder or machine | section 6, `moved` and `foreign` |
| Pid reused by another program | process start time compared; on systems without `/proc`, a task of another process is always `RZJ_LOST` |
| Secret in the state | section 4.4; results are never stored |
| Session without a file | state in memory; the first `/session` path triggers a checkpoint to the new file |
| `/new` | `rz_close()`: checkpoint, unlock, zero memory (beside `flow_reset`, `loop.c:674`) |
| Switch to another session | `rz_close()` then `rz_open(new path)` |
| Sub-agent | records nothing (`g_ct_depth > 1`); its result is one `W` in the parent |
| Task writes while the state is read | tasks write only their own output files; the state file is written only by the agent's thread, never from a signal handler |
| Disk full or write error | the stream is closed, `readonly = 1`, the agent goes on without persistence |
| Gemma 4 and harmony formats | not hooked, like the flows (`cml` only) |
| Size | caps of 4.1; rings overwrite the oldest; the block and the capsule are capped in bytes |

## 11. Build plan

Rules for every step: read the cited code first; edit `src/` with Python replacement scripts written to a file,
with `assert` on the match count (`CLAUDE.md`); compile with `sh build.sh` on Linux
(`cmd //c "cd /d C:\source\katana && .\build.bat"` on Windows); the criterion of done is **it compiles with no new
warning**; no test and no measurement is run; report what was built and that nothing was measured; one commit per
step (the owner pushes). Steps 11 to 14 are off by default behind their variable.

1. **State and log, no hooks.** Create `src/agent/resume.c`: the types of 4.1; `rz_on()`; `rz_path()`;
   `rz_clean()`; field escape and unescape; `rz_emit(kind, fmt, ...)`; `rz_apply(kind, fields, n)`; `rz_load()`
   (header check, crc, replay); `rz_checkpoint()`; `rz_open(sess_path)` (lock, load); `rz_close()`. Add the include
   in `src/katana.c` after line 178. Done: compiles; nothing calls it yet (mark unused entry points
   `__attribute__((unused))` until step 2).
2. **Lifecycle.** `loop.c`: in the `/session` and `/resume` handler (`:611-631`) call `rz_close()` before the
   switch, add the missing `flow_reset()` beside `ct_session_reset()` (`:618`), call `rz_open(pa)` before
   `sess_resume` and set `g_rz.reason = RZR_SESSION` when it loaded work; at the reset (`:674`) `rz_close()`;
   after the per-turn `sess_save` (`:850`) `rz_emit` `C` and the checkpoint rule; at exit (`:1058`)
   `rz_close()`; in `sess_purge` (`:109-141`) remove the `.kstate*` siblings. Done: compiles.
3. **Flow mirror and goal.** `resume.c`: `rz_sync_flow()`, `rz_restore_flow()` (writes the mirror into `g_fl` by
   name, also `S` and `R` into `g_fl.seen` and `g_fl.ring`), `rz_request_begin(ureq)` (`U`, goal rule of 4.2).
   `flow.c`: prototypes at `:453`; call `rz_sync_flow()` at the end of `flow_request` (`:488`) and of `flow_after`
   (every return after `:537`; simplest: rename the body to `flow_after_body` and wrap it). `loop.c`: call
   `rz_request_begin(ureq)` before `flow_request` (`:735`). `rz_load` ends with `rz_restore_flow()`. Done:
   compiles.
4. **Calls.** `resume.c`: `rz_before()`, `rz_after()` with rules 1 to 5 of section 5 (attempts, evidence, files,
   decisions). `loop.c`: `rz_before` just before `agent_tool` (`:951`), `rz_after` just after `flow_after`
   (`:999`), with the event and gain: make `flow_after` store them in two globals `g_fl_last_ev`, `g_fl_last_gain`.
   Done: compiles.
5. **Mirrors of tasks, plan, modes.** `rz_sync_all()` (rule 6 of section 5) called at the end of `rz_after` and of
   `rz_request_begin`; restore of `g_todo` and `g_plan_mode` in `rz_load`. `jobs.c`: prototype
   `static int rz_keep_logs(void);` and skip the `remove` in `job_all_kill` (`:48`) when it returns 1.
   `sess_purge`: delete files in `~/.katana/jobs` older than the session age limit. Done: compiles.
6. **Reconciliation.** `rz_reconcile()` with the table of section 6 and `g_rz.delta`; called at the end of
   `rz_load` and in `rz_request_begin`. `agent_regrow` early return of 8.5. Done: compiles.
7. **The block.** `knote.c`: `KN_RESUME`, `KNF_RESUME`, table row (ORDER, REQUEST, no budget), and
   `KNF_RESUME_CAPSULE` as a macro. Create `src/agent/resume_note.c` (include after `resume.c`): `rz_has_work()`,
   `rz_body(out, cap, form)`, `rz_block(out, cap)`; `rz_grace()` and its test in `flow.c:478`. `loop.c:732-736`:
   after `flow_request`, if `g_rz.reason`, build the block and insert it in front of the flow line (same length
   checks as the existing `strcat`); write `N`, log `resume enter`, clear the reason. `flow_main`: command `state`
   and the `R` line of the simulator. Done: compiles; `katana flow sim` with an `R` line prints a block.
8. **Compaction.** `context.c`: `g_cp_summed`, prototype `static int rz_capsule(Buf *o);`, the call in the two
   summary builders (`:581`, `:613`); `loop.c:158` likewise; `rz_after_compact()` after both `agent_compact` calls.
   Done: compiles.
9. **In-process re-entries.** `RZR_INTERRUPT` at `loop.c:855`; `RZR_IDLE` in `rz_request_begin`; `rz_job_clause`
   in `flow_after` (`flow.c:580`); the short form in `rz_body`. Done: compiles.
10. **Dead attempts and score.** The `rz_before` deviation of 8.1 inside `flow_before` (`flow.c:499`), strict
    mode; the passive score of section 9 and `katana flow resume-report`. Done: compiles.
11. **Rehydration (off).** `rz_rehydrate()` and its call at `loop.c:272`. Done: compiles; default path unchanged.
12. **Probe (off).** `rz_probe()` in the first `flow_after` after a re-entry. Done: compiles.
13. **Wake and recall (off).** `rz_wake_line` in `ct_idle_line` (`tools/table.c:239`, prototype there);
    `rz_suspend`, `rz_try_recall` in `flow_request`. Done: compiles.
14. **Documents.** `docs/flows.md`: section 2.1 last sentence and section 7 item 6 point here; the switch table
    gains the rows of section 3. `docs/katana-notes.md` section 10: F4 `resume` and the capsule. `docs/handoff.md`:
    state, what is off, that nothing was measured. `desktop-qt` needs no change (no new marker).

Open after this plan: `done=N` checked against the evidence table (failure 12 of `docs/flows.md`: the `W` events
are the data it needs); attempts with a `fix` exported to `flow-lessons.tsv` at session end; the state shown in
the UIOE packet; rollback using the attempt table as the epitaph (owner decision); a curriculum family for the
`resume` block (the model has not been trained on it).

## 12. Build notes (2026-11-26)

All 14 steps of section 11 were built, one commit each; every step compiled with no new warning. No test, no measurement and no model run;
`katana flow sim` (with the new `R` line), `katana flow state` and `katana flow resume-report` were the only things executed, on scripted input.
Where the build differs from the text above:

- **Event fields.** `H` carries the epoch as a fifth field and `A` carries the epoch as a ninth (a checkpoint has no `N` events, so the epoch
  would otherwise be lost). `P` with op `0` removes a file from the state (used when an edited or made file is gone). `rz_emit` takes an array of
  fields; `rz_ev(kind, ...)` takes a NULL-terminated list.
- **Opening.** `rz_open(path, adopt)`: with `adopt` the in-memory state of a session that had no file yet moves into the new file when that file does not
  exist. The load itself (restore of flow, plan and plan mode, reconcile) runs inside `rz_open`.
- **Epoch.** A load also writes an `N session` event, even when no block is written, so a dead attempt from before a restart is recognised in a session
  that had no other work to show.
- **Dead-attempt note.** The `B` event is written by `rz_before` in `loop.c` just before `agent_tool`; the deviation of 8.1 is `rz_dead_check`, called from
  `flow_before` (it returns 1 only with `KATANA_RESUME=strict`).
- **Reasons.** A delta found at a request with no other reason gives the short block under the reason `idle` ("back after ..."); `RZR_TASK` is also rendered
  as the short block ("a background task ended"), not as a bare clause in front of the `next` line. Ended tasks that are not the tracked one are also announced
  by `rz_job_clause` in `flow_after`, as designed. A tracked task counts as told only when its flow has noted the end.
- **Passive score.** `answered` is 1 only when the request ended before the third call; it is 0 when the third call was reached. Calls are classified when
  they start (`rz_before`), so a refused call is not scored.
- **Probe.** It needs `KATANA_FLOW_RERANK=1` as well (it uses `fm_probe_rank`); without it the log says `probe unavailable`.
- **Windows.** The task mirror, the re-adoption of tasks and the wake are POSIX only (`jobs.c` compiles to nothing there); the file, flow and plan parts are portable.
- **Paths not hooked.** Like the flows, only the ChatML loop path records and projects; the harmony (`go`) and Gemma 4 result paths are not hooked.
- **Not verified.** That the file survives a real `kill -9`, that an `agent_regrow` reload re-adopts a running task, that the block reaches the model once
  per re-entry, and every claim about what the model does with it.
