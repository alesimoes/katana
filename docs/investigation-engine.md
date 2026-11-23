# Investigation engine (research prototype)

Status: research in Python (`tools/findbench/`), nothing in `src/` changed. Measured on 2026-11-22, without running the
language model. This document covers the experiment design, the numbers (strong and weak ones alike), and a specification
for porting the parts that worked to C.

Scripts (one command per table; set `FINDBENCH_CACHE` to a scratch folder, `OMP_NUM_THREADS=1` recommended):

| Table | Command |
|---|---|
| Commit bench: BM25 x graph x learned ranker | `python3 tools/findbench/investigator.py commits` |
| 76 questions: recall, hop slice, simulated policy, ablation | `python3 tools/findbench/investigator.py questions` |
| Hop slice definition (which answer files have no question word) | `python3 tools/findbench/investigator.py hop` |
| Confidence / risk-coverage / calibrated thresholds | `python3 tools/findbench/investigator.py confidence` |
| Sparse activation (the FolderModel as a network) | `python3 tools/findbench/activation.py questions` / `commits` / `show ID` |
| Folder embeddings (PPMI+SVD, joint space, Qwen table) | `python3 tools/findbench/folder.py questions` |
| Folder profile card and its evaluation | `python3 tools/findbench/folder.py card REPO` / `cardeval` |
| Fold-in vs full SVD | `python3 tools/findbench/folder.py foldin` |
| Case study | `python3 tools/findbench/investigator.py case grey-gkatana`, `activation.py show grey-gkatana` |
| Tool choices vs verdict in eval runs | `python3 tools/findbench/investigator.py toolstats [EVAL_OUT ...]` |

`INVEST_FAST=1` restricts the question set to a quick sample (all greymod and katana questions plus 6 sonora
questions, 3 train and 3 validation, 46 in total). The sparse-activation and embedding tables below were measured
on that sample because the full run takes about one hour in Python. The ranker, policy, and confidence tables used
all 76 questions.

## 1. Data and leakage rules

- **Questions:** `tools/investigate/questions.json`, 76 questions (52 train, 24 validation) over greymod (18), katana
  (22), and sonora (36). Each copy in `/mnt/dados/investigate-repos/<id>` is a detached worktree at the parent commit;
  I checked that every copy's HEAD equals `base`, so the copy's `git log` holds only history before the fix.
  One gold path is incomplete (`grey-channel.mjs`, the file is `bridge/grey-channel.mjs`); answers are resolved by
  unique path suffix.
- **Commit bench:** question = commit subject, gold = files of the commit (same filter as `bench.py`, `--stem --uniq`).
  Temporal split per source repo: queries come from the most recent `min(400, 35%)` commits, and the co-change graph
  and recency features use only the older commits.
- **Learned ranker:** trained only on commit queries of the OTHER two repositories (leave-one-repo-out). It never sees
  the 76 questions or any commit of the repository being evaluated.
- **Hop slice:** an answer file is a "hop" file when it contains none of the question's informative terms. A term is
  informative when it is not a request word (a short, project-independent list: find/explain/fix/descubra/diga/...)
  and occurs in at most 15% of the files. 27 of 127 answer files (21%), in 18 questions, are hop files. Most of them
  come from Portuguese questions about English code (sonora), so the hop is mostly vocabulary, not structure.
  The first version of this slice used every question word and reported 0% hop files, because words like "nao"
  occur everywhere. That version was wrong and has been replaced.

## 2. Signals

All of them can be computed from a local checkout in one pass:

| Signal | Definition | Size (sonora, 4.7k files) |
|---|---|---|
| BM25 by fields | current Find algorithm (path x3, symbols x2, body x1 over 40-line chunks; identifier split, stemming) | baseline |
| `sym` edges | identifier used in file A and defined (symbol regex) in file B; code files only; skip names used in > max(30, 2%) of files; weight idf | 72k edges |
| `path` edges | file A names file B: path-like strings with known extensions, relative imports (`./x`), Python imports | 18k |
| `rare` edges | identifier-like token (snake/camel/ENV_VAR, len >= 6) shared by 2..4 files; weight idf/(df-1) | 9k |
| `co` edges | files changed together in a commit with 2..20 files, before the cutoff; weight 1/(n-1), capped | 20k |
| priors | is_doc, is_test, is_cfg, log size, recency of last change | per file |
| PPMI token matrix | co-occurrence of terms in a +/-w window (weight 1/d), PPMI with context smoothing 0.75 | 0.4 M nnz (greymod), a few M (sonora) |

Prose files must not create `sym` edges. When they did, words like "session" and "agente" connected every
markdown file to code. This was the first bug found in the case study.

## 3. Results

### 3.1 Commit bench (hit@k, MRR). The graph does not help here

| repo (queries) | method | hit@1 | hit@5 | hit@10 | MRR |
|---|---|---|---|---|---|
| greymod (30) | BM25 (current Find) | 73.3% | 90.0% | 90.0% | 0.805 |
| | learned (LORO) | 73.3% | 90.0% | 93.3% | 0.809 |
| katana (83) | BM25 | 57.8% | 84.3% | 90.4% | 0.701 |
| | hybrid bm25 + 0.5 PPR | 57.8% | 83.1% | 90.4% | 0.700 |
| | learned (LORO) | 57.8% | 84.3% | 90.4% | 0.692 |
| sonora (150) | BM25 | 48.0% | 76.0% | 87.3% | 0.608 |
| | hybrid | 50.0% | 76.0% | 86.0% | 0.617 |
| | learned (LORO) | 48.7% | 77.3% | 88.7% | 0.609 |
| | learned without graph features | 48.0% | 82.7% | 91.3% | 0.623 |

Commit subjects contain the words of the files they touch, so lexical search is already near its ceiling. Removing any
single edge type changes the numbers by at most one or two queries. The standardized ranker weights (all repositories)
were: log_size +0.59, ppr_all_rr +0.39, ppr_path +0.29, ppr_rare +0.28, ppr_all +0.28, bm25_rr +0.28, is_test +0.20,
ppr_co -0.14, recency +0.13, is_doc -0.06.

### 3.2 The 76 investigation questions (recall of `answer_files`)

| set | method | R@3 | R@5 | R@10 | min_files within top 10 |
|---|---|---|---|---|---|
| all 76 | BM25 (current Find) | 9.0% | 16.9% | 28.9% | 27.6% |
| | hybrid bm25 + 0.5 PPR | 10.3% | 18.2% | 32.9% | 30.3% |
| | learned, no graph | 14.3% | 19.5% | 30.9% | 30.3% |
| | learned (LORO) | 11.8% | 20.2% | 34.2% | 31.6% |
| train 52 | BM25 / learned | 11.5 / 13.5% | 14.4 / 18.3% | 25.0 / 30.8% | |
| validation 24 | BM25 / learned | 3.5 / 8.3% | 22.2 / 24.3% | 37.5 / 41.7% | |
| 75, without grey-gkatana | BM25 / learned | 9.1 / 12.0% | 17.1 / 20.4% | 29.3 / 34.7% | 28.0 / 32.0% |

### 3.3 Sparse activation (the owner's "which neurons fire")

46-question sample (greymod 18, katana 22, sonora 6):

| method | R@3 | R@5 | R@10 | hop@10 (7 files) | top1 has gold | top5 has gold |
|---|---|---|---|---|---|---|
| BM25 (current Find) | 8.3% | 15.9% | 28.3% | 28.6% | 4.3% | 23.9% |
| personalized PageRank | 8.7% | 15.9% | 33.7% | 14.3% | 2.2% | 23.9% |
| activation, no graph | 14.9% | 23.6% | 40.6% | 14.3% | 8.7% | 32.6% |
| activation, no token association | 18.1% | 31.9% | 44.9% | 14.3% | 6.5% | 41.3% |
| **activation (all)** | **18.1%** | **33.0%** | **44.9%** | 14.3% | 4.3% | **41.3%** |
| activation, without grey-gkatana (45) | 18.5% | 33.7% | 44.4% | | | 42.2% |
| validation only (16): BM25 / activation | 5.2 / 20.8% | 24.0 / 41.7% | 34.4 / 46.9% | | | 37.5 / 43.8% |

This is the best result of the study: R@5 roughly doubles (15.9% to 33.0%), on train and on validation, and the gain
holds with grey-gkatana removed. Where it comes from:
1. Dropping request words from the input ("descubra", "diga", "nao altere nada") moves R@5 from 15.9% to 23.6%. That is
   most of the "no graph" gain. Note that activation step 2 also uses per-token weights, not plain BM25.
2. Two rounds of file-to-file spreading over the graph with k-winners-take-all add 23.6% to 33.0%.
3. Token-to-token association (the PPMI matrix) adds nothing measurable (31.9% to 33.0%, within noise).

Activation takes 12 ms per query in Python (sparse dict loops). Each query keeps on average 39 nodes above threshold.
The engine never abstained, because something always passes the relative threshold.

### 3.4 Hop slice (what the owner cares about)

| method | hop R@5 | hop R@10 | hop file reached by the simulated policy |
|---|---|---|---|
| BM25 | 3.7% | 14.8% | 11.1% (paged) |
| learned (LORO) | 7.4% | 11.1% | 25.9% (paged) |
| investigator policy | | | 14.8% |
| activation (46-sample, 7 hop files) | | 14.3% | |

**This is a negative result.** No signal reliably reaches files that share no informative word with the
question. The hop files are mostly English code behind a Portuguese question. The graph cannot bridge that gap,
because the seed itself is wrong. See the FolderModel section for the embedding attempt.

### 3.5 Simulated investigation policy (where / what / how / when)

One step = one tool call that reads the best 40-line chunk of up to 2 new files (cost: chars/3.5 + 30 tokens). Success
= `min_files` answer files read within 10 steps and 8k tokens. All 76 questions:

| policy | success | mean steps (successes) | mean tokens (successes) | steps, failures = 11 |
|---|---|---|---|---|
| P0 BM25, paged | 27.6% | 2.5 | 3451 | 8.66 |
| P1 learned ranker, paged | **38.2%** | 3.6 | 4665 | 8.16 |
| P2 investigator (anchors + expansion + stuck) | 34.2% | 3.3 | 4869 | 8.36 |
| P2 without expansion | 30.3% | | | |
| P2 without anchors | 35.5% | | | |
| P2 without stuck trigger | 34.2% | | | |
| P2 on a BM25 base | 30.3% | | | |
| P2 expansion without `co` | 36.8% | | | |
| P2 expansion without `rare` | 32.9% | | | |
| without grey-gkatana (75): P0 / P1 / P2 | 28.0 / 38.7 / 34.7% | | | |
| validation (24): P0 / P1 / P2 | 29.2 / 41.7 / 41.7% | | | |

What this shows:
- A better initial ranking (P1) is worth more than any trigger. Graph expansion after each read helps on top of
  the same base (30.3% to 34.2%).
- The anchor trigger (jump to the files containing the rarest question term) hurts slightly. The rarest term is
  often an incidental word.
- The co-change edges hurt the expansion slightly. They are noisy in repositories where commits are large.
- The stuck trigger is neutral.

The policy was frozen before the case study was run. No parameter was tuned on grey-gkatana.

### 3.6 Confidence and abstention

Confidence = 2 x relative margin (1st vs 2nd BM25 file) + 0.15 x score per query term + agreement terms. The
agreement terms contributed nothing: the BM25 top 1 is almost always the PPR top 1, because PPR is seeded from it.
Risk-coverage curve (top-1 precision / top-5 precision among the most confident X% of queries):

| set | 10% | 25% | 50% | 100% |
|---|---|---|---|---|
| commits katana (83) | 100 / 100% | 90 / 95% | 76 / 88% | 58 / 84% |
| commits greymod (30) | 67 / 67% | 62 / 88% | 73 / 93% | 73 / 90% |
| commits sonora (150) | 67 / 80% | 68 / 84% | 63 / 81% | 48 / 76% |
| **76 questions without gkatana (75)** | 0 / 38% | 0 / 42% | 3 / 39% | 4 / 25% |
| activation, 45 questions without gkatana | | 0 / 45% | 5 / 45% | 4 / 42% |

Thresholds calibrated LORO, aiming for top-1 precision >= 90% for unrequested seeds:
- On commits, the threshold covers 7-20% of the target repository, with real top-1 precision of 67-100%.
- On the investigation questions, the same threshold would inject a seed into 21 of 75 questions, with top-1
  precision **0%** and top-5 precision 43%.

**Conclusion: for investigation questions, nothing reaches 90% precision at any coverage.** The engine must not
inject an unrequested "answer". The most it can offer is a ranked list of at most 5 candidates, each with its
evidence. Even that only when the model asks (Find call) or as a clearly marked suggestion. The confidence measured
on commits does not transfer to investigation questions, so calibrate on investigation runs, not on commits.

### 3.7 Case study: grey-gkatana (illustration only, measured after everything was frozen)

The question asks why `gclaude` works with the phone and `gkatana` does not appear. Gold: `bridge/sessions.mjs`,
`bridge/server.mjs`, `bridge/grey-channel.mjs`.

- **Anchors extracted:** gkatana (rare, 2 files), gclaude (filename, 10), comando. Request words and words found
  only in prose were dropped.
- **BM25 top 10:** install scripts and docs. None of the three gold files is in it.
- **P0 BM25 paged:** reads 12 files, none of them gold.
- **P2 investigator:**
  1. Step 1 reads `linux/install.sh` (anchor) and `bridge/README.md` (path reference from install.sh).
  2. Step 2 reaches `bridge/server.mjs` (graph).
  3. It then drifts into the Android launcher through the symbol edges. It never reaches `sessions.mjs`.
- **Sparse activation:** `bridge/grey-channel.mjs` ranks 6th (matched in its first chunk) and `bridge/server.mjs`
  9th. `bridge/sessions.mjs` is not among the 22 active files.
- **Links between the gold files that the graph does find:**
  - `grey-channel.mjs` <-> `install.sh` (path)
  - `server.mjs` <-> `sessions.mjs` (sym+path+rare+co, symbol `describeSessions`)
  - `grey-channel.mjs` <-> `server.mjs` (sym/rare/co)

So the chain install.sh -> grey-channel -> server -> sessions exists in the graph. Following it needs either a
better first hop or a model that reads `server.mjs` and asks for refs of `listSessions`. That is the case for
offering graph neighbours as evidence after each read, rather than injecting a final answer.

## 4. FolderModel (the folder as a micro-model)

Implemented in `tools/findbench/folder.py`:
- **Tokens and matrix:** the folder's term stream (identifier split, stemming), vocabulary of terms with df >= 2,
  capped at 30k. PPMI over a symmetric window, then a randomized eigendecomposition of the symmetric matrix
  (Halko: range finder with two power iterations, then a small eigh), 64-128 dimensions. A file vector is the
  idf-weighted mean of its term vectors.
- **Joint space:** a single matrix over terms and files with three blocks (term-term window counts, term-file log tf,
  file-file graph edges), each scaled to the same mass, then PPMI and the eigendecomposition. Terms and files end up
  in one space, and the nearest neighbours of a node answer "what is related to this".
- **Qwen table:** `token_embd.weight` (Q8_0, 248320 x 2048) is read directly from the GGUF with the gguf-py reader in
  `ref/llama-src`, and only the rows the folder needs are dequantized. A term maps to its tokens by greedy
  longest-prefix match on the vocabulary (with the space prefix first). The token vectors are averaged and
  projected by PCA to the folder dimension. Variants: Qwen only, and the Qwen projection concatenated with the
  folder vector.
- **Profile card:** languages by bytes, build and entry files, areas by spherical k-means on the joint file
  vectors. Each area gets the 4 most salient terms, its main directories, and its 3 most connected files, plus the
  strongest links between areas. At most 12 lines, deterministic (fixed seed).
- **Fold-in:** a changed file's vector is recomputed from the current term vectors (one sparse row times dense).
  A new term's vector is its PPMI row against the known context vectors. The full eigendecomposition is redone only
  when many files change.

Results on the 46-question sample (R@10 of answer files; `mu` = weight of the cosine in `bm25_norm + mu * cos_norm`):

| variant | train R@10 | val R@10 | hop@10 | all without gkatana R@10 |
|---|---|---|---|---|
| BM25 | 25.0% | 34.4% | 28.6% | 28.9% |
| folder tokens d128 w5, cosine only | 3.3% | 3.1% | 0% | 3.3% |
| folder tokens d128 w5, mu 0.25 (best on train) | 28.3% | 32.3% | 28.6% | 30.4% |
| folder tokens, mu 0.5 / 1.0 | 16.7 / 15.0% | 32.3 / 21.9% | 0% | 22.6 / 17.8% |
| window 2 / 10, d64, cds 1.0 (mu 0.25) | 23-28% | 32-38% | 14-29% | 27-30% |
| Qwen table only | 3.3% | 0% | 0% | 2.2% |
| Qwen mu 0.25 | 21.7% | 32.3% | 14.3% | 25.9% |
| Qwen projected + folder, mu 0.25 | 25.0% | 32.3% | 28.6% | 28.1% |
| **joint space (terms + files + graph), mu 0.25** | 26.7% | **40.6%** | 14.3% | **32.2%** |
| joint space, cosine only | 11.7% | 21.9% | 0% | 15.6% |
| sparse activation (section 3.3), for comparison | 43.9% | 46.9% | 14.3% | 44.4% |

Findings:
- Static token embeddings of a single folder are noise for this task. On their own they retrieve almost nothing
  (about 3% R@10). Mixed in with weight >= 0.5 they hurt BM25. With weight 0.25 they tie it (the best
  train setting loses 2 points on validation).
- The Qwen embedding table does not help. Its token vectors encode subword identity, not the folder's
  relations, and the Portuguese-question / English-code gap is not closed by averaging input embeddings.
- The joint space is the only embedding variant with a signal: alone it reaches 15.6% R@10, and mixed it gives
  +3 points (32.2%). The signal comes from the graph block. The same graph used directly by the sparse activation
  gives much more (44.4%), at a fraction of the cost.
- **Hop slice: no embedding variant reaches hop files better than BM25.**
- **Fold-in** (`folder.py foldin`, folder tokens d128 w5):

  | copy | files changed | new terms | fold-in cost | full rebuild | cosine changed / all | top-10 neighbours kept |
  |---|---|---|---|---|---|---|
  | greymod (179 files) | 1 (1%) | 3 | 62 ms | 2.0 s | 0.84 / 0.93 | 80% |
  | greymod | 17 (10%) | 402 | 56 ms | 2.0 s | 0.92 / 0.93 | 82% |
  | sonora (4723 files) | 47 (1%) | 99 | 1.2 s | 42.6 s | 0.89 / 0.90 | 87% |
  | sonora | 472 (10%) | 1599 | 0.9 s | 42.6 s | 0.90 / 0.90 | 83% |

  The "all files" cosine includes files that did not change. So about 0.9 is the noise floor of the randomized
  decomposition itself (two builds of nearly the same matrix), not fold-in error. Fold-in is cheap and good enough.
  A full rebuild is only needed when about 10% of the files have changed.
- **Profile card:** generated (example below for a 179-file copy). Languages and entry points are right. The
  k-means areas are coarse: labels such as "modifier foundation compose padd" or "json project slice timeout",
  and mixed directories. `cardeval` (does the card point to the right area for the 76 questions) **was not run**
  for lack of time. Until it is, the card should be built from the directory tree plus graph communities, not
  from embedding clusters.

  ```
  Folder: 179 text files; languages: Kotlin 93%, JavaScript 5%, PowerShell 1%, Markdown 0%
  Build/entry: bridge/server.mjs
  Area 2 (63 files, .../ui, .../ui/kit): modifier foundation compose padd -- HomeScreen.kt, DialerActivity.kt, ...
  Area 3 (47 files, .../clock, .../phone): channel zone update put -- GreyAgent.kt, Settings.kt, PhoneActions.kt
  Area 1 (41 files, bridge, .../ui): json project slice timeout -- Brain.kt, ShadeActivity.kt, bridge/sessions.mjs
  Area 0 (28 files, res/drawable, res/xml): adb 108 shell apk -- GreyIcons.kt, GIcons.kt, testar-grey.ps1
  Links: 2-3 (1131); 1-2 (1025); 1-3 (954)
  ```
- **Cost:** Python build time is 6-8 s on small copies and 28-51 s on sonora: vocabulary 30k terms, 2-4 M PPMI
  non-zeros, d = 64-128. A randomized SVD in C is feasible. It needs one sparse-times-dense routine (CSR, float32)
  plus a QR and a 144x144 eigh. Estimate: 1-3 s for sonora on one core, and about 16 MB for the vectors. It
  works, but it buys nothing measured.

**FolderModel recommendation:** do not port the embeddings now. Port the FolderModel as a **sparse network**:
- token and file nodes;
- the inverted index as token-to-chunk weights;
- the four graph edge types as file-to-file weights;
- activation with k-winners and evidence (section 3.3).

That is the form that measured a gain. Keep `folder.py` as the testbed in case a larger corpus or a bilingual
term map makes embeddings worthwhile later. The language gap is the biggest remaining failure. A small PT-EN
term map learned from the folder's own comments and identifiers is the next experiment to try.

## 5. Tool router (the HOW axis)

No completed eval run with tool logs existed when this was written. `/mnt/dados/investigate-eval/B1-val` was still
running. So there is **no measured correlation** between tool choice and success yet.

`investigator.py toolstats` reads any `eval.sh` output folder (`summary.tsv` + `<id>.log`). For each verdict it
reports the mean count per tool and per bad-call tag (`grep_phrase`, `grep_wide`, `glob_word`, `read_whole`).
Run it after the current eval finishes. The rules below are the specification. They are motivated by the
failure modes already seen (laziness, whole-file reads, loops) and must be validated with that count.

| # | condition (engine sees) | action | mode |
|---|---|---|---|
| R1 | Grep pattern is a natural-language phrase (3+ words, no regex metacharacter, no identifier shape) | run Find with the phrase, return its top 5 with evidence. Append a note that Grep searches literal text and suggest the identifiers seen in the result | intercept |
| R2 | Grep with no path or glob over a repository with > 2k files and the pattern has df > 5% | run it, but return counts per directory first plus a note suggesting `path` | intercept |
| R3 | Glob `**/*word*` over the whole repository | run it. If more than 50 matches, append the 5 best by path BM25 for the word and suggest Find | intercept |
| R4 | Read of a file > 400 lines without offset/limit | already exists (map of the file). Keep it | intercept |
| R5 | Same read or search as an earlier call in this request | already exists (repetition note). Add the 3 unvisited graph neighbours of the files read so far as "not yet read" | intercept |
| R6 | Three consecutive calls without a new file | stuck note: list the activation candidates not yet read, with evidence | intercept |
| R7 | Question contains an identifier that exists in the symbol table | the seed shows definition and refs (file:line) | seed |
| R8 | Question contains a file name or path fragment that matches paths | the seed shows the matching paths (Glob result) | seed |
| R9 | Question contains a quoted message or error text | the seed shows a literal Grep result | seed |
| R10 | Otherwise | the seed is the sparse-activation top 5 with evidence, only above the seed threshold; otherwise nothing | seed |

Advice table for the system prompt (short):
`Find: you do not know where; describe it in words. Grep: you know an exact identifier or message.
Glob: you know part of a file name. Read: you know the file; read with offset/limit around a line you found.`

## 6. Recommendation

| Idea | Verdict | Evidence |
|---|---|---|
| Request-word filter on the query | **port** (trivial) | R@5 15.9% -> 23.6% (46 questions) |
| Sparse activation over the file graph (sym + path + rare + co, 2 rounds, k-WTA, evidence path) | **port** as the ranking behind Find and the per-read "neighbours" note | R@5 -> 33.0%, top5-has-gold 24% -> 41%; same without gkatana and on validation |
| Learned logistic ranker | optional | +5 points R@10 on questions, neutral on commits; the activation does better without training |
| Co-change graph | keep as one edge type with low weight | neutral or slightly negative alone |
| Token-token association (PPMI) inside the activation | do not port yet | no measured gain |
| Folder embeddings / Qwen table | do not port | single-folder embeddings ~3% R@10 alone; best mix ties BM25; joint space +3 pts, below activation
| Unrequested seed injected as an answer | **do not do** | 0% top-1 precision at any threshold on the questions |
| Anchor jump trigger | do not port | -1.3 points |
| Stuck trigger | port only as a note (R6), not as an automatic jump | neutral in simulation |

## 7. Specification for C

Language- and project-independent. No names, paths or words of any measured repository appear in it.

### 7.1 Data structures (built with the Find index, same pass)

```
file table      : n files; per file: path id, size, flags (code/doc/test/cfg), last-change rank (uint16)
ident table     : hash of identifier strings (raw case) -> {df_files uint32, def_files[<=3] uint32}
edge lists      : 4 COO arrays (sym, path, rare, co) of (uint32 a, uint32 b, float w); build, then one CSR
                  over files with per-type row normalisation, weights beta = {sym 1, path 1, rare 1, co 0.5}
                  sonora-size repository: ~120k undirected edges -> ~2.9 MB (CSR, int32 + float32)
request words   : static list (PT + EN), compiled into the binary
```

Construction cost in Python, sonora (4.7k files, 35 MB): about 8 s for the graph and about 30 s for the BM25 index. In C the
graph is two hash passes over the identifier stream plus the git log parse (`git log --name-only`, already spawned for
co-change). Expected cost: 1-2 s on top of the current index.

### 7.2 Query: sparse activation (milliseconds)

```
input    : query text
tokens   : terms(query) minus request words; weight = idf (absent terms dropped)
step 2   : chunk score = per-field BM25 with per-token weight (current Find), file = max chunk
           keep top K_FILE=40, normalise by max, drop < THETA=0.15
step 3   : repeat ROUNDS=2:
              for the top K_GRAPH=20 active files i: for edge (i,j,w): act[j] += DECAY(0.5) * act[i] * w
              keep top K_GRAPH new nodes (lateral inhibition); remember for j the strongest source i and edge type
output   : files with act >= THETA * max, sorted; for each: evidence =
              ("match", chunk lines, matched terms)  or  ("edge", type, linking identifier or path, source file)
confidence: margin = (a1 - a2) / a1, concentration = a1 / sum(top10)
```

### 7.3 Ranker features (if the learned ranker is ported; standardized weights from section 3.1)

`log_size +0.59, ppr_all_rr +0.39, ppr_path +0.29, ppr_rare +0.28, ppr_all +0.28, bm25_rr +0.28, is_test +0.20,
ppr_co -0.14, recency +0.13, is_cfg +0.08, is_doc -0.06, ppr_sym +0.03, bm25 +0.02`. Store mean and sd per feature.

### 7.4 Policy (where / what / how / when) and injection into `src/agent/loop.c`

```
WHAT  : anchors = query tokens minus request words, minus prose-only terms; kind by lookup:
        identifier (in ident table) | filename (in path terms) | rare (df <= max(3, 1%)) | term
HOW   : router table, section 5 (R7-R10 for the seed, R1-R6 for intercepts)
WHERE : sparse activation top 5
WHEN  : (1) seed: before the first model turn, only if confidence >= SEED_THRESHOLD, which is calibrated on the
            investigation eval (not on commits). With today's numbers it is never reached, so the seed is OFF and
            the engine only adds the R7-R9 deterministic facts (definition/refs, matching paths, literal hits),
            which are facts rather than guesses
        (2) after each Read/Grep result: append up to 3 graph neighbours not yet read, with the edge as evidence
        (3) stuck (R6), (4) before the final answer: if the answer cites no file read in this request, note it
```

Injection points:
- Per-result notes go where `loop_plan_note` already appends to the tool result, in the same format as the
  existing notes:
  ```
  [katana: related files not read yet (evidence, not facts): bridge-like/x.c via symbol foo_bar (defined there);
   y.c changed together 6 times. Ask Find or Read them if relevant.]
  ```
- The seed goes in the same place as the `cd_note` (a note prepended to the user turn), starting with
  `[katana: candidates for this request ... evidence: ...]`. It is sent only when the threshold passes.

## 8. What did not work, plainly

- The graph, the learned ranker, and the folder signals do not improve the commit bench.
- Nothing solves the hop slice. Most hop files are a language gap (Portuguese questions, English code), not a
  structural one.
- Confidence calibrated on commits does not transfer: on investigation questions the top-1 is right about 4% of
  the time, and the confidence does not identify those cases.
- The investigator policy's anchor jump is slightly harmful. The learned base and the graph expansion carry the
  gain.
- The simulation reads the best chunk of each suggested file. A real model may ignore suggestions or read more,
  so these step counts are optimistic for every policy alike.
