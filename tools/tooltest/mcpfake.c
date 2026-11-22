/* Servidor MCP falso (stdio) para tools/tooltest/mcpx.sh. Uma linha JSON por mensagem. Compilado pelo proprio teste.
 * Uso: mcpfake MODO [args...]   Os modos ligam comportamentos: env (ferramentas env/argv), roots (pede roots/list),
 * lists (avisa mudanca de resources/prompts), annot (anotacoes), elicit (pede dados ao usuario). Sem rede. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *mode = "env";
static int g_argc; static char **g_argv;
static char line[1 << 16];

static void out(const char *s) { fputs(s, stdout); fputc('\n', stdout); fflush(stdout); }
/* valor numerico ou texto cru depois de "key": ; copia ate a virgula/chave */
static int field(const char *js, const char *key, char *dst, size_t cap) {
    char k[100]; snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(js, k);
    if (!p) return 0;
    p += strlen(k); while (*p == ' ') p++;
    size_t n = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && n + 1 < cap) { if (*p == '\\' && p[1]) p++; dst[n++] = *p++; } }
    else while (*p && *p != ',' && *p != '}' && n + 1 < cap) dst[n++] = *p++;
    dst[n] = 0;
    return 1;
}
static void reply(const char *id, const char *result) { printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id, result); fflush(stdout); }
static void text(const char *id, const char *t) { char b[4000]; snprintf(b, sizeof b, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", t); reply(id, b); }
static void esc(const char *s, char *o, size_t cap) { size_t n = 0; for (; *s && n + 3 < cap; s++) { if (*s == '"' || *s == '\\') o[n++] = '\\'; o[n++] = *s; } o[n] = 0; }

static void fake_initialize(const char *id) {
    char b[600];
    snprintf(b, sizeof b, "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":true}},\"serverInfo\":{\"name\":\"fake-%s\",\"version\":\"1\"}}", mode);
    reply(id, b);
}
static void fake_notification(const char *meth, const char *js) { (void)meth; (void)js; }
static void fake_response(const char *js, const char *id) { (void)js; (void)id; }
static void fake_tools_list(const char *id) {
    reply(id, "{\"tools\":[{\"name\":\"env\",\"description\":\"value of an environment variable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},"
             "{\"name\":\"argv\",\"description\":\"the program arguments\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}");
}
static void fake_call(const char *id, const char *js, long *nid) {
    char tn[100] = "", a[300] = "", r[2000] = "";
    (void)nid;
    field(js, "name", tn, sizeof tn);
    if (!strcmp(tn, "env")) {
        const char *p = strstr(js, "\"arguments\"");
        if (p) field(p, "name", a, sizeof a);
        const char *v = getenv(a);
        snprintf(r, sizeof r, "%s=%s", a, v ? v : "(unset)");
        char e[2200]; esc(r, e, sizeof e); text(id, e);
    } else if (!strcmp(tn, "argv")) {
        for (int i = 2; i < g_argc; i++) { size_t l = strlen(r); snprintf(r + l, sizeof r - l, "%s[%s]", i > 2 ? " " : "", g_argv[i]); }
        char e[2200]; esc(r, e, sizeof e); text(id, e);
    } else text(id, "unknown");
}

int main(int argc, char **argv) {
    g_argc = argc; g_argv = argv;
    if (argc > 1) mode = argv[1];
    long nid = 1000;
    while (fgets(line, sizeof line, stdin)) {
        char id[40] = "", meth[100] = "";
        const int hasid = field(line, "id", id, sizeof id);
        const int hasm = field(line, "method", meth, sizeof meth);
        if (!hasm) { fake_response(line, id); continue; }      /* resposta a um pedido nosso */
        if (!strcmp(meth, "initialize")) { fake_initialize(id); continue; }
        if (!hasid) { fake_notification(meth, line); continue; }
        if (!strcmp(meth, "tools/list")) { fake_tools_list(id); continue; }
        if (!strcmp(meth, "tools/call")) { fake_call(id, line, &nid); continue; }
        if (!strcmp(meth, "resources/list")) { reply(id, "{\"resources\":[]}"); continue; }
        if (!strcmp(meth, "prompts/list")) { reply(id, "{\"prompts\":[]}"); continue; }
        if (!strcmp(meth, "ping")) { reply(id, "{}"); continue; }
        printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n", id); fflush(stdout);
    }
    return 0;
}
