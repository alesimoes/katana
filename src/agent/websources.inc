/* ---------- registro de fontes embutido (dados; formato em docs/web-sources.md secao 3) ----------
 * Arquivo de DADOS: os enderecos reais das fontes escolhidas. Cada linha e uma fonte; ~/.katana/sources.txt do dono usa o
 * mesmo formato, troca uma fonte pela mesma id e remove com -id. Todos os enderecos foram conferidos com curl em 2026-11-25
 * (resposta, formato, limite, sem chave); o resultado esta em docs/web-sources.md secao 8.
 * Campos: id | topics | country | lang | kind | query template | freshness | trust | limit/min | map (opcional) */
static const char g_ws_builtin[] =
"# codigo, perguntas e respostas, pacotes\n"
"code-hosting | code,ai | * | * | json | https://api.github.com/search/repositories?q=%s&per_page=6 | days | 4 | 8 | items=items title=full_name url=html_url snip=description date=pushed_at page=https://github.com/{1}/{2} fetch=https://raw.githubusercontent.com/{1}/{2}/HEAD/README.md\n"
"q-and-a | code,q-and-a | * | en | json | https://api.stackexchange.com/2.3/search/excerpts?order=desc&sort=relevance&q=%s&site=stackoverflow&pagesize=6 | years | 4 | 4 | items=items title=title url=https://stackoverflow.com/q/{question_id} snip=excerpt date=creation_date\n"
"packages-js | code | * | * | json | https://registry.npmjs.org/-/v1/search?text=%s&size=5 | days | 3 | 20 | items=objects title=package.name url=package.links.npm snip=package.description date=package.date\n"
"packages-rust | code | * | * | json | https://crates.io/api/v1/crates?q=%s&per_page=5 | days | 3 | 20 | items=crates title=name url=https://crates.io/crates/{name} snip=description date=updated_at\n"
"tech-news | code,ai,news | * | en | json | https://hn.algolia.com/api/v1/search?query=%s&hitsPerPage=6 | hours | 3 | 20 | items=hits title=title url=url,https://news.ycombinator.com/item?id={objectID} snip=story_text,points date=created_at\n"
"# modelos de IA e papers\n"
"models-hub | ai | * | * | json | https://huggingface.co/api/models?search=%s&limit=6&sort=downloads | days | 4 | 20 | items=. title=id url=https://huggingface.co/{id} snip=pipeline_tag+downloads+likes date=createdAt page=https://huggingface.co/{1}/{2} fetch=https://huggingface.co/{1}/{2}/raw/main/README.md\n"
"papers | papers,ai | * | en | feed | https://export.arxiv.org/api/query?search_query=all:%s&max_results=6&sortBy=relevance | days | 4 | 15 | page=https://arxiv.org/abs/{1} fetch=https://export.arxiv.org/api/query?id_list={1}\n"
"# enciclopedia\n"
"encyclopedia-pt | encyclopedia,general | br | pt | json | https://pt.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=5&srsearch=%s | years | 4 | 30 | items=query.search title=title url=https://pt.wikipedia.org/wiki/{title} snip=snippet date=timestamp page=https://pt.wikipedia.org/wiki/{1} fetch=https://pt.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1&format=json&titles={1} fetchpath=query.pages.*.extract\n"
"encyclopedia-en | encyclopedia,general | * | en | json | https://en.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=5&srsearch=%s | years | 4 | 30 | items=query.search title=title url=https://en.wikipedia.org/wiki/{title} snip=snippet date=timestamp page=https://en.wikipedia.org/wiki/{1} fetch=https://en.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1&format=json&titles={1} fetchpath=query.pages.*.extract\n"
"# noticias do Brasil (feeds sem consulta: o roteador filtra as entradas pelas palavras da pergunta)\n"
"news-br-1 | news | br | pt | feed | https://g1.globo.com/rss/g1/ | hours | 3 | 6\n"
"news-br-2 | news | br | pt | feed | https://agenciabrasil.ebc.com.br/rss/ultimasnoticias/feed.xml | hours | 4 | 6\n"
"news-br-3 | news | br | pt | feed | https://feeds.folha.uol.com.br/emcimadahora/rss091.xml | hours | 3 | 6\n"
"news-br-search | news,world-news | br | pt | feed | https://news.google.com/rss/search?q=%s&hl=pt-BR&gl=BR&ceid=BR:pt-419 | hours | 3 | 10\n"
"# noticias do mundo\n"
"news-world-1 | world-news,news | * | en | feed | https://feeds.bbci.co.uk/news/world/rss.xml | hours | 4 | 6\n"
"news-world-2 | world-news,news | us | en | feed | https://feeds.npr.org/1004/rss.xml | hours | 4 | 6\n"
"news-world-3 | world-news | * | en | feed | https://www.aljazeera.com/xml/rss/all.xml | hours | 3 | 6\n"
"news-world-pt | world-news | * | pt | feed | https://feeds.bbci.co.uk/portuguese/rss.xml | hours | 4 | 6\n"
"news-search-en | news,world-news | us | en | feed | https://news.google.com/rss/search?q=%s&hl=en-US&gl=US&ceid=US:en | hours | 3 | 10\n"
"# busca geral: so o ultimo passo (menos de 2 fontes especificas, ou todas voltaram vazias)\n"
"general-1 | general | * | * | html | https://html.duckduckgo.com/html/?q=%s | days | 2 | 20 | parser=result-a\n"
"general-2 | general | * | * | html | https://www.bing.com/search?q=%s | days | 2 | 20 | parser=b-algo\n";
