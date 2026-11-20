# katana

Motor de inferência de modelos de linguagem escrito do zero em C. `src/katana.c` é a unidade de compilação única e só inclui as partes: `src/engine/` (inferência), `src/agent/` (camadas em cima do modelo: ferramentas, prompt, terminal, contexto, sessões) e `src/app/` (console, servidor, main). Leia primeiro:
- `docs/handoff.md`: estado atual, próximos passos em ordem, pendências e como retomar.
- `docs/comparisons.md`: todas as comparações medidas (contra o motor de referência e entre versões do katana).
- `docs/knowledge.md`: objetivo, o que o motor faz, tudo o que já foi medido, pontos fortes e fracos.
- `docs/plan.md`: prioridades e critérios de pronto.
- `docs/optimization-roadmap.md`: técnicas ainda não usadas, com ganho esperado e esforço.
- `desktop-qt/README.md`: katana-desktop, aplicativo nativo Qt6/QML (sem navegador) que conversa com o agente.

## Regras

- Conversa com o dono em português. Documentação (README, `docs/`, README do desktop) em inglês, por pedido do dono (2026-11-18). Comentários no código em português sem acentos.
- O código não cita outros motores de inferência; em documentos, "reference engine".
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
- Modelos de referencia (os que usamos e julgamos melhores): Qwen3.6-35B-A3B e Gemma 4 26B-A4B. Testes, medidas e
  comparacoes sao feitos neles. Depois de mexer em kernel: `selftest` nos dois.
  Os modelos de 2024 e 2025 (qwen2.5, OLMoE, Qwen3-30B etc.) foram apagados de `models/` por decisão do dono (2026-11-20).

## Prática

- Compilar: `cmd //c "cd /d C:\source\katana && .\build.bat"`.
- Editar os arquivos de `src/` com scripts Python de substituição gravados em arquivo (com `assert` de
  contagem); comandos embutidos no shell estragam `\n` e aspas.
- Shaders Vulkan: `ref/vk/glslang/bin/glslang.exe`; cabeçalhos em `ref/vk/include`.
- `models/` e `ref/` ficam fora do git.
