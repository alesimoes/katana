#!/bin/sh
# Vision tests without real weights: oracle of every stage, unit and state tests, command line, API server.
#   sh tools/visiontest/run.sh ./katana      (needs python3 with numpy and Pillow; gcc)
# tools/visiontest/fuzz.sh: damaged files. gpu.py: CPU against the Vulkan graph. real.py: real weights.
K=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")/../.." || exit 1
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
bad=0
python3 tools/visiontest/oracle.py "$K" > "$D/o.txt" 2>&1 || bad=1
grep -v "^ok" "$D/o.txt"; echo "oracle: $(grep -c '^ok' "$D/o.txt") ok, $(grep -c '^FAIL' "$D/o.txt") failures"
python3 - "$D" <<'PY' || exit 1
import sys, os
d = sys.argv[1]
src = open("tools/visiontest/oracle.py").read().split("\nonly = ")[0]
sys.argv = ["x", "x"]
ns = {"__file__": os.path.abspath("tools/visiontest/oracle.py"), "__name__": "lib"}
exec(compile(src, "oracle", "exec"), ns)
ns["projector"](d + "/mmproj-a.gguf", 32, 72, 2, 4, 64, base="tiny-model")
ns["projector"](d + "/mmproj-0other.gguf", 32, 72, 2, 4, 64, base="another")
ns["projector"](d + "/wrong.gguf", 32, 72, 2, 4, 65)
ns["picture"](150, 110, 2).save(d + "/a.png"); ns["picture"](150, 110, 9).save(d + "/b.png")
PY
python3 tools/tinymodel/make_tiny_gguf.py "$D/tiny-model.gguf" --arch qwen35moe --seed 7 --vision > /dev/null || exit 1
gcc -O1 -g -fsanitize=address -mavx2 -mfma -mf16c -ffast-math -w -I . -I src -I vk/include tools/visiontest/unit.c -o "$D/unit" -lm -lpthread -ldl || exit 1
mkdir "$D/store"
KATANA_CPU=1 ASAN_OPTIONS=detect_leaks=0 "$D/unit" "$D/tiny-model.gguf" "$D/mmproj-a.gguf" "$D/a.png" "$D/b.png" "$D/store" 2> "$D/err" || { bad=1; tail -20 "$D/err"; }
export KATANA_CPU=1 KATANA_IMAGE_TOKENS=64 HOME="$D/store"
t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: [$2] expected [$3]"; bad=1; fi; }
"$K" run "$D/tiny-model.gguf" "hello" 2 0 --image "$D/a.png" 2> "$D/err" > /dev/null
t "projector found by its base name" "$(grep -c 'vision: .*mmproj-a.gguf, up to 64' "$D/err")" 1
t "cost reported" "$(grep -c '^\[vision: image 150x110 -> 320x224, 70 tokens, encoded in' "$D/err")" 1
"$K" run "$D/tiny-model.gguf" "hello" 2 0 2> "$D/err" > /dev/null
t "text only: the projector is not loaded" "$(grep -c 'encoded' "$D/err")" 0
KATANA_MMPROJ=0 "$K" run "$D/tiny-model.gguf" "hello" 2 0 --image "$D/a.png" > /dev/null 2> "$D/err"; t "off: --image refused" "$?" 1
KATANA_MMPROJ="$D/wrong.gguf" "$K" run "$D/tiny-model.gguf" "hello" 2 0 2> "$D/err" > /dev/null
t "projector of another width refused" "$(grep -c 'vision: off' "$D/err")" 1
head -c 5000 "$D/mmproj-a.gguf" > "$D/cut.gguf"
KATANA_MMPROJ="$D/cut.gguf" "$K" run "$D/tiny-model.gguf" "hello" 2 0 --image "$D/a.png" > /dev/null 2> "$D/err"; t "cut projector: a message, no crash" "$(grep -c 'cannot encode the image' "$D/err")" 1
"$K" serve "$D/tiny-model.gguf" 18973 > "$D/srv" 2>&1 & S=$!
python3 - "$D" <<'PY' || bad=1
import base64, json, sys, time, urllib.request
d = sys.argv[1]
u = "http://127.0.0.1:18973/v1/chat/completions"
def ask(parts, n=4):
    q = json.dumps({"messages": [{"role": "user", "content": parts}], "max_tokens": n, "temperature": 0}).encode()
    return json.load(urllib.request.urlopen(urllib.request.Request(u, q, {"Content-Type": "application/json"}), timeout=60))
img = {"type": "image_url", "image_url": {"url": "data:image/png;base64," + base64.b64encode(open(d + "/a.png", "rb").read()).decode()}}
for _ in range(50):
    try: r0 = ask("hello"); break
    except OSError: time.sleep(0.2)
r1, r2 = ask([img, {"type": "text", "text": "hello"}]), ask([img, {"type": "text", "text": "hello"}])
n = r1["usage"]["prompt_tokens"] - r0["usage"]["prompt_tokens"]
good = n == 72 and r1["choices"][0]["message"]["content"] == r2["choices"][0]["message"]["content"]
print(("ok  " if good else "FAIL"), "API: image_url costs %d prompt tokens (70 vectors, start and end); repeated request gives the same answer" % n)
sys.exit(not good)
PY
kill $S; wait $S 2> /dev/null
t "API: the image was encoded once" "$(grep -c 'encoded in' "$D/srv")" 1
[ $bad = 0 ] && echo OK || echo FAILED
exit $bad
