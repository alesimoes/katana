#!/usr/bin/env python3
"""Gera nfc.h: tabelas da normalizacao Unicode NFC (decomposicao canonica, classes de combinacao, pares de composicao).
Uso: python3 tools/tokref/gen_nfc.py > nfc.h"""
import unicodedata as U

dec = []       # (cp, [cps])  decomposicao canonica completa (sem silabas Hangul, que sao algoritmicas)
for cp in range(0x110000):
    if 0xD800 <= cp <= 0xDFFF or 0xAC00 <= cp <= 0xD7A3:
        continue
    ch = chr(cp)
    d = U.normalize("NFD", ch)
    if d != ch:
        dec.append((cp, [ord(x) for x in d]))
maxlen = max(len(d) for _, d in dec)

ccc = []       # faixas (lo, hi, classe)
lo = None; cur = 0
for cp in range(0x110001):
    c = U.combining(chr(cp)) if cp < 0x110000 and not 0xD800 <= cp <= 0xDFFF else 0
    if lo is not None and c != cur:
        ccc.append((lo, cp - 1, cur)); lo = None
    if c and lo is None:
        lo = cp; cur = c

pairs = []     # (a, b, composto): so as composicoes primarias (fora das exclusoes)
for cp, d in dec:
    if len(d) == 2 or True:
        pass
# pares: decomposicao canonica de UM passo de tamanho 2 cujo NFC recompoe o caractere
for cp in range(0x110000):
    if 0xD800 <= cp <= 0xDFFF or 0xAC00 <= cp <= 0xD7A3:
        continue
    d = U.decomposition(chr(cp))
    if not d or d.startswith("<"):
        continue
    parts = [int(x, 16) for x in d.split()]
    if len(parts) != 2:
        continue
    if U.normalize("NFC", chr(parts[0]) + chr(parts[1])) == chr(cp):
        pairs.append((parts[0], parts[1], cp))
pairs.sort()

print("/* gerado por tools/tokref/gen_nfc.py (Unicode %s): nao editar a mao */" % U.unidata_version)
print("#define NFC_MAXD %d" % maxlen)
print("static const uint32_t NFC_DEC[][%d] = {   /* cp, n, d1..dn */" % (2 + maxlen))
line = ""
for cp, d in dec:
    item = "{" + ",".join(["0x%X" % cp, str(len(d))] + ["0x%X" % x for x in d] + ["0"] * (maxlen - len(d))) + "},"
    if len(line) + len(item) > 110:
        print(line); line = ""
    line += item
print(line); print("};"); print("#define NFC_DEC_N %d" % len(dec))
print("static const uint32_t NFC_CCC[][3] = {")
line = ""
for a, b, c in ccc:
    item = "{0x%X,0x%X,%d}," % (a, b, c)
    if len(line) + len(item) > 110:
        print(line); line = ""
    line += item
print(line); print("};"); print("#define NFC_CCC_N %d" % len(ccc))
print("static const uint32_t NFC_PAIR[][3] = {")
line = ""
for a, b, c in pairs:
    item = "{0x%X,0x%X,0x%X}," % (a, b, c)
    if len(line) + len(item) > 110:
        print(line); line = ""
    line += item
print(line); print("};"); print("#define NFC_PAIR_N %d" % len(pairs))
