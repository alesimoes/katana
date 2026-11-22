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

static char g_init[4000];
static int g_n_roots_changed, g_mutated, g_n_plist;
static void fake_initialize(const char *id) {
    char b[600];
    snprintf(g_init, sizeof g_init, "%s", line); g_init[strcspn(g_init, "\n")] = 0;
    snprintf(b, sizeof b, "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":true},\"prompts\":{\"listChanged\":true},\"resources\":{\"listChanged\":true}},\"serverInfo\":{\"name\":\"fake-%s\",\"version\":\"1\"}}", mode);
    reply(id, b);
}
static void fake_notification(const char *meth, const char *js) { (void)js; if (!strcmp(meth, "notifications/roots/list_changed")) g_n_roots_changed++; }
static void fake_response(const char *js, const char *id) { (void)js; (void)id; }
static void fake_tools_list(const char *id) {
    reply(id, "{\"tools\":[{\"name\":\"env\",\"description\":\"value of an environment variable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},"
             "{\"name\":\"roots\",\"description\":\"ask the client for its roots\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
             "{\"name\":\"rcount\",\"description\":\"how many roots/list_changed arrived\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
             "{\"name\":\"caps\",\"description\":\"the initialize message received\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
             "{\"name\":\"mutate\",\"description\":\"announce that resources and prompts changed\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
             "{\"name\":\"plistcount\",\"description\":\"how many prompts/list were served\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
             "{\"name\":\"peek\",\"title\":\"Peek at it\",\"description\":\"read-only tool\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"readOnlyHint\":true,\"idempotentHint\":true}},"
             "{\"name\":\"wipe\",\"description\":\"destructive tool\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"title\":\"Wipe all\",\"destructiveHint\":true,\"openWorldHint\":true}},"
             "{\"name\":\"liar\",\"description\":\"claims read-only and destructive\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":true}},"
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
    } else if (!strcmp(tn, "caps")) {
        char e[4400]; esc(g_init, e, sizeof e); text(id, e);
    } else if (!strcmp(tn, "mutate")) {
        g_mutated = 1;
        out("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/list_changed\"}");
        out("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/prompts/list_changed\"}");
        text(id, "mutated");
    } else if (!strcmp(tn, "plistcount")) {
        snprintf(r, sizeof r, "prompts_list=%d", g_n_plist); text(id, r);
    } else if (!strcmp(tn, "peek") || !strcmp(tn, "wipe") || !strcmp(tn, "liar")) {
        snprintf(r, sizeof r, "%s ran", tn); text(id, r);
    } else if (!strcmp(tn, "rcount")) {
        snprintf(r, sizeof r, "roots_changed=%d", g_n_roots_changed); text(id, r);
    } else if (!strcmp(tn, "roots")) {
        char resp[8000] = "";
        out("{\"jsonrpc\":\"2.0\",\"id\":7001,\"method\":\"roots/list\"}");
        while (fgets(line, sizeof line, stdin)) {
            char i2[40] = "", m2[100] = "";
            if (field(line, "method", m2, sizeof m2)) { if (!field(line, "id", i2, sizeof i2)) fake_notification(m2, line); continue; }
            if (field(line, "id", i2, sizeof i2) && !strcmp(i2, "7001")) { snprintf(resp, sizeof resp, "%s", line); break; }
        }
        resp[strcspn(resp, "\n")] = 0;
        char e[8200]; esc(resp, e, sizeof e); text(id, e);
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
        if (!strcmp(meth, "resources/list")) { reply(id, g_mutated ? "{\"resources\":[{\"uri\":\"file:///r2\",\"name\":\"second\"}]}" : "{\"resources\":[{\"uri\":\"file:///r1\",\"name\":\"first\"}]}"); continue; }
        if (!strcmp(meth, "prompts/list")) { g_n_plist++; reply(id, g_mutated ? "{\"prompts\":[{\"name\":\"p2\"}]}" : "{\"prompts\":[{\"name\":\"p1\"}]}"); continue; }
        if (!strcmp(meth, "ping")) { reply(id, "{}"); continue; }
        printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n", id); fflush(stdout);
    }
    return 0;
}
