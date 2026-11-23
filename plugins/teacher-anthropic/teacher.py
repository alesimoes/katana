#!/usr/bin/env python3
"""katana teacher plugin for the Anthropic Messages API (and, optionally, the `claude` command line program).

Speaks the katana teacher protocol (docs/teacher-protocol.md): one JSON request line on stdin, JSON event lines on
stdout. Python standard library only. Configuration is in the environment (see README.md):

  KATANA_TEACHER_KEY, ANTHROPIC_API_KEY or ~/.katana/teacher.key   API key (the file must be readable only by you)
  KATANA_TEACHER_URL        API base URL                      (default https://api.anthropic.com)
  KATANA_TEACHER_MODEL      model name                        (default claude-opus-5-5; katana's /teacher <name> and
                                                                KATANA_TEACHER_MODEL arrive as options.model)
  KATANA_TEACHER_EFFORT     low | medium | high | xhigh | max (default high; arrives as options.effort)
  KATANA_TEACHER_FALLBACK=0 do not ask for the server-side fallback model when a request is declined
  KATANA_TEACHER_VIA        api | cli | auto (default auto: api when a key exists, else cli)
  KATANA_TEACHER_CLI        the command line program for cli mode (default: claude)
"""
import json, os, shutil, subprocess, sys, tempfile, time, urllib.error, urllib.request

DEFAULT_MODEL = "claude-opus-5-5"
DEFAULT_URL = "https://api.anthropic.com"
API_VERSION = "2026-10-21"
FALLBACK_BETA = "server-side-fallback-2026-07-01"


def emit(**ev):
    sys.stdout.write(json.dumps(ev, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def fail(msg, code=1):
    emit(event="error", message=str(msg)[:600])
    sys.exit(code)


def read_key():
    k = (os.environ.get("KATANA_TEACHER_KEY") or os.environ.get("ANTHROPIC_API_KEY") or "").strip()
    if k:
        return k
    try:
        with open(os.path.expanduser("~/.katana/teacher.key")) as f:
            return f.read().strip()
    except OSError:
        return ""


# ---------------------------------------------------------------- neutral -> API messages
def to_api_messages(msgs):
    out = []
    for m in msgs:
        content = []
        for b in m["content"]:
            t = b.get("type")
            if t == "text":
                content.append({"type": "text", "text": b["text"]})
            elif t == "tool_call":
                content.append({"type": "tool_use", "id": b["id"], "name": b["name"], "input": b.get("arguments") or {}})
            elif t == "tool_result":
                content.append({"type": "tool_result", "tool_use_id": b["id"], "content": b.get("content", "")})
            elif t == "opaque" and isinstance(b.get("data"), list):      # the blocks this plugin returned as "opaque" (signed thinking)
                content.extend(b["data"])
        out.append({"role": m["role"], "content": content})
    return out


def to_api_tools(tools):
    return [{"name": t["name"], "description": t.get("description", ""),
             "input_schema": t.get("parameters") or {"type": "object", "properties": {}}} for t in tools]


# ---------------------------------------------------------------- API mode (streaming)
def api_turn(req, key, model, effort):
    opts = req.get("options", {})
    body = {"model": model, "stream": True, "max_tokens": int(opts.get("max_tokens") or 32000),
            "cache_control": {"type": "ephemeral"}, "thinking": {"type": "adaptive", "display": "summarized"},
            "output_config": {"effort": effort}, "system": req["system"], "tools": to_api_tools(req["tools"]),
            "messages": to_api_messages(req["messages"])}
    fallback = os.environ.get("KATANA_TEACHER_FALLBACK", "1") != "0"
    headers = {"content-type": "application/json", "anthropic-version": API_VERSION, "x-api-key": key}
    if fallback:
        body["fallbacks"] = "default"
        headers["anthropic-beta"] = FALLBACK_BETA
    url = os.environ.get("KATANA_TEACHER_URL", DEFAULT_URL).rstrip("/") + "/v1/messages"
    data = json.dumps(body).encode()
    last_err = "no answer"
    for attempt in range(4):
        if attempt:
            time.sleep((3, 10, 25)[attempt - 1])
        shown = False
        try:
            r = urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers, method="POST"), timeout=900)
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read()).get("error", {}).get("message", "")
            except Exception:
                msg = ""
            last_err = "HTTP %d: %s" % (e.code, msg)
            if e.code < 500 and e.code not in (408, 409, 429):
                fail(last_err)
            continue
        except Exception as e:
            last_err = "network: %s" % e
            continue
        blocks, usage, stop = {}, {}, ""
        stream_error = False
        with r:
            for raw in r:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not line.startswith("data:"):
                    continue
                try:
                    ev = json.loads(line[5:])
                except ValueError:
                    continue
                t = ev.get("type")
                if t == "error":
                    last_err = ev.get("error", {}).get("message", "stream error")
                    stream_error = True
                    break
                if t == "message_start":
                    usage.update(ev.get("message", {}).get("usage", {}))
                elif t == "content_block_start":
                    b = dict(ev["content_block"]); b["_acc"] = ""; b["_sig"] = ""
                    blocks[ev["index"]] = b
                elif t == "content_block_delta":
                    b = blocks.get(ev["index"]); d = ev["delta"]
                    if b is None:
                        continue
                    if d["type"] == "thinking_delta":
                        b["_acc"] += d["thinking"]; emit(event="thinking", text=d["thinking"]); shown = True
                    elif d["type"] == "text_delta":
                        b["_acc"] += d["text"]; emit(event="text", text=d["text"]); shown = True
                    elif d["type"] == "signature_delta":
                        b["_sig"] += d["signature"]
                    elif d["type"] == "input_json_delta":
                        b["_acc"] += d["partial_json"]
                        tot = sum(len(x["_acc"]) for x in blocks.values() if x["type"] == "tool_use")
                        emit(event="progress", bytes=tot)
                elif t == "message_delta":
                    stop = ev.get("delta", {}).get("stop_reason", stop) or stop
                    usage.update(ev.get("usage", {}))
        if not stream_error:
            last_err = ""
        if last_err:
            if shown:
                break           # text already reached the screen: do not repeat the request
            continue
        text, thinking, calls, opaque = [], [], [], []
        for i in sorted(blocks):
            b = blocks[i]
            if b["type"] == "text":
                text.append(b["_acc"])
            elif b["type"] == "thinking":
                thinking.append(b["_acc"])
                opaque.append({"type": "thinking", "thinking": b["_acc"], "signature": b["_sig"]})
            elif b["type"] == "redacted_thinking":
                opaque.append({k: v for k, v in b.items() if not k.startswith("_")})
            elif b["type"] == "tool_use":
                try:
                    args = json.loads(b["_acc"]) if b["_acc"].strip() else {}
                except ValueError:
                    args = {}
                calls.append({"id": b["id"], "name": b["name"], "arguments": args})
        cached = int(usage.get("cache_read_input_tokens", 0))
        total_in = int(usage.get("input_tokens", 0)) + cached + int(usage.get("cache_creation_input_tokens", 0))
        done = {"event": "done", "text": "\n\n".join(x for x in text if x), "thinking": "\n".join(x for x in thinking if x),
                "tool_calls": calls, "stop": {"max_tokens": "max_tokens", "refusal": "refusal"}.get(stop, "end_turn"),
                "usage": {"input": total_in, "output": int(usage.get("output_tokens", 0)), "cached": cached}}
        if opaque:
            done["opaque"] = opaque
        emit(**done)
        return
    fail(last_err)


# ---------------------------------------------------------------- cli mode (the `claude` program, logged in on this machine)
def cli_turn(req, model, effort):
    prog = os.environ.get("KATANA_TEACHER_CLI", "claude")
    if not shutil.which(prog):
        fail("no API key (ANTHROPIC_API_KEY or ~/.katana/teacher.key) and the %s program was not found" % prog)
    sysmsg = (req["system"] + "\n\n# Functions you can call\n" + json.dumps(to_api_tools(req["tools"])) +
              "\n\n# How to answer\nYou write the next turn of this agent; you do not run anything yourself here. Answer with the structured object: "
              "\"text\" is what you say to the user (it may be empty when you only call functions) and \"tool_calls\" lists the functions to call now, "
              "each with its name and an \"arguments\" object. The agent runs them on the user's machine and their results come in the next message. "
              "When the job is done, or you need the user, answer with text and an empty tool_calls.")
    prompt = ("The conversation so far, as a JSON array of messages (tool_call blocks are the calls made, tool_result blocks what they returned):\n" +
              json.dumps([{"role": m["role"], "content": [b for b in m["content"] if b.get("type") != "opaque"]} for m in req["messages"]]) +
              "\n\nWrite the next assistant turn.")
    schema = {"type": "object", "properties": {"text": {"type": "string"}, "tool_calls": {"type": "array", "items": {"type": "object",
              "properties": {"name": {"type": "string"}, "arguments": {"type": "object"}}, "required": ["name", "arguments"]}}},
              "required": ["text", "tool_calls"]}
    d = tempfile.mkdtemp(prefix="katana_teacher_cli_")
    try:
        sf = os.path.join(d, "system.txt")
        with open(sf, "w") as f:
            f.write(sysmsg)
        env = {k: v for k, v in os.environ.items() if k not in ("CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT")}
        args = [prog, "-p", "--system-prompt-file", sf, "--tools", "", "--json-schema", json.dumps(schema), "--output-format", "stream-json",
                "--verbose", "--include-partial-messages", "--model", model, "--effort", effort, "--no-session-persistence",
                "--strict-mcp-config", "--setting-sources", ""]
        p = subprocess.Popen(args, cwd=d, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            outs, errs = p.communicate(prompt, timeout=900)
        except subprocess.TimeoutExpired:
            p.kill(); fail("timeout")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    res = None
    for line in outs.splitlines():
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        if ev.get("type") == "result":
            res = ev
    so = (res or {}).get("structured_output")
    if not res or res.get("is_error") or not isinstance(so, dict):
        fail((res or {}).get("result") or errs.strip() or "no answer from the %s program (is it logged in? run it once in a terminal)" % prog)
    calls = [{"id": "cli_%d" % i, "name": c["name"], "arguments": c.get("arguments") or {}} for i, c in enumerate(so.get("tool_calls") or []) if c.get("name")]
    u = res.get("usage", {})
    cached = int(u.get("cache_read_input_tokens", 0))
    emit(event="done", text=so.get("text", ""), tool_calls=calls, stop="end_turn",
         usage={"input": int(u.get("input_tokens", 0)) + cached + int(u.get("cache_creation_input_tokens", 0)), "output": int(u.get("output_tokens", 0)), "cached": cached})


def main():
    try:
        req = json.loads(sys.stdin.readline())
    except ValueError as e:
        fail("the request is not JSON: %s" % e)
    if req.get("protocol") != 1:
        fail("unsupported protocol %r" % req.get("protocol"))
    opts = req.get("options", {})
    model = opts.get("model") or DEFAULT_MODEL
    effort = opts.get("effort") or "high"
    key = read_key()
    via = os.environ.get("KATANA_TEACHER_VIA", "auto")
    if via == "cli" or (via == "auto" and not key):
        cli_turn(req, model, effort)
    elif not key:
        fail("no API key: set ANTHROPIC_API_KEY or write the key to ~/.katana/teacher.key")
    else:
        api_turn(req, key, model, effort)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:
        fail("%s: %s" % (type(e).__name__, e))
