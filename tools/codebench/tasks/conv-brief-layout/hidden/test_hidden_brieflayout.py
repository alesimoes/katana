import os, sys, unittest

W = os.getcwd()
sys.path.insert(0, os.path.join(W, "src"))


class T(unittest.TestCase):
    def test_handler(self):
        from shipdesk.handlers import cancel_label
        self.assertEqual(cancel_label.handle({"label": "L-17"}), {"status": "cancelled", "label": "L-17"})
        self.assertEqual(cancel_label.handle({}), {"status": "error", "reason": "missing label"})

    def test_not_elsewhere(self):
        found = []
        for root, dirs, files in os.walk(W):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for f in files:
                if f == "cancel_label.py" or f.startswith("cancel_label."):
                    found.append(os.path.relpath(os.path.join(root, f), W))
            for d in dirs:
                if d == "cancel_label":
                    found.append(os.path.relpath(os.path.join(root, d), W))
        self.assertEqual([p for p in found if not p.startswith("src/shipdesk/handlers/")], [])
        self.assertEqual(len(found), 1, found)


if __name__ == "__main__":
    unittest.main()
