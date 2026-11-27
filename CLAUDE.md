# katana

Regras de trabalho do assistente neste repositório. O que qualquer contribuidor precisa (compilar, estrutura, estilo, verificações) está em `CONTRIBUTING.md`; o estado atual do projeto, em `docs/handoff.md`.

Motor de inferência de modelos de linguagem escrito do zero em C. `src/katana.c` é a unidade de compilação única e só inclui as partes: `src/engine/` (inferência), `src/agent/` (camadas em cima do modelo: ferramentas, prompt, terminal, contexto, sessões) e `src/app/` (console, servidor, main). Leia primeiro:
- `docs/handoff.md`: estado atual, próximos passos em ordem, pendências e como retomar.
- `docs/comparisons.md`: todas as comparações medidas (contra o motor de referência e entre versões do katana).
- `docs/knowledge.md`: objetivo, o que o motor faz, tudo o que já foi medido, pontos fortes e fracos.
- `docs/plan.md`: prioridades e critérios de pronto.
- `docs/optimization-roadmap.md`: técnicas ainda não usadas, com ganho esperado e esforço.
- `desktop-qt/README.md`: katana-desktop, aplicativo nativo Qt6/QML (sem navegador) que conversa com o agente.

## Regras (do dono)

- Conversa com o dono em português. Documentação (README, `docs/`, README do desktop) em inglês, por pedido do dono (2026-11-18). Comentários no código em português sem acentos.
- O código não cita outros motores de inferência; em documentos, "reference engine".
- Nada proprietário. O motor de inferência roda sem serviços externos; as ferramentas do agente podem
  buscar conhecimento fora do modelo (internet, arquivos da máquina, memória própria), como o dono quer.
- Exceção ao "nada proprietário", só na camada de GPU (decisão do dono, 2026-11-24): CUDA, pela API do driver carregada em tempo de execução (dlopen/LoadLibrary, sem SDK nem biblioteca da NVIDIA; kernels em PTX; `src/engine/cu_*.c`); sem placa NVIDIA tudo continua funcionando.
- Repositório público (github.com/alesimoes/katana), licença Apache-2.0; recriado em 2026-11-27 com o histórico limpo
  (e-mail dos commits: o noreply do GitHub, configurado em `.git/config`). Commit e push a cada passo concluído. Nunca
  commitar com e-mail de empresa, chaves, tokens, binários de terceiros nem arquivos de `models/`.
- Projeto novo, sem legado e sem compatibilidade (decisão do dono, 2026-11-23): não manter formatos antigos, aliases de nomes antigos, chaves de ambiente que só voltam ao comportamento antigo, nem código de migração. Quando um formato muda, o dado antigo é reconstruído ou descartado.
- O dono do projeto quer trabalho contínuo, sem perguntas desnecessárias. Perguntar só antes de
  ações difíceis de desfazer ou que saiam da máquina.
- O agente do katana grava arquivos, executa comandos e chama ferramentas MCP sem pedir confirmação
  (decisão do dono); cada ação aparece no terminal e fica num log. `KATANA_CONFIRM=1` volta a pedir.

## Modelo de trabalho

- Sem testes nem medidas nesta fase (decisão do dono, 2026-11-22): a regra antiga de só comparar velocidade com medidas
  alternadas foi retirada. O foco é melhorar o motor. Compilar continua obrigatório; ao relatar, dizer o que foi feito e
  que nada foi medido, sem apresentar estimativa como resultado.
- Modelo de trabalho: Qwen3.6-35B-A3B, e so ele (decisao do dono, 2026-11-22). Testes, medidas, treino, provas e
  `selftest` sao feitos nele; nada e testado no Gemma 4, nem regressao. O Qwen3.8-Flash-Next esta em construcao.
  Os modelos de 2024 e 2025 (qwen2.5, OLMoE, Qwen3-30B etc.) foram apagados de `models/` por decisão do dono (2026-11-20).

## Prática

- Compilar: `cmd //c "cd /d C:\source\katana && .\build.bat"`.
- Editar os arquivos de `src/` com scripts Python de substituição gravados em arquivo (com `assert` de
  contagem); comandos embutidos no shell estragam `\n` e aspas.
- Shaders Vulkan: `ref/vk/glslang/bin/glslang.exe`; cabeçalhos em `ref/vk/include`.
- `models/` e `ref/` ficam fora do git.
