# Pattern knowledge base (`data/coder/kb`)

Plain-text, line-oriented data used by the coder module's semantic search (no AI, no embeddings in the core).
It records what is known about *words*: which domain concepts they point to, which operation a verb or an
identifier suggests, how abbreviations expand, and how words inflect. The loader is `src/coder/kb.c`;
inspect it with `katana coder kb <check|lookup|expand|stats|edges>`.

## Syntax

UTF-8, one directive per line. `#` starts a comment (to end of line). Blank lines are ignored. Indentation is decorative.
Text is normalised on load: lower case and accents folded to ASCII (`aprovação` and `aprovacao` are the same). Write data without accents.

### Concepts (`concepts/<domain>.kb`)

```
concept INVENTORY_UPDATE
  category BUSINESS_OPERATION
  canonical inventory_update
  term pt estoque 1.0
  term pt quantidade disponivel 0.95
  term en inventory 1.0
  term code available_quantity 0.95
  action update
  related ORDER_APPROVAL 0.6
  source seed
  note free text (optional)
end
```

| field | meaning |
|---|---|
| `concept ID` | upper case, digits and `_`; opens the block |
| `category` | free label used for counts (`BUSINESS_OPERATION`, ...) |
| `canonical` | canonical snake_case name |
| `term <pt\|en\|code> <text> <weight>` | text may contain spaces; the last word is the weight in 0..1 |
| `action <verb>` | canonical verb associated with the concept |
| `related <ID> <weight>` | weak link to another concept (checked by `check`) |
| `source <seed\|mined\|domain\|user>` | provenance (see below) |
| `note <text>` | free text |

### Operations (`operations.kb`)

```
op REFUND
  category BUSINESS_OPERATION
  verb en refund|reimburse|reverse
  verb pt estornar|estorno|reembolsar|desfazer cobranca
  pattern en refund_*
  antonym PAY
end
```

`verb <lang> a|b|c` lists words and short phrases (include noun forms). `pattern <lang> <glob>` matches identifiers.
`antonym` points at another op. A verb listed in several ops is reported by `check` as ambiguous.

### Naming patterns (`naming.kb`)

`pattern <glob> <OPERATION> <role>`: what a name shape suggests (`get_*` is a `READ` accessor, `*_factory` a `BUILD` factory).
Globs are snake_case lower case, `*` = any run, `?` = one char. Identifiers are converted to snake_case before matching
(`getUserName` -> `get_user_name`, `UserManager` -> `user_manager`).

### Abbreviations (`abbreviations.kb`)

`abbr <abbreviation> <expansion>[,<expansion2>...]`. The first expansion is the most likely. Several lines may share the
same key (expansions are merged). Consumed by the tokenizer (`src/coder/text.c`) to expand identifier parts.

### Morphology (`morphology.<lang>.rules`, lang = `pt` or `en`)

```
suffix <suffix> <replacement|-> <minimum stem length> <order>
irregular <form> <canonical>
```

Algorithm for a word: (1) if an `irregular` matches the whole form, return its canonical form; (2) otherwise scan the
`suffix` rules of that language in ascending `<order>` (ties: longer suffix first); the first rule whose suffix matches
and leaves at least `<minimum stem length>` letters wins: result = word minus suffix plus replacement (`-` = empty).
The replacement may be a comma list (`er,ir`): candidates in preference order, the consumer keeps the first one present
in its vocabulary, else the first. One rule is applied, not a cascade. Results are search forms, not perfect lemmas.
`coder_kb_stems()` implements this.

### Relations (`relations.kb`)

`edge <type> <weight> [description]`: initial weights for code-graph edges (call 1.00, implements 0.95, event 0.95,
module 0.70, import 0.70, git co-change 0.50, same domain 0.30, ...). These are **illustrative starting parameters to be
calibrated**, not probabilities.

## Provenance and merging

* `source seed`: written by hand by the project (this directory). `mined`: extracted automatically from code or git history.
  `domain`: written for one project. `user`: added by a person later.
* Load order: this directory first (priority 0), then the project's `.katana/coder/*.kb` and `*.rules` (priority 1).
* A concept, op or abbreviation key defined in the project **replaces** the seed one with the same ID/key
  (the seed entry is ignored). Naming patterns, edges, suffix rules and irregulars are added.
* Inside the same priority, a repeated concept or op ID keeps the last one and `check` warns.

## Locating the base

`KATANA_KB_DIR` if set; otherwise `../data/coder/kb` or `data/coder/kb` relative to the executable; otherwise `data/coder/kb`
under the current directory. The project overlay is `.katana/coder/` in the working directory, when present.

## Extending

1. Add concepts under `concepts/<domain>.kb` (one file per domain); add verbs to an existing `op` or add a new `op`.
2. Prefer real vocabulary found in code (identifiers, column names, route names) and give both `pt` and `en` terms.
3. Run `katana coder kb check`: it must report 0 errors; fix duplicate terms, missing `related` targets and ambiguous terms
   (a term that points at more than four concepts is too generic to be useful).
4. Try `katana coder kb lookup <term>` and `katana coder kb expand "free phrase"` to see the bridge between languages.
