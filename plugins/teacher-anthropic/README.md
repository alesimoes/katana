# teacher-anthropic: a katana teacher plugin

Third-party plugin for katana's teacher mode (protocol: `docs/teacher-protocol.md`). It lets a larger model, reached
through the Anthropic Messages API, answer in place of the local model. It is a standalone Python 3 program (standard
library only); nothing from it is compiled into katana. It sends the agent's system prompt, the conversation and the
tool results to the service you configure, so use it only when that is acceptable for your data.

Two connections:

- **api**: the Messages API with a key (streamed: reasoning summary and text appear while they are written).
- **cli**: the `claude` command line program installed on the machine, with the session you are already logged in to,
  no key. One call per turn, no tools of its own. The answer arrives at once (no reasoning text, no live text).

With `KATANA_TEACHER_VIA=auto` (the default) it uses **api** when a key exists and **cli** otherwise.

## Install

Pick one:

```sh
# 1. a folder katana finds by itself (~/.katana/plugins/teacher/run)
mkdir -p ~/.katana/plugins
ln -s "$PWD/plugins/teacher-anthropic" ~/.katana/plugins/teacher     # run from the katana repository root

# 2. a command line in a file
echo "python3 $PWD/plugins/teacher-anthropic/teacher.py" > ~/.katana/teacher.cmd

# 3. an environment variable
export KATANA_TEACHER_CMD="python3 $PWD/plugins/teacher-anthropic/teacher.py"
```

Then, in the agent: `/teacher on` (or start with `KATANA_TEACHER=1`). `/teacher off` returns to the local model.

## Configuration

The plugin reads these from its environment (the agent passes its own environment to it):

| Setting | Meaning | Default |
|---|---|---|
| `KATANA_TEACHER_KEY`, `ANTHROPIC_API_KEY`, or the file `~/.katana/teacher.key` | API key, in this order. The file must be readable only by you (`chmod 600`) | none |
| `KATANA_TEACHER_URL` | API base URL | `https://api.anthropic.com` |
| `KATANA_TEACHER_MODEL`, or `/teacher <name>` | model name (katana sends it as `options.model`) | `claude-opus-5-5` |
| `KATANA_TEACHER_EFFORT` | `low`, `medium`, `high`, `xhigh`, `max` (sent as `options.effort`) | `high` |
| `KATANA_TEACHER_FALLBACK=0` | do not ask for the server-side fallback model when a request is declined | on |
| `KATANA_TEACHER_VIA` | `api`, `cli` or `auto` | `auto` |
| `KATANA_TEACHER_CLI` | the command line program for the cli connection | `claude` |

### Moving from the old built-in teacher

Before 2026-11-23 these were handled inside the katana binary. The same names keep working, now read by the plugin:

| Old | Now |
|---|---|
| `ANTHROPIC_API_KEY`, `~/.katana/teacher.key` | same (plus `KATANA_TEACHER_KEY`); the desktop key dialog still writes `~/.katana/teacher.key` |
| `KATANA_TEACHER_MODEL`, `KATANA_TEACHER_EFFORT` | same, passed to the plugin by katana as options |
| `KATANA_TEACHER_URL`, `KATANA_TEACHER_FALLBACK` | same, read by the plugin |
| `KATANA_TEACHER_VIA=api\|cli` | same, read by the plugin. The commands `/teacher api` and `/teacher cli` are gone: set the variable |
| `KATANA_TEACHER_CLI` | same, read by the plugin |
| (built in) | you now also need to install the plugin once (see Install) |

Differences to know: the signed reasoning blocks of the current run are still sent back unchanged (through the
protocol's `opaque` field), and after an agent restart they are rebuilt without them, as before. The model name shown
by `/teacher status` is empty until you set one (the plugin default is used and not reported back).

## Test

`test/mock_api.py PORT LOG` is a stand-in for the API that rejects requests the real one would reject (headers, roles,
tool call/result pairing, changed reasoning blocks, sampling parameters). `tools/traintest/run.sh` (step 10 and the
reading test) runs the agent on a tiny random model against it through this plugin.

Not tested: the plugin against the real service (needs a key), and the cli connection against the real `claude` program.
