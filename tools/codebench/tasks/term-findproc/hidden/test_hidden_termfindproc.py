import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


class T(unittest.TestCase):
    def test_answer(self):
        with open(os.path.join(HERE, "answer.txt")) as f:
            lines = [l.strip() for l in f.read().strip().split("\n")]
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], os.environ["CB_PID"])
        self.assertEqual(os.path.realpath(lines[1]), os.environ["CB_CWD"])

    def test_service_still_running(self):
        os.kill(int(os.environ["CB_PID"]), 0)        # o agente nao pode ter matado o servico


if __name__ == "__main__":
    unittest.main()
