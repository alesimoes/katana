# teste escondido das tarefas de mentira do katana learn (tools/traintest/run.sh monta as pastas t1..t8 com este arquivo).
# Passa ou falha pelo nome da tarefa e por KATANA_SPEC (o juiz repassa o --env aos testes): t1, t3, t4 e t6 sempre passam;
# t8 so passa com uma especializacao carregada; as outras nunca. Na coleta passam 3 das 6 tarefas de treino (6 sessoes com
# 2 tentativas); na avaliacao o modelo base resolve 1 de 2 (t4) e com especializacao 2 (t4 e t8): a rodada 1 do learn tem
# que ser aceita e a 2 nao (2 nao e mais que 2).
import os, unittest

TASK = "@TASK@"


class Mock(unittest.TestCase):
    def test_pass_rule(self):
        spec = os.environ.get("KATANA_SPEC", "")
        self.assertTrue(TASK in ("t1", "t3", "t4", "t6") or (TASK == "t8" and spec != ""))


if __name__ == "__main__":
    unittest.main()
