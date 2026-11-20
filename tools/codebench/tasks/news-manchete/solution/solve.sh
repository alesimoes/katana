#!/bin/sh
python3 - <<'EOF'
import re, urllib.request
req = urllib.request.Request("https://g1.globo.com/rss/g1/", headers={"User-Agent": "Mozilla/5.0 codebench/1.0"})
import gzip
x = urllib.request.urlopen(req, timeout=40).read()
x = (gzip.decompress(x) if x[:2] == b"\x1f\x8b" else x).decode("utf-8", "replace")
item = x.split("<item>")[1]
def tag(t):
    v = re.search(r"<%s>(.*?)</%s>" % (t, t), item, re.S).group(1).strip()
    return re.sub(r"^<!\[CDATA\[|\]\]>$", "", v).strip()
open("answer.txt", "w").write(tag("title") + "\n" + tag("link") + "\n")
EOF
