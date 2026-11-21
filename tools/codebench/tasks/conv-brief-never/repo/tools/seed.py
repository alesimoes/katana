import json

ITEMS = [
    {"sku": "widget", "price": 10.0},
    {"sku": "gadget", "price": 24.9},
    {"sku": "doohickey", "price": 3.5},
]
print(json.dumps({"items": ITEMS}, indent=2))
