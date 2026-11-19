import json
import os


class Store:
    def __init__(self, path):
        self.path = path
        self._data = {}
        self._lines = 0
        if os.path.exists(path):
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    rec = json.loads(line)
                    self._lines += 1
                    if rec["op"] == "set":
                        self._data[rec["key"]] = rec["value"]
                    elif rec["op"] == "del":
                        self._data.pop(rec["key"], None)

    @staticmethod
    def _check(key):
        if not isinstance(key, str) or key == "":
            raise ValueError("key must be a non-empty string")

    def _append(self, rec):
        with open(self.path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        self._lines += 1

    def set(self, key, value):
        self._check(key)
        self._append({"op": "set", "key": key, "value": value})
        self._data[key] = value

    def get(self, key, default=None):
        self._check(key)
        return self._data.get(key, default)

    def delete(self, key):
        self._check(key)
        if key not in self._data:
            return False
        self._append({"op": "del", "key": key})
        del self._data[key]
        return True

    def keys(self):
        return sorted(self._data)

    def __len__(self):
        return len(self._data)

    def compact(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            for k in sorted(self._data):
                f.write(json.dumps({"op": "set", "key": k, "value": self._data[k]}) + "\n")
        os.replace(tmp, self.path)
        removed = self._lines - len(self._data)
        self._lines = len(self._data)
        return removed
