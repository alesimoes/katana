#!/usr/bin/env python3
"""Gera src/coder/codegenc_rt.inc a partir de tools/codegen/rt/cgrt.h: cada secao (/*@@ nome */) vira um vetor de linhas de texto C.
Todas as funcoes estaticas viram `static inline` (nada de aviso de funcao nao usada num programa que nao usa a secao inteira).
Uso: python3 tools/codegen/rt2inc.py [--check]"""
import re, sys, os
here = os.path.dirname(os.path.abspath(__file__)); root = os.path.dirname(os.path.dirname(here))
def convert(src):
    secs, cur = [], None
    for ln in src.split("\n"):
        m = re.match(r"/\*@@ (\w+) \*/$", ln)
        if m: cur = (m.group(1), []); secs.append(cur); continue
        if cur is not None: cur[1].append(ln)
    out = ["/* GERADO por tools/codegen/rt2inc.py a partir de tools/codegen/rt/cgrt.h: nao edite aqui (docs/codegen.md, \"Backend C\"). */\n"]
    for name, lines in secs:
        while lines and not lines[-1].strip(): lines.pop()
        out.append("static const char *const CGX_C_RT_%s[] = {\n" % name.upper())
        for ln in lines:
            if "CG_NOINL" in ln: out.append('    "%s",\n' % ln.replace("\\", "\\\\").replace('"', '\\"')); continue
            ln = re.sub(r"^((?:CG_NORET )?)static (?!inline)((?:const )?[A-Za-z_][\w ]*?[\s\*]+cg_\w+\()", r"\1static inline \2", ln)
            ln = re.sub(r"^static (?!inline)(CgList|CgMap|CgStr|CgV|CgRn|CgRt)(\s*\*?\s*cg_\w+\()", r"static inline \1\2", ln)
            e = ln.replace("\\", "\\\\").replace('"', '\\"')
            out.append('    "%s",\n' % e)
        out.append("    NULL};\n")
    return "".join(out)
def convert_go(src):
    out = ["/* GERADO por tools/codegen/rt2inc.py a partir de tools/codegen/rt/cgrt.go: nao edite aqui (docs/codegen.md, \"Backend Go\"). */\n", "static const char *const CGX_GO_RT[] = {\n"]
    for ln in src.rstrip("\n").split("\n"):
        out.append('    "%s",\n' % ln.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t"))
    out.append("    NULL};\n")
    return "".join(out)
if __name__ == "__main__":
    jobs = [("cgrt.h", "codegenc_rt.inc", convert), ("cgrt.go", "codegengo_rt.inc", convert_go)]
    bad = 0
    for srcn, dstn, fn in jobs:
        srcp = os.path.join(here, "rt", srcn)
        if not os.path.exists(srcp): continue
        new = fn(open(srcp).read()); dst = os.path.join(root, "src", "coder", dstn)
        if "--check" in sys.argv:
            if not (os.path.exists(dst) and open(dst).read() == new): bad = 1
        else: open(dst, "w").write(new)
    sys.exit(bad)
