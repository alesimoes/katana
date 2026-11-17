#!/usr/bin/env python3
# Teste "agulha no palheiro" para contexto longo: monta um texto de enchimento com docs/*.md
# (concatenados e repetidos), esconde uma frase com um fato arbitrario numa posicao relativa,
# pergunta no fim e confere se a resposta traz a palavra.
#
# uso: tools/agulha.py [--modelo M] [--tokens 8000,16000] [--pos 0.1,0.5,0.9] [--modo fp16,q8]
#                      [--saida DIR] [--timeout S] [--semente N]
# Tokens sao estimados por caracteres (~3,3 caracteres por token em portugues com markdown);
# o katana imprime o numero real de tokens do prompt, que vai para a tabela.
import argparse, glob, os, random, re, subprocess, sys, time

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PALAVRAS = ["girassol", "tamborim", "periquito", "zabumba", "carambola", "jabuticaba", "marimba",
            "pirarucu", "taturana", "ventania", "bergamota", "cambuci", "guaxinim", "sabia"]

def enchimento(nchar):
    textos = [open(f, encoding="utf-8", errors="replace").read() for f in sorted(glob.glob(os.path.join(RAIZ, "docs", "*.md")))]
    base = "\n\n".join(textos)
    out = ""
    while len(out) < nchar:
        out += base + "\n\n"
    return out[:nchar]

def monta(ntok, rel, palavra, cpt):
    pergunta = ("\n\n---\nPergunta: no texto acima ha uma frase sobre a senha do cofre. "
                "Qual e a senha do cofre? Responda so com a palavra.\nResposta:")
    agulha = "\n\nA senha do cofre e %s. Guarde esta informacao.\n\n" % palavra
    corpo = enchimento(int(ntok * cpt))
    i = int(len(corpo) * rel)
    j = corpo.rfind("\n", 0, i)        # corta numa quebra de linha
    if j > 0: i = j + 1
    return corpo[:i] + agulha + corpo[i:] + pergunta

def roda(katana, modelo, arq, modo, ctx, timeout):
    env = dict(os.environ, KATANA_CPU="1", KATANA_CTX=str(ctx))
    env.pop("KATANA_BENCH_POS", None)
    if modo == "fp16": env.update(KATANA_CPU_KV16="1", KATANA_CPU_KV8="0")
    elif modo == "q8": env.update(KATANA_CPU_KV8="1")
    elif modo == "fp32": env.update(KATANA_CPU_KV16="0", KATANA_CPU_KV8="0")
    t0 = time.time()
    try:
        p = subprocess.run([katana, "run", modelo, "@" + arq, "12", "0"], env=env, capture_output=True,
                           text=True, errors="replace", timeout=timeout)
        out, err = p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return None, "timeout", 0, time.time() - t0
    m = re.search(r"\[prompt: (\d+) tokens", err)
    return out, err, int(m.group(1)) if m else 0, time.time() - t0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modelo", default=os.path.join(RAIZ, "modelos", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf"))
    ap.add_argument("--katana", default=os.path.join(RAIZ, "katana"))
    ap.add_argument("--tokens", default="8000,16000")
    ap.add_argument("--pos", default="0.1,0.5,0.9")
    ap.add_argument("--modo", default="fp16,q8")
    ap.add_argument("--saida", default="/tmp/agulha")
    ap.add_argument("--timeout", type=int, default=7200)
    ap.add_argument("--semente", type=int, default=7)
    ap.add_argument("--cpt", type=float, default=3.3, help="caracteres por token (estimativa)")
    a = ap.parse_args()
    if not os.path.exists(a.modelo):
        sys.exit("modelo nao encontrado: %s (use --modelo)" % a.modelo)
    os.makedirs(a.saida, exist_ok=True)
    rnd = random.Random(a.semente)
    linhas = []
    print("| tokens (pedido / real) | posicao | modo | resultado | resposta | tempo |", flush=True)
    print("|---|---|---|---|---|---|", flush=True)
    for nt in [int(x) for x in a.tokens.split(",")]:
        for rel in [float(x) for x in a.pos.split(",")]:
            palavra = rnd.choice(PALAVRAS)
            arq = os.path.join(a.saida, "agulha-%d-%02d.txt" % (nt, int(rel * 100)))
            with open(arq, "w", encoding="utf-8") as f: f.write(monta(nt, rel, palavra, a.cpt))
            ctx = 1 << max(12, (int(nt * 1.3) + 64).bit_length())
            for modo in a.modo.split(","):
                out, err, real, dt = roda(a.katana, a.modelo, arq, modo, ctx, a.timeout)
                if out is None: res, resp = "timeout", ""
                else:
                    resp = " ".join(out.split())[:60]
                    res = "acerto" if palavra.lower() in out.lower() else "erro"
                    with open(arq + "." + modo + ".err", "w") as f: f.write(err)
                print("| %d / %d | %d%% | %s | %s (%s) | %s | %.0f s |" % (nt, real, int(rel * 100), modo, res, palavra, resp, dt), flush=True)

if __name__ == "__main__":
    main()
