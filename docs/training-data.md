# Training data from agent sessions

Goal: teach a model how the katana agent should *act* (pick a tool, call it in the exact format the agent parses,
check the result, know when to stop), not only what it knows. The data comes from the agent's own sessions, and
codebench says which sessions solved their task.

Status (2026-11-20): the session format, the codebench flags and the exporter are written and checked without a
model (synthetic sessions for ChatML, Gemma 4 and GLM; see "What was checked"). No real session has been exported
yet and nothing has been trained.

## Pipeline

1. **Collect.** Run codebench with sessions on and several attempts per task:

   ```
   python3 tools/codebench/run.py --model models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf \
       --out runs/q36.jsonl --sessions runs/q36-sess --repeat 4 --label q36
   ```

   Each run writes `runs/q36-sess/<task>.<attempt>.ksess`; the results line carries `session`, `attempt` and
   `instruction`. Everyday sessions (`katana agent --sessao FILE`) work too, but they have no pass/fail label.

2. **Export.**

   ```
   python3 tools/dataset/ksess2jsonl.py runs/q36-sess --bench runs/q36.jsonl --only-passed -o sft.jsonl
   python3 tools/dataset/ksess2jsonl.py runs/q36-sess --bench runs/q36.jsonl --stats
   ```

3. **SFT (supervised fine-tuning), rejection sampling.** Train a LoRA/QLoRA adapter on the passed sessions only,
   with the loss on the model's turns (`train_spans`). This copies the agent's best behaviour back into the model:
   the same tool format, fewer wasted calls, checking before answering.

4. **RL with verifiable rewards (GRPO or similar).** codebench already is the environment: a fresh copy of the repo,
   the instruction, the agent's tools, and hidden tests as the reward. `--repeat N` gives N attempts per task, which
   is the group a GRPO step compares. This needs a trainer that can run the agent loop and is the expensive stage;
   do it after SFT shows a gain.

5. **Measure.** Convert the adapter back to GGUF (merge it into the base weights, then the reference engine's GGUF
   converter and quantizer), run codebench on base and tuned models, and use `run.py compare`. Tasks used for
   training must not be the tasks used to measure: hold out a split, or write new tasks.

## Importing conversations from elsewhere (`katana import`)

```
katana import <model.gguf> <in.jsonl> <out.jsonl> [--system keep|ksess:FILE|file:FILE|none] [--only-known-tools]
```

Reads one conversation per line, in the messages format (`role`/`content`, `tool_calls` with
`function.name`/`function.arguments`, `tool` messages with `tool_call_id`, `reasoning_content`) or ShareGPT
(`conversations` with `from`/`value`, including `function_call` and `observation`), and writes the same JSONL as
`ksess2jsonl.py` (`text` + `train_spans`), ready for `katana train`. Only the model's metadata is read (no weights).

- The text is rendered by the agent's own `ah_render`, so it is the template the agent uses: ChatML (Qwen family),
  Gemma 4 turns, or GLM 4.5+.
- Tool calls become the model's native syntax: XML (`<function=..><parameter=..>`) when the model's template uses it
  (Qwen3.6), JSON inside `<tool_call>` otherwise, `<|tool_call>call:name{..}<tool_call|>` for Gemma 4,
  `<arg_key>/<arg_value>` for GLM. `tools/traintest/import_calls.c` checks that the agent's own call readers read
  them back in all four formats.
- Tool results are wrapped exactly as the agent wraps them (`[resultado da ferramenta ...]`), after the same
  special-token defanging.
- Reasoning goes in the thinking block (`<think>`, or the Gemma 4 thought channel) when the model reasons; an
  assistant turn without reasoning gets the empty block the template expects.
- `--system`: `keep` (the conversation's own system message), `ksess:FILE` (the exact system prompt of a real katana
  agent session, from its `KSX1` block: the imported data then conditions on the prompt the agent really uses),
  `file:FILE`, or `none`. Calls to tools the system prompt does not mention are counted; `--only-known-tools` drops
  those conversations (a model trained on tool names the agent does not have will call tools that do not exist).
- Where the data comes from matters: conversations of katana itself, or of open-weight models whose license allows
  it. Closed services usually forbid in their terms using their outputs to train other models.

## What a session file holds

`.ksess` (written by `sess_save` in `src/agent/loop.c`): model fingerprint, expert heat, the compact conversation
used to resume, its token ids, and the whole conversation (roles `u` user, `a` assistant, `t` tool results) in the
exact text of the cache. Sessions written from 2026-11-20 on end with a `KSX1` block (`sess_wext`):

| field | meaning |
|---|---|
| arch, chat_fmt, think | model family and chat format; `think` is the Gemma 4 thinking switch at save time |
| exact | 1 when the cache held the whole conversation; 0 when the agent compacted its context |
| system | the system prompt (tools, memory, folder block) |
| text | the whole conversation rendered in the model's own format (`ah_render`) |
| per message | role and the `[start, end)` byte range of its content inside `text` |

Resuming reads up to the whole conversation and stops, so old and new files keep working in both directions.

## Exported record (`--format text`)

```
{"source": "bank.0.ksess", "arch": "qwen35moe", "exact": true,
 "text": "<|im_start|>system\n...<|im_end|>\n<|im_start|>user\n...",
 "train_spans": [[1834, 2101], ...], "system": "...", "messages": [...],
 "task": "bank", "passed": true, "attempt": 0, "seconds": 62.0, "tool_calls": 9}
```

`train_spans` are character offsets into `text` (Python string indices, not bytes): each assistant turn plus the
token that ends it (`<|im_end|>`, `<|tool_response>`, `<turn|>`, `<|observation|>` or `<|user|>`), so the model also
learns where to stop and where to hand over to a tool. The last turn gets its end-of-turn token appended. To train,
tokenize `text` with offset mapping and set the label of every token that does not fall inside a span to -100.

`--format messages` gives `system/user/assistant/tool` lists for trainers that apply a chat template themselves.
Tool calls stay inside the assistant text in the model's native syntax, so the template must not rewrite them.

## Cautions

- **Compacted sessions** (`exact: false`): the model was conditioned on summaries, not on the full text it is now
  trained against, and the text may exceed the context. Start with `--only-exact`.
- **Overfitting to codebench.** 30 small tasks repeated many times teach the tasks, not the behaviour. Mix in
  everyday sessions, vary the instructions, and keep a held-out set.
- **Private data.** Sessions contain file contents, command output and the agent's memory (in the system prompt).
  Read the data before uploading it to a rented GPU or sharing it; `--redact-home` only replaces the home path.
- **Licenses.** The base model's license applies to the tuned model. Trajectories generated by another model fall
  under that model's terms; some providers forbid using outputs to train other models.

## What was checked

- `katana` builds with the change (same 36 warnings as before; none new).
- A test program compiled against `src/katana.c` (no model) called `sess_wext` on conversations in the ChatML,
  Gemma 4 and GLM formats; the exporter read the files back and every train span covers exactly the assistant text
  plus its stop token. A file without the block exports with `--format messages` and is skipped by `--format text`.
- `run.py --verify` on `bank,rotate,csvsum`: unchanged.
- Not done: a real agent session exported end to end, and any training run.
