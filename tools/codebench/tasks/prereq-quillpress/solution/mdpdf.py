import os
import shutil
import subprocess
import sys


def main(argv):
    if len(argv) != 1 or not os.path.isdir(argv[0]):
        print("uso: mdpdf.py PASTA (a pasta precisa existir)", file=sys.stderr)
        return 2
    if shutil.which("quillpress") is None:
        print("erro: o comando quillpress nao esta instalado (ou nao esta no PATH); veja o README", file=sys.stderr)
        return 3
    folder = argv[0]
    done, failed = 0, 0
    for name in sorted(os.listdir(folder)):
        src = os.path.join(folder, name)
        if not name.endswith(".md") or not os.path.isfile(src):
            continue
        out = os.path.join(folder, name[:-3] + ".pdf")
        p = subprocess.run(["quillpress", src, "-o", out], capture_output=True, text=True)
        if p.returncode != 0:
            failed += 1
            print("falhou: %s (quillpress saiu com %d) %s" % (name, p.returncode, p.stderr.strip()), file=sys.stderr)
        else:
            done += 1
    print("%d convertidos" % done)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
