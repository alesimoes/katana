#!/bin/sh
# Damaged image files against the decoders, in a build with the address and undefined-behaviour checkers.
#   sh tools/visiontest/fuzz.sh [files per sample]      (needs Pillow to write the samples)
cd "$(dirname "$0")/../.." || exit 1
N=${1:-20000}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
sh build.sh -o "$D/katana-asan" -g -O1 -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined >/dev/null 2>&1 || { echo "build failed"; exit 1; }
python3 - "$D" <<'PY'
import sys
from PIL import Image
d = sys.argv[1]
im = Image.radial_gradient("L").resize((61, 47)).convert("RGB")
for i, (m, kw) in enumerate((("RGB", {}), ("RGBA", {}), ("P", {}), ("1", {}), ("LA", {}), ("I;16", {}))):
    im.convert(m if m != "I;16" else "I").save("%s/p%d.png" % (d, i), **kw)
for i, kw in enumerate((dict(quality=75), dict(quality=90, subsampling=0, progressive=True), dict(quality=40, progressive=True), dict(quality=80, restart_marker_blocks=2), dict(quality=80, subsampling=1))):
    im.save("%s/j%d.jpg" % (d, i), **kw)
im.convert("CMYK").save(d + "/j9.jpg"); im.convert("L").save(d + "/j8.jpg", progressive=True)
ex = Image.Exif(); ex[0x0112] = 6; im.save(d + "/j7.jpg", exif=ex)
im.save(d + "/b0.bmp"); im.convert("P").save(d + "/b1.bmp"); im.convert("RGBA").save(d + "/b2.bmp"); im.convert("1").save(d + "/b3.bmp")
PY
bad=0
for f in "$D"/*.png "$D"/*.jpg "$D"/*.bmp; do
  o=$(ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 timeout 1200 "$D/katana-asan" vision fuzz "$f" "$N" "$(date +%s)" 2>"$D/err") || { bad=1; echo "FAIL $(basename "$f")"; head -20 "$D/err"; }
  echo "$(basename "$f"): $o"
done
[ $bad = 0 ] && echo OK
exit $bad
