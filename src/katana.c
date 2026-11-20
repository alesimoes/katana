/* katana: motor de inferencia de modelos de linguagem escrito do zero em C.
 * Este arquivo e a unidade de compilacao unica: so inclui as partes, na ordem em que dependem umas das outras.
 * As partes nao sao compiladas sozinhas (funcoes static compartilhadas); para compilar: ./build.sh
 *
 *   engine/  inferencia: GGUF, tokenizador, kernels de CPU, Vulkan/OpenCL, MoE, DeltaNet, amostragem
 *   agent/   camadas em cima do modelo: ferramentas, prompt de sistema, terminal, MCP, contexto, sessoes
 *   app/     console, servidor HTTP, subcomandos e main
 */
#include "engine/base.c"               /* includes, utilitarios, pool de threads, mapa string->int */
#include "engine/model.c"              /* estruturas, leitura do GGUF, tipos de quantizacao, carga do modelo */
#include "engine/tokenizer.c"          /* tokenizadores (BPE, SentencePiece, pre-tokenizacao por familia) */
#include "engine/cpu_kernels.c"        /* matematica e produtos na CPU (float, inteiros, VNNI), backends de GPU base */
#include "engine/shards.c"             /* SHA-256 e shards (pesos ja no layout da GPU) */
#include "engine/vk_dense.c"           /* Vulkan: grafo denso e prompt em lote */
#include "engine/vk_moe.c"             /* Vulkan: MoE, cache de especialistas, MLA */
#include "engine/qwen35.c"             /* qwen35/qwen35moe: DeltaNet, atencao com portao, grafo Vulkan */
#include "engine/vk_gemma4.c"          /* gemma4 em Vulkan (hibrido) */
#include "engine/vk_qwen35d.c"         /* qwen35 denso no modo hibrido: prompt em lote com os produtos na GPU */
#include "engine/forward.c"            /* camadas na CPU em lote, forward, amostragem, decodificacao especulativa, generate */
#include "engine/qtest.c"              /* katana qtest: teste dos tipos de quantizacao */
#include "engine/prepare.c"            /* perfil por modelo e katana prepare */
#include "app/console.c"               /* console (cores, Ctrl+C, comandos de barra) e conversa simples */
#include "agent/tools.c"               /* agente: ferramentas basicas (arquivos, web, memoria, sudo, caminhos) */
#include "agent/todo.c"                /* lista de tarefas do proprio modelo (todo) */
#include "agent/project.c"             /* instrucoes do projeto (KATANA.md, AGENTS.md) */
#include "agent/undo.c"                /* versao anterior dos arquivos alterados; /undo */
#include "agent/web.c"                 /* web: curl, HTML para texto, web_fetch, busca */
#include "agent/webnav.c"              /* navegacao: paginas guardadas, offset, find, links, conteudo principal */
#include "agent/prompt.c"              /* agente: ambiente e prompt de sistema */
#include "agent/mcp.c"                 /* cliente MCP */
#include "agent/devtools.c"            /* ferramentas de programacao: edit_file, glob, download, pacotes, git */
#include "agent/code.c"               /* leitura por trechos e verificacao de sintaxe depois de gravar */
#include "agent/exec.c"                /* execucao de comandos: stdin fechado, timeout, inicio+fim da saida, anel dos ultimos comandos */
#include "agent/sudo.c"                /* sudo: pedido de senha (terminal ou modal), arquivos como root */
#include "agent/jobs.c"                /* comandos em segundo plano (run_command background, job) */
#include "agent/diagnose.c"            /* dica de proximo passo quando um comando falha */
#include "agent/howto.c"               /* receitas de uso das ferramentas (howto) */
#include "agent/terminal.c"            /* run_command, sessoes de terminal (pty), shell root persistente */
#include "agent/logs.c"                /* read_log e terminal_screen */
#include "agent/locate.c"              /* navegacao no computador: list_dir com detalhes, find_file */
#include "agent/ask.c"                 /* interacao com o usuario: pergunta com opcoes (ask_user) */
#include "agent/planmode.c"            /* modo plano: so leitura ate o usuario aprovar (/plan, /go) */
#include "agent/dispatch.c"            /* despacho das ferramentas e leitura das chamadas em XML */
#include "agent/fmt_glm.c"             /* chamada de ferramenta no formato do GLM */
#include "agent/fmt_gemma4.c"          /* ferramentas no formato nativo do Gemma 4 */
#include "agent/prefix_cache.c"        /* cache de prefixo persistente e aquecimento */
#include "agent/fmt_harmony.c"         /* ferramentas no formato harmony (gpt-oss) */
#include "agent/context.c"             /* compactacao do contexto */
#include "agent/delegate.c"            /* subagente com contexto limpo (delegate) */
#include "agent/loop.c"                /* sessoes (gravar, retomar) e o laco do agente */
#include "app/server.c"                /* servidor HTTP (pagina, /api/chat, rede, fila do modelo) */
#include "app/openai.c"                /* API no formato da OpenAI (/v1/chat/completions, /v1/models) */
#include "app/train.c"                 /* especializacao: passo para tras, AdamW, katana train, .kspec (KATANA_SPEC) */
#include "app/importer.c"              /* katana import: conversas de fora -> dados de treino no formato do modelo */
#include "app/distill.c"               /* katana distill: um modelo professor trabalha com as ferramentas do katana */
#include "app/learn.c"                 /* katana export (sessoes -> dados) e katana learn (aprende com o proprio uso) */
#include "app/catalog.c"               /* maquina, catalogo de modelos e download (hw, catalog, pull) */
#include "app/main.c"                  /* toktest, subcomandos e main */
