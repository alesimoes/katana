# Concept base

Hand-written knowledge base used by the code search engine (module `coder`). A **concept** is an
intent or behavior, not a synonym list: it links what a person asks (Portuguese, English or mixed:
"why is the stock not updated when an order is approved?") to the names the code uses (`inventory`,
`available_quantity`, `decrease_stock`, `process_order_confirmation`, `finalize_transaction`).
No AI and no embeddings are involved: everything here is plain text, reviewed by hand.

## Files

One file per domain: `business`, `security`, `data`, `network`, `concurrency`, `ui`, `system`,
`build`, `error`, `algorithm`, `media`, `ai`, `mobile`, `project` (`.kb`). A concept lives in the file
of its main domain; the category (below) is independent of the file. Concept IDs are unique across
the whole base.

## Format

Line oriented, UTF-8. `#` starts a comment, blank lines are ignored. Fields are indented but the
indentation is not significant.

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
  note free text, optional
end
```

| Line | Rule |
|---|---|
| `concept ID` | `A-Z`, digits and `_`; unique in the whole base |
| `category` | one of the categories below (required) |
| `canonical` | snake_case English name, normally the lower-case ID (required) |
| `term <pt\|en\|code> <text> <weight>` | text may contain spaces; weight in 0..1; no duplicate (lang, text) in a concept. Accents may appear in `pt` (the tokenizer strips them) |
| `action <verb>` | canonical English verb the operation usually performs (repeatable) |
| `related <ID> <weight>` | neighbouring concept, must exist (repeatable) |
| `source` | `seed` (author knowledge) or `mined` (came from `tools/coder/mine_vocab.py` output) (required) |
| `note` | free text; used to record ambiguities and which concept to use instead |

Terms: `pt` = what a person writes in Portuguese (nouns and verbs: "estornar", "estorno",
"devolucao"); `en` = the same in English; `code` = forms common in identifiers (`available_quantity`,
`OrderApprovalService`, `stockLevel`). Aim for 6 to 20 terms per concept.

## Categories

`BUSINESS_OPERATION`, `BUSINESS_ENTITY`, `SECURITY`, `DATA`, `NETWORK`, `CONCURRENCY`, `UI`,
`SYSTEM`, `BUILD_TOOLING`, `ERROR_HANDLING`, `ALGORITHM`, `MEDIA`, `AI_ML`, `MOBILE`.
Operations (verbs, workflows) use `*_OPERATION`/domain categories; things (nouns) in business use
`BUSINESS_ENTITY`.

## Weight criterion

| Weight | Meaning |
|---|---|
| 1.0 | the term is the concept itself ("estoque", "inventory" for INVENTORY_UPDATE) |
| 0.9 to 0.7 | close: strong evidence for the concept, but it can also point somewhere else |
| 0.5 to 0.3 | vague or ambiguous ("token", "stock", "sessao", "registro") |
| below 0.3 | only to prevent a wrong guess; prefer a note |

`related` weights: 0.8 to 0.9 = almost the same area (ORDER <-> ORDER_APPROVAL), 0.5 to 0.7 = usual
neighbour in the same flow (approval -> stock -> payment), 0.2 to 0.4 = weak association.

Ambiguity: create **distinct concepts** instead of one overloaded concept (`AUTH_TOKEN` vs
`LM_TOKEN`, `INVENTORY_UPDATE` vs `STOCK_MARKET_SHARE`, `TRANSACTION` vs `DB_TRANSACTION`), give
each a low-weight related link to the other and a `note` pointing at it.

`ABBR_*` concepts hold abbreviations found in code (`buf`, `cnt`, `req`); they are low-signal on
purpose and their weights are scaled down.

## Adding a concept

1. Pick the domain file and an unused ID (`grep -h '^concept' *.kb | sort`).
2. Write the pt and en words a person would type, then the identifiers a programmer would write
   (abbreviated and compound forms included). Put the most characteristic term first with weight 1.0.
3. Add `action` verbs the operation performs and at least one `related` neighbour.
4. Use `source seed`; use `source mined` only for concepts created from `mine_vocab.py` output.
5. Validate: `python3 tools/coder/kb_lint.py` (format, unique IDs, weights, existing related targets,
   duplicate terms) and `katana coder kb check`.
6. Check coverage on your repositories: `python3 tools/coder/mine_vocab.py <folder> ...`. It prints the
   fraction of identifier words (weighted by frequency) found in some term, canonical or action, and
   the most frequent uncovered words. Most uncovered words are language keywords, filler words and
   private abbreviations; add a concept only for words that carry a domain intent.
