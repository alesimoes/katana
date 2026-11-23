# The `[katana: ...]` notes: inventory, real use, effect and consolidation proposal

Status: analysis only, 2026-11-23. No code was changed, no model was run and nothing was measured for this document.
Everything marked "observed" was read from the owner's saved sessions (`~/.katana/desktop/sessoes/*.ksess`, 8 files,
2026-11-20 to 2026-11-23, read with a script over the raw text) or from the code and the curriculum files as they are in
this tree. Everything marked "opinion" or "estimate" is judgment or arithmetic, not a result. Token figures use the
rule of `docs/uioe.md` (4 bytes per token).

Why this document exists: the owner asked why the engine sends notes to the model and authorized consolidation:
keep the notes that break loops and have a clear effect (repeat and the cut at the third repeat; cited folder),
move the rest into the context block of the UIOE layer (`docs/uioe.md` 4.6), and cut what has no benefit.

## 0. Summary

1. There are 25 note kinds with the `[katana: ...]` prefix emitted from about 30 places (section 2), plus 6 sibling kinds
   that look the same to the model but do not carry the prefix (`[plan: step ...]`, `[hint: ...]`, `[web: ...]`,
   the undo note, the plan-mode note, `[symbol index created ...]`). The system prompt rule
   (`src/agent/prompt.c:171`) names only `[katana: ...]`, so the siblings are outside the rule the model was taught.
2. The counts the owner made are raw string counts. The `.ksess` files store the conversation text several times
   (snapshots of the context), so one event appears 1 to 3 times. Distinct events are lower: "this exact call already
   gave this exact result" 44 raw, 19 distinct (13 of them in one session); "linked files you have not read yet" 33
   raw, 11 distinct; "condensed note" 12 raw, 12 distinct contexts; "you mention" 6 raw, 2 distinct (section 3).
3. The notes that are orders and interrupt loops work when they fire early and fail when they fire late. Observed:
   the first "same result" note (count 2) was followed in 8 of 9 events; the same note at counts 3 to 12 was ignored
   in 10 of 10 events, in one session that repeated an identical `WebSearch` 12 times while saying "I will change
   the approach" before each repeat (section 3.1). The cut at the third repeat (`rr_check`) did not apply there
   because `rr_covered` (`loop.c:477`) does not include `WebSearch` or `WebFetch`.
4. The informational notes mostly have a weak or invisible effect, at a small cost. `[plan: step ...]`: the next
   call was `TodoWrite` in 3 of 18 events. "linked files": the next call touched a listed file in 1 of 11 events.
   "was removed to save context": 0 of 14 events followed the pointer (`Read <file>`); all 14 re-ran the web tool.
5. Cost in the 8 sessions: 100 distinct note contexts, 19,341 bytes in total, about 4,800 tokens, between 254 bytes
   and 7,924 bytes per session (63 to about 1,980 tokens). The notes are cheap as text. The expensive part is the
   behavior around them: the 12-time `WebSearch` loop re-ran a call that returned 3,775 bytes each time, about 38 KB
   or 9,500 tokens (estimate), and each repeat also cost one full model turn.
6. Recommendation (section 5): keep 16 note ids, move 3 (R10, Q2, Q3; Q1 later) into the UIOE block once it exists (U4/U5, `docs/uioe.md`
   section 11; the builder is not implemented, `KATANA_UIOE` is off and the loop calls nothing), fuse 7 into the
   tool results as plain text without the `[katana:` voice, cut 3 (R9, the R17 pointer, the T2 echo). Add `WebSearch`/`WebFetch` to the repeat cover.
7. The v7 curriculum has not been trained (`data/curriculum/v7/README.md`: "`train.sh` ... not run"). Changing the
   notes now costs no retraining; changing them after training makes the trained format stale (section 6).
8. One function `knote(kind, ...)` with a kind table, a budget per request and one escape routine replaces about 30
   emission sites and 3 escape mechanisms (section 7).

## 1. Method and its limits

* Inventory: `grep -rn '\[katana' src` (57 lines) read one by one, plus `grep` for the siblings (`[plan:`, `[hint:`,
  `[web:`, `[The user`, `[Plan mode`). Tool-result vs request attachment was read from the call sites in
  `src/agent/loop.c`.
* Training data: `gunzip -c data/curriculum/v7/v7-train.jsonl.gz | grep -c PATTERN`. One line is one conversation, so
  the count is the number of conversations that contain the text, not the number of occurrences. 1,042 conversations
  in train, 239 in validation (`v7-validation.jsonl.gz`, counted for the main kinds).
* Real use: a script over each `.ksess` (a binary header and the raw chat text with a few binary bytes between
  turns). An event is a note text plus the 200 characters before it, so snapshot copies of the same turn collapse to
  one event; a text-only key gives a lower bound. "Next call" is the first assistant output after the
  `</tool_response>` that carries the note. I read the first 10 events of each kind by hand and counted the rest by
  script. Judgments such as "obeyed" are mine.
* `~/.katana/agente.log` has 9,781 lines and only 5 lines containing `katana: `: it logs tool results, not the notes
  appended afterwards, so it cannot count notes. `~/.katana/api-sessions` does not exist. The sessions dated
  2026-11-20 and 21 were made by older builds (tool names `read_file`, `run_command`; the "your todo list" note appears
  there in the old wording), so they are weaker evidence for the current texts.
* What I could not determine: what the model would have done without each note (there is no control). All "effect"
  statements below are "what happened on the next call", not "what the note caused".

## 2. Inventory

Columns: id; text (shortened); `file:line` and emitting function; trigger; where it is attached; typical size in
characters (T = template alone, O = observed in the sessions); variable data; class (ORDER = tells the model what to
do, INFO = states a fact or offers a choice); v7 training (conversations in train / validation that contain it).
Env switches are the ones in the code.

### 2.1 Notes appended to a tool result

| id | text | where, function | trigger | size | variable data | class | v7 train/val |
|---|---|---|---|---|---|---|---|
| R1 | "you already made this exact call at step N of this request; the result is earlier in this conversation. Do not repeat it: pick a different place to look ..., or answer with what you have" | `loop.c:487`, `rr_check` | Read, Grep, Glob, Find, Symbols or a read-only MCP tool repeats a call that succeeded in this request (2nd time). The tool does not run: the note IS the result. `KATANA_REPEAT_NOTE=0` off | T 215, O 260 | step number | ORDER | 25 / 6 |
| R2 | "this is the Nth time you make this exact call (first at step M) and it will not run again ... You can still: Read another part ..., Grep ..., Glob ..., or stop looking and answer" | `loop.c:490`, `rr_check` | same call, 3rd time or more. Also `rp_stuck >= 3` forces the closing round (`loop.c:920,926`) | T 430 | N, M | ORDER | 2 / 0 |
| R3 | "this exact call already gave this exact result (N times now). Repeating it changes nothing. Do something different: ..." | `loop.c:936` | same call and same result hash seen before, for any tool the `rr` table does not cover (Bash, Web*, TodoWrite, ToolSearch, MCP not read-only); appended after the real result | T 245, O 254 | count | ORDER | 0 / 0 |
| R4 | "N calls in this request failed around the same thing: KEY (last error: "...") ... stop probing it. Tell the user in one sentence ..." | `loop.c:942`, `bk_count` | the 3rd and 6th failure with the same key (`bk_key_of`) | T 420, up to 700 | count, key (96), error (120) | ORDER | 0 / 0 |
| R5 | "N calls in a row failed. Stop guessing. What do the errors have in common? Check the help or the documentation (WebSearch), or tell the user ..." | `loop.c:949` | `rp_fail >= 4` consecutive failures | T 215 | count | ORDER | 0 / 0 |
| R6 | "this request has used its tool rounds. Do not call a tool now. Tell the user in a few sentences what you did, what is left ..." | `loop.c:959` | `round == lim - 2` (`lim` 40 by default, `KATANA_ROUNDS`, up to 200) or forced by `rp_stuck >= 3` | T 215 | none | ORDER | 0 / 0 |
| R7 | "plan first. This request already took N tool calls without a plan. Now call TodoWrite with 3 to 8 concrete steps ..." | `loop.c:436`, `loop_plan_note` | 3rd non-read-only call with no plan in this request (`!g_todo_touched && !pl_old`). Prints the count of ALL calls, the trigger counts only non-read-only ones | T 205, O 195 | N, tool name | ORDER | 15 / 2 |
| R8 | "still no plan after N tool calls. Stop and write the plan now with TodoWrite ..." | `loop.c:438`, same | 6th non-read-only call, still no plan | T 180 | N, tool name | ORDER | 0 / 0 |
| R9 | `[plan: step K of N in progress: TEXT; mark it completed with TodoWrite when done]` (no `katana` prefix) | `loop.c:441`, `loop_plan_note` | every 5th call while a plan has a step in progress and the call is not a todo tool | T 100, O 138 | K, N, step text (220) | INFO | 87 / 51 |
| R10 | "linked files you have not read yet: F1 (why), F2 (why), F3 (why)" | `findgraph.c:652`, `fg_neighbor_note` | after Read (not a short re-read), Grep or Find, when the evidence graph has neighbors with score >= 0.06 not yet read or cited. At most 8 per request (`FG_NOTES_MAX`), at most 3 files. `KATANA_NEIGHBORS=0` off | O 95 to 290, avg 215 | file names, reasons | INFO | 0 / 0 |
| R11 | "file has N lines; this is its map, its start [and the parts that match what you are looking for]. Read another part with offset and limit (40 to 80 lines), or search inside it with Grep (-n -C 2)" | `read.c:396,398`, Read | Read without a range of a big file (`ol_big`) seen fewer than twice | T 150 (+ map, head and windows, 6 to 25 KB) | N | INFO + suggestion | 7 / 0 |
| R12 | "lines 1-N of PATH were already given to you in this request; here is the rest, from line M" | `read.c:382`, Read | `Read` of a file already given up to `cov_end`, same offset or none. `KATANA_READ_CURSOR=0` off | T 140 + path | N, path, M | INFO | 24 / 8 |
| R13 | "you already read this whole file (N lines) and it has not changed. These are the notes kept from that reading ..." | `code.c:96`, `read_file` (older tool set, `g_code`) | whole file read before, unchanged | T 210 | N | INFO | 0 / 0 |
| R14 | "condensed note of lines A-B of this file (S bytes read; the raw text was left out of the context and the note is kept) ..." and "condensed note of this TOOL output (S bytes ...)" | `delegate.c:238,243`, `tool_digest_in` | `WebFetch`/`web_fetch`/`read_file` result >= 3,500 bytes (`DG_MIN`), ChatML or Gemma 4, side generation of up to 420 tokens by the model itself. `KATANA_DIGEST=0` off | T 200, O 172 | A, B, S | INFO | 0 / 0 |
| R15 | "you already read this and it has not changed; this is the note kept from that reading (S bytes of raw text left out) ..." | `delegate.c:178` | same read request as R14 seen again | T 190 | S | INFO | 0 / 0 |
| R16 | "notes kept from the lines you read before in this file ... [now the next lines]" | `delegate.c:256` | `read_file` after earlier chunks were condensed | T 90 + notes | notes | INFO | 0 / 0 |
| R17 | "the result of TOOL KEY was removed to save context (S bytes); call it again if you need it | to see it again, Read FILE" | `context.c:291`, history compaction | old tool results dropped when the context fills | T 170, O 208 | tool, key, size, stash path | INFO + pointer | 5 / 0 |
| R18 | "message cut at N bytes" | `mcp_channel.c:180` | channel message above `CHN_TEXT` | T 35 | N | INFO | 0 / 0 |
| R19 | `[hint: ...]` family, 17 sites (no `katana` prefix) | `diagnose.c:28-74`, `terminal.c:446,448`, `devtools.c:404` | a Bash/terminal result matches a known failure (password prompt, missing module, port taken, ...) | 60 to 250 | program, path, port | ORDER (soft) | 3 / 0 |
| R20 | `[web: from cache, N bytes returned]` and the other `[web: ...]` footers | `webfetch.c:315` and the web tool | every web result | 60 | sizes, times | INFO | 34 / 0 |

### 2.2 Notes appended to the user's request (one per request unless stated)

| id | text | where, function | trigger | size | variable data | class | v7 train/val |
|---|---|---|---|---|---|---|---|
| Q1 | "you mention "X", which does not exist from the working folder (CWD). It is at: PATH. Work there: pass that folder as `path` to Find, Glob and Grep, or use absolute paths." | `locate.c:300`, `place_note`, attached at `loop.c:708` | request names a folder or project (`pasta`, `projeto`, `repo`, a path with 2+ slashes) that does not exist from the cwd and `ff_resolve_missing` finds it; at most 2 searches, 4 candidates. `KATANA_PLACE_NOTE=0` off. Side effect: indexes the folder (`autoindex_dir`) | T 250, O 515 to 613 per request | name, cwd, path | ORDER | 42 / 5 |
| Q2 | "the engine picked this from the project documents that KATANA.md points to, because it matches this request. It is a selection: open the file if you need more." + `== file:a-b` blocks | `docsel.c:204`, `ds_select`, attached at `loop.c:698` | project has `KATANA.md`; up to 3 rounds of blocks, at most 6 blocks, 8,000-byte buffer; only for ChatML/Gemma (`cml`) | T 150 + data up to 8,000 | document lines | INFO | 19 / 11 |
| Q3 | "your todo list from before still has N unfinished steps. This message is newer and comes first: if it changes the plan, follow it and rewrite or drop the old steps." | `loop.c:719` | new request while the previous list has open steps, not the first request | T 210 | N | ORDER | 0 / 0 |
| Q4 | "The user undid your file changes of an earlier request; these files are back as they were before it: LIST" (no `katana` prefix) | `loop.c:690` | user ran undo | 110 + list | files | INFO | 0 / 0 |
| Q5 | "Plan mode is ON: you may only read and investigate; the engine refuses anything that changes files ... answer with a short numbered plan, and stop." (no prefix) | `planmode.c:6`, attached at `loop.c:711` | `/plan` active, every request | T 270 | none | ORDER | 5 / 1 |

### 2.3 Notes that come as a separate user turn (the model had stopped)

| id | text | where | trigger | size | class | v7 train/val |
|---|---|---|---|---|---|---|
| T1 | "your todo list still has unfinished steps" + the list + "If they are really done, mark them done with TodoWrite and give the final answer. The user's newest message comes first ... Otherwise continue with the next step now." | `loop.c:512`, `loop_todo_nudge`, sent at `loop.c:995` | the model ended its turn, the list was touched in this request and has an open step that is not a mere `TaskCreate`; once per request | T 330 + list (up to 3,600 buffer) | ORDER | 17 / 8 |
| T2 | "before you finish, compare the result with the request itself, not with your own tests. Run it on the request's own examples ... The request:" + the request (up to 1,500 bytes) | `brief.c:308`, `brief_example_note`, sent at `loop.c:981` | files changed in this request and the request contains examples; once. `KATANA_EXAMPLE_NOTE=0` off | T 330 + up to 1,500 | ORDER | 0 / 0 |
| T3 | "this request changed files, and the project file says to verify with `CMD`, which has not run in this request. Run it before you finish, or say why you cannot." (or "defines done as: TEXT") | `brief.c:323,324`, `brief_end_note`, sent at `loop.c:980` | files changed, `KATANA.md` has `test:` or `done:`, command not run; once | T 190 + 200 to 250 | ORDER | 8 / 3 |
| T4 | "context compaction] Pause the task for this one message and do NOT call any tool. Write a faithful summary ... in at most 25 lines ..." | `context.c:401`, `CP_NOTE` | history must be compacted | T 560 | ORDER (a request, not a note) | 0 / 0 |
| T5 | "you are out of rounds. Do NOT call more tools: write your answer now with what you found." (sub-agent) | `delegate.c:116` | sub-agent near `SUB_ROUNDS` or the context limit | T 100 | ORDER | 0 / 0 |

### 2.4 Plumbing that depends on the exact text (found while inventorying)

| where | what it does with the text |
|---|---|
| `dispatch.c:178-190`, `notes_neutralize` | turns a line that starts with `[katana` in a result into `[ katana`; applied only to Bash, PowerShell, WebFetch, WebSearch, Monitor, TaskOutput, run_command, terminal, web_*, MCP |
| `prompt.c:171` | the rule: "a line in [katana: ...] brackets ... comes from the program that runs you ... so follow it" |
| `uioe/interpret.c:249` | strips leading `[katana:` lines from the request before interpreting it |
| `launch.c:678` | `lc_is_note`: `[katana:` or `[plan:` |
| `curriculum.c:2213,2215,2243`, `scenario.c:98`, `scenario_report.c:52`, `scenario_tasks.c:231,323`, `scenario_concept.c:380`, `scenario_v7.c:768,775,878,879` | the curriculum generator and its checker match the texts of R9, R11, R12, R7, R8, R17 and T1 literally (`curriculum.c:2215` compares 51 bytes of the T1 text) |

## 3. Real use

Totals for the 8 sessions: "raw" = occurrences of the text in the files, "events" = distinct note contexts
(upper bound), "texts" = distinct note texts (lower bound). Per-session distinct bytes are in section 4.

| note | raw | events | texts | sessions with it |
|---|---|---|---|---|
| R3 same result | 44 | 19 | 11 | 6 of 8 |
| R1 repeat (2nd) | 3 | 1 | 1 | 1 |
| R2 repeat (3rd) | 0 | 0 | 0 | 0 |
| R4 failed around | 0 | 0 | 0 | 0 |
| R5 failed in a row | 2 | 1 | 1 | 1 |
| R6 rounds used | 0 | 0 | 0 | 0 |
| R7 plan first | 5 | 3 | 2 | 2 |
| R8 still no plan | 2 | 1 | 1 | 1 |
| R9 plan step | 34 | 18 | 10 | 4 |
| R10 linked files | 33 | 11 | 11 | 3 |
| R11 file has N lines | 9 | 3 | 3 | 2 |
| R12 read cursor | 0 (14 of the old wording) | 0 | 0 | 0 |
| R14 condensed note | 12 | 12 | 5 | 2 |
| R17 removed to save context | 14 | 14 | 14 | 1 |
| R19 hints | 31 | 23 | 10 | 2 |
| Q1 you mention | 6 | 2 | 1 | 2 |
| Q2 docs picked | 0 | 0 | 0 | 0 |
| Q3 todo from before | 3 | 2 | 1 | 1 |
| T1 todo unfinished | 15 | 15 | 6 | 3 (9 in 2026-11-20, old build) |
| T2, T3, R13, R15, R16, T4 | 0 | 0 | 0 | 0 |

### 3.1 R1, R2, R3: repeats (the "ORDER" notes the owner wants kept)

R1 (observed, session 2026-11-23 10:19): a `Grep` repeated at step 3 got the R1 text as its whole result (the tool
did not run). The next call was `Read grey-channel.mjs offset 20 limit 40`: a different place. Verdict: obeyed (1 of 1).

R3 first occurrence (count 2), classification of the next call, 9 events:

| # | session | tool | next call | verdict |
|---|---|---|---|---|
| 1 | 11-22 15:09 | TodoWrite | final answer in words ("Todos os 4 passos ja foram concluidos") | obeyed (stopped) |
| 2 | 11-22 16:34 | TodoWrite | `TodoWrite` again with other arguments (`done=1`, new text) | partial |
| 3 | 11-22 16:34 | WebSearch | `WebFetch` of a page | obeyed |
| 4 | 11-22 16:34 | WebFetch | `WebSearch` with new words | obeyed |
| 5 | 11-23 07:46 | ToolSearch | `ToolSearch` with the same `query` | ignored |
| 6 | 11-23 08:07 | Find | answered in words, no call | obeyed |
| 7 | 11-23 10:19 | Find | `TodoWrite` + `Read server.mjs` | obeyed |
| 8 | 11-23 11:26 | Find | `Read channels.mjs` | obeyed |
| 9 | 11-23 11:26 | Read | `TodoWrite` + `Read channels.mjs offset 29 limit 40` | obeyed |

R3 at counts 3 to 12 (observed, session 2026-11-22 16:34, 10 distinct events): the same `WebSearch`
("portobello.com.br colecoes porcelanato imagens galeria") came back each time with `(3 times now)` up to
`(12 times now)`. Each next call was the identical `WebSearch`. In 7 of the 10 the model wrote the sentence "Vou
mudar a abordagem — baixar imagens diretamente das paginas de colecoes e buscar SketchUp models." immediately before
the identical call. Verdict: ignored 10 of 10 (the note was read and answered in words, but the action did not change).
Nothing in the files shows how the loop ended: the last record is the 12th call. `rp_stuck >= 3` should have forced
the closing round (`loop.c:920,926`), but R6 never appears in any session, so I cannot tell why it did not fire (open
question; `round < lim - 2` is required, so a request already past round 38 never forces).

Two defects visible in this session (observed in the code and in the text):

* `rr_covered` (`loop.c:477`) lists Read, Grep, Glob, Find, Symbols and read-only MCP tools. `WebSearch` and
  `WebFetch` are not covered, so a repeated web call is executed again (the tool returned
  "(cached) ... 3775 bytes returned" each time) and only R3 is appended, never the R2 "will not run again". Opinion:
  covering the two web tools gives them the same hard cut as Read, at no new code.
* R1 and R2 say "the result is earlier in this conversation". In the same session R17 had replaced old results with
  "was removed to save context" (14 events), so the claim can be false: the model had neither the result nor a way to
  get it by repeating the call. In 14 of 14 events after R17 the next call re-ran the web tool (section 3.5).

### 3.2 R7, R8, R9, Q3, T1: plan and todo notes

* R7 "plan first": 3 events. 2026-11-22 15:09 (after 3 calls): the model answered in prose and kept working; R8 at
  call 6 was followed by `TodoWrite` with 5 steps (obeyed). 2026-11-23 10:19 (2 events at 8 calls): my script could
  not read the next call; by hand the next output of that turn was a `TodoWrite` (obeyed). Total: 2 of 3 obeyed
  on the next call, plus R8 1 of 1. Sample too small for a rate.
* R9 `[plan: step K of N ...]`: 18 events (4 sessions). Next call: `Bash` 7, `WebSearch` 4, `WebFetch` 3,
  `TodoWrite` 3, `Read` 1, prose 1. Only 3 of 18 (17%) were `TodoWrite`. In the 2026-11-22 15:09 session the note read
  "step 1 of 5 in progress: Explorar a pagina ... - CONCLUIDO; mark it completed with TodoWrite when done" on three
  consecutive results while the step text itself said it was finished, and the model never marked it: the note
  repeated an instruction the model had already judged moot. Verdict: not followed, and stale.
* Q3 "todo list from before": 2 events, same turn. The next call was `Bash mkdir ...` for the new request: the new
  message won, as the note asks (obeyed; the note's effect cannot be separated from the model doing the newest
  request anyway).
* T1 "todo list still has unfinished steps": 15 events. Next output: prose with a final answer 5, `TodoWrite` 3,
  other tools 7. In the 3 sessions with it, the model closed the list or answered, listing what is left in words
  ("Faltam dois passos que dependem de voce"). No loop was seen after it. Verdict: followed or reasonably answered
  in all 15; the nudge never produced a second nudge (once per request by design).

### 3.3 R10 linked files (neighbors)

11 events in 3 sessions, each 95 to 290 bytes (avg 215). Next call: `Read` 5, `TodoWrite` 4, `Grep` 2, `Find` 1.
The next call touched a listed file in 1 of 11 (`bridge/server.mjs`, session 11-23 11:26, the first listed item).
Within the following 30,000 characters (several calls) 3 of 11 touched a listed file. Two listed items were
sometimes the file the model then opened by its own plan (`grey-channel.mjs`), which the note did not list. Verdict:
weak effect on navigation. It is cheap, and the model was never trained on it (0 conversations in v7).

### 3.4 R11 file map, R12 cursor, R14 digest, Q1 place

* R11: 3 events. The next call followed the offered alternative once (`Grep -n -C 2 pattern katana`, the
  suggestion's own flags), moved to a different file once, and used `Find` once. The note is part of a bigger payload
  (the map itself), so its words are a minor share.
* R12 (the new cursor wording): 0 events. The 14 "lines 1-N of M shown; the position is kept" hits are the old
  wording from 2026-11-20 and 21.
* R14: 12 contexts (5 distinct texts), all `WebFetch`. Next call: another `WebFetch` 4, `WebSearch` 3, `Bash` 1, and
  the rest on repeated snapshots of the same turns. No sign that the note caused a repeat fetch. Side cost not in the
  byte count: each digest is a side generation of up to 420 tokens by the same model, with the main context saved and
  reloaded (`delegate.c:140-170`), the largest hidden cost of any note.
* Q1: 2 sessions, both obeyed. Session 11-23 11:26: the model answered "That is a note from the program that runs me,
  not from you. I will work in the folder it says." and the first call was `Find ... path=/mnt/dados/source/greymod`.
  Session 11-23 10:19: first call `Read /mnt/dados/source/greymod/bridge/grey-channel.mjs` (absolute path). Without
  the note the first `Find` would have started at the home folder (`docs/uioe.md` line 134 records a 10 s `Find` from
  home as the original defect). 2 of 2.

### 3.5 R17 removed to save context

14 events in one session, all in the 12-time `WebSearch` loop. The note carries a pointer ("to see it again, Read
/home/.../context/.../r103.txt"). Next call: re-run `WebSearch` 11, re-run `WebFetch` 3, `Read` of the pointer 0.
Verdict: the pointer was not followed (0 of 14). The model's recovery was to repeat the tool, which R3 then flagged.
Two of the next outputs again contained the "Vou mudar a abordagem" sentence.

### 3.6 R19 hints (siblings)

23 distinct contexts in 2 sessions (old tool names). Next call `Bash` 20, `run_command` 1, prose 2. Example: after
"the program pbzx is not installed or not in PATH ... install it (install_package ...)" the next call was
`sudo apt-get install -y pbzx`: followed in substance. I did not classify the rest.

## 4. Effect and cost per note

"Avoids" is what the code comment or the text says the note is for; "evidence" is what section 3 shows. Cost is bytes
of note text in the 8 sessions (distinct contexts, section 3 method) and per occurrence, tokens at 4 bytes each.

| id | tries to avoid | evidence | cost |
|---|---|---|---|
| R1/R2 | re-reading the same file/search (a loop that wastes a 1 to 2.5 minute turn) | 1 event, obeyed. No R2 anywhere. R1 replaces a result (the note is smaller than the content it replaces, so net negative cost) | 260 B = 65 tokens per event |
| R3 | repeating a call that gave the same result | 8 of 9 obeyed at count 2; 0 of 10 at counts 3 to 12 (one loop) | 19 events, 4,829 B, avg 254 B = 64 tokens. The 10 late notes cost 2,540 B (about 640 tokens) and avoided nothing; the loop cost about 38 KB of repeated web results (estimate) and 10 model turns |
| R4, R5, R6 | circling a failing thing; running past the round limit | R5 1 event (next call: prose, then `WebSearch` with other words: obeyed). R4 and R6 never fired | R5 199 B. R4 up to 700 B |
| R7/R8 | working without a plan (the owner's weak spot: no plan, no order) | 3 of 4 obeyed on the next call (small sample) | 4 events, 783 B, avg 195 B = 49 tokens |
| R9 | forgetting the plan | 3 of 18 followed; stale text seen 3 times in a row | 18 events, 2,493 B = about 620 tokens |
| R10 | opening files in the wrong order; missing the neighbor | 1 of 11 on the next call, 3 of 11 later | 11 events, 2,365 B = about 590 tokens |
| R11, R12 | reading a big file by pieces blind; re-reading from line 1 | R11: 1 of 3 followed the offered alternative; R12 0 events | R11 170 B + the map payload |
| R14, R15, R16 | filling the context with raw web pages | no measured effect on loops; costs a side generation (up to 420 tokens of the same model) and the save/load of the context per digest | 12 contexts, 2,066 B; compute cost not measured |
| R17 | context overflow | pointer followed 0 of 14; model re-ran the tool 14 of 14 | 9 to 14 events, 1,876 B |
| Q1 | working from the wrong folder (home) | 2 of 2 obeyed with the path on the first call | 2 events, 1,128 B = 282 tokens (avg 564 B) |
| Q2 | the model not seeing the project documents | 0 events in these sessions (no `KATANA.md`) | up to 8,000 B (2,000 tokens) when it fires |
| Q3 | the old list capturing the new request | 2 events, obeyed | 1,205 B for 2 (603 B each: the list is included in the count by my pattern) |
| T1 | ending with open steps | 15 events, no loop after it | one per request; list-sized |
| T2 | verifying against the author's own tests | 0 events; 0 training conversations | up to 330 + 1,500 B = 460 tokens; the 1,500 B repeat what is already in the context |
| T3 | skipping the project's own verification | 0 events | about 190 to 440 B |
| R19 | repeating a known failure | followed in substance (1 example) | 60 to 250 B each, 23 events |

Per session (distinct contexts, bytes of note text): 254 (11-23 07:46), 459 (11-20, T1 only, the counting stops at
the first `]`), 560, 1,722, 2,210, 2,847, 3,365 and 7,924 (11-22 16:34, the loop session). Median about 2,000 B = 500
tokens; the worst session spent about 1,980 tokens, 4.7% of a 42k-token context, an estimate (I did not measure the
context size). Cost is not where the benefit or harm is; the notes' effect on behavior is.

## 5. Recommended disposition (opinion, with the numbers above)

The UIOE block (`[katana: context ...]`, `docs/uioe.md` 4.6) is a design: `KATANA_UIOE` defaults to off and
`types.c:153` says the loop calls nothing yet; the builder is phase U4. So "MOVE" means: keep the emission point
unchanged now, and make it feed the block when U4 lands. The block has fixed labels (4.6.3): `Question understood
as`, `Answer path` or `Findings`, `Evidence`, `Links`, `Not found / limits`, `Next`.

| id | disposition | reason (numbers from sections 2 to 4) | where it lands |
|---|---|---|---|
| R1, R2 | KEEP, and add `WebSearch` and `WebFetch` to `rr_covered` | the one hard cut that works (obeyed 1 of 1; R3 at count 2 obeyed 8 of 9); the 12-time loop is the evidence for the gap. Fix the wording "the result is earlier in this conversation" to "the result is above, or listed as removed" | as is |
| R3 | KEEP, and make it a second wording of the same kind `REPEAT` (count 2: "do something different"; count 3+ for tools outside the cover: say it will stop). Opinion: past count 3 add a forced stop (the engine ends the tool round and asks for the answer, as R6 does), since the note alone was ignored 10 of 10 | order, interrupts loops, effective early, useless late | as is |
| R4, R5 | KEEP (merge into one kind `FAIL`) | never or once fired, but they are the only guard for failure loops; text only when the condition holds, zero cost otherwise | as is |
| R6 | KEEP | the only guard at the round limit; 0 events, so no evidence either way; safety valve | as is |
| R7, R8 | KEEP as one kind `PLAN` (first, then the repeat only if still none) | 3 of 4 obeyed; the owner's rule "no plan, no order" is the reason; 15 + 0 training conversations | as is |
| R9 `[plan: step ...]` | CUT the every-5-calls reminder | 3 of 18 followed; went stale 3 times in a row; 87 + 51 training conversations carry it, but the curriculum generator regenerates them (section 6). Keep the plan state inside T1 and in the `TodoWrite` result, where it is requested | none |
| T1, Q3 | KEEP T1 (once per request, only when the model tries to stop); MOVE Q3 into the block as a `Not found / limits`-style line ("an earlier list has N open steps") | T1: 15 events, no loop after; Q3: informational, 2 events, the newest request wins anyway | T1 as is; Q3 into the block, `Question understood as` clause |
| R10 neighbors | MOVE to `Links:` of the `Find` facade (U5) and to the Read footer; until then KEEP | 1 of 11 on the next call; cost 2,365 B in 3 sessions; the design (`docs/uioe.md` 1137, 1637) already assigns it to the evidence graph | `Links:` (open ends) |
| R11 file map | FUSE into the Read result: the "[katana: file has N lines; this is its map ...]" sentence becomes the map's own footer line without the `[katana:` voice | informational; the model followed the offered flags once of 3; the payload is the map, not the sentence | the Read result |
| R12 cursor | FUSE: the header `[PATH: lines 1-N given before; this is N+1-M of T]` (the owner's own suggestion) | 0 events in current sessions; 24 + 8 conversations train it, regenerated by the generator | the Read result |
| R13, R15, R16 | FUSE into the same Read result header as R14 | all say "this is a stored note, not the text; for exact lines use offset and limit" | the Read result |
| R14 digest | KEEP the mechanism, FUSE the label as `[condensed: ...]` without `katana` voice; opinion: re-check whether it is worth its side generation | no measured effect on loops; the cost is compute, not text | the result |
| R17 removed | KEEP the label, CUT the "Read FILE" pointer for web tools and say instead "run the search again with a different query only if you need more" | 0 of 14 followed the pointer; 14 of 14 re-ran the identical tool; the pointer teaches nothing the model uses | the history |
| R18, T5 | KEEP T5 (sub-agent order); FUSE R18 as plain text | order vs info | as is |
| R19 hints | KEEP, give them the same `knote` voice and prefix; MOVE the static ones (venv, PATH, port taken) to a lookup in the tool description | the sibling kinds are outside the rule in `prompt.c:171` | as is |
| Q1 place | KEEP as is until U4; then it becomes `Question understood as: ... (project PATH; the folder you named does not exist from the working folder)` and `Next: use path=PATH ...` | owner decision; 2 of 2 obeyed | `Question understood as`, `Next` |
| Q2 docs | MOVE into `Evidence:` of the block | design in `docs/uioe.md` 1100 to 1118; 0 events in these sessions | `Evidence` |
| Q4 undo, Q5 plan mode | KEEP, move to the `knote` table with the `[katana:` prefix | stateful, rare; 5 + 0 training | request |
| T2 example check | CUT the echo of the request (up to 1,500 B); keep one sentence and fuse with T3 as kind `FINISH` | 0 events, 0 training conversations; the request is already in the context; the code comment cites 3 failures in one proof (21/11/2026) that I cannot verify | turn end |
| T3 verify not run | KEEP (merged with T2) | the project asked for it; 8 + 3 training conversations | turn end |
| T4 compaction | KEEP, exempt from the budget | a request for a summary, not a note | turn |

Tally by id: KEEP R1 to R8, R19, T1, T3, T4, T5, Q1 (until U4), Q4, Q5. MOVE R10, Q2, Q3 (and Q1 after U4).
FUSE R11, R12, R13, R15, R16, R18 (and the R14 label). CUT R9 whole, the R17 pointer, the T2 echo. The table above is the
authority; Q1 and R10 appear twice because they stay now and move later.

### 5.1 The consolidated text in the block (sections it absorbs)

Only what is already known before the first call goes into the block. Draft, in the 4.6 grammar:

```
[katana: context for this request. Gathered by the program before you started. Items marked with a number were checked in the files; (guess) means not checked.]
Question understood as: <task template> (project /mnt/dados/source/greymod; the folder "greymod" you named does not exist from the working folder).   <- Q1, Q3
Answer path / Findings: <from retrieval>
Evidence:
- [C] KATANA.md:12-18 <picked project document lines>                                                               <- Q2
Links: <open ends of the evidence graph: files not yet read, each with why>                                         <- R10
Not found / limits: an earlier list has 2 open steps (this message is newer and comes first); <index state>        <- Q3
Next: <imperative with a stop criterion>. Use path=/mnt/dados/source/greymod in Find, Glob and Grep.                <- Q1
[end of context]
```

The block is attached once per request, before the first model call; notes that depend on what the model does (R1 to
R9, T1 to T3) cannot be in it. That is why only Q1, Q2, Q3 and the first `Links` move; R10 keeps being useful after
each result and stays as a short `Links:` line inside the `Find`/`Read` result (U5).

## 6. What changes for the curriculum, and the risks

* The curriculum is generated by the engine (`katana curriculum`, `data/curriculum/v7/generate.sh`): the notes in the
  examples are the engine's own output. Changing the emitted text changes the examples when v8 is generated; no
  example is edited by hand. The checker (`katana curriculum check`) replays every conversation against the real
  tools and will fail on any example whose note no longer matches, so a half-done change is caught.
* v7 has not been trained (`README.md`: `train.sh` not run). Doing the consolidation before the v7 run costs nothing
  in retraining. Doing it after leaves the model with the old formats in its weights and the engine speaking a new
  one: the model was taught to follow `[katana: ...]` lines (269 of 1,042 conversations contain the prefix; 125 of
  them only because the system prompt carries the rule, so about 144 teach a note), and an unseen format only has the
  system prompt rule to rely on.
* Conversations that change by disposition (train/val counts from section 2):
  - CUT R9: 87 / 51 conversations lose the line `[plan: step ...]` (the `x-katananotes` plan family and every
    conversation with a plan open for 5 calls). Safe for an old model: a missing note is harmless; an unseen one is
    not.
  - FUSE R11: 7 conversations. FUSE R12: 24 / 8 (`x-cursor`, 40 train / 8 val). The scenario code asserts the texts
    (`scenario_v7.c:768,775`) and must change in the same commit.
  - R17 pointer: 5 conversations; scenario code `scenario_tasks.c:231,323` and `scenario_concept.c:380` parse
    `[katana: the result of ` literally.
  - MOVE Q1/Q2: 42 + 19 train, 5 + 11 val conversations (`x-place` 60/7, `x-docs` 21/11) change from "request +
    appended note" to "request + context block". This is the large change and belongs with U4.
  - T2 echo: 0 conversations. T1 (17 / 8) and R7 (15 / 2) keep their text.
* Notes with no training at all today: R3 (0), R4 (0), R5 (0), R6 (0), R8 (0), R10 (0), R14 (0), Q3 (0), T2 (0), T4 (0).
  R3 is the most frequent note in real use (19 events) and the model has not been taught it except by the
  system prompt line. Opinion: before training v8, add the R3 count-2 and count-3 cases to `x-katananotes`
  (today 27 for R1, 2 for R2).
* Risks of consolidating:
  1. A missing or reworded note can reopen a loop. Mitigation: do the CUTs first (R9, T2 echo, R17 pointer), leave
     R1 to R6 and T1 byte for byte, and add the web tools to `rr_covered` (new behavior, would need a scenario).
  2. The system prompt rule (`prompt.c:171`) says the notes "tell you what it did or what to do next". Notes
     fused into plain results would no longer carry that authority. That is intended for pure information, but then
     the ORDER notes must keep the prefix so that the model can tell them apart.
  3. `rr_check`'s text says the result is earlier; with R17 that can be false (section 3.1). If R1/R2 are kept as
     they are, the wording must be fixed in the same change or the web fix makes it worse (a web call that is cut
     and whose result was removed leaves the model with nothing).
  4. Budget logic (section 7) can drop an INFO note that the curriculum shows; the budget must be applied the same
     way in the generator, or the examples will not match the engine.
  5. Anything that is cut or fused changes the strings the checker and the scenario files match; section 7 lists
     them.

## 7. One place to emit notes: `knote`

Today: about 30 `snprintf` sites in 13 files, each with its own size guard (`rl + 500 < sizeof result`), its own escape
(one function, `notes_neutralize`, for 12 tool names; none for the user request, for `ds_select` lines, for the
todo list text, for query keys and error text), no common budget, and consumers that match the text by
`strncmp` with a hardcoded length (`curriculum.c:2215`, 51 bytes).

### 7.1 Proposed API (not implemented)

```c
typedef enum { KN_REPEAT, KN_SAME_RESULT, KN_FAIL, KN_ROUNDS, KN_PLAN, KN_PLAN_STEP, KN_TODO_OPEN, KN_TODO_PREV,
               KN_LINKS, KN_READ_MAP, KN_READ_CURSOR, KN_STORED, KN_REMOVED, KN_PLACE, KN_DOCS, KN_FINISH, KN_SUB_ROUNDS,
               KN_HINT, KN_COUNT } KnKind;
typedef struct {
    const char *name;        /* stable label for the log and the curriculum: "repeat", "plan", ... */
    unsigned char cls;       /* KN_ORDER | KN_INFO | KN_STATE */
    unsigned char site;      /* KN_RESULT (appended to a tool result) | KN_REQUEST (end of the user message) | KN_TURN (own user turn) */
    unsigned char prio;      /* 0 = highest. Stops, repeats and failures before plan before info */
    unsigned char once;      /* 1 = at most once per request (todo open, finish, example) */
    unsigned short maxb;     /* hard cap of the whole note in bytes; the data is cut, never the order sentence */
    const char *env;         /* off switch, or NULL */
    const char *fmt;         /* printf template, the only place where the text lives */
} KnSpec;
extern const KnSpec KN_TAB[KN_COUNT];

typedef struct { int n_res, n_req; size_t bytes; unsigned char fired[KN_COUNT]; } KnBudget;   /* reset where rr_reset() is called */
size_t knote(KnBudget *b, KnKind k, char *out, size_t cap, ...);       /* returns bytes appended, 0 if refused */
void   kn_escape(char *text, size_t cap);                               /* outside data: "[katana" -> "[ katana", also "[plan:", "[hint:" */
```

Rules inside `knote`:

1. Escape every `%s` argument that comes from outside (file names, queries, error text, step text, document lines,
   the request echo) with `kn_escape` before formatting; today only 12 tool results are covered, by name
   (`dispatch.c:182`). Opinion: move to an allow-list of tools that are known safe (Read, Grep, Find, Symbols,
   TodoWrite), so a new tool is escaped by default.
2. Budget per request (numbers are my proposal, not measured): at most 1 ORDER and 1 INFO per tool result; at most 1,200
   bytes of INFO per request (about 300 tokens); ORDER notes are never refused, an INFO note is dropped when an ORDER
   fires in the same result; `once` kinds checked against `fired[]`.
3. Order of precedence when two notes want the same result: `ROUNDS` > `REPEAT` > `SAME_RESULT` > `FAIL` > `PLAN` >
   `LINKS`/`READ_MAP`.
4. Every call writes one line to `~/.katana/agente.log` (kind, bytes, tool, round). This is what section 3 had to
   reconstruct from raw text; with it the next analysis is `grep`.
5. Type safety: the table holds the template, but a C variadic loses argument checking. Opinion: keep one thin
   wrapper per kind (`kn_repeat(b, out, cap, step, reps)`) built with `__attribute__((format(printf, ...)))`
   from the table's template macro, so the compiler checks the arguments.

### 7.2 Points to change

Emitters (replace the `snprintf` by `knote`):

| file:line | id | kind |
|---|---|---|
| `src/agent/loop.c:436,438` | R7, R8 | `KN_PLAN` |
| `src/agent/loop.c:441` | R9 | removed (CUT) |
| `src/agent/loop.c:487,490` | R1, R2 | `KN_REPEAT` |
| `src/agent/loop.c:512` | T1 | `KN_TODO_OPEN` |
| `src/agent/loop.c:719` | Q3 | `KN_TODO_PREV` (then into the block) |
| `src/agent/loop.c:708` | Q1 attach | `KN_PLACE` |
| `src/agent/loop.c:698` | Q2 attach | `KN_DOCS` |
| `src/agent/loop.c:690`, `planmode.c:6` | Q4, Q5 | `KN_STATE` kinds |
| `src/agent/loop.c:936,942,949` | R3, R4, R5 | `KN_SAME_RESULT`, `KN_FAIL` |
| `src/agent/loop.c:959` | R6 | `KN_ROUNDS` |
| `src/agent/brief.c:308,323,324` | T2, T3 | `KN_FINISH` |
| `src/agent/docsel.c:204` | Q2 | `KN_DOCS` |
| `src/agent/locate.c:300` | Q1 | `KN_PLACE` |
| `src/tools/findgraph.c:652` | R10 | `KN_LINKS` |
| `src/tools/read.c:382,396,398` | R12, R11 | `KN_READ_CURSOR`, `KN_READ_MAP` |
| `src/agent/code.c:96`, `src/agent/delegate.c:178,238,243,256` | R13 to R16 | `KN_STORED` |
| `src/agent/context.c:291`, `context.c:401` | R17, T4 | `KN_REMOVED`, exempt |
| `src/agent/delegate.c:116` | T5 | `KN_SUB_ROUNDS` |
| `src/agent/mcp_channel.c:180` | R18 | plain text |
| `src/agent/diagnose.c:28-74`, `terminal.c:446,448`, `devtools.c:404` | R19 | `KN_HINT` |

Consumers (change in the same commit as the matching emitter):

| file:line | what it does today |
|---|---|
| `src/agent/dispatch.c:178-190` | the only escape; becomes `kn_escape`, called from `knote` and at the end of `agent_tool` for every tool not on the safe list |
| `src/agent/prompt.c:171` | the rule; extend to "`[katana: ...]`" only after the siblings (`[plan:`, `[hint:`, `[web:`) are renamed, or name them |
| `src/uioe/interpret.c:249` | strips leading note lines from the request; must also strip the context block when it exists |
| `src/app/launch.c:678` | `lc_is_note`; drop the `[plan:` branch if R9 is cut |
| `src/app/curriculum.c:1050,2213,2215,2243` | match R7/R8, `[katana: ` and the 51-byte T1 prefix; use the kind table instead |
| `src/app/scenario.c:98`, `scenario_report.c:52` | match R7/R8 and the request filter |
| `src/app/scenario_tasks.c:231,323`, `scenario_concept.c:380` | match `[katana: the result of ` (R17) |
| `src/app/scenario_v7.c:768,775,878,879` | match R12 and R7 |
| `data/curriculum/v7/*` | regenerate as v8; v7 files stay as they are |

### 7.3 Order of work (opinion)

1. CUT first (R9, the T2 echo): no model sees a new format, so nothing to retrain. Small change.
2. Add `WebSearch` and `WebFetch` to `rr_covered`, fix the R1/R2 wording about removed results, add a forced stop
   after count 3 for uncovered tools. This addresses the only loop observed in 8 sessions.
3. Introduce `knote` and the table without changing any text (a pure refactor: the strings in the table equal the
   strings today, so the curriculum checker passes unchanged). Add the log line.
4. FUSE R11, R12, R13 to R16 into the Read result and regenerate the affected families.
5. MOVE Q1 to Q3 and Q2 into the block with U4; do the neighbor note with U5.
6. Generate v8 and train once, instead of training v7 and then retraining.

## 8. Open questions for the owner

* Why did the `rp_stuck >= 3` forced stop not end the 12-time `WebSearch` loop? I could not find out from the
  files; the answer is in `loop.c` at lines 920 and 926 and `round`/`lim` for that request.
* Is the "plan first" count (calls) or the trigger (non-read-only calls) the one the owner wants shown to the model?
  R7 says "8 tool calls" when it fires at the 3rd non-read-only one (observed 2026-11-23 10:19).
* T2 comes from three failures in the proof of 2026-11-21. Should it stay without a training example, or be removed
  with the other notes that have 0 conversations (R3 to R6, R8, R10, R14, Q3, T2, T4)?
