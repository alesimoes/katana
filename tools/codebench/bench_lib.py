"""Auxiliares para os setup.py das tarefas (servidor local de paginas, porta livre)."""
import os, shutil, socket, subprocess, sys, tempfile, time, urllib.request


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_site(site_dir):
    """copia site_dir para uma pasta temporaria e serve em 127.0.0.1 numa porta livre (python3 -m http.server).
    Devolve (porta, parar); parar() encerra o servidor e apaga a copia."""
    root = tempfile.mkdtemp(prefix="cb_site_")
    shutil.copytree(site_dir, root, dirs_exist_ok=True)
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop():
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        shutil.rmtree(root, ignore_errors=True)

    for _ in range(100):                      # espera o servidor responder
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=1).read()
            return port, stop
        except Exception:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    stop()
    raise RuntimeError("servidor local nao subiu")


# ---- tarefas de noticias (news-*): a resposta certa e buscada ao vivo, na hora, em fontes publicas

def http_get(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) codebench/1.0"})
    raw = urllib.request.urlopen(req, timeout=timeout).read()
    if raw[:2] == b"\x1f\x8b":               # alguns sites mandam gzip mesmo sem pedir
        import gzip
        raw = gzip.decompress(raw)
    return raw.decode("utf-8", "replace")


def wikidata_holder(entity, prop):
    """nomes (rotulos e apelidos em pt e en) de quem ocupa hoje o cargo prop da entidade (declaracao sem data de fim)"""
    import json, urllib.parse
    q = ("SELECT ?l WHERE { wd:%s p:%s ?s. ?s ps:%s ?p. FILTER NOT EXISTS { ?s pq:P582 ?e } "
         "{ ?p rdfs:label ?l } UNION { ?p skos:altLabel ?l } FILTER(lang(?l) IN (\"pt\", \"pt-br\", \"en\")) }" % (entity, prop, prop))
    d = json.loads(http_get("https://query.wikidata.org/sparql?format=json&query=" + urllib.parse.quote(q)))
    names = []
    for b in d["results"]["bindings"]:
        v = b["l"]["value"].strip()
        if v and v not in names:
            names.append(v)
    if not names:
        raise RuntimeError("wikidata sem resposta para %s %s" % (entity, prop))
    names.sort(key=lambda n: -len(n))          # o nome mais completo primeiro
    return names
