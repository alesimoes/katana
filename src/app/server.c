/* ---------- servidor HTTP (so conversa, sem ferramentas) ----------
 * Por padrao escuta apenas em 127.0.0.1. GET / entrega a pagina embutida; POST /api/chat recebe as mensagens
 * em JSON e responde em Server-Sent Events, um evento por token; /v1/... e a API no formato da OpenAI (openai.c).
 * Cada conexao tem sua thread (Linux); o modelo gera uma resposta por vez e as outras esperam na fila, em ordem
 * de chegada (srv_lock). O cache KV da requisicao anterior e reaproveitado quando a nova comeca com os mesmos
 * tokens. Requisicoes locais com Origin ou Host que nao sejam locais sao recusadas (protege contra paginas de
 * outros sites falando com o servidor).
 * Rede: KATANA_HOST=0.0.0.0 (ou um endereco da maquina) abre para outras maquinas, e exige KATANA_API_KEY;
 * de fora da maquina tudo pede "Authorization: Bearer <chave>". Com chave definida, /v1 sempre pede a chave. */
#ifdef _WIN32
typedef SOCKET sock_t;
#define sock_close closesocket
#define SOCK_BAD(s) ((s) == INVALID_SOCKET)
#else
typedef int sock_t;
#define sock_close close
#define SOCK_BAD(s) ((s) < 0)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SRV_MAX_BODY (8 << 20)
#define SRV_MAX_MSGS 512
#define SRV_MAX_CONN 64                 /* conexoes abertas ao mesmo tempo (na fila ou gerando) */

static char g_srv_name[256] = "katana";     /* nome do modelo em /v1/models (arquivo sem .gguf) */
static char g_srv_key[256];                 /* KATANA_API_KEY */
static time_t g_srv_started;
static int g_srv_agent;                     /* katana serve --agent: o agente pela rede (agentapi.c), sem o modelo neste processo */
static char g_srv_model_path[4096];         /* caminho absoluto do modelo (o agente filho carrega) */

/* fila do modelo: uma geracao por vez, na ordem de chegada (senha numerada) */
#ifndef _WIN32
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_srv_cv = PTHREAD_COND_INITIALIZER;
static unsigned long g_srv_next, g_srv_serving;
static volatile int g_srv_conns;
static void srv_lock(void) {
    pthread_mutex_lock(&g_srv_mu);
    const unsigned long me = g_srv_next++;
    while (me != g_srv_serving) pthread_cond_wait(&g_srv_cv, &g_srv_mu);
    pthread_mutex_unlock(&g_srv_mu);
}
static void srv_unlock(void) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_serving++;
    pthread_cond_broadcast(&g_srv_cv);
    pthread_mutex_unlock(&g_srv_mu);
}
#else
static void srv_lock(void) {}           /* Windows: as conexoes sao atendidas uma de cada vez */
static void srv_unlock(void) {}
#endif

static const char SRV_PAGE[] =
"<!doctype html>\n"
"<html lang=\"pt-BR\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>katana</title>\n"
"<style>\n"
":root{--bg:#221f26;--side:#1b1920;--fg:#ece9f0;--mut:#a39fab;--bd:#38343f;--hov:#2b2831;--us:#332f3a;--ac:#b66cf2;--acfg:#1a1022;--card:#2b2831;--code:#1a181e;--codeh:#26232b;--err:#f2928b;--shadow:#000;--mono:\"JetBrains Mono\",ui-monospace,\"SF Mono\",Menlo,Consolas,monospace}\n"
"@media(prefers-color-scheme:light){:root{--bg:#faf9fc;--side:#f3f0f7;--fg:#1f1c24;--mut:#6b6773;--bd:#e2dde9;--hov:#ebe6f1;--us:#ece7f3;--ac:#820ad1;--acfg:#fff;--card:#fff;--code:#f5f2f8;--codeh:#ece7f3;--err:#b3261e;--shadow:#1e1028}}\n"
"*{box-sizing:border-box}\n"
"html,body{height:100%}\n"
"body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.6 var(--mono);display:flex;overflow:hidden}\n"
"button{font:inherit;color:inherit;cursor:pointer}\n"
"#side{width:270px;flex:none;background:var(--side);border-right:1px solid var(--bd);display:flex;flex-direction:column;padding:14px 12px;gap:12px;transition:margin .2s}\n"
"body.nosb #side{margin-left:-271px}\n"
".brand{display:flex;align-items:center;gap:10px;font-weight:800;letter-spacing:.14em;color:var(--ac);font-size:15px;padding:2px 4px}\n"
".logo{width:34px;height:34px;border-radius:10px;background:linear-gradient(145deg,#a030ee,#6a05b0);color:#fff;display:flex;align-items:center;justify-content:center;font-weight:800;font-size:20px;letter-spacing:0}\n"
".logo.s{width:24px;height:24px;border-radius:7px;font-size:14px}\n"
"#new{border:1px solid var(--bd);background:var(--bg);border-radius:10px;padding:9px 12px;text-align:left;font-weight:600}\n"
"#new:hover{background:var(--hov)}\n"
".lbl{font-size:10.5px;letter-spacing:.12em;color:var(--mut);padding:4px 6px 0;font-weight:700}\n"
"#list{flex:1;overflow:auto;display:flex;flex-direction:column;gap:4px}\n"
".cv{border-radius:10px;padding:8px 10px;cursor:pointer;border:0;background:transparent;text-align:left;min-width:0;width:100%}\n"
".cv:hover{background:var(--hov)}\n"
".cv.on{background:var(--us)}\n"
".cv span{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".cv small{display:block;color:var(--mut);font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
"details{font-size:12px;color:var(--mut);border-top:1px solid var(--bd);padding-top:8px}\n"
"details summary{cursor:pointer;padding:2px 4px}\n"
"details label{display:block;margin:8px 4px 0}\n"
"details input,details textarea{font:inherit;width:100%;margin-top:3px;background:var(--bg);color:var(--fg);border:1px solid var(--bd);border-radius:8px;padding:5px 8px}\n"
"details textarea{height:4.2rem;resize:vertical}\n"
"#main{flex:1;min-width:0;display:flex;flex-direction:column;height:100%}\n"
"#top{display:flex;align-items:center;gap:8px;padding:10px 14px;min-height:46px}\n"
"#tg{border:0;background:transparent;border-radius:8px;padding:6px 8px;color:var(--mut);font-size:17px;line-height:1}\n"
"#tg:hover{background:var(--hov)}\n"
"#mdl{color:var(--mut);font-size:12px}\n"
"#log{flex:1;overflow:auto;padding:6px 20px 12px}\n"
".col{max-width:900px;margin:0 auto;display:flex;flex-direction:column;gap:18px}\n"
".row{display:flex;gap:12px;align-items:flex-start}\n"
".row.u{justify-content:flex-end;flex-direction:column;align-items:flex-end}\n"
".bub{background:var(--us);border-radius:18px;padding:9px 16px;max-width:82%;white-space:pre-wrap;word-wrap:break-word}\n"
".tm{font-size:10.5px;color:var(--mut);margin-top:3px}\n"
".row.a .logo{margin-top:1px;flex:none}\n"
".txt{min-width:0;flex:1;white-space:pre-wrap;word-wrap:break-word;padding-top:2px}\n"
".txt pre{background:var(--code);border:1px solid var(--bd);border-radius:10px;padding:10px 12px;overflow:auto;margin:8px 0;white-space:pre}\n"
".txt code{font:13px/1.5 var(--mono)}\n"
".txt :not(pre)>code{background:var(--code);border:1px solid var(--bd);padding:0 5px;border-radius:5px}\n"
".empty{margin:14vh auto 0;max-width:640px;text-align:center}\n"
".empty .logo{width:64px;height:64px;border-radius:18px;font-size:38px;margin:0 auto 18px}\n"
".empty h1{margin:0 0 6px;font-size:22px;font-weight:700}\n"
".empty p{margin:0;color:var(--mut)}\n"
"#bot{padding:6px 20px 14px}\n"
"#bot .col{gap:6px}\n"
"form{border:1px solid var(--bd);border-radius:18px;background:var(--bg);box-shadow:0 8px 28px color-mix(in srgb,var(--shadow) 35%,transparent);display:flex;align-items:flex-end;gap:8px;padding:10px 10px 10px 16px}\n"
"form:focus-within{border-color:var(--ac)}\n"
"textarea#in{flex:1;font:inherit;background:transparent;color:var(--fg);border:0;outline:0;resize:none;height:3.2rem;padding:4px 0}\n"
"textarea#in::placeholder{color:var(--mut)}\n"
".send{width:36px;height:36px;border-radius:50%;border:0;background:var(--ac);color:var(--acfg);font-size:18px;font-weight:700;flex:none}\n"
".send:disabled{opacity:.4;cursor:default}\n"
"#stop{border:1px solid var(--bd);background:var(--hov);border-radius:50%;width:36px;height:36px;flex:none;display:none;font-size:13px}\n"
"body.run #stop{display:block}body.run #go{display:none}\n"
"#st{color:var(--mut);font-size:11.5px;text-align:center;min-height:1.3em}\n"
"@media(max-width:760px){#side{position:fixed;z-index:5;height:100%;box-shadow:0 0 30px var(--shadow)}body.nosb #side{margin-left:-290px}}\n"
"</style></head><body class=\"nosb\">\n"
"<aside id=\"side\">\n"
"<div class=\"brand\"><span class=\"logo\">K</span>KATANA</div>\n"
"<button id=\"new\" type=\"button\">+ Nova conversa</button>\n"
"<div class=\"lbl\">CONVERSAS</div>\n"
"<div id=\"list\"></div>\n"
"<details><summary>Ajustes</summary>\n"
"<label>temperatura <input id=\"temp\" type=\"number\" min=\"0\" max=\"2\" step=\"0.1\" value=\"0.7\"></label>\n"
"<label>max. tokens <input id=\"mt\" type=\"number\" min=\"1\" max=\"8192\" step=\"1\" value=\"1024\"></label>\n"
"<label>prompt de sistema<textarea id=\"sys\"></textarea></label></details>\n"
"</aside>\n"
"<div id=\"main\">\n"
"<div id=\"top\"><button id=\"tg\" type=\"button\" title=\"Mostrar ou esconder a lista\">&#9776;</button><span id=\"mdl\"></span></div>\n"
"<div id=\"log\"><div class=\"col\" id=\"col\"></div></div>\n"
"<div id=\"bot\"><div class=\"col\">\n"
"<form id=\"f\"><textarea id=\"in\" placeholder=\"Escreva uma mensagem...\" autofocus></textarea>\n"
"<button id=\"stop\" type=\"button\" title=\"Parar\">&#9632;</button><button id=\"go\" class=\"send\" title=\"Enviar\">&uarr;</button></form>\n"
"<div id=\"st\"></div></div></div>\n"
"</div>\n"
"<script>\n"
"var $=function(i){return document.getElementById(i)},H=[],ctl=null,cur=null,CV=[];\n"
"function esc(s){return s.replace(/&/g,\"&amp;\").replace(/</g,\"&lt;\").replace(/>/g,\"&gt;\")}\n"
"function render(t){var p=t.split(\"```\"),o=\"\";\n"
"for(var i=0;i<p.length;i++){\n"
"if(i%2){var c=p[i].replace(/^[^\\n]*\\n/,\"\");o+=\"<pre><code>\"+esc(c)+\"</code></pre>\"}\n"
"else o+=esc(p[i]).replace(/`([^`\\n]+)`/g,\"<code>$1</code>\")}\n"
"return o}\n"
"function hm(t){var d=t?new Date(t):new Date();return (\"0\"+d.getHours()).slice(-2)+\":\"+(\"0\"+d.getMinutes()).slice(-2)}\n"
"function scroll(){$(\"log\").scrollTop=1e9}\n"
"function empty(){$(\"col\").innerHTML='<div class=\"empty\"><div class=\"logo\">K</div><h1>Em que posso ajudar hoje?</h1><p>Escreva abaixo para conversar com o modelo local.</p></div>'}\n"
"function add(r,t,tm){var e=$(\"col\").querySelector(\".empty\");if(e)$(\"col\").innerHTML=\"\";\n"
"var row=document.createElement(\"div\");\n"
"if(r==\"user\"){row.className=\"row u\";var b=document.createElement(\"div\");b.className=\"bub\";b.textContent=t;var m=document.createElement(\"div\");m.className=\"tm\";m.textContent=hm(tm);row.appendChild(b);row.appendChild(m);$(\"col\").appendChild(row);scroll();return b}\n"
"row.className=\"row a\";row.innerHTML='<span class=\"logo s\">K</span>';var x=document.createElement(\"div\");x.className=\"txt\";x.innerHTML=render(t);row.appendChild(x);$(\"col\").appendChild(row);scroll();return x}\n"
"function busy(b){document.body.classList.toggle(\"run\",b);$(\"go\").disabled=b}\n"
"function store(){try{localStorage.setItem(\"katana.web\",JSON.stringify({cur:cur,CV:CV.slice(0,40)}))}catch(e){}}\n"
"function load(){try{var s=JSON.parse(localStorage.getItem(\"katana.web\")||\"{}\");CV=s.CV||[];cur=s.cur||null}catch(e){CV=[]}}\n"
"function lst(){var l=$(\"list\");l.innerHTML=\"\";CV.forEach(function(c){var b=document.createElement(\"button\");b.className=\"cv\"+(c.id==cur?\" on\":\"\");b.innerHTML=\"<span></span><small></small>\";b.firstChild.textContent=c.title;b.lastChild.textContent=hm(c.t)+\" · \"+(c.model||\"\");b.onclick=function(){open_(c.id)};l.appendChild(b)})}\n"
"function open_(id){if(ctl)ctl.abort();var c=CV.filter(function(x){return x.id==id})[0];if(!c)return;cur=id;H=c.H.slice();$(\"col\").innerHTML=\"\";H.forEach(function(m){add(m.role,m.content,c.t)});if(!H.length)empty();$(\"st\").textContent=\"\";lst();store();if(innerWidth<=760)document.body.classList.add(\"nosb\")}\n"
"function newc(){if(ctl)ctl.abort();cur=null;H=[];empty();$(\"st\").textContent=\"\";lst();store();$(\"in\").focus()}\n"
"function save(t){var c=CV.filter(function(x){return x.id==cur})[0];\n"
"if(!c){c={id:String(Date.now()),title:t.slice(0,60),t:Date.now(),model:$(\"mdl\").textContent,H:[]};CV.unshift(c);cur=c.id}\n"
"c.H=H.slice();c.t=Date.now();lst();store()}\n"
"$(\"stop\").onclick=function(){if(ctl)ctl.abort()};\n"
"$(\"new\").onclick=newc;\n"
"$(\"tg\").onclick=function(){document.body.classList.toggle(\"nosb\")};\n"
"$(\"in\").onkeydown=function(e){if(e.key==\"Enter\"&&!e.shiftKey){e.preventDefault();$(\"f\").requestSubmit()}};\n"
"$(\"f\").onsubmit=function(e){e.preventDefault();var t=$(\"in\").value.trim();if(!t||ctl)return;\n"
"$(\"in\").value=\"\";H.push({role:\"user\",content:t});add(\"user\",t);\n"
"var m=[],s=$(\"sys\").value.trim();if(s)m.push({role:\"system\",content:s});\n"
"var body=JSON.stringify({messages:m.concat(H),max_tokens:+$(\"mt\").value||1024,temperature:+$(\"temp\").value});\n"
"var box=add(\"assistant\",\"\"),acc=\"\",n=0;ctl=new AbortController();busy(true);$(\"st\").textContent=\"processando...\";\n"
"function fim(msg){if(!ctl)return;if(acc)H.push({role:\"assistant\",content:acc});else H.pop();\n"
"save(t);if(msg)$(\"st\").textContent=msg;ctl=null;busy(false);$(\"in\").focus()}\n"
"fetch(\"/api/chat\",{method:\"POST\",body:body,signal:ctl.signal}).then(function(r){\n"
"if(!r.ok)return r.text().then(function(x){throw new Error(x)});\n"
"var rd=r.body.getReader(),dec=new TextDecoder(),buf=\"\";\n"
"function pump(){return rd.read().then(function(x){\n"
"if(x.done){fim();return}\n"
"buf+=dec.decode(x.value,{stream:true});var k;\n"
"while((k=buf.indexOf(\"\\n\\n\"))>=0){var ev=buf.slice(0,k);buf=buf.slice(k+2);\n"
"if(ev.indexOf(\"data: \")!=0)continue;var j=JSON.parse(ev.slice(6));\n"
"if(j.token!==undefined){acc+=j.token;n++;box.innerHTML=render(acc);scroll()}\n"
"if(j.error)acc+=\"\\n[erro: \"+j.error+\"]\";\n"
"if(j.done){fim(j.tok_s.toFixed(1)+\" tok/s · \"+j.tokens+\" tokens · contexto \"+j.ctx+\" de \"+j.ctx_max+\" · prompt reaproveitado: \"+j.cached+\" de \"+j.prompt_tokens);return}}\n"
"return pump()})}\n"
"return pump()}).catch(function(er){\n"
"if(er.name==\"AbortError\")fim(\"interrompido\");else{box.textContent=\"[erro: \"+er.message+\"]\";fim(\"erro\")}})};\n"
"fetch(\"/v1/models\").then(function(r){return r.json()}).then(function(j){var d=j&&j.data&&j.data[0];if(d)$(\"mdl\").textContent=d.id}).catch(function(){});\n"
"load();lst();if(cur&&CV.some(function(c){return c.id==cur}))open_(cur);else empty();\n"
"if(innerWidth>760)document.body.classList.remove(\"nosb\");\n"
"</script></body></html>\n";

static int sock_send(sock_t s, const char *p, size_t n) {
    while (n > 0) {
        const int k = (int)send(s, p, (int)(n > 65536 ? 65536 : n), MSG_NOSIGNAL);
        if (k <= 0) return 0;
        p += k; n -= (size_t)k;
    }
    return 1;
}

static void http_reply(sock_t s, int code, const char *ctype, const char *body, size_t n) {
    char h[256];
    const char *st = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
                     code == 405 ? "Method Not Allowed" : code == 413 ? "Payload Too Large" : code == 401 ? "Unauthorized" :
                     code == 503 ? "Service Unavailable" : code == 409 ? "Conflict" : code == 500 ? "Internal Server Error" :
                     code == 501 ? "Not Implemented" : "Error";
    const int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                           code, st, ctype, n);
    sock_send(s, h, (size_t)k);
    sock_send(s, body, n);
}

static void http_error(sock_t s, int code, const char *msg) {
    char b[300];
    const int k = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    http_reply(s, code, "application/json; charset=utf-8", b, (size_t)k);
}

/* valor de um cabecalho (sem diferenciar maiusculas); 0 se nao existe */
static int hdr_get(const char *hs, const char *name, char *out, size_t cap) {
    const size_t nl = strlen(name);
    for (const char *p = hs; *p; ) {
        const char *e = strstr(p, "\r\n");
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > nl && p[nl] == ':') {
            size_t i = 0;
            for (; i < nl; i++) if ((p[i] | 32) != (name[i] | 32)) break;
            if (i == nl) {
                const char *v = p + nl + 1;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(e - v);
                if (vl >= cap) vl = cap - 1;
                memcpy(out, v, vl); out[vl] = 0;
                return 1;
            }
        }
        if (!*e) break;
        p = e + 2;
    }
    return 0;
}

/* ---- JSON minimo ---- */
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static void utf8_put(Buf *b, unsigned cp) {
    char o[4]; int n = 0;
    if (cp < 0x80) o[n++] = (char)cp;
    else if (cp < 0x800) { o[n++] = (char)(0xC0 | (cp >> 6)); o[n++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o[n++] = (char)(0xE0 | (cp >> 12)); o[n++] = (char)(0x80 | ((cp >> 6) & 63)); o[n++] = (char)(0x80 | (cp & 63)); }
    else { o[n++] = (char)(0xF0 | (cp >> 18)); o[n++] = (char)(0x80 | ((cp >> 12) & 63)); o[n++] = (char)(0x80 | ((cp >> 6) & 63)); o[n++] = (char)(0x80 | (cp & 63)); }
    buf_add(b, o, (size_t)n);
}

static int hex4(const char *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        const char ch = p[i];
        *v <<= 4;
        if (ch >= '0' && ch <= '9') *v |= (unsigned)(ch - '0');
        else if ((ch | 32) >= 'a' && (ch | 32) <= 'f') *v |= (unsigned)((ch | 32) - 'a' + 10);
        else return 0;
    }
    return 1;
}

/* le uma string JSON em *pp (que aponta para a aspa de abertura) e acrescenta o texto decodificado em out */
static int js_string(const char **pp, Buf *out) {
    const char *p = *pp;
    if (*p != '"') return 0;
    for (p++; *p && *p != '"'; p++) {
        if ((unsigned char)*p < 0x20 && *p != '\n' && *p != '\t' && *p != '\r') return 0;
        if (*p != '\\') { buf_add(out, p, 1); continue; }
        p++;
        switch (*p) {
            case 'n': buf_add(out, "\n", 1); break;
            case 't': buf_add(out, "\t", 1); break;
            case 'r': buf_add(out, "\r", 1); break;
            case 'b': buf_add(out, "\b", 1); break;
            case 'f': buf_add(out, "\f", 1); break;
            case '"': case '\\': case '/': buf_add(out, p, 1); break;
            case 'u': {
                unsigned cp, lo;
                if (!hex4(p + 1, &cp)) return 0;
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u' && hex4(p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                utf8_put(out, cp);
                break;
            }
            default: return 0;
        }
    }
    if (*p != '"') return 0;
    *pp = p + 1;
    return 1;
}

/* pula um valor JSON qualquer */
static int js_skip(const char **pp, int depth) {
    const char *p = js_ws(*pp);
    if (depth > 32) return 0;
    if (*p == '"') {
        Buf tmp = {0};
        const int ok = js_string(&p, &tmp);
        free(tmp.p);
        if (!ok) return 0;
    } else if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1);
        if (*p == close) { *pp = p + 1; return 1; }
        for (;;) {
            if (close == '}') {
                Buf tmp = {0};
                const int ok = js_string(&p, &tmp);
                free(tmp.p);
                if (!ok) return 0;
                p = js_ws(p);
                if (*p++ != ':') return 0;
            }
            if (!js_skip(&p, depth + 1)) return 0;
            p = js_ws(p);
            if (*p == ',') { p = js_ws(p + 1); continue; }
            if (*p == close) { p++; break; }
            return 0;
        }
    } else {
        const char *s = p;
        while (*p && strchr("+-.0123456789eEtrufalsn", *p)) p++;
        if (p == s) return 0;
    }
    *pp = p;
    return 1;
}

typedef struct { char *role, *content; } SrvMsg;
typedef struct { SrvMsg msg[SRV_MAX_MSGS]; int n; int max_tokens; float temp; int rf; char *rf_schema; } SrvReq;

static void srv_req_free(SrvReq *r) {
    for (int i = 0; i < r->n; i++) { free(r->msg[i].role); free(r->msg[i].content); }
    r->n = 0;
    free(r->rf_schema); r->rf_schema = NULL;
}

/* {"messages":[{"role":..,"content":..},..],"max_tokens":N,"temperature":T}; 0 se invalido */
static int srv_parse(const char *js, SrvReq *r) {
    const char *p = js_ws(js);
    if (*p++ != '{') return 0;
    p = js_ws(p);
    if (*p == '}') return 1;
    for (;;) {
        Buf key = {0};
        if (!js_string(&p, &key)) { free(key.p); return 0; }
        p = js_ws(p);
        if (*p++ != ':') { free(key.p); return 0; }
        p = js_ws(p);
        const char *k = key.p ? key.p : "";
        int ok = 1;
        if (strcmp(k, "messages") == 0 && *p == '[') {
            p = js_ws(p + 1);
            while (ok && *p != ']') {
                if (*p != '{' || r->n >= SRV_MAX_MSGS) { ok = 0; break; }
                Buf role = {0}, content = {0};
                p = js_ws(p + 1);
                while (*p != '}') {
                    Buf k2 = {0};
                    if (!js_string(&p, &k2)) { free(k2.p); ok = 0; break; }
                    p = js_ws(p);
                    if (*p++ != ':') { free(k2.p); ok = 0; break; }
                    p = js_ws(p);
                    const char *kk = k2.p ? k2.p : "";
                    if (strcmp(kk, "role") == 0 && *p == '"') ok = js_string(&p, &role);
                    else if (strcmp(kk, "content") == 0 && *p == '"') ok = js_string(&p, &content);
                    else ok = js_skip(&p, 1);
                    free(k2.p);
                    if (!ok) break;
                    p = js_ws(p);
                    if (*p == ',') p = js_ws(p + 1);
                    else if (*p != '}') { ok = 0; break; }
                }
                if (!ok) { free(role.p); free(content.p); break; }
                p++;
                r->msg[r->n].role = role.p ? role.p : xalloc(1, 1);
                r->msg[r->n].content = content.p ? content.p : xalloc(1, 1);
                r->n++;
                p = js_ws(p);
                if (*p == ',') p = js_ws(p + 1);
                else if (*p != ']') ok = 0;
            }
            if (ok) p++;
        } else if (strcmp(k, "max_tokens") == 0 && (*p == '-' || (*p >= '0' && *p <= '9'))) {
            r->max_tokens = (int)strtol(p, NULL, 10);
            ok = js_skip(&p, 1);
        } else if (strcmp(k, "response_format") == 0 && *p == '{') {
            /* {"type":"json_object"} ou {"type":"json_schema","schema":{...}} (ou json_schema:{schema:{...}}) */
            const char *st = p;
            ok = js_skip(&p, 1);
            if (ok) {
                char *raw = xalloc((size_t)(p - st) + 1, 1);
                memcpy(raw, st, (size_t)(p - st));
                char key2[64], ty[32] = ""; const char *val, *q = raw + 1, *e;
                while ((e = gs_member(q, key2, sizeof key2, &val))) {
                    if (!strcmp(key2, "type") && *val == '"') { size_t n = 0; for (const char *w = val + 1; *w && *w != '"' && n < 31; w++) ty[n++] = *w; ty[n] = 0; }
                    else if (!strcmp(key2, "schema") && *val == '{') { const char *ve = gs_skipv(val); if (ve) { free(r->rf_schema); r->rf_schema = xalloc((size_t)(ve - val) + 1, 1); memcpy(r->rf_schema, val, (size_t)(ve - val)); } }
                    else if (!strcmp(key2, "json_schema") && *val == '{' && !r->rf_schema) {
                        char k3[64]; const char *v3, *q3 = val + 1, *e3;
                        while ((e3 = gs_member(q3, k3, sizeof k3, &v3))) {
                            if (!strcmp(k3, "schema") && *v3 == '{') { const char *ve = gs_skipv(v3); if (ve) { r->rf_schema = xalloc((size_t)(ve - v3) + 1, 1); memcpy(r->rf_schema, v3, (size_t)(ve - v3)); } }
                            q3 = e3;
                        }
                    }
                    q = e;
                }
                r->rf = strcmp(ty, "json_object") == 0 ? 1 : (strcmp(ty, "json_schema") == 0 || r->rf_schema) ? 2 : 0;
                free(raw);
            }
        } else if (strcmp(k, "temperature") == 0 && (*p == '-' || (*p >= '0' && *p <= '9'))) {
            r->temp = (float)strtod(p, NULL);
            ok = js_skip(&p, 1);
        } else ok = js_skip(&p, 1);
        free(key.p);
        if (!ok) return 0;
        p = js_ws(p);
        if (*p == ',') { p = js_ws(p + 1); continue; }
        return *p == '}';
    }
}

/* tamanho da sequencia UTF-8 valida que comeca em s (0 se invalida ou cortada) */
static size_t utf8_seq(const unsigned char *s, size_t n) {
    const unsigned c = s[0];
    size_t k; unsigned cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { k = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 4; cp = c & 0x07; }
    else return 0;
    if (n < k) return 0;
    for (size_t i = 1; i < k; i++) { if ((s[i] & 0xC0) != 0x80) return 0; cp = (cp << 6) | (s[i] & 0x3F); }
    if ((k == 3 && (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000))) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    return k;
}

/* escapa texto UTF-8 para dentro de uma string JSON; bytes que nao formam UTF-8 valido viram U+FFFD */
static void js_escape(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)s[i];
        char o[8];
        if (ch >= 0x80) {
            const size_t k = utf8_seq((const unsigned char *)s + i, n - i);
            if (!k) buf_add(b, "\\ufffd", 6);
            else { buf_add(b, s + i, k); i += k - 1; }
            continue;
        }
        switch (ch) {
            case '"': buf_add(b, "\\\"", 2); break;
            case '\\': buf_add(b, "\\\\", 2); break;
            case '\n': buf_add(b, "\\n", 2); break;
            case '\r': buf_add(b, "\\r", 2); break;
            case '\t': buf_add(b, "\\t", 2); break;
            default:
                if (ch < 0x20) { snprintf(o, sizeof o, "\\u%04x", ch); buf_add(b, o, 6); }
                else buf_add(b, s + i, 1);
        }
    }
}

/* tamanho do maior prefixo de s formado so por sequencias UTF-8 completas (o resto espera o proximo token) */
static size_t utf8_complete(const char *s, size_t n) {
    size_t i = n;
    int back = 0;
    while (i > 0 && back < 4 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) { i--; back++; }
    if (i == 0) return n;
    const unsigned char lead = (unsigned char)s[i - 1];
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return (size_t)back + 1 >= need ? n : i - 1;
}

/* bytes de um token (inclui os tokens de byte <0xNN> dos modelos SPM) */
static size_t tok_bytes(const Tokenizer *tk, int id, char *out, size_t cap) {
    const uint8_t *t = (const uint8_t *)tk->tok[id];
    if (tk->spm && tk->toktype && tk->toktype[id] == 6 && tk->toklen[id] == 6 && t[0] == '<') {
        out[0] = (char)strtol((const char *)t + 3, NULL, 16); out[1] = 0;
        return 1;
    }
    return tok_append(tk, id, out, 0, cap);
}

static int sse_send(sock_t s, const char *json, size_t n) {
    return sock_send(s, "data: ", 6) && sock_send(s, json, n) && sock_send(s, "\n\n", 2);
}

/* estado do cache KV entre requisicoes: tokens ja processados, na ordem das posicoes */
static Ids srv_cache;

static void srv_chat(Model *m, sock_t s, const char *body) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    SrvReq *rq = xalloc(1, sizeof *rq);
    rq->max_tokens = 1024; rq->temp = 0.7f;
    if (!srv_parse(body, rq) || rq->n == 0) { http_error(s, 400, "JSON invalido: esperado {\\\"messages\\\":[...]}"); srv_req_free(rq); free(rq); return; }
    if (rq->max_tokens < 1) rq->max_tokens = 1;
    if (rq->temp < 0) rq->temp = 0;
    if (rq->temp > 2) rq->temp = 2;
    int max_tokens = rq->max_tokens;
    const float temp = rq->temp;
    const int rf = rq->rf;
    char *rf_schema = rq->rf_schema; rq->rf_schema = NULL;      /* fica comigo ate o fim da requisicao */

    /* monta o texto da conversa no formato do modelo, como o console faz turno a turno */
    const char *sys = "";
    int i0 = 0;
    if (strcmp(rq->msg[0].role, "system") == 0) { sys = rq->msg[0].content; i0 = 1; }
    if (strcmp(rq->msg[rq->n - 1].role, "user") != 0) { http_error(s, 400, "a ultima mensagem deve ser do usuario"); srv_req_free(rq); free(rq); return; }
    Buf text = {0};
    int first = 1;
    for (int i = i0; i < rq->n; i++) {
        if (strcmp(rq->msg[i].role, "user") != 0) continue;
        const size_t cap = strlen(rq->msg[i].content) + strlen(sys) + 1024;
        char *piece = xalloc(cap, 1);
        turn_text(c, piece, cap, sys, rq->msg[i].content, first);
        buf_str(&text, piece);
        free(piece);
        first = 0;
        if (i + 1 < rq->n && strcmp(rq->msg[i + 1].role, "assistant") == 0) buf_str(&text, rq->msg[i + 1].content);
    }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text.p ? text.p : "", &ids);
    free(text.p);
    srv_req_free(rq); free(rq);

    if (ids.n + 16 >= c->ctx) {
        char b[200];
        snprintf(b, sizeof b, "conversa grande demais para o contexto (%d tokens de %d)", ids.n, c->ctx);
        http_error(s, 413, b);
        free(ids.ids); free(rf_schema);
        return;
    }
    if (max_tokens > c->ctx - ids.n - 1) max_tokens = c->ctx - ids.n - 1;
    if (rf) {                                  /* saida estruturada: valida o schema antes de comecar */
        gr_prepare(tk, c->vocab);
        g_gr.stop2 = -1;
        if (rf == 2 && rf_schema) { if (!gr_json_schema(rf_schema)) { http_error(s, 400, "response_format: schema invalido"); free(ids.ids); free(rf_schema); return; } }
        else gr_json_any();
        gr_off();
    }

    /* reuso de prefixo: mantem as posicoes do cache que coincidem com o inicio do novo prompt */
    int common = 0;
    while (common < ids.n && common < srv_cache.n && ids.ids[common] == srv_cache.ids[common]) common++;
    common = cache_rewind(m, srv_cache.n, common, ids.n);      /* DeltaNet e o anel do gemma4 nem sempre podem voltar */
    srv_cache.n = common;

    const char *hd = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                     "X-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
    if (!sock_send(s, hd, strlen(hd))) { free(ids.ids); free(rf_schema); srv_cache.n = 0; return; }

    const double t0 = now_sec();
    int pos = common;
    float *lg = forward_seq(m, ids.ids + common, ids.n - common, common, 1);
    pos = ids.n;
    for (int i = common; i < ids.n; i++) ids_push(&srv_cache, ids.ids[i]);
    const double t1 = now_sec();

    Buf pend = {0}, ev = {0};
    int gen = 0, alive = 1;
    char tmp[256];
    if (rf) { if (rf == 2 && rf_schema) gr_json_schema(rf_schema); else gr_json_any(); }
    while (gen < max_tokens && pos < c->ctx - 1 && alive) {
        g_gr.left = max_tokens - gen;
        const int t = sample_c(lg, c->vocab, temp, c->eos);
        if (t == c->eos || (tk->toktype && tk->toktype[t] == 3)) break;
        gr_accept(t);
        buf_add(&pend, tmp, tok_bytes(tk, t, tmp, sizeof tmp));
        const size_t ok = utf8_complete(pend.p, pend.n);
        if (ok > 0) {
            buf_clear(&ev);
            buf_str(&ev, "{\"token\":\"");
            js_escape(&ev, pend.p, ok);
            buf_str(&ev, "\"}");
            alive = sse_send(s, ev.p, ev.n);
            memmove(pend.p, pend.p + ok, pend.n - ok);
            pend.n -= ok; pend.p[pend.n] = 0;
        }
        gen++;
        lg = forward(m, t, pos++, 1);
        ids_push(&srv_cache, t);
    }
    const double t2 = now_sec();
    gr_off(); free(rf_schema);
    if (alive) {
        char d[320];
        const int k = snprintf(d, sizeof d,
            "{\"done\":true,\"tok_s\":%.2f,\"tokens\":%d,\"prompt_tokens\":%d,\"cached\":%d,\"ctx\":%d,\"ctx_max\":%d}",
            gen / (t2 - t1 + 1e-9), gen, ids.n, common, pos, c->ctx);
        sse_send(s, d, (size_t)k);
    }
    fprintf(stderr, "[serve] prompt %d tokens (%d reaproveitados, %.1f tok/s), resposta %d tokens, %.1f tok/s%s\n",
            ids.n, common, (ids.n - common) / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9), alive ? "" : " (cliente saiu)");
    free(ids.ids); free(pend.p); free(ev.p);
}

static int host_local(const char *h) {   /* "localhost" ou "127.0.0.1", com porta opcional */
    return (strncmp(h, "localhost", 9) == 0 && (h[9] == 0 || h[9] == ':')) ||
           (strncmp(h, "127.0.0.1", 9) == 0 && (h[9] == 0 || h[9] == ':'));
}

static void oa_chat(Model *m, sock_t s, const char *body);
static void oa_models(sock_t s);
static void oa_error(sock_t s, int code, const char *msg, const char *type);
static char *srv_body(sock_t s, const char *head, size_t hn, size_t hend);
static void ag_route(sock_t s, const char *method, const char *path, const char *head, size_t hn, size_t hend);

/* "Authorization: Bearer <chave>" confere com KATANA_API_KEY (comparacao em tempo constante) */
static int srv_auth(const char *head) {
    char v[512];
    if (!g_srv_key[0] || !hdr_get(head, "Authorization", v, sizeof v) || strncmp(v, "Bearer ", 7) != 0) return 0;
    const char *t = v + 7;
    const size_t a = strlen(t), b = strlen(g_srv_key);
    unsigned d = a != b;
    for (size_t i = 0; i < b; i++) d |= (unsigned char)(i < a ? t[i] : 0) ^ (unsigned char)g_srv_key[i];
    return d == 0;
}

/* corpo da requisicao (Content-Length); NULL se ja respondeu com erro ou a conexao caiu */
static char *srv_body(sock_t s, const char *head, size_t hn, size_t hend) {
    char val[64];
    size_t clen = 0;
    if (hdr_get(head, "Content-Length", val, sizeof val)) clen = (size_t)strtoull(val, NULL, 10);
    else { http_error(s, 400, "falta Content-Length"); return NULL; }
    if (clen > (g_vis.on ? (size_t)32 << 20 : (size_t)SRV_MAX_BODY)) { http_error(s, 413, "corpo grande demais"); return NULL; }
    if (hdr_get(head, "Expect", val, sizeof val)) sock_send(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    char *body = xalloc(clen + 1, 1);
    size_t have = hn - hend;
    if (have > clen) have = clen;
    memcpy(body, head + hend, have);
    while (have < clen) {
        const int k = (int)recv(s, body + have, (int)(clen - have > 65536 ? 65536 : clen - have), 0);
        if (k <= 0) { free(body); return NULL; }
        have += (size_t)k;
    }
    return body;
}

#define SRV_HEAD 65536
static void srv_conn(Model *m, sock_t s, int peer_local) {
    char *head = xalloc(SRV_HEAD, 1);
    size_t hn = 0, hend = 0;
    while (!hend) {      /* cabecalhos ate a linha em branco */
        if (hn + 1 >= SRV_HEAD) { http_error(s, 413, "cabecalhos grandes demais"); free(head); return; }
        const int k = (int)recv(s, head + hn, (int)(SRV_HEAD - 1 - hn), 0);
        if (k <= 0) { free(head); return; }
        hn += (size_t)k; head[hn] = 0;
        const char *e = strstr(head, "\r\n\r\n");
        if (e) hend = (size_t)(e - head) + 4;
    }
    head[hend - 2] = 0;                                  /* termina a lista de cabecalhos */
    char method[16] = "", path[512] = "", val[512];
    if (sscanf(head, "%15s %511s", method, path) != 2) { http_error(s, 400, "requisicao invalida"); free(head); return; }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    const int v1 = strncmp(path, "/v1/", 4) == 0;
    if (strcmp(path, "/health") == 0) {                  /* sem chave: so diz que esta de pe */
        http_reply(s, 200, "application/json; charset=utf-8", "{\"status\":\"ok\"}", 15);
        free(head); return;
    }
    if (!peer_local || (v1 && g_srv_key[0])) {          /* de fora da maquina (ou /v1 com chave): so com a chave */
        if (!srv_auth(head)) {
            if (v1) oa_error(s, 401, "missing or invalid API key (Authorization: Bearer ...)", "invalid_request_error");
            else http_error(s, 401, "chave invalida ou ausente");
            free(head); return;
        }
    } else {                                             /* local sem chave: protege contra paginas de outros sites */
        if (hdr_get(head, "Host", val, sizeof val) && !host_local(val)) { http_error(s, 403, "host nao permitido"); free(head); return; }
        if (hdr_get(head, "Origin", val, sizeof val)) {
            const char *o = strstr(val, "://");
            if (!o || !host_local(o + 3)) { http_error(s, 403, "origem nao permitida"); free(head); return; }
        }
    }
    if (strncmp(path, "/v1/agent/", 10) == 0 || strcmp(path, "/v1/agent") == 0) { ag_route(s, method, path, head, hn, hend); free(head); return; }
    if (g_srv_agent && (strcmp(path, "/v1/chat/completions") == 0 || strcmp(path, "/api/chat") == 0)) {   /* o modelo esta so no agente */
        oa_error(s, 503, "agent mode: this server runs katana's agent (/v1/agent/sessions) and does not hold the model for chat completions", "server_error");
        free(head); return;
    }
    if (strcmp(path, "/v1/models") == 0) {
        if (strcmp(method, "GET") != 0) oa_error(s, 405, "use GET", "invalid_request_error");
        else oa_models(s);
        free(head); return;
    }
    if (strcmp(path, "/v1/chat/completions") == 0) {
        if (strcmp(method, "POST") != 0) { oa_error(s, 405, "use POST", "invalid_request_error"); free(head); return; }
        char *body = srv_body(s, head, hn, hend);
        if (body) oa_chat(m, s, body);
        free(body); free(head);
        return;
    }
    if (v1) { oa_error(s, 404, "unknown endpoint", "invalid_request_error"); free(head); return; }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        if (strcmp(method, "GET") != 0) http_error(s, 405, "metodo nao permitido");
        else http_reply(s, 200, "text/html; charset=utf-8", SRV_PAGE, sizeof SRV_PAGE - 1);
        free(head); return;
    }
    if (strcmp(path, "/api/chat") == 0) {
        if (strcmp(method, "POST") != 0) { http_error(s, 405, "use POST"); free(head); return; }
        char *body = srv_body(s, head, hn, hend);
        if (body) { srv_lock(); srv_chat(m, s, body); srv_unlock(); }
        free(body); free(head);
        return;
    }
    http_error(s, 404, "nao encontrado");
    free(head);
}

static int srv_peer_local(const struct sockaddr_in *a) { return (ntohl(a->sin_addr.s_addr) >> 24) == 127; }

#ifndef _WIN32
typedef struct { Model *m; sock_t s; int local; } SrvArg;
static void *srv_thread(void *p) {
    SrvArg *a = (SrvArg *)p;
    srv_conn(a->m, a->s, a->local);
    sock_close(a->s);
    __sync_fetch_and_sub(&g_srv_conns, 1);
    free(a);
    return NULL;
}
#endif

static int serve(Model *m, int port) {
    srand((unsigned)time(NULL));
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup falhou");
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    g_srv_started = time(NULL);
    const char *host = getenv("KATANA_HOST") && *getenv("KATANA_HOST") ? getenv("KATANA_HOST") : "127.0.0.1";
    if (getenv("KATANA_API_KEY")) snprintf(g_srv_key, sizeof g_srv_key, "%s", getenv("KATANA_API_KEY"));
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) die("KATANA_HOST: endereco IPv4 invalido (ex.: 127.0.0.1 ou 0.0.0.0)");
    const int net = !srv_peer_local(&a);
    if (g_srv_agent && net && strlen(g_srv_key) < 16)       /* o agente executa comandos: nunca aberto sem chave */
        die("katana serve --agent: KATANA_HOST abre para a rede e o agente executa comandos nesta maquina; defina KATANA_API_KEY com pelo menos 16 caracteres");
    if (net && strlen(g_srv_key) < 16) die("KATANA_HOST abre o servidor para a rede: defina KATANA_API_KEY com pelo menos 16 caracteres");
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCK_BAD(ls)) die("nao consegui criar o socket");
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) die("nao consegui escutar nessa porta (em uso?)");
    if (listen(ls, 64) != 0) die("listen falhou");
    fprintf(stderr, "katana serve: http://%s:%d  modelo \"%s\"  API: %s%s%s; Ctrl+C encerra\n",
            host, port, g_srv_name, g_srv_agent ? "/v1/agent/sessions (agente: executa comandos nesta maquina), /v1/models" : "/v1/chat/completions, /v1/models",
            net ? "; aberto para a rede, com chave" : "; so esta maquina", g_srv_key[0] && !net ? "; /v1 com chave" : "");
    for (;;) {
        struct sockaddr_in pa; socklen_t pl = sizeof pa;
        memset(&pa, 0, sizeof pa);
        sock_t cs = accept(ls, (struct sockaddr *)&pa, &pl);
        if (SOCK_BAD(cs)) continue;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef _WIN32
        DWORD to = 5000, sto = 60000;
#else
        struct timeval to = { 5, 0 }, sto = { 60, 0 };
#endif
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sto, sizeof sto);   /* cliente parado nao segura a fila */
        const int local = srv_peer_local(&pa);
#ifdef _WIN32
        srv_conn(m, cs, local);
        sock_close(cs);
#else
        if (__sync_fetch_and_add(&g_srv_conns, 1) >= SRV_MAX_CONN) {
            __sync_fetch_and_sub(&g_srv_conns, 1);
            http_error(cs, 503, "servidor ocupado, tente de novo");
            sock_close(cs);
            continue;
        }
        SrvArg *arg = xalloc(1, sizeof *arg);
        arg->m = m; arg->s = cs; arg->local = local;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, srv_thread, arg) != 0) {
            __sync_fetch_and_sub(&g_srv_conns, 1);
            http_error(cs, 503, "servidor ocupado, tente de novo");
            sock_close(cs); free(arg);
        }
        pthread_attr_destroy(&at);
#endif
    }
    return 0;
}

