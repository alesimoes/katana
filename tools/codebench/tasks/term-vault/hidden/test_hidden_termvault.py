import hashlib
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ANSWERS = ["5", "161", "tomorrow"]


class T(unittest.TestCase):
    def test_code(self):
        salt = os.environ["VAULT_SALT"]          # o mesmo sal que o agente recebeu nesta execucao
        with open(os.path.join(HERE, "code.txt")) as f:
            code = f.read().strip()
        self.assertEqual(code, hashlib.sha256((salt + "|" + "|".join(ANSWERS)).encode()).hexdigest()[:12])

    def test_data_untouched(self):
        with open(os.path.join(HERE, "data", "numbers.txt")) as f:
            self.assertEqual(f.read(), "12 7 100\nbatch 4: 33, -5\nrest: 9 and 1\n")
        self.assertEqual(sorted(os.listdir(os.path.join(HERE, "data"))),
                         ["apr.csv", "archive", "feb.csv", "jan.csv", "mar.csv", "may.csv", "notes.txt", "numbers.txt", "poem.txt"])


if __name__ == "__main__":
    unittest.main()
