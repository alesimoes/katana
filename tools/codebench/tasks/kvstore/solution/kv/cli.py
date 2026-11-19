import json

from kv.store import Store


def main(argv):
    if len(argv) < 2:
        print("ERROR usage: PATH COMMAND [ARGS]")
        return 2
    path, cmd, args = argv[0], argv[1], argv[2:]
    want = {"set": 2, "get": 1, "del": 1, "keys": 0, "compact": 0}
    if cmd not in want:
        print("ERROR unknown command: %s" % cmd)
        return 2
    if len(args) != want[cmd]:
        print("ERROR wrong number of arguments for %s" % cmd)
        return 2
    try:
        s = Store(path)
        if cmd == "set":
            try:
                value = json.loads(args[1])
            except ValueError:
                print("ERROR value is not valid JSON")
                return 2
            s.set(args[0], value)
            print("OK")
        elif cmd == "get":
            missing = object()
            v = s.get(args[0], missing)
            if v is missing:
                print("NOT FOUND")
                return 1
            print(json.dumps(v))
        elif cmd == "del":
            if not s.delete(args[0]):
                print("NOT FOUND")
                return 1
            print("DELETED")
        elif cmd == "keys":
            for k in s.keys():
                print(k)
        else:
            print("removed %d" % s.compact())
    except ValueError as e:
        print("ERROR %s" % e)
        return 2
    return 0
