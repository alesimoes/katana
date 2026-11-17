# katana-desktop

Janela de conversa com o agente do katana (camada acima do motor, executavel separado).

- Compilar: `cd desktop && go build -o ../katana-desktop .` (so biblioteca padrao do Go).
- Rodar: `./katana-desktop` (abre uma janela de app no Chrome/Chromium/Edge, ou imprime a URL).
  Opcoes: `-katana CAMINHO`, `-models PASTA`, `-port N`, `-no-window`.
- Escolha do modelo na lateral. Padrao: Qwen3.6-35B; tambem utilizavel: Gemma 4. Os demais aparecem
  desabilitados ("ainda nao esta utilizavel no agente").
- Chamadas de ferramenta viram cartoes (nome + argumentos legiveis); o XML cru do modelo nao aparece.
- Escuta so em 127.0.0.1 e recusa Host/Origin que nao sejam locais.

Como funciona: sobe `katana agent <modelo>` com `KATANA_UI=1` e `NO_COLOR=1`. Nesse modo o katana
escreve em stdout marcadores de linha (`\x01READY` no fim do turno, `\x01TOOL nome<TAB>args` a cada
ferramenta); o resto do stdout e texto da resposta, filtrado de blocos `<tool_call>`/`<tool_response>`.
As estatisticas vem do stderr. O canal interno entre o processo e a pagina e JSON/SSE, invisivel ao usuario.

Limitacoes: o raciocinio fica desligado (`KATANA_THINK=0`); o agente le uma linha por turno, entao
quebras de linha da mensagem viram espacos; no Windows "interromper" mata o agente (sem SIGINT).
