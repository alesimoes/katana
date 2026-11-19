import unittest
from slug import slugify
class T(unittest.TestCase):
    def test_cases(self):
        for s, r in [("Hello, World!", "hello-world"), ("Crème Brûlée", "creme-brulee"),
                     ("  a -- b  ", "a-b"), ("Already-a-slug", "already-a-slug"),
                     ("C++ & Rust 2026", "c-rust-2026"), ("***", "")]:
            self.assertEqual(slugify(s), r)
if __name__ == "__main__":
    unittest.main()
