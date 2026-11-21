"""JSON file storage: {"next_id": N, "events": [...]}. Missing file means an empty calendar."""
import json
import os

from .events import Event


class Storage:
    def __init__(self, path):
        self.path = path

    def load(self):
        if not os.path.exists(self.path):
            return 1, []
        with open(self.path, encoding="utf-8") as f:
            data = json.load(f)
        return data.get("next_id", 1), [Event.from_dict(d) for d in data.get("events", [])]

    def save(self, next_id, events):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"next_id": next_id, "events": [e.to_dict() for e in events]}, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.path)
