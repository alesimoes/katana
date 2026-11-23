#!/usr/bin/env python3
"""compare_gguf: compares two GGUF files tensor by tensor (names, dims, types, bytes) and key by key.

  python3 tools/convert/compare_gguf.py A.gguf B.gguf [--tol-names ssm_a] [--ignore-keys k1,k2]

A is the reference (the tiny GGUF written by make_tiny_gguf.py), B the converted one. Tensors listed in --tol-names
(default: ssm_a, the only one that goes through exp/log in float32 on both sides) are compared as float32 within a
relative 1e-6; everything else must be byte-identical. Keys present in only one file are listed; values of common keys
must be equal (floats within 1e-6 relative). Only the standard library is used. Exit code 0 = equal.
"""
import argparse, struct, sys, math


class G:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.kv, self.tensors = {}, {}
        u = self.u
        magic, ver, nt, nk = u("<IIQQ")
        assert magic == 0x46554747, "not a GGUF: " + path
        for _ in range(nk):
            k = self.s()
            t, = u("<I")
            self.kv[k] = self.val(t)
        for _ in range(nt):
            n = self.s()
            nd, = u("<I")
            dims = list(u("<%dQ" % nd))
            ty, off = u("<IQ")
            self.tensors[n] = (dims, ty, off)
        self.align = self.kv.get("general.alignment", 32)
        pos = self.f.tell()
        self.data0 = (pos + self.align - 1) // self.align * self.align
        # tamanho em bytes de cada tensor: pelo proximo deslocamento
        offs = sorted(set(o for _, _, o in self.tensors.values()))
        import os
        end = os.path.getsize(path) - self.data0
        self.size = {}
        for n, (d, ty, o) in self.tensors.items():
            nxt = [x for x in offs if x > o]
            self.size[n] = (nxt[0] if nxt else end) - o

    def u(self, fmt):
        return struct.unpack(fmt, self.f.read(struct.calcsize(fmt)))

    def s(self):
        n, = self.u("<Q")
        return self.f.read(n).decode("utf-8", "replace")

    def val(self, t):
        if t == 8:
            return self.s()
        if t == 9:
            et, n = self.u("<IQ")
            return [self.val(et) for _ in range(n)]
        sz = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "B", 10: "Q", 11: "q", 12: "d"}[t]
        v, = self.u("<" + sz)
        return bool(v) if t == 7 else v

    def raw(self, name):
        self.f.seek(self.data0 + self.tensors[name][2])
        return self.f.read(self.size[name])


def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return abs(a - b) <= 1e-6 * max(abs(a), abs(b), 1e-30)
    return a == b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tol-names", default="ssm_a")
    ap.add_argument("--tol", type=float, default=1e-6, help="relative tolerance for the --tol-names tensors (bf16/f16 checkpoints need 1e-2: A_log is stored rounded)")
    ap.add_argument("--ignore-keys", default="general.name,general.alignment,general.quantized_by,general.file_type,general.quantization_version,general.type")
    args = ap.parse_args()
    A, B = G(args.a), G(args.b)
    tol = set(args.tol_names.split(","))
    ign = set(args.ignore_keys.split(","))
    bad = 0
    # chaves
    ka, kb = set(A.kv) - ign, set(B.kv) - ign
    for k in sorted(ka - kb):
        print("key only in A:", k)
    for k in sorted(kb - ka):
        print("key only in B:", k)
    for k in sorted(ka & kb):
        va, vb = A.kv[k], B.kv[k]
        if isinstance(va, list):
            ok = len(va) == len(vb) and all(same(x, y) for x, y in zip(va, vb))
        else:
            ok = same(va, vb)
        if not ok:
            bad += 1
            print("KEY DIFFERS:", k, str(va)[:80], "!=", str(vb)[:80])
    # tensores
    na, nb = set(A.tensors), set(B.tensors)
    for n in sorted(na - nb):
        bad += 1; print("TENSOR only in A:", n)
    for n in sorted(nb - na):
        bad += 1; print("TENSOR only in B:", n)
    nbytes = 0
    maxrel = 0.0
    for n in sorted(na & nb):
        da, ta, _ = A.tensors[n]
        db, tb, _ = B.tensors[n]
        if da != db or ta != tb:
            bad += 1; print("TENSOR META DIFFERS:", n, da, ta, db, tb)
            continue
        ra, rb = A.raw(n), B.raw(n)
        nbytes += len(ra)
        if ra == rb:
            continue
        base = n.split(".")[-1] if n.endswith(".bias") else n.split(".")[-1]
        if (n.split("blk.")[-1].split(".", 1)[-1] in tol or n.rsplit(".", 1)[-1] in tol) and ta == 0 and len(ra) == len(rb):
            xs = struct.unpack("<%df" % (len(ra) // 4), ra[: len(ra) // 4 * 4])
            ys = struct.unpack("<%df" % (len(rb) // 4), rb[: len(rb) // 4 * 4])
            rel = max(abs(x - y) / max(abs(x), 1e-30) for x, y in zip(xs, ys))
            maxrel = max(maxrel, rel)
            if rel > args.tol:
                bad += 1; print("TENSOR (tolerant) DIFFERS:", n, "max rel", rel)
            continue
        bad += 1
        i = next(i for i in range(min(len(ra), len(rb))) if ra[i] != rb[i]) if len(ra) == len(rb) else -1
        print("TENSOR BYTES DIFFER:", n, "size", len(ra), len(rb), "first diff at byte", i)
    print("compared %d tensors (%d bytes), %d common keys; tolerant max rel diff %.2e; %d problem(s)" % (len(na & nb), nbytes, len(ka & kb), maxrel, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
