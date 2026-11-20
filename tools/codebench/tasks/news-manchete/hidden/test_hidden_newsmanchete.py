import os, re, unicodedata, unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def norm(s):
    s = unicodedata.normalize("NFKD", s)
    return " ".join("".join(c for c in s if not unicodedata.combining(c)).lower().split())


def answer():
    with open(os.path.join(HERE, "answer.txt"), encoding="utf-8") as f:
        return f.read().strip()

import urllib.request


class T(unittest.TestCase):
    def test_answer(self):
        lines = [l.strip() for l in answer().splitlines() if l.strip()]
        self.assertEqual(len(lines), 2, "duas linhas: titulo e URL")
        title, url = lines
        self.assertTrue(re.match(r"https?://[^/\s]+/\S+", url), "a segunda linha nao e a URL de uma pagina")
        host = url.split("/")[2].lower()
        self.assertFalse(re.search(r"(google|bing|duckduckgo|yahoo|searx|brave)\.", host), "URL de buscador, nao da noticia")
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) codebench/1.0"})
        raw = urllib.request.urlopen(req, timeout=40).read()
        if raw[:2] == b"\x1f\x8b":           # alguns sites mandam gzip mesmo sem pedir
            import gzip
            raw = gzip.decompress(raw)
        html = raw.decode("utf-8", "replace")
        page = norm(re.sub(r"<[^>]+>", " ", html))
        words = [w for w in re.findall(r"[a-z0-9]+", norm(title)) if len(w) >= 4]
        self.assertGreaterEqual(len(words), 3, "titulo curto demais")
        hit = sum(1 for w in words if w in page)
        self.assertGreaterEqual(hit, 0.7 * len(words), "o titulo nao aparece na pagina")
        dates = []
        for d in os.environ["CB_DAYS"].split("|"):
            y, m, dd = d.split("-")
            dates += [d, "%s/%s/%s" % (dd, m, y)]
        self.assertTrue(any(d in html for d in dates), "a pagina nao traz data de hoje, ontem ou anteontem")


if __name__ == "__main__":
    unittest.main()
