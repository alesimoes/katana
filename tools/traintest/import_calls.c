/* as chamadas que o katana import escreve voltam pelos leitores do proprio agente, nos 4 formatos:
     gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/traintest/import_calls.c -o /tmp/import_calls && /tmp/import_calls */
#define main katana_main
#include "src/katana.c"
#undef main
int main(void) {
    int fails = 0;
    const char *args = "{\"path\": \"a b.py\", \"n\": 3, \"text\": \"linha 1\\nlinha \\\"2\\\"\"}";
    for (int f = 0; f < 4; f++) {
        Config c; memset(&c, 0, sizeof c);
        c.chat_fmt = f == 2 ? 8 : f == 3 ? 10 : 0; c.tool_xml = f == 1; c.glm_args = f == 3;
        Buf b = {0}; im_call(&c, &b, "Write", args);
        char name[128] = ""; static char js[8192]; int ok;
        const char *tc = b.p, *end = b.p + b.n;
        if (f == 2) ok = g4_call_parse(tc, end, name, sizeof name, js, sizeof js);
        else if (f == 3) ok = glm_call_parse(tc, end, name, sizeof name, js, sizeof js);
        else if (f == 1) ok = tool_xml_parse(tc, end, name, sizeof name, js, sizeof js);
        else { ok = json_str(tc, end, "name", name, sizeof name); snprintf(js, sizeof js, "%s", strstr(tc, "\"arguments\"")); }
        char path[64] = "", text[64] = "";
        json_str(js, js + strlen(js), "path", path, sizeof path);
        json_str(js, js + strlen(js), "text", text, sizeof text);
        const int good = ok && !strcmp(name, "Write") && !strcmp(path, "a b.py") && !strcmp(text, "linha 1\nlinha \"2\"");
        printf("formato %d: %s  name=%s path=%s text=%s\n", f, good ? "ok" : "FALHOU", name, path, text);
        if (!good) { printf("   %s\n   js=%s\n", b.p, js); fails++; }
        free(b.p);
    }
    return fails;
}
