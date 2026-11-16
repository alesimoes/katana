#!/usr/bin/env python3
# Le so os metadados de tokenizador de um GGUF (sem dependencias). Uso: ggufmeta.py modelo.gguf [--vocab saida.json]
import struct, sys, json

def rd(f, fmt):
    n = struct.calcsize(fmt)
    return struct.unpack("<" + fmt, f.read(n))[0]

def rstr(f):
    n = rd(f, "Q")
    return f.read(n).decode("utf-8", "surrogateescape")

SZ = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}

def rval(f, t):
    if t == 8: return rstr(f)
    if t == 9:
        et = rd(f, "I"); n = rd(f, "Q")
        return [rval(f, et) for _ in range(n)]
    return rd(f, SZ[t])

def meta(path):
    f = open(path, "rb")
    assert f.read(4) == b"GGUF"
    rd(f, "I"); rd(f, "Q"); nkv = rd(f, "Q")
    kv = {}
    for _ in range(nkv):
        k = rstr(f); t = rd(f, "I")
        kv[k] = rval(f, t)
    return kv

if __name__ == "__main__":
    kv = meta(sys.argv[1])
    for k, v in kv.items():
        if k.startswith("tokenizer.") and isinstance(v, list):
            print(k, "[%d]" % len(v))
        elif k.startswith("tokenizer.") or k == "general.architecture":
            print(k, repr(v)[:300])
    if len(sys.argv) > 3 and sys.argv[2] == "--vocab":
        json.dump({k: kv[k] for k in kv if k.startswith("tokenizer.ggml.")}, open(sys.argv[3], "w"))
