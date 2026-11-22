# Servidor HTTP local para tools/tooltest/web.sh (so o teste usa; nao faz parte do produto).
# Uso: python3 websrv.py <arquivo-da-porta>. Paginas: /p1 /p2 (falam de "borrow checker"), /p3 (sem relacao), /long (60 KB),
# /slow (demora 12 s), /fail (derruba a conexao), /count (acessos por caminho).
import http.server, socketserver, sys, time

HITS = {}
def filler(n, tag):
    return "".join("<p>Section %s paragraph %d talks about gardening, weather, tomato plants and nothing else of interest.</p>\n" % (tag, i) for i in range(n))
PAGES = {
    "/p1": "<html><head><title>Rust ownership guide</title></head><body><main>" + filler(40, "a") +
           "<p>The borrow checker is the part of the Rust compiler that enforces ownership: every value has one owner and references must not outlive it.</p>\n" +
           filler(20, "b") + "<p>Mutable borrows are exclusive, so the borrow checker rejects two live mutable references to the same data.</p>" + filler(10, "c") + "</main></body></html>",
    "/p2": "<html><head><title>Lifetimes</title></head><body><article>" + filler(30, "x") +
           "<p>Lifetimes annotate how long references live; the borrow checker uses them to prove that no reference dangles.</p>" + filler(30, "y") + "</article></body></html>",
    "/p3": "<html><head><title>Tomatoes</title></head><body><main>" + filler(60, "z") + "</main></body></html>",
}
PAGES["/long"] = ("<html><head><title>Long page about compilers</title></head><body><main>" + "".join(
    "<p>Chapter %d of the long page: the parser reads tokens and builds a tree, nothing special at all in this paragraph number %d.</p>\n" % (i, i) for i in range(500)) +
    "<p>Special note: the optimizer inlining pass removes calls and is described only here.</p>" + "".join(
    "<p>Chapter %d appendix: more tokens and trees and filler text to make the page long enough.</p>\n" % i for i in range(100)) + "</main></body></html>")

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        path = self.path.split("?")[0]
        if path != "/count": HITS[path] = HITS.get(path, 0) + 1
        if path == "/count":
            body = repr(HITS).encode()
        elif path == "/slow":
            time.sleep(12); body = b"<html><body>too late</body></html>"
        elif path == "/fail":
            self.connection.close(); return
        elif path in PAGES:
            body = PAGES[path].encode()
        elif path in ("/gone", "/boom"):
            code = 404 if path == "/gone" else 500
            body = b"<html><head><title>Oops</title></head><body><h1>Sorry</h1><p>This friendly page says the borrow checker article moved.</p></body></html>"
            self.send_response(code); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body); return
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except Exception: pass

class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

s = S(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(s.server_address[1]))
s.serve_forever()
