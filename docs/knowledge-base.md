# Knowledge base from earlier sessions

(`docs/knowledge.md` is a different file: it holds measurements.)

The owner has many sessions of a coding agent on this machine (transcript files, one JSON record per line). The
knowledge base turns them into something katana can use: plain text notes (the source of truth) and, derived from
them, training pairs. Everything is extraction: no model is loaded, nothing leaves the machine, and a full build of
~1200 transcripts takes about 20 seconds.

The notes are the lasting asset. They are Markdown files in `~/.katana/knowledge/` (override with `--out` or
`KATANA_KNOWLEDGE`), updated incrementally as new sessions appear. The training corpus is regenerated from the notes
at any time and is not kept as a source.

## Commands

```
katana knowledge build <root|folder|file.jsonl> [--out DIR] [--force] [--replace 'REGEX=TEXT']... [--cut-matching REGEX]
katana knowledge search <words...> [--project NAME] [--limit N] [--dir DIR]
katana knowledge stats [DIR | corpus.jsonl]
katana knowledge corpus <notes DIR> <out.jsonl> [--project NAME]... [--max-per-project N] [--holdout FILE.jsonl]
```

- `build` reads the transcripts with the same reader as `katana import` (`src/app/transcript.c`: automatic notices and
  interrupted calls are dropped, repeated records of resumed sessions count once) and writes one note per session.
  It is incremental: a marker in `DIR/.src/` records the note of each transcript; a transcript older than its marker
  is skipped (`--force` redoes everything). Sub-agent transcripts are skipped (their final report is already in the
  result of the main session). `--replace` rewrites text everywhere (paths, names); `--cut-matching` drops every text
  line that matches.
- `search` ranks episodes: all words present first, then the number of words matched, then newest first. It prints the
  note path, the request and the first 300 characters of the outcome. It is a terminal tool for the owner and for tests.
- `stats` on a folder prints projects, notes, episodes, bytes and the newest note. On a corpus file it prints pairs per
  project, characters of answers (what the training learns) and an estimate of 512-token training windows
  (characters / 3.3 / 512; an estimate) with the cost in nights at 100 to 150 windows per night.
- `corpus` writes the training form of the base (below).

## Note format

`DIR/<project>/<date>-<slug of the first request>-<hash>.md`, plus `DIR/<project>/INDEX.md` and `DIR/INDEX.md`. The
project is the last component of the working directory recorded in the transcript (fallback: the transcript folder).
The header has `Project:` (the folder), `Date:`, `Session:` and `Episodes:`. Each user request is an episode:

- `**Request**`: the user's words as written (trimmed to 800 bytes), quoted with `> `;
- `**Outcome**`: the last message of the model in the episode (trimmed to 1500 bytes). It is whatever the earlier model
  wrote; nothing is summarised here;
- `**Files written or edited**`: paths of successful `Write`/`Edit` calls, deduplicated (at most 30);
- `**Commands**`: successful `Bash` calls, at most 12, as `description: command` (command cut at 200 characters);
  commands with a description come first, then the ones that are not plain `ls`/`cd`/`cat`-like noise;
- `**Problems and fixes**`: a failing command followed later by a success with the same first word, as
  `` `failing` failed (error) -> `working` worked``, at most 5.

## Masking

Applied to every text written, notes and corpus alike (the corpus is built from the notes): strings in the 8-4-4-4-12
hexadecimal key shape become `[key]`; the value after `token`, `password`, `passwd`, `secret`, `api_key`, `senha` and
`Authorization:` (also `Bearer`/`Basic` values and `--password value` flags) becomes `[masked]`; known key prefixes
(`sk-`, `ghp_`, `AKIA`, `xox*-`, JSON web tokens) and random-looking alphanumeric runs of 32 or more characters (upper
case, lower case and digits) are masked too. Use `--replace` for anything machine specific. Masking is pattern based:
review a sample before sharing the notes.

## Training corpus

`corpus` turns the notes into `{"messages": [user, assistant], "project": NAME}` lines (the messages format of
`katana import`; the extra `project` key is ignored there). Per project it writes:

- overview pairs: what the project is (folder, number of sessions and dates, the earliest requests), where it is, the
  files touched most often;
- per episode: what was done about a request (the outcome), which files were changed (the list), how to do something
  (a described command -> the command), and a problem -> what worked pair.

The question always names the project and is written in the language of the request (Portuguese or English, a simple
word-list detection). Answers are the text of the notes, never invented; a field that is empty gives no pair. Each fact
gets two phrasings in the training file. Near-identical facts (same normalised question) are kept once. The cap is
`--max-per-project` pairs (default 400), filled with the overview first and then the newest sessions.

`--holdout FILE` writes one more pair per fact with a third phrasing that the training file does not contain. To
measure what training achieved: train on the corpus, ask the held-out questions, count the answers that match the
expected text (or contain its key paths and commands).

## What was measured on the real data (2026-11-21)

1238 transcript files (497 of sub-agents skipped), 96 projects: 734 notes written (729 distinct names; copies of the
same session in two folders collapse), about 1440 episodes, 3.2 MB of notes, 22 seconds, about 7 MB resident. A second
run skipped 738 transcripts and rewrote only the 3 that were still growing. Corpus with the default cap: 3000 pairs,
1.2 million characters of answers, about 715 windows of 512 tokens (estimate), so roughly 5 to 7 nights at 100 to 150
windows per night (estimate). Projects are named after the working directory, so each git worktree of one repository
is a separate project.

## Limits

- Extraction only. The outcome is the last message of the episode as the earlier model wrote it: good when it is a
  report, poor when the episode ended on a question or on a tool result. Notes can be stale (a command or a decision
  may have been replaced later).
- Masking is heuristic.
- Expectation, not a measurement: with rank-8 attention adapters and 100 to 150 windows per night, training can make the
  model familiar with project names, paths, commands and decisions that are repeated across pairs. It cannot make it
  recall hundreds of megabytes of sessions. Whether it does even that is open until the held-out measurement above is
  run.
