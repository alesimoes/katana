import json, os


def load(path=None):
    path = path or os.path.join(os.path.dirname(__file__), "data", "fixtures", "catalog.json")
    with open(path) as f:
        return {i["sku"]: i["price"] for i in json.load(f)["items"]}
