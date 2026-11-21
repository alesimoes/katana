import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
from shipdesk.legacy import ping


class T(unittest.TestCase):
    def test_ping(self):
        self.assertEqual(ping.handle({}), {"status": "ok", "pong": True})
