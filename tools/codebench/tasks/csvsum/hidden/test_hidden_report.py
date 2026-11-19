import os, tempfile, unittest
from report import sum_column
class T(unittest.TestCase):
    def write(self, text):
        fd, p = tempfile.mkstemp(suffix=".csv")
        with os.fdopen(fd, "w") as f:
            f.write(text)
        self.addCleanup(os.remove, p)
        return p
    def test_quoted_commas(self):
        p = self.write('name,note,amount\nbob,"hi, there",5\namy,plain,2.5\n')
        self.assertEqual(sum_column(p, "amount"), 7.5)
    def test_blank(self):
        p = self.write("a,b\n1,\n2,3\n,4\n")
        self.assertEqual(sum_column(p, "a"), 3.0)
        self.assertEqual(sum_column(p, "b"), 7.0)
if __name__ == "__main__":
    unittest.main()
