# katana-desktop (Qt6)

Aplicativo nativo de conversa com o agente do katana (camada acima do motor, executavel separado).
Janela propria em Qt Quick; sem navegador e sem servidor HTTP. O backend (C++17) sobe
`katana agent <modelo>` direto com QProcess.

- Compilar: `desktop-qt/build.sh` (CMake; so modulos Qt: Core, Gui, Qml, Quick, QuickControls2, Test).
  Gera `desktop-qt/build/katana-desktop-qt`. Para instalar no lugar do app antigo:
  `cp desktop-qt/build/katana-desktop-qt katana-desktop`.
- Rodar: `./katana-desktop-qt` (ou o binario copiado). Opcoes: `--katana CAMINHO`, `--models PASTA`.
  O `katana` e `modelos/` sao procurados ao lado do binario, em `..` e no diretorio atual.
- Modelo: seletor embaixo da caixa de texto, ao lado de enviar (a lista abre para cima). Padrao Qwen3.6-35B (inicia sozinho); tambem utilizavel: Gemma 4.
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
- Caixa de entrada grande (minimo ~2 linhas) que cresce com o texto ate ~240 px e depois rola. Enter
  envia, Shift+Enter quebra linha (o agente le uma linha por turno: as quebras viram espacos, e a
  interface avisa).
- Estatisticas discretas sob a caixa: tok/s (stderr) e indicador de contexto (`ContextMeter.qml`): anel de
  progresso + "X mil / Y mil tokens · Z%", com dica ao passar o mouse (usados, total, livre). Cor normal
  abaixo de 70%, ambar a partir de 70%, vermelha a partir de 85%. Alimentado pelo marcador `TOKENS`
  (e pela linha de estatisticas do stderr). O marcador `COMPACT` mostra "Contexto compactado: N -> M
  tokens". Marcadores desconhecidos sao ignorados (ponto de extensao: `Backend::handleMarker`).
- Icone: squircle roxo em degrade com um K grosso em pixel art (branco para lilas, sombra roxa de 1 pixel). `icons/katana.svg`
  e PNGs 16 a 512 sao gerados por `tools/gerar-icone-k.py` (PIL), embutidos no executavel (recursos Qt) e usados como icone
  da janela, do .desktop e da dock. O samurai fica so na tela de boas-vindas.
- Fonte: JetBrains Mono embutida (licenca OFL em `fonts/`), interface e codigo.
- Identidade roxa: o destaque (`Theme.ac`: botao enviar, anel de contexto, selecao de texto, avatar, foco,
  titulo) e `#820AD1` no tema claro e `#B66CF2` no escuro (texto sobre o destaque: branco / `#1A1022`); os
  neutros tem um leve tom lilas. Tudo em `qml/Theme.qml`.
- Tela de boas-vindas (`emptyState` em `MessageView.qml`): aparece so com a conversa vazia (inicio, 'Nova
  conversa'); some ao enviar a primeira mensagem e nao aparece ao abrir sessao com mensagens. Mostra o
  samurai (`icons/samurai.png`, 16x16 logico, 5 tons: contorno, sombra, base roxa, luz, branco) ampliado em escala
  inteira 5x (80 px; 4x em janela baixa) com `smooth: false`, o titulo "Katana" como imagem retro anos 80
  (`icons/titulo-claro.png` / `titulo-escuro.png`: letras blocadas 5x7 em pixel, degrade roxo e sombra de 1 px,
  ampliadas em escala inteira), "Em que posso ajudar hoje?" e 3 sugestoes clicaveis que preenchem e enviam.
  Sprite e titulo saem de `tools/gerar-samurai.py` (o icone, de `tools/gerar-icone-k.py`); rode-o e depois `build.sh`.
- Aparencia (engrenagem na barra de titulo propria, janela sem moldura): tema Escuro (padrao), Claro ou IDE escuro (estilo editor, codigo com realce de sintaxe) e fonte JetBrains Mono (padrao) ou IBM Plex; salvos em ui.ini. `KATANA_THEME=light|dark`, `KATANA_TEMA` e `KATANA_FONTE` forcam um.

Sudo: quando o agente roda um comando com `sudo` que pede senha, o motor emite o marcador `NEEDPASS`; o desktop abre um dialogo de senha e responde com `\x01PASS <senha>` (vazio = cancelar). A senha fica so na memoria do agente e no ambiente do comando (askpass), nao vai ao modelo nem ao historico. No terminal (`katana agent`) a senha e pedida na propria tela, sem eco.

Modelo lembrado: o ultimo modelo iniciado fica em ui.ini (`ultimoModelo`) e e o que abre na proxima vez; ao abrir uma conversa salva, o desktop troca para o modelo em que ela estava (se estiver utilizavel) e retoma a sessao.

Ambiente: o desktop descarta variaveis KATANA_* de desempenho herdadas do terminal (KATANA_CPU, KATANA_CTX, KATANA_KV16, KATANA_VK_*...) para o motor usar os padroes mais rapidos; `KATANA_DESKTOP_ENV=1` preserva o ambiente herdado.

Protocolo: o agente roda com `KATANA_UI=1 NO_COLOR=1 KATANA_THINK=0`. Linhas de stdout que comecam com
o byte 0x01 sao marcadores (`READY`, `TOOL nome<TAB>args`, `TOKENS pos ctx`, `COMPACT antes depois`); o
resto e texto da resposta, filtrado de blocos de ferramenta. Parar = SIGINT ao grupo do agente (no
Windows, mata e reinicia). Nova conversa = `/novo`; trocar de modelo = `/sair` e, se nao sair em 3 s, kill.

Teste: `QT_QPA_PLATFORM=offscreen desktop-qt/build/katana-desktop-qt --selftest --capturas desktop-qt/capturas`
(rodar na raiz do repo; usa `modelos/qwen2.5-1.5b-q4km.gguf` (rapido, cobre a interface e o protocolo do agente; para a conferencia final no modelo real use `--selftest-model Qwen3.6-35B-A3B-UD-Q4_K_M.gguf`, lento a frio) e uma pasta de dados temporaria). Digita e
clica na janela com eventos reais (QTest), confere cartao `list_dir`, sessoes, dropdown, apagar, parar
(SIGINT), troca de modelo com resumo pendente, e imprime PASSOU/FALHOU. `capturas/` guarda imagens (`12-boasvindas-claro.png` e `12-boasvindas-escuro.png`: o escuro sai de uma
segunda execucao com `KATANA_THEME=dark`). Tambem confere a tela de boas-vindas (visivel com conversa
vazia, some apos enviar, nao aparece em sessao antiga, volta em 'Nova conversa').

Limitacoes: raciocinio desligado (`KATANA_THINK=0`); o agente le uma linha por turno (quebras de linha
viram espacos); no Windows, parar mata o agente; a versao Windows nao foi compilada nem testada.

## Instalar na raiz do repositorio

Nao copie o binario: ele precisa da pasta `Katana/` (modulo QML) que fica ao lado dele em `desktop-qt/build/`.
Use um link simbolico (o Qt resolve o caminho real): `ln -sf desktop-qt/build/katana-desktop-qt katana-desktop`.
Copiar o binario sozinho da o erro `Module "Katana" contains no type named "Main"`.

## Dock e menu de aplicativos (GNOME/Ubuntu)

`desktop-qt/instalar-dock.sh` instala `katana-desktop.desktop` em `~/.local/share/applications/`, os icones em
`~/.local/share/icons/hicolor/<tam>/apps/katana-desktop.png`, atualiza os caches e acrescenta
`katana-desktop.desktop` a `org.gnome.shell favorite-apps` (sem remover os favoritos nem duplicar). Em outros
ambientes instala so o atalho do menu. O `Exec` do .desktop usa o link `/mnt/dados/source/katana/katana-desktop`
(veja acima); o script nao mexe no binario.
