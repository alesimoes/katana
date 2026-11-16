#!/usr/bin/env python3
"""Compara o `katana toktest` com a referencia (out/<modelo>.ref.json).

Uso: python3 -I tools/tokref/compare.py [--bin ./katana] [--detalhe N] [modelo ...]
Para cada modelo roda o binario nos casos de tokenizacao e de conversa e imprime: % de casos identicos e, para cada caso
diferente, o primeiro ponto de divergencia (posicao, ids e textos dos tokens de cada lado)."""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ggufmeta, ref

OUT = os.path.join(HERE, "out")


def run(binary, gguf, arq, chat):
    cmd = [binary, "toktest", gguf, arq] + (["--chat"] if chat else [])
    env = dict(os.environ, KATANA_DATE="26 Jul 2024")      # a data que o template de referencia usa
    r = subprocess.run(cmd, capture_output=True, check=True, env=env)
    return r.stdout.decode("utf-8", "replace").split("\n")[:-1]


def first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return min(len(a), len(b))


def show(ids, toks, i, n=4):
    seg = ids[max(0, i - 1): i + n]
    return "%s (%s)" % (seg, "|".join(repr(toks[x]) if 0 <= x < len(toks) else "?" for x in seg))


def main():
    args = sys.argv[1:]
    binary = "./katana"; detalhe = 12; only = []
    while args:
        a = args.pop(0)
        if a == "--bin": binary = args.pop(0)
        elif a == "--detalhe": detalhe = int(args.pop(0))
        else: only.append(a)
    resumo = []
    for name, (dd, gg) in ref.MODELS.items():
        if only and name not in only:
            continue
        rp = os.path.join(OUT, name + ".ref.json")
        gp = os.path.join(ref.MODELOS, gg)
        if not os.path.exists(rp) or not os.path.exists(gp):
            print("== %s: sem referencia ou sem GGUF (pulado)" % name); continue
        R = json.load(open(rp))
        toks = ggufmeta.meta(gp)["tokenizer.ggml.tokens"]
        print("==", name, "| tokenizador de referencia:", R["tokenizer"], "| template de referencia:", R["ref_tmpl"])
        # ---- tokenizacao
        kat = run(binary, gp, os.path.join(OUT, "corpus.hex"), False)
        corpus = [bytes.fromhex(l).decode("utf-8") for l in open(os.path.join(OUT, "corpus.hex")).read().split("\n") if l]
        ok = 0; bad = []
        for i, (ln, rids) in enumerate(zip(kat, R["tok"])):
            kids = [int(x) for x in ln.split()]
            if kids == rids: ok += 1
            else: bad.append((i, kids, rids))
        print("  tokenizacao: %d/%d identicos (%.1f%%)" % (ok, len(corpus), 100.0 * ok / len(corpus)))
        for i, kids, rids in bad[:detalhe]:
            d = first_diff(kids, rids)
            print("    caso %d %r\n      divergencia na posicao %d | katana %s\n                                  | ref    %s" % (
                i, corpus[i][:60], d, show(kids, toks, d), show(rids, toks, d)))
        if len(bad) > detalhe: print("    ... mais %d casos diferentes" % (len(bad) - detalhe))
        tok_ok = ok; tok_n = len(corpus)
        # ---- conversa
        kat = run(binary, gp, os.path.join(OUT, "chat.hex"), True)
        txt_ok = ids_ok = n = 0; badc = []
        for ci, ch in enumerate(R["chat"]):
            for ti, key in enumerate(("t1", "t2")):
                line = kat[2 * ci + ti]
                th, ids = line.split("\t")
                ktext = bytes.fromhex(th).decode("utf-8", "replace")
                kids = [int(x) for x in ids.split()]
                rtext, rids = ch[key]
                if key == "t2" and R["think"]:
                    # modelos que raciocinam: o template descarta o raciocinio das respostas antigas, mas o cache KV do motor
                    # as mantem (o prompt termina em <think>\n e a resposta gerada vem depois); compara sem esse trecho
                    ktext = ktext.replace("assistant\n<think>\n", "assistant\n", 1)
                    kids = rids if ktext == rtext else kids
                n += 1
                if ktext == rtext: txt_ok += 1
                if kids == rids: ids_ok += 1
                if ktext != rtext or kids != rids: badc.append((ci, key, ktext, rtext))
        print("  conversa (texto do template): %d/%d identicos (%.1f%%); ids: %d/%d" % (txt_ok, n, 100.0 * txt_ok / n, ids_ok, n))
        for ci, key, kt, rt in badc[:detalhe]:
            d = first_diff(kt, rt)
            print("    conversa %d %s: divergencia no caractere %d\n      katana ...%r\n      ref    ...%r" % (
                ci, key, d, kt[max(0, d - 20): d + 30], rt[max(0, d - 20): d + 30]))
        if len(badc) > detalhe: print("    ... mais %d diferentes" % (len(badc) - detalhe))
        resumo.append((name, tok_ok, tok_n, txt_ok, n))
    print("\nresumo (modelo | tokenizacao | conversa-texto)")
    for name, a, b, c, d in resumo:
        print("  %-18s %3d/%d %5.1f%%   %2d/%d %5.1f%%" % (name, a, b, 100.0 * a / b, c, d, 100.0 * c / d))


if __name__ == "__main__":
    main()
