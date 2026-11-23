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
#include "engine/cpu_lowbit.c"         /* produtos inteiros dos tipos de poucos bits (IQ2/IQ3/IQ4, Q2_0): AVX2 e AVX-512 */
#include "engine/shards.c"             /* SHA-256 e shards (pesos ja no layout da GPU) */
#include "engine/vk_dense.c"           /* Vulkan: grafo denso e prompt em lote */
#include "engine/vk_moe.c"             /* Vulkan: MoE, cache de especialistas, MLA */
#include "engine/qwen35.c"             /* qwen35/qwen35moe: DeltaNet, atencao com portao, grafo Vulkan */
#include "engine/vk_gemma4.c"          /* gemma4 em Vulkan (hibrido) */
#include "engine/vk_qwen35d.c"         /* qwen35 denso no modo hibrido: prompt em lote com os produtos na GPU */
#include "engine/qwen4exp.c"           /* qwen4exp na CPU: 4 ramos residuais, atencao esparsa, n-gramas no disco */
#include "engine/forward.c"            /* camadas na CPU em lote, forward, amostragem, decodificacao especulativa, generate */
#include "engine/lookup.c"             /* decodificacao especulativa por consulta ao contexto (KATANA_LOOKUP) */
#include "engine/qtest.c"              /* katana qtest: teste dos tipos de quantizacao */
#include "engine/expert_tier.c"          /* camadas de memoria dos especialistas: RAM com leitura paralela do SSD, previsao, medidas */
#include "engine/prepare.c"            /* perfil por modelo e katana prepare */
#include "app/console.c"               /* console (cores, Ctrl+C, comandos de barra) e conversa simples */
#include "agent/knote.c"               /* notas [katana: ...] do motor: tabela de tipos, orcamento e escape unico */
#include "agent/tools.c"               /* agente: ferramentas basicas (arquivos, web, memoria, sudo, caminhos) */
#include "coder/roots.c"               /* coder: raiz do indice por init (marcador ROOT), indice da home, absorcao */
/* visao: imagens na conversa (docs/vision.md) */
#include "vision/tensor.c"             /* algebra float32 na CPU: produto em blocos, norma, GELU, atencao (sem nada de imagem) */
#include "vision/png.c"                /* limites, inflate, PNG e BMP */
#include "vision/jpeg.c"               /* JPEG sequencial e progressivo; escolha do formato */
#include "vision/resize.c"             /* tamanho pela regra de referencia, bicubico, recortes */
#include "vision/vit.c"                /* projetor (GGUF) e codificador de imagem */
#include "vision/image.c"              /* marcas, ids, vetores, posicoes, projetor ao lado do modelo */
#include "agent/todo.c"                /* lista de tarefas do proprio modelo (todo) */
#include "agent/project.c"             /* instrucoes do projeto (KATANA.md, AGENTS.md) */
#include "agent/undo.c"                /* versao anterior dos arquivos alterados; /undo */
#include "agent/web.c"                 /* web: curl, HTML para texto, web_fetch, busca */
#include "agent/webnav.c"              /* navegacao: paginas guardadas, offset, find, links, conteudo principal */
#include "agent/prompt.c"              /* agente: ambiente e prompt de sistema */
#include "agent/mcp.c"                 /* cliente MCP */
#include "agent/mcp_channel.c"         /* canal MCP: mensagens e confirmacoes empurradas por um servidor */
#include "agent/mcp_extra.c"           /* MCP alem do basico: variaveis na configuracao, roots, anotacoes, elicitation */
#include "agent/devtools.c"            /* ferramentas de programacao: edit_file, glob, download, pacotes, git */
#include "agent/code.c"               /* leitura por trechos e verificacao de sintaxe depois de gravar */
#include "agent/reaper.c"              /* vigia: encerra os comandos do agente quando ele morre (SIGKILL incluido) */
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
#include "agent/facts.c"               /* caderno de fatos e leitura por diferenca */
#include "agent/context.c"             /* compactacao do contexto */
#include "agent/delegate.c"            /* subagente com contexto limpo (delegate) */
/* segundo conjunto de ferramentas do agente, uma por arquivo (docs/tools.md) */
#include "tools/common.c"              /* argumentos em JSON, arquivos vistos na conversa */
#include "tools/bash.c"                /* Bash, TaskStop, Monitor */
#include "tools/read.c"
#include "tools/write.c"
#include "tools/edit.c"
#include "tools/notebookedit.c"
#include "tools/regex.c"               /* motor de expressoes regulares do Grep */
#include "tools/glob.c"
#include "tools/grep.c"
#include "tools/find.c"                /* Find: navegacao por intencao, indice da pasta e referencias */
#include "tools/findgraph.c"           /* Find: grafo de arquivos, ativacao esparsa, nota de vizinhos */
#include "tools/symbols.c"             /* Symbols: consultas do indice de simbolos (def, refs, callers, ...), via coder/symtool.c */
#include "tools/multiedit.c"           /* Rename, Replace, Patch: mudancas em varios lugares de uma vez */
#include "tools/webfetch.c"
#include "tools/websearch.c"
#include "tools/tasks.c"               /* TodoWrite */
#include "tools/askuserquestion.c"
#include "tools/planmode.c"            /* EnterPlanMode, ExitPlanMode */
#include "tools/agent.c"
#include "tools/worktree.c"              /* EnterWorktree, ExitWorktree */
#include "tools/powershell.c"            /* PowerShell */
#include "tools/lsp.c"                   /* LSP */
#include "tools/schedule.c"              /* CronCreate, CronDelete, CronList, ScheduleWakeup */
#include "tools/notify.c"                /* PushNotification, SendUserFile, EndConversation */
#include "tools/mcpres.c"                /* ListMcpResourcesTool, ReadMcpResourceTool, WaitForMcpServers */
#include "tools/toolsearch.c"            /* ToolSearch */
#include "tools/skill.c"                 /* Skill */
#include "tools/reportfindings.c"        /* ReportFindings */
#include "tools/messages.c"              /* SendMessage, ListAgents, SubagentHandback */
#include "tools/workflow.c"              /* Workflow */
#include "tools/table.c"               /* tabela, despacho e katana tool */
#include "agent/brief.c"               /* formato brief do KATANA.md: lint, never/ask impostos, nota de fim de pedido */
#include "agent/docsel.c"              /* o motor escolhe, nos .md do projeto, os trechos que importam para o pedido */
#include "coder/store.c"             /* armazenamento generico: segmentos imutaveis, manifesto, escritor unico (sem tipos de codigo) */
#include "coder/ignore.c"           /* coder: o que o indice deixa de fora e por que (lista interna unica, segredos, overrides) */
#include "coder/storedocs.c"         /* catalogo generico de documentos, atualizacao incremental, index/status/verify/compact */
#include "coder/watch.c"             /* coder: vigia de arquivos da sessao (STUB, fase 4) */
#include "coder/fresh.c"             /* coder: confere arquivo citado contra o catalogo (STUB, fase 4) */
#include "coder/statemap.c"          /* coder: mapa de estado por nivel (STUB, fase 6) */
#include "coder/graph.c"             /* coder: grafo de arquivos derivado dos niveis (STUB, fase 7) */
#include "coder/maint.c"             /* coder: manutencao do indice, gc, quarentena (STUB, fase 8) */
#include "coder/indexctl.c"          /* coder: quando revalidar o indice (index_ensure, index_touch) e cache das visoes de leitura */
#include "coder/types.c"             /* modulo de inteligencia de codigo (em cima do motor): tipos compartilhados */
/* coder: os submodulos entram aqui, um #include por linha, antes de coder.c */
#include "coder/parse.c"              /* katana coder symbols: analisador estrutural exato por linguagem */
#include "coder/parse2.c"             /* coder: linguagens e formatos acrescentados (C#, Swift, PHP, Dart, Scala, Lisp, SQL, configuracao...) */
#include "coder/keys.c"              /* coder: chaves de texto (ligacoes por texto igual: eventos, rotas, ambiente, mensagens) */
#include "coder/kb.c"                /* base de conhecimento de padroes (katana coder kb) */
#include "coder/text.c"              /* coder: tokenizador e normalizador morfologico pt/en */
#include "agent/memory.c"            /* memoria: remember, recall, memory_search, katana memory (usa o tokenizador) */
#include "coder/symindex.c"          /* coder: indice de simbolos e referencias (secoes 0x200) */
#include "coder/symquery.c"          /* coder: consultas estruturais (def, refs, callers, callees, uses, outline, at, deps, impact) */
#include "coder/flow.c"              /* coder: ligacoes por chave de texto (katana coder flow) */
#include "coder/symtool.c"           /* coder: ferramenta Symbols e Find mode=refs em cima do indice de simbolos */
#include "coder/search.c"            /* coder: busca por nome e texto (indice em secoes 0x100, canais, fusao, evidencia) */
#include "coder/findstore.c"        /* coder: o Find unificado: candidatos do indice, indice de trechos por consulta, grafo derivado de N2/N3 (fase 7) */
#include "coder/crawl.c"              /* coder: o rastreador do indice (katana coder crawl): niveis N0..N3, retomavel, sob demanda */
#include "coder/coder.c"              /* despacho de `katana coder <sub>` */
#include "uioe/types.c"
#include "uioe/trace.c"
#include "uioe/state.c"
#include "uioe/registry.c"
#include "uioe/interpret.c"
#include "uioe/classify.c"
#include "uioe/route.c"
#include "uioe/evgraph.c"              /* UIOE U3: grafo de evidencias, fusao e ordem */
#include "uioe/verify.c"               /* UIOE U3: reler as linhas citadas */
#include "uioe/conn_file.c"            /* UIOE U3: conectores de arquivos e documentos */
#include "uioe/conn_code.c"            /* UIOE U3: conectores de codigo (API estruturada do coder) */
#include "uioe/budget.c"               /* UIOE U3: prazos, disjuntor, uma chamada de conector */
#include "uioe/pipes.c"                /* UIOE U3: pipelines por tipo de tarefa */
#include "uioe/orch.c"                 /* UIOE U3: maquina de estados, porta G4 a G6 */
#include "uioe/evrender.c"             /* UIOE U3: saida em texto e JSON, traco da recuperacao */
#include "uioe/ctxb.c"                /* UIOE U4: o Construtor de Contexto (o bloco `[katana: context ...]`) */
#include "uioe/api.c"                 /* UIOE U4: uioe_prepare, a porta antes do modelo */
#include "uioe/cli.c"
#include "agent/extend.c"              /* katana init, skill, mcp; /init /skill /mcp e prompts do MCP */
#include "agent/mcp_serve.c"           /* katana mcp serve: o katana como servidor MCP (stdio) */
#include "agent/callgrammar.c"         /* chamadas de ferramenta validas por construcao (nome, chaves, valores tipados) */
#include "agent/teacher.c"             /* modo professor: um modelo maior responde no lugar do local (API de mensagens) */
#include "agent/loop.c"                /* sessoes (gravar, retomar) e o laco do agente */
#include "agent/agentlog.c"            /* katana agentlog: chamadas, falhas e repeticoes por ferramenta no registro de acoes */
#include "app/server.c"                /* servidor HTTP (pagina, /api/chat, rede, fila do modelo) */
#include "app/openai.c"                /* API no formato da OpenAI (/v1/chat/completions, /v1/models) */
#include "app/agentapi.c"              /* o agente pela rede: /v1/agent/sessions (katana serve --agent) */
#include "app/train.c"                 /* especializacao: passo para tras, AdamW, katana train, .kspec (KATANA_SPEC) */
#include "app/transcript.c"            /* transcricoes de um agente de programacao -> conversas, para o katana import */
#include "app/importer.c"              /* katana import: conversas de fora -> dados de treino no formato do modelo */
#include "app/knowledge.c"             /* katana knowledge: notas de texto das sessoes anteriores (build, search, stats) */
#include "app/distill.c"               /* katana distill: um modelo professor trabalha com as ferramentas do katana */
#include "app/learn.c"                 /* katana export (sessoes -> dados) e katana learn (aprende com o proprio uso) */
#ifndef KATANA_BUILD
#define KATANA_BUILD "?"
#endif
#if defined(__has_include)
#if __has_include("app/changes.inc")
static const char KATANA_CHANGES[] =
#include "app/changes.inc"
;
#else
static const char KATANA_CHANGES[] = "";
#endif
#else
static const char KATANA_CHANGES[] = "";
#endif
#include "app/codebench.c"             /* katana codebench: o juiz (tarefas com testes escondidos): run, verify, compare, list */
#include "app/update.c"                /* katana update: compila, confere e troca o programa instalado */
#include "app/specialize.c"            /* katana specialize: versoes especialistas (teacher, tools, code) de ponta a ponta */
#include "app/curriculum.c"            /* katana curriculum: as convencoes (KATANA.md, skills, MCP) viram dados de treino */
#include "app/catalog.c"               /* maquina, catalogo de modelos e download (hw, catalog, pull) */
#include "app/launch.c"                /* katana sem subcomando: console na pasta atual, headless (-p), install, adopt */
#include "app/deeptest.c"              /* katana selftest --deep: caminhos, estado, contexto longo, precisao */
#include "app/attbench.c"              /* katana attbench: custo da atencao do qwen35moe por posicao */
#include "app/visiontest.c"            /* katana vision: cabecalho, custo, etapas para o oraculo, fuzz dos decodificadores */
#include "app/main.c"                  /* toktest, subcomandos e main */
#include "app/convert.c"               /* katana convert: checkpoint do Hugging Face (safetensors) -> GGUF */
