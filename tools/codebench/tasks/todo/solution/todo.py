import json
import os

def _load(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return {"next": 1, "items": []}
    with open(path) as f:
        return json.load(f)

def _save(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f)
    os.replace(tmp, path)

def add(path, text):
    if not text or not text.strip():
        raise ValueError("empty text")
    data = _load(path)
    item_id = data["next"]
    data["next"] = item_id + 1
    data["items"].append({"id": item_id, "text": text, "done": False})
    _save(path, data)
    return item_id

def list_items(path, only_open=False):
    items = sorted(_load(path)["items"], key=lambda i: i["id"])
    return [i for i in items if not (only_open and i["done"])]

def done(path, item_id):
    data = _load(path)
    for i in data["items"]:
        if i["id"] == item_id:
            i["done"] = True
            _save(path, data)
            return
    raise KeyError(item_id)

def remove(path, item_id):
    data = _load(path)
    keep = [i for i in data["items"] if i["id"] != item_id]
    if len(keep) == len(data["items"]):
        raise KeyError(item_id)
    data["items"] = keep
    _save(path, data)
