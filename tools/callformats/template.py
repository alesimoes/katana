# Imprime o tokenizer.chat_template de um GGUF (so le os metadados; nao toca nos pesos).
import struct, sys
f = open(sys.argv[1], 'rb')
assert f.read(4) == b'GGUF'
ver, nt, nkv = struct.unpack('<IQQ', f.read(20))
SZ = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
def rs(): n, = struct.unpack('<Q', f.read(8)); return f.read(n)
def skip(t):
    if t == 8: rs()
    elif t == 9:
        et, n = struct.unpack('<IQ', f.read(12))
        if et == 8:
            for _ in range(n): rs()
        else: f.seek(SZ[et] * n, 1)
    else: f.seek(SZ[t], 1)
for _ in range(nkv):
    k = rs().decode(); t, = struct.unpack('<I', f.read(4))
    if k == 'tokenizer.chat_template': sys.stdout.write(rs().decode()); break
    skip(t)
