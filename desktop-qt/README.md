# katana-desktop (Qt6)

Aplicativo nativo de conversa com o agente do katana (camada acima do motor, executavel separado).
Janela propria em Qt Quick; sem navegador e sem servidor HTTP. O backend (C++17) sobe
`katana agent <modelo>` direto com QProcess.

- Compilar: `desktop-qt/build.sh` (CMake; so modulos Qt: Core, Gui, Qml, Quick, QuickControls2, Test).
  Gera `desktop-qt/build/katana-desktop-qt`. Para instalar no lugar do app antigo:
  `cp desktop-qt/build/katana-desktop-qt katana-desktop`.
- Rodar: `./katana-desktop-qt` (ou o binario copiado). Opcoes: `--katana CAMINHO`, `--models PASTA`.
  O `katana` e `modelos/` sao procurados ao lado do binario, em `..` e no diretorio atual.
- Modelo: dropdown no topo. Padrao Qwen3.6-35B (inicia sozinho); tambem utilizavel: Gemma 4.
  Os demais aparecem desabilitados ("ainda nao esta utilizavel no agente").
- Lateral esquerda recolhivel (em janela estreita vira gaveta): "Nova conversa" e as conversas salvas
  (titulo, hora relativa, modelo); lixeira ao passar o mouse, com confirmacao na propria interface.
- Sessoes: um arquivo por conversa em `~/.katana/desktop/sessoes/<id>.json` (id, titulo, modelo, criada,
  atualizada, mensagens[{papel, texto, nome?}]), gravado ao enviar e a cada fim de turno (escrita atomica).
  `KATANA_DESKTOP_HOME` troca a pasta base (`~/.katana/desktop`). O cache do agente nao se restaura: ao
  continuar uma sessao antiga, a primeira mensagem nova segue ao agente precedida de um resumo textual
  (ate ~6000 caracteres, mantendo o fim); a tela mostra so o digitado. O resumo pendente sobrevive a troca
  de modelo.
- Chamadas de ferramenta viram cartoes (nome + argumentos legiveis); o XML cru do modelo nao aparece.
- Estatisticas discretas sob a caixa: tok/s (stderr) e uso de contexto (marcador `TOKENS`). O marcador
  `COMPACT` mostra "Contexto compactado: N -> M tokens". Marcadores desconhecidos sao ignorados
  (ponto de extensao: `Backend::handleMarker`).
- Tema claro/escuro segue o sistema (`KATANA_THEME=light|dark` forca um).

Protocolo: o agente roda com `KATANA_UI=1 NO_COLOR=1 KATANA_THINK=0`. Linhas de stdout que comecam com
o byte 0x01 sao marcadores (`READY`, `TOOL nome<TAB>args`, `TOKENS pos ctx`, `COMPACT antes depois`); o
resto e texto da resposta, filtrado de blocos de ferramenta. Parar = SIGINT ao grupo do agente (no
Windows, mata e reinicia). Nova conversa = `/novo`; trocar de modelo = `/sair` e, se nao sair em 3 s, kill.

Teste: `QT_QPA_PLATFORM=offscreen desktop-qt/build/katana-desktop-qt --selftest --capturas desktop-qt/capturas`
(rodar na raiz do repo; usa `modelos/qwen2.5-1.5b-q4km.gguf` e uma pasta de dados temporaria). Digita e
clica na janela com eventos reais (QTest), confere cartao `list_dir`, sessoes, dropdown, apagar, parar
(SIGINT), troca de modelo com resumo pendente, e imprime PASSOU/FALHOU. `capturas/` guarda imagens.

Limitacoes: raciocinio desligado (`KATANA_THINK=0`); o agente le uma linha por turno (quebras de linha
viram espacos); no Windows, parar mata o agente; a versao Windows nao foi compilada nem testada.
