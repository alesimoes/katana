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
        a = norm(answer())
        self.assertTrue(0 < len(a) <= 80, "resposta vazia ou longa demais (so o nome)")
        names = [norm(n) for n in os.environ["CB_TRUTH"].split("|")]
        ok = [n for n in names if len(n) >= 4 and re.search(r"(?<![a-z])" + re.escape(n) + r"(?![a-z])", a)]
        last = names[0].split()[-1]
        if len(last) >= 5 and re.search(r"(?<![a-z])" + re.escape(last) + r"(?![a-z])", a):
            ok.append(last)
        self.assertTrue(ok, "nome nao confere com quem ocupa o cargo hoje")


if __name__ == "__main__":
    unittest.main()
