import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import catalog


class T(unittest.TestCase):
    def test_widget(self):
        self.assertEqual(catalog.load()["widget"], 12.5)
