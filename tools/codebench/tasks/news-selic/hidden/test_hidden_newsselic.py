import os, re, unicodedata, unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def norm(s):
    s = unicodedata.normalize("NFKD", s)
    return " ".join("".join(c for c in s if not unicodedata.combining(c)).lower().split())


def answer():
    with open(os.path.join(HERE, "answer.txt"), encoding="utf-8") as f:
        return f.read().strip()


def number(a):
    m = re.search(r"\d+(?:[.,]\d+)?", a)
    return float(m.group(0).replace(",", ".")) if m else None


class T(unittest.TestCase):
    def test_answer(self):
        a = answer()
        self.assertTrue(0 < len(a) <= 40, "resposta vazia ou longa demais (so o numero)")
        v, t = number(a), float(os.environ["CB_TRUTH"])
        self.assertIsNotNone(v, "sem numero na resposta")
        self.assertLessEqual(abs(v - t), 0.011, "valor %s longe do atual" % v)


if __name__ == "__main__":
    unittest.main()
