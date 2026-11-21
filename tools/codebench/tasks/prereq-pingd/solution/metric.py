import os
import re
import socket
import sys


def main(argv):
    if len(argv) != 2 or not re.fullmatch(r"[A-Za-z0-9._]+", argv[0]) or not re.fullmatch(r"-?\d+(\.\d+)?", argv[1]):
        print("uso: metric.py NOME VALOR (NOME: letras, numeros, . e _; VALOR: numero)", file=sys.stderr)
        return 2
    name, value = argv
    addr = os.environ.get("PINGD_ADDR") or "127.0.0.1:9125"
    host, _, port = addr.rpartition(":")
    try:
        with socket.create_connection((host, int(port)), timeout=3) as s:
            s.settimeout(3)
            s.sendall(("%s %s\n" % (name, value)).encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(1024)
                if not chunk:
                    break
                buf += chunk
    except (OSError, ValueError) as e:
        print("erro: o pingd nao esta acessivel em %s (%s). Suba o servico ou ajuste PINGD_ADDR." % (addr, e), file=sys.stderr)
        return 1
    reply = buf.decode("utf-8", "replace").strip()
    if reply == "OK":
        print("enviado %s %s" % (name, value))
        return 0
    if reply.startswith("ERR"):
        print("pingd recusou: %s" % reply[3:].strip(), file=sys.stderr)
    else:
        print("erro: resposta inesperada do pingd: %r" % reply, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
