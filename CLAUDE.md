# katana

Motor de inferência de modelos de linguagem escrito do zero em C (`katana.c`). Leia primeiro:
- `docs/handoff.md`: estado atual, próximos passos em ordem, pendências e como retomar.
- `docs/comparacoes.md`: todas as comparações medidas (contra o motor de referência e entre versões do katana).
- `docs/conhecimento.md`: objetivo, o que o motor faz, tudo o que já foi medido, pontos fortes e fracos.
- `docs/plano.md`: prioridades e critérios de pronto.
- `docs/roteiro-otimizacoes.md`: técnicas ainda não usadas, com ganho esperado e esforço.

## Regras

- Conversa e documentação em português. Comentários no código em português sem acentos.
- O código não cita outros motores de inferência; em documentos, "motor de referência".
- Nada proprietário. O motor de inferência roda sem serviços externos; as ferramentas do agente podem
  buscar conhecimento fora do modelo (internet, arquivos da máquina, memória própria), como o dono quer.
- Repositório privado (github.com/alesimoes/katana): commit e push a cada passo concluído; não
  tornar público sem pedido.
- O dono do projeto quer trabalho contínuo, sem perguntas desnecessárias. Perguntar só antes de
  ações difíceis de desfazer ou que saiam da máquina.
- O agente do katana grava arquivos, executa comandos e chama ferramentas MCP sem pedir confirmação
  (decisão do dono); cada ação aparece no terminal e fica num log. `KATANA_CONFIRM=1` volta a pedir.

## Medição

- Só comparar velocidade com medidas alternadas na mesma hora (`compare.sh`, ou dois binários lado
  a lado). A máquina deriva muito ao longo do tempo.
- Dizer o que foi medido e o que é estimativa. Hipótese de gargalo se confirma medindo as partes.
- Depois de mexer em kernel: `selftest` num modelo denso, no OLMoE e no Qwen3-30B
  (`KATANA_KV16=0 KATANA_CTX=512` neste último).

## Prática

- Compilar: `cmd //c "cd /d C:\source\katana && .\build.bat"`.
- Editar `katana.c` com scripts Python de substituição gravados em arquivo (com `assert` de
  contagem); comandos embutidos no shell estragam `\n` e aspas.
- Shaders Vulkan: `ref/vk/glslang/bin/glslang.exe`; cabeçalhos em `ref/vk/include`.
- `modelos/` e `ref/` ficam fora do git.
