#!/bin/sh
# Teste da ferramenta LSP: sh tools/tooltest/lsp.sh ./katana
# Usa um servidor de linguagem falso (python3) criado na pasta temporaria; clangd real so se estiver instalado.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$K" ] || { echo "usage: $0 <katana binary>"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
N=0; F=0
ok()  { N=$((N+1)); echo "ok   $1"; }
bad() { N=$((N+1)); F=$((F+1)); echo "FAIL $1"; printf '%s\n' "$2" | sed 's/^/     | /' | head -20; }
has() { case "$2" in *"$3"*) ok "$1";; *) bad "$1 (expected: $3)" "$2";; esac; }
hasnt() { case "$2" in *"$3"*) bad "$1 (unexpected: $3)" "$2";; *) ok "$1";; esac; }

mkdir -p "$T/proj/.git" "$T/proj/sub"
cd "$T/proj"
# linha 2: "/* àé😀 */ int foo;" -> "int" no byte 16 (1-based) = unidade UTF-16 11 (0-based)
printf 'int main(void) {\n/* \303\240\303\251\360\237\230\200 */ int foo;\n  return 0;\n}\n' > sub/a.c
printf 'x\n' > sub/a.zzz

# ---- erros que nao precisam de servidor ----
R=$("$K" tool LSP '{"operation":"bogus","filePath":"sub/a.c"}' 2>&1); has "bad operation" "$R" "error: operation must be one of"
R=$("$K" tool LSP '{"operation":"hover"}' 2>&1); has "missing filePath" "$R" "error: filePath is required"
R=$("$K" tool LSP '{"operation":"hover","filePath":"sub/none.c","line":1,"character":1}' 2>&1); has "missing file" "$R" "error:"
R=$("$K" tool LSP '{"operation":"hover","filePath":"sub/a.c"}' 2>&1); has "position required" "$R" "error: hover needs line and character"
R=$("$K" tool LSP '{"operation":"documentSymbol","filePath":"sub/a.zzz"}' 2>&1); has "unknown extension" "$R" "no language server is known for .zzz files; set KATANA_LSP_ZZZ"
R=$(KATANA_LSP_C="no-such-lsp-xyz --stdio" "$K" tool LSP '{"operation":"documentSymbol","filePath":"sub/a.c"}' 2>&1); has "server not on PATH" "$R" "'no-such-lsp-xyz' was not found on PATH"
if ! command -v gopls >/dev/null 2>&1; then
  printf 'package main\n' > sub/m.go
  R=$(PATH=/usr/bin:/bin "$K" tool LSP '{"operation":"documentSymbol","filePath":"sub/m.go"}' 2>&1); has "default server missing names it" "$R" "Install gopls"
else echo "SKIPPED default server missing (gopls installed)"; fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIPPED fake server checks (no python3)"
else
cat > "$T/fake.py" <<'EOF'
import sys, json, os
mode = sys.argv[1] if len(sys.argv) > 1 else "ok"
inp, out = sys.stdin.buffer, sys.stdout.buffer
def send(o):
    b = json.dumps(o).encode()
    out.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); out.flush()
def recv():
    n = None
    while True:
        l = inp.readline()
        if not l: sys.exit(0)
        l = l.strip()
        if not l: break
        if l.lower().startswith(b"content-length:"): n = int(l.split(b":")[1])
    return json.loads(inp.read(n))
if mode == "dieonce":
    f = os.environ["FAKE_COUNT"]
    c = int(open(f).read()) if os.path.exists(f) else 0
    open(f, "w").write(str(c + 1))
    mode = "die" if c == 0 else "ok"
docs = {}
U = lambda p: p
while True:
    m = recv()
    meth, mid = m.get("method"), m.get("id")
    p = m.get("params", {})
    if meth == "initialize":
        if mode == "srvreq":
            send({"jsonrpc": "2.0", "id": "s1", "method": "workspace/configuration", "params": {"items": [{}, {}]}})
            r = recv()
            if r.get("id") != "s1" or r.get("result") != [None, None]: sys.exit(3)
            send({"jsonrpc": "2.0", "method": "window/logMessage", "params": {"type": 3, "message": "hi"}})
        send({"jsonrpc": "2.0", "id": mid, "result": {"capabilities": {}, "root": p.get("rootUri")}})
        if mode == "die":
            recv(); sys.exit(0)
        continue
    if mid is None:
        if meth in ("textDocument/didOpen", "textDocument/didChange"):
            td = p["textDocument"]; docs[td["uri"]] = td.get("version", 1)
            if meth == "textDocument/didOpen": docs["root"] = td["uri"]
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {"uri": td["uri"], "diagnostics": [
                {"range": {"start": {"line": 1, "character": 11}, "end": {"line": 1, "character": 14}}, "severity": 2, "message": "unused variable\nfoo"},
                {"range": {"start": {"line": 0, "character": 4}, "end": {"line": 0, "character": 8}}, "severity": 1, "message": "boom"}]}})
        continue
    if mode == "hang": continue
    if mode == "garbage":
        b = b"{not json"; out.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); out.flush(); continue
    if mode == "noframe":
        out.write(b"hello this is not lsp\n" * 1000); out.flush(); continue
    uri = p.get("textDocument", {}).get("uri", "")
    pos = p.get("position", {})
    L = lambda u, l, c: {"uri": u, "range": {"start": {"line": l, "character": c}, "end": {"line": l, "character": c + 1}}}
    res = None
    if meth == "textDocument/hover":
        res = {"contents": {"kind": "markdown", "value": "```c\nint foo\n```\npos %d:%d version %d" % (pos["line"], pos["character"], docs.get(uri, 0))}}
        if os.environ.get("FAKE_TOUCH"): open(uri[7:], "a").write("// changed\n")
    elif meth == "textDocument/definition":
        res = [L(uri, pos["line"], pos["character"])]
    elif meth == "textDocument/references":
        if os.environ.get("FAKE_MANY"): res = [L(uri, 0, 4)] * 20000
        else: res = [L(uri, 0, 4), L(uri, 1, 11)]
    elif meth == "textDocument/implementation":
        res = [{"targetUri": uri, "targetRange": {"start": {"line": 2, "character": 0}, "end": {"line": 2, "character": 5}},
                "targetSelectionRange": {"start": {"line": 2, "character": 2}, "end": {"line": 2, "character": 5}}}]
    elif meth == "textDocument/documentSymbol":
        R = {"start": {"line": 0, "character": 0}, "end": {"line": 3, "character": 1}}
        res = [{"name": "main", "detail": "int (void)", "kind": 12, "range": R, "selectionRange": R,
                "children": [{"name": "foo", "kind": 13, "range": {"start": {"line": 1, "character": 11}, "end": {"line": 1, "character": 14}},
                              "selectionRange": {"start": {"line": 1, "character": 15}, "end": {"line": 1, "character": 18}}}]}]
    elif meth == "workspace/symbol":
        res = [] if p["query"] == "nothing" else [{"name": "q_" + p["query"], "kind": 23, "containerName": "ns", "location": L(docs["root"], 1, 11)}]
    elif meth == "textDocument/prepareCallHierarchy":
        res = [{"name": "main", "kind": 12, "uri": uri, "range": L(uri, 0, 4)["range"], "selectionRange": L(uri, 0, 4)["range"], "data": {"x": 1}}]
    elif meth == "callHierarchy/incomingCalls":
        it = p["item"]
        assert it["data"] == {"x": 1}
        res = [{"from": {"name": "caller", "kind": 12, "uri": it["uri"], "range": L(it["uri"], 2, 2)["range"], "selectionRange": L(it["uri"], 2, 2)["range"]}, "fromRanges": []}]
    elif meth == "callHierarchy/outgoingCalls":
        res = [{"to": {"name": "callee", "kind": 6, "uri": "file:///elsewhere/x%20y.c", "range": L("", 9, 0)["range"], "selectionRange": L("", 9, 0)["range"]}, "fromRanges": []}]
    elif meth == "shutdown":
        res = None
    else:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}); continue
    send({"jsonrpc": "2.0", "id": mid, "result": res})
EOF
  FK="python3 $T/fake.py"
  export KATANA_LSP_FIRST_TIMEOUT=3 KATANA_LSP_TIMEOUT=3
  L() { KATANA_LSP_C="$FK ${FM:-ok}" "$K" tool "$@" 2>&1; }
  R=$(L LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}')
  has "hover plain text" "$R" "int foo"; hasnt "hover drops code fences" "$R" '```'
  has "byte column -> UTF-16 (accents + emoji)" "$R" "pos 1:11 version 1"
  R=$(L LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":9}')
  has "column inside emoji rounds to its start" "$R" "pos 1:5"
  R=$(L LSP '{"operation":"hover","filePath":"sub/a.c","line":9,"character":1}'); has "line past end" "$R" "error: sub/a.c has no line 9"
  R=$(L LSP '{"operation":"goToDefinition","filePath":"sub/a.c","line":2,"character":16}')
  has "definition: UTF-16 -> byte column, relative path, source line" "$R" "sub/a.c:2:16: /* àé😀 */ int foo;"
  R=$(L LSP '{"operation":"findReferences","filePath":"sub/a.c","line":1,"character":5}')
  has "references count" "$R" "2 references:"; has "references entry" "$R" "sub/a.c:1:5: int main(void) {"
  R=$(FAKE_MANY=1 L LSP '{"operation":"findReferences","filePath":"sub/a.c","line":1,"character":5}')
  has "cap: omitted entries reported" "$R" "more entries omitted)"; has "cap: total count" "$R" "20000 references:"
  R=$(L LSP '{"operation":"goToImplementation","filePath":"sub/a.c","line":1,"character":5}')
  has "implementation (LocationLink)" "$R" "sub/a.c:3:3: return 0;"
  R=$(L LSP '{"operation":"documentSymbol","filePath":"sub/a.c"}')
  has "documentSymbol outline" "$R" "Function main - int (void) (line 1)"; has "documentSymbol child indented" "$R" "  Variable foo (line 2)"
  R=$(L LSP '{"operation":"workspaceSymbol","filePath":"sub/a.c","query":"bar"}')
  has "workspaceSymbol" "$R" "Struct q_bar in ns - sub/a.c:2:16"
  R=$(L LSP '{"operation":"workspaceSymbol","filePath":"sub/a.c","query":"nothing"}'); has "workspaceSymbol empty" "$R" "No symbols match 'nothing'."
  R=$(L LSP '{"operation":"prepareCallHierarchy","filePath":"sub/a.c","line":1,"character":5}'); has "prepareCallHierarchy" "$R" "Function main - sub/a.c:1:5"
  R=$(L LSP '{"operation":"incomingCalls","filePath":"sub/a.c","line":1,"character":5}')
  has "incomingCalls header" "$R" "Callers of main:"; has "incomingCalls entry" "$R" "  Function caller - sub/a.c:3:3"
  R=$(L LSP '{"operation":"outgoingCalls","filePath":"sub/a.c","line":1,"character":5}')
  has "outgoingCalls (path outside root, decoded URI)" "$R" "  Method callee - /elsewhere/x y.c:10:1"
  R=$(L LSP '{"operation":"diagnostics","filePath":"sub/a.c"}')
  has "diagnostics sorted, error" "$R" "sub/a.c:1:5: error: boom"; has "diagnostics warning, byte column" "$R" "sub/a.c:2:16: warning: unused variable foo"
  # didChange: o servidor falso altera o arquivo depois do primeiro hover; o servidor continua o mesmo
  R=$(FAKE_TOUCH=1 L LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}' LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}')
  has "didChange after file changed (version 2)" "$R" "pos 1:11 version 2"
  # modos de falha: cada um tem que terminar com erro dentro do prazo
  t0=$(date +%s)
  R=$(FM=hang L LSP '{"operation":"hover","filePath":"sub/a.c","line":1,"character":1}'); has "hang -> timeout error" "$R" "did not answer textDocument/hover within 3 s"
  t1=$(date +%s); [ $((t1 - t0)) -lt 10 ] && ok "hang returns within deadline ($((t1 - t0)) s)" || bad "hang took $((t1 - t0)) s" ""
  R=$(FM=garbage L LSP '{"operation":"hover","filePath":"sub/a.c","line":1,"character":1}'); has "invalid JSON -> error" "$R" "error: language server 'python3' failed: the server sent invalid JSON"
  R=$(FM=noframe L LSP '{"operation":"hover","filePath":"sub/a.c","line":1,"character":1}'); has "no framing -> error" "$R" "not LSP framing"
  R=$(FM=die L LSP '{"operation":"hover","filePath":"sub/a.c","line":1,"character":1}'); has "dies after initialize -> error" "$R" "error: language server 'python3'"
  R=$(FM=srvreq L LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}'); has "server-to-client request answered" "$R" "pos 1:11"
  R=$(FAKE_COUNT=$T/count FM=dieonce L LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}' LSP '{"operation":"hover","filePath":"sub/a.c","line":2,"character":16}')
  has "first start dies -> error" "$R" "error: language server"; has "restarted on next call" "$R" "pos 1:11 version 1"
  R=$(KATANA_LSP_C="python3 -c pass" "$K" tool LSP '{"operation":"hover","filePath":"sub/a.c","line":1,"character":1}' 2>&1); has "exits at once -> error" "$R" "failed to start: the server exited"
  sleep 1; R=$(ps -eo args | grep "$T/fake.py" | grep -v grep); [ -z "$R" ] && ok "no fake servers left running" || bad "servers left running" "$R"
  unset KATANA_LSP_FIRST_TIMEOUT KATANA_LSP_TIMEOUT
fi

# ---- clangd real, se houver ----
if command -v clangd >/dev/null 2>&1; then
  mkdir -p "$T/c"; printf 'static int twice(int x) { return 2 * x; }\nint main(void) { return twice(3) + undefined_thing; }\n' > "$T/c/m.c"
  R=$("$K" tool LSP "{\"operation\":\"goToDefinition\",\"filePath\":\"$T/c/m.c\",\"line\":2,\"character\":26}" 2>&1); has "clangd definition" "$R" "m.c:1:12:"
  R=$("$K" tool LSP "{\"operation\":\"documentSymbol\",\"filePath\":\"$T/c/m.c\"}" 2>&1); has "clangd symbols" "$R" "Function twice"
  R=$("$K" tool LSP "{\"operation\":\"diagnostics\",\"filePath\":\"$T/c/m.c\"}" 2>&1); has "clangd diagnostics" "$R" "undefined_thing"
else echo "SKIPPED clangd checks (clangd not installed)"; fi

[ $F -eq 0 ] && { echo "OK $N checks"; exit 0; }
echo "FAILED $F"; exit 1
