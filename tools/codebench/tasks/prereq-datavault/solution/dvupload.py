import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

CRED = os.path.join("~", ".datavault", "credentials")


def token():
    path = os.path.expanduser(CRED)
    try:
        with open(path, encoding="utf-8") as f:
            tok = json.load(f).get("token")
    except (OSError, ValueError, AttributeError):
        tok = None
    if not tok or not isinstance(tok, str):
        print("error: no Datavault token. Create %s containing {\"token\": \"...\"}" % CRED, file=sys.stderr)
        sys.exit(1)
    return tok


def main(argv):
    if len(argv) != 1 or not os.path.isfile(argv[0]):
        print("usage: dvupload.py FILE (the file must exist)", file=sys.stderr)
        return 2
    tok = token()
    base = os.environ.get("DATAVAULT_URL") or "https://api.datavault.example"
    with open(argv[0], "rb") as f:
        data = f.read()
    url = "%s/v1/files?name=%s" % (base.rstrip("/"), urllib.parse.quote(os.path.basename(argv[0])))
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Authorization": "Bearer " + tok, "Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = json.loads(r.read().decode("utf-8"))
        print(body["id"])
        return 0
    except urllib.error.HTTPError as e:
        if e.code == 401:
            print("error: Datavault rejected the token (401); check %s" % CRED, file=sys.stderr)
        else:
            print("error: Datavault answered HTTP %d" % e.code, file=sys.stderr)
    except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
        print("error: could not upload to %s: %s" % (base, getattr(e, "reason", e)), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
