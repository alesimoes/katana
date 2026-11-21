import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NOTES = {}
NEXT = [1]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        if code != 204:
            self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def route(self, method):
        path = self.path.split("?")[0]
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        if path == "/notes":
            if method == "GET":
                return self.send(200, [NOTES[k] for k in sorted(NOTES)])
            if method == "POST":
                try:
                    data = json.loads(raw.decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    return self.send(400, {"error": "invalid json"})
                text = data.get("text") if isinstance(data, dict) else None
                if not isinstance(text, str) or not text.strip():
                    return self.send(400, {"error": "text must be a non-empty string"})
                note = {"id": NEXT[0], "text": text}
                NOTES[NEXT[0]] = note
                NEXT[0] += 1
                return self.send(201, note)
            return self.send(405, {"error": "method not allowed"})
        m = re.fullmatch(r"/notes/(\d+)", path)
        if m:
            if method not in ("GET", "DELETE"):
                return self.send(405, {"error": "method not allowed"})
            nid = int(m.group(1))
            if nid not in NOTES:
                return self.send(404, {"error": "note not found"})
            if method == "GET":
                return self.send(200, NOTES[nid])
            del NOTES[nid]
            return self.send(204)
        self.send(404, {"error": "not found"})

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_DELETE(self):
        self.route("DELETE")

    def do_PUT(self):
        self.route("PUT")

    def do_PATCH(self):
        self.route("PATCH")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
