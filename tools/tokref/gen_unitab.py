#!/usr/bin/env python3
"""Gera unitab.h: tabelas de faixas Unicode usadas pelo pre-tokenizador do katana.
Uso: python3 tools/tokref/gen_unitab.py > unitab.h
Categorias (unicodedata): L, N, M, U = Lu|Lt|Lm|Lo|M, W = Ll|Lm|Lo|M (as duas do regex estilo Kimi, sem Han: o corte do
Han e feito no C). A classe de letras do DeepSeek sai do tokenizer.json oficial (DS)."""
import sys, json, os, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))


def ranges(pred):
    out = []
    lo = None
    for c in range(0x110000):
        if 0xD800 <= c <= 0xDFFF:
            ok = False
        else:
            ok = pred(unicodedata.category(chr(c)))
        if ok and lo is None:
            lo = c
        elif not ok and lo is not None:
            out.append((lo, c - 1)); lo = None
    if lo is not None:
        out.append((lo, 0x10FFFF))
    return out


def emit(name, rs):
    print("static const uint32_t %s[][2] = {" % name)
    line = ""
    for a, b in rs:
        item = "{0x%X,0x%X}," % (a, b)
        if len(line) + len(item) > 110:
            print(line); line = ""
        line += item
    print(line)
    print("};")
    print("#define %s_N %d" % (name, len(rs)))


def ds_letters():
    j = json.load(open(os.path.join(HERE, "dl/DeepSeek-V2-Lite-Chat/tokenizer.json")))
    pat = j["pre_tokenizer"]["pretokenizers"][1]["pattern"]["Regex"]
    assert pat.startswith("\\s?[") and pat.endswith("]+")
    body = pat[4:-2]
    cs = list(body)
    rs = []
    i = 0
    while i < len(cs):
        a = cs[i]
        if i + 2 < len(cs) and cs[i + 1] == "-":
            rs.append((ord(a), ord(cs[i + 2]))); i += 3
        else:
            rs.append((ord(a), ord(a))); i += 1
    rs.sort()
    return rs


if __name__ == "__main__":
    print("/* gerado por tools/tokref/gen_unitab.py (Unicode %s): nao editar a mao */" % unicodedata.unidata_version)
    emit("UT_L", ranges(lambda c: c[0] == "L"))
    emit("UT_N", ranges(lambda c: c[0] == "N"))
    emit("UT_M", ranges(lambda c: c[0] == "M"))
    emit("UT_U", ranges(lambda c: c in ("Lu", "Lt", "Lm", "Lo") or c[0] == "M"))
    emit("UT_W", ranges(lambda c: c in ("Ll", "Lm", "Lo") or c[0] == "M"))
    emit("UT_DS", ds_letters())
