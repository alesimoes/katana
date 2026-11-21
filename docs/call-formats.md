# Tool-call formats and calls valid by construction

Written 2026-11-21. Question from the owner: would JSON calls be better than the XML calls Qwen3.6 writes? Short
answer: no. Keep each model's native format, keep code as raw text, and make the call valid while it is generated
instead of repairing it afterwards. This file has the formats as the chat templates write them, the token cost of
each one measured with the real tokenizers, what the evidence outside katana says, and the design of the constraint.

## 1. The formats katana supports

The syntax below is copied from the chat templates stored in the GGUF files (`tokenizer.chat_template`, read with
`tools/callformats/template.py`, metadata only). Qwen3.6-35B-A3B and Qwen-AgentWorld-35B-A3B have the same tool part
of the template (the AgentWorld template only adds audio input).

### ChatML with XML calls (Qwen3.6, Qwen-AgentWorld)

- Declaration: in the system turn, `# Tools` then `<tools>` with one JSON object per tool (`tool | tojson`), then
  the instruction to answer in the format below.
- Call (assistant turn):
  ```
  <tool_call>
  <function=NAME>
  <parameter=KEY>
  value
  </parameter>
  </function>
  </tool_call>
  ```
- Typed values: the template writes a mapping or a list as JSON (`tojson`), anything else as text (`| string`). So a
  number is `320`, a boolean `True`/`true` as text, an array is a JSON array, and a string is the raw text, with no
  escaping at all: multi-line code goes in as it is.
- Result: a `user` turn with `<tool_response>\n...\n</tool_response>` (several results in the same turn).
- Reader in katana: `tool_xml_parse` (`src/agent/dispatch.c`); every value arrives as a JSON string and
  `ct_unwrap` (`src/tools/table.c`) unwraps the ones the schema declares array or object.

### ChatML with JSON calls

- Call: `<tool_call>\n{"name": "NAME", "arguments": {...}}\n</tool_call>`. Strings are JSON strings: newlines,
  quotes and backslashes in code must be escaped.
- Used by models whose ChatML template has no `<function=` (none of the two reference models).

### Gemma 4 (Gemma 4 26B-A4B)

- Declaration: `<|tool>declaration:NAME{description:<|"|>...<|"|>,parameters:{properties:{...},required:[...],type:<|"|>OBJECT<|"|>}}<tool|>`,
  one per tool, in the system turn (types in upper case).
- Call: `<|tool_call>call:NAME{key:<|"|>text<|"|>,n:3,flag:true,list:[<|"|>a<|"|>]}<tool_call|>`. Keys are bare
  (sorted by the template), strings are delimited by the special token `<|"|>` and are raw text (no escaping),
  numbers and booleans are bare, arrays and objects use `[]` and `{}` with the same rules inside.
- Result: `<|tool_response>response:NAME{value:<|"|>...<|"|>}<tool_response|>`.
- Reader: `g4_call_parse` (`src/agent/fmt_gemma4.c`).

### GLM

`<tool_call>NAME<arg_key>k</arg_key><arg_value>v</arg_value></tool_call>`; read by `glm_call_parse`. Not a
reference model; left unconstrained.

## 2. Token cost of the envelope (measured)

The same 10 calls rendered in each format (`tools/callformats/render_calls.py`) and counted with
`katana tokenize <model> <file>` (vocabulary only, no weights). "raw" is the argument values alone, one per line:
the payload without any envelope. Counted 2026-11-21.

| call | Qwen3.6 XML | Qwen3.6 JSON | Qwen3.6 G4 | Qwen3.6 raw | Gemma 4 XML | Gemma 4 JSON | Gemma 4 G4 | Gemma 4 raw |
|---|---|---|---|---|---|---|---|---|
| Bash (one line) | 38 | 33 | 37 | 14 | 47 | 41 | 26 | 15 |
| Bash + timeout | 52 | 41 | 42 | 18 | 61 | 49 | 32 | 19 |
| Write, 40 lines of Python | 531 | 520 | 529 | 497 | 571 | 572 | 544 | 527 |
| Edit (quotes, tab escape, 2-line new_string) | 89 | 73 | 86 | 44 | 105 | 88 | 72 | 47 |
| TodoWrite, 5 steps (array) | 105 | 97 | 141 | 80 | 116 | 108 | 91 | 83 |
| AskUserQuestion, 3 options | 89 | 81 | 111 | 63 | 98 | 90 | 74 | 64 |
| Read with offset/limit | 57 | 40 | 40 | 13 | 69 | 51 | 32 | 15 |
| Grep (4 arguments) | 62 | 41 | 52 | 9 | 76 | 52 | 35 | 11 |
| Glob | 27 | 20 | 25 | 3 | 36 | 29 | 15 | 4 |
| WebSearch | 29 | 24 | 29 | 5 | 39 | 33 | 18 | 6 |
| **total** | **1079** | **970** | 1092 | 746 | 1218 | 1113 | **939** | 791 |

The columns that matter are the native ones: Qwen3.6 XML (1079) and Gemma 4 native (939). What the table says:

- For Qwen3.6, JSON calls are about 10% fewer tokens than XML (970 against 1079), mostly in calls with several
  short arguments (`Read`: 40 against 57). `<parameter=k>\n...\n</parameter>\n` costs about 8 tokens per argument.
- For a call that carries code (`Write`), the formats are within 2% of each other: the payload dominates.
- Gemma 4's own format is the cheapest of all for Gemma 4 (its quote is one special token).
- The cost of XML is real but small, and it buys raw strings. The evidence below says escaping code inside JSON
  costs accuracy, which is worth far more than 10% of the envelope. **Answer to the owner's question: keep XML for
  Qwen3.6.** Fewer tokens can be had where they are cheap to get: the plan (section 4).

## 3. Evidence outside katana

- A public benchmark of code editing (133 exercises, 5 runs each, 2024) found that models write worse code when the
  code must be returned inside JSON than as plain text; schema-strict JSON was no better than plain JSON, and both
  were below plain text for nearly every model tested (about 3 to 9 points of pass rate for the most affected ones,
  read from the published chart).
- Hosted vendors added free-form tool calls that send raw text instead of a JSON envelope, optionally constrained by
  a context-free grammar, and guarantee schema-valid JSON arguments by constraining the decoding (the schema becomes
  a grammar and invalid tokens are masked). One of them reported that training alone reached 93% schema validity on
  its test set and the constraint brought it to 100%.
- The coding variant of the Qwen family moved to the XML form with raw parameter values, the format Qwen3.6 uses.
- Whether format constraints hurt reasoning is disputed: one paper reported drops under JSON or XML restrictions; a
  reply re-ran it with matched prompts and found structured output equal or slightly better. The constraint here
  touches only the call, not the reasoning or the text, and leaves string values free.
- Fast grammar engines split the vocabulary into tokens whose validity is known in advance and tokens checked at run
  time; katana does the simpler lazy check of section 5.
- Gemma 4's call syntax is documented by its authors and matches the template above.

The sources (links) are in the report of the work of 2026-11-21, not in the repository.

## 4. Plan as plain lines

`TodoWrite.todos` also accepts the plan as text, one step per line: `[x]` done, `[>]` or `[~]` in progress, `[ ]` or
nothing pending (a leading `- `, `* ` or `1. ` is ignored). The JSON array form still works. The short declaration
of `TodoWrite` in the prompt (`g_ct_short`, `src/tools/table.c`) now declares `todos` as a string and describes only
the line form; the full schema used by `ToolSearch` still describes the array. System prompt: 9114 -> 8990 bytes.

Six-step plan, measured with the real tokenizers (`plan_*` files of `render_calls.py`):

| format | array of {content, status} | lines | saving |
|---|---|---|---|
| Qwen3.6, XML call | 120 | 68 | 43% |
| Qwen3.6, JSON call | 112 | 62 | 45% |
| Gemma 4, native call | 106 | 59 | 44% |

(The array here has no `activeForm`; with it, as the full schema asks, the array costs more.)

## 5. Calls valid by construction (`src/agent/callgrammar.c`)

When the model opens a call (`<tool_call>` in ChatML, `<|tool_call>` in Gemma 4; not inside a reasoning block), a
state machine reads every following byte and answers "can this text still become a valid call?":

- the tool name can only be one of the agent's tools: the ones declared in the `<tools>` block of the system prompt
  plus, with the second tool set (the default), every ready tool of `src/tools/table.c`;
- a parameter name can only be a property of that tool's schema, each at most once, and the call (`</function>`,
  the closing `}` of the arguments, or `}` before `<tool_call|>`) cannot come before the required ones;
- a value whose schema type is array, object, integer, number or boolean must be valid JSON of that schema (the JSON
  grammar of `forward.c`, the same one behind the server's `response_format`); in Gemma 4, `<|"|>` is read as a quote
  and bare keys as keys, so the same grammar checks it;
- a string value is free: any byte, until the format's closing marker (`</parameter>`, `<|"|>`, or the end of the
  JSON string). Code is never escaped and never restricted;
- `TodoWrite.todos` takes either the JSON array or text lines (`[x] step` is told apart from `[{...` by the first
  three bytes).

Lazy use: the token is sampled as usual and only that token is checked (a token is checked byte by byte on a copy of
the state, so a token that spans several symbols, like `>\n<parameter=`, is handled; inside a free string a token
without `<` is accepted without copying anything). Only when the token is rejected does the machine scan the
vocabulary, keep the best allowed tokens and sample again among them at the same temperature. If no token is
allowed it gives up and leaves the call alone (counted in `g_cg.gaveup`). `KATANA_CALL_GRAMMAR=0` turns it off;
`=2` prints each correction. The earlier constraint in `forward.c` (JSON calls from the `<tools>` block, and only the
function name in XML) is still used for formats this module does not handle.

Integration: `gen_console` in `src/app/console.c` (the sampling loop of the agent): `cg_turn_begin` when a
generation starts, `cg_check` on the sampled token, `cg_after` on the accepted token.

Tests: `katana calltest` (132 checks without a model: accepted and rejected calls in the three formats, the same
calls cut into pieces of 2 to 11 bytes, required parameters, repeated parameters, markup inside a free string, both
TodoWrite forms, the real system prompt in the three formats), and `tools/traintest/callgrammar_e2e.sh` (a tiny model
with random weights; with the opener forced by `KATANA_CALL_FORCE` and each value limited by
`KATANA_CALL_GRAMMAR_BUDGET`, its garbage still comes out as a call the agent reads, with the right tool and integer
values where the schema says integer; with the constraint off, no call). Both run in `tools/traintest/run.sh`.

## 6. What was measured and what was not

- Measured: the token counts above (real tokenizers, no weights loaded); the unit tests and the random-model test.
- **Not measured with a real model** (the machine was busy and real inference was not allowed for this work):
  whether Qwen3.6 or Gemma 4 produce fewer broken calls, whether the line form of `TodoWrite` is used, and the cost
  of the check per token (expected near zero, since the vocabulary scan only runs on a rejected token). The
  measurement that would show the benefit: rejected or malformed calls per 100 calls in `~/.katana/agente.log` on the
  same task set, before and after (`KATANA_CALL_GRAMMAR=0` against the default), alternating runs in the same hour.
- Gemma 4 with `KATANA_TOOLS=1`: the first tool set is declared in Gemma's own syntax, which this module does not
  read, so only the table tools would be known; with no tools found the constraint stays off.
- Limits: at most 160 tools and 40 parameters per tool; a schema the JSON grammar does not understand (anyOf, oneOf)
  accepts any JSON value; GLM and the harmony format are not constrained.
