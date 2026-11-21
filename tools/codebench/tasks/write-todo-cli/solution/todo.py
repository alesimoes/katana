import json, os, sys

USO = "uso: todo.py add TEXTO... | list | done N | rm N"


def carregar(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def salvar(path, itens):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(itens, f, ensure_ascii=False)


def indice(arg, itens):
    try:
        n = int(arg)
    except ValueError:
        print("erro: numero invalido: %s" % arg, file=sys.stderr)
        sys.exit(1)
    if n < 1 or n > len(itens):
        print("erro: tarefa %d nao existe" % n, file=sys.stderr)
        sys.exit(1)
    return n - 1


def main(argv):
    path = os.environ.get("TAREFAS_ARQUIVO") or "tarefas.json"
    if not argv or argv[0] not in ("add", "list", "done", "rm"):
        print(USO, file=sys.stderr)
        return 2
    cmd, resto = argv[0], argv[1:]
    itens = carregar(path)
    if cmd == "add":
        texto = " ".join(resto).strip()
        if not texto:
            print(USO, file=sys.stderr)
            return 2
        itens.append({"texto": texto, "feita": False})
        salvar(path, itens)
    elif cmd == "list":
        if not itens:
            print("nenhuma tarefa")
        for i, t in enumerate(itens, 1):
            print("%d [%s] %s" % (i, "x" if t["feita"] else " ", t["texto"]))
    else:
        if len(resto) != 1:
            print(USO, file=sys.stderr)
            return 2
        i = indice(resto[0], itens)
        if cmd == "done":
            itens[i]["feita"] = True
        else:
            del itens[i]
        salvar(path, itens)
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
