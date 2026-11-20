import os, re, unicodedata, unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def norm(s):
    s = unicodedata.normalize("NFKD", s)
    return " ".join("".join(c for c in s if not unicodedata.combining(c)).lower().split())


def answer():
    with open(os.path.join(HERE, "answer.txt"), encoding="utf-8") as f:
        return f.read().strip()


class T(unittest.TestCase):
    def test_answer(self):
        a = answer()
        self.assertTrue(0 < len(a) <= 40, "resposta vazia ou longa demais (so a versao)")
        m = re.search(r"\d+\.\d+(?:\.\d+)?", a)
        self.assertIsNotNone(m, "sem numero de versao na resposta")
        want = os.environ["CB_TRUTH"].split(".")[:2]
        self.assertEqual(m.group(0).split(".")[:2], want, "nao e a serie estavel mais nova")


if __name__ == "__main__":
    unittest.main()
