/* testes de unidade da API OpenAI (openai.c), sem modelo:
     gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/servertest/oa_unit.c -o /tmp/oa_unit -lm -lpthread -ldl && /tmp/oa_unit */
#define main katana_main
#include "src/katana.c"
#include <sys/un.h>
#undef main
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void feed_all(OaOut *o, const char **parts) { for (int i = 0; parts[i]; i++) oa_feed(o, parts[i], strlen(parts[i]), 0); oa_feed(o, "", 0, 1); }
int main(void) {
    { /* raciocinio separado, </think> partido entre pedacos */
        OaOut o = {0}; o.alive = 1; o.think = 1;
        const char *p[] = {"let me ", "think</th", "ink>\n\nThe ans", "wer is 4.", NULL};
        feed_all(&o, p);
        CHECK(o.reason.p && !strcmp(o.reason.p, "let me think"), "reason=[%s]", o.reason.p);
        CHECK(o.content.p && !strcmp(o.content.p, "The answer is 4."), "content=[%s]", o.content.p);
    }
    { /* quebras depois do </think> chegando em pedacos separados nao entram na resposta */
        OaOut o = {0}; o.alive = 1; o.think = 1;
        const char *p[] = {"\n</think>", "\n", "\n", "Hi", " there", NULL};
        feed_all(&o, p);
        CHECK(o.content.p && !strcmp(o.content.p, "Hi there"), "lstrip content=[%s]", o.content.p);
    }
    { /* stop partido entre pedacos, nada depois dele sai */
        OaOut o = {0}; o.alive = 1; char *st[] = {"END", "zz"}; o.stop = st; o.nstop = 2; o.maxstop = 3;
        const char *p[] = {"abc E", "N", "Dxyz", NULL};
        feed_all(&o, p);
        CHECK(o.content.p && !strcmp(o.content.p, "abc "), "stop content=[%s]", o.content.p);
        CHECK(o.stopped, "not stopped");
    }
    { /* comeco de stop que nao vira stop sai inteiro no fim */
        OaOut o = {0}; o.alive = 1; char *st[] = {"END"}; o.stop = st; o.nstop = 1; o.maxstop = 3;
        const char *p[] = {"abc E", "N", NULL};
        feed_all(&o, p);
        CHECK(o.content.p && !strcmp(o.content.p, "abc EN"), "partial=[%s]", o.content.p);
        CHECK(!o.stopped, "stopped wrongly");
    }
    { /* parser: partes de texto, papeis, stop string, erros */
        OaReq *r = xalloc(1, sizeof *r);
        const char *e = oa_parse("{\"model\":\"m\",\"messages\":[{\"role\":\"developer\",\"content\":\"S\"},{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"text\",\"text\":\"b\"}]},{\"role\":\"assistant\",\"content\":\"x\"},{\"role\":\"user\",\"content\":\"q \\u00e9\"}],\"stop\":\"##\",\"max_completion_tokens\":7,\"stream\":true}", r);
        CHECK(!e, "parse err %s", e);
        CHECK(r->n == 4 && r->msg[0].role == 's' && !strcmp(r->msg[1].content, "a\nb") && r->msg[2].role == 'a', "msgs");
        CHECK(!strcmp(r->msg[3].content, "q \xc3\xa9"), "unicode [%s]", r->msg[3].content);
        CHECK(r->nstop == 1 && !strcmp(r->stop[0], "##") && r->max_tokens == 7 && r->stream && !strcmp(r->model, "m"), "fields");
        oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"x\"}}]}]}", r) != NULL, "image accepted");
        oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"type\":\"function\"}]}", r) != NULL, "tools accepted");
        oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[]}", r) == NULL, "empty tools rejected");
        oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}", r) != NULL, "broken json accepted");
        oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"n\":2}", r) != NULL, "n=2 accepted");
        oa_free(r); free(r);
    }
    { /* prompt ChatML com raciocinio: historico ganha bloco vazio, ultimo turno abre <think> */
        Config c; memset(&c, 0, sizeof c); c.chat_fmt = 0; c.think = 1;
        OaReq *r = xalloc(1, sizeof *r);
        oa_parse("{\"messages\":[{\"role\":\"system\",\"content\":\"S\"},{\"role\":\"user\",\"content\":\"u1\"},{\"role\":\"assistant\",\"content\":\"a1\"},{\"role\":\"user\",\"content\":\"u2\"}]}", r);
        Buf t = {0};
        const char *e = oa_prompt(&c, r, &t);
        const char *want = "<|im_start|>system\nS<|im_end|>\n<|im_start|>user\nu1<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\na1<|im_end|>\n<|im_start|>user\nu2<|im_end|>\n<|im_start|>assistant\n<think>\n";
        CHECK(!e && t.p && !strcmp(t.p, want), "prompt:\n%s", t.p);
        free(t.p); oa_free(r); memset(r, 0, sizeof *r);
        oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"u\"},{\"role\":\"assistant\",\"content\":\"a\"}]}", r);
        t.p = NULL; t.n = 0;
        CHECK(oa_prompt(&c, r, &t) != NULL, "assistant-last accepted");
        free(t.p); oa_free(r); free(r);
    }
    { /* escape: UTF-8 invalido vira U+FFFD, valido passa */
        Buf b = {0}; js_escape(&b, "a\xff" "b\xc3\xa9\xe2\x82", 7);
        CHECK(b.p && !strcmp(b.p, "a\\ufffdb\xc3\xa9\\ufffd\\ufffd"), "escape [%s]", b.p);
        free(b.p);
    }
    { /* Gemma 4: canal de pensamento aberto pela resposta, marcas partidas entre pedacos */
        OaOut o = {0}; o.alive = 1; o.g4start = 1;
        const char *p[] = {"<|chan", "nel>thou", "ght\nplan", "ning<chan", "nel|>Answer", " here", NULL};
        feed_all(&o, p);
        CHECK(o.reason.p && !strcmp(o.reason.p, "planning"), "g4 reason=[%s]", o.reason.p);
        CHECK(o.content.p && !strcmp(o.content.p, "Answer here"), "g4 content=[%s]", o.content.p);
    }
    { /* Gemma 4: resposta sem canal sai inteira; e comecando dentro do canal (depois de um resultado) */
        OaOut o = {0}; o.alive = 1; o.g4start = 1;
        const char *p[] = {"<", "b>hi", NULL};
        feed_all(&o, p);
        CHECK(o.content.p && !strcmp(o.content.p, "<b>hi") && !o.reason.n, "g4 plain=[%s]", o.content.p);
        OaOut q = {0}; q.alive = 1; q.think = 1; q.tclose = "<channel|>";
        const char *p2[] = {"why", "<channel", "|>\n\nok", NULL};
        feed_all(&q, p2);
        CHECK(q.reason.p && !strcmp(q.reason.p, "why") && q.content.p && !strcmp(q.content.p, "ok"), "g4 inside=[%s][%s]", q.reason.p, q.content.p);
    }
    { /* abertura de chamada partida: o texto antes fica em content, o resto vai para calls */
        OaOut o = {0}; o.alive = 1; o.calls_on = 1;
        const char *p[] = {"Sure.\n\n<tool_ca", "ll>\n<function=f>\n</function>\n</tool_call>", NULL};
        feed_all(&o, p);
        CHECK(o.content.p && !strcmp(o.content.p, "Sure.\n\n"), "calls content=[%s]", o.content.p);
        CHECK(o.in_calls && o.calls.p && !strncmp(o.calls.p, "<tool_call>\n<function=f>", 24), "calls=[%s]", o.calls.p);
        OaOut q = {0}; q.alive = 1; q.calls_on = 1;
        const char *p2[] = {"a <tool", " b", NULL};
        feed_all(&q, p2);
        CHECK(q.content.p && !strcmp(q.content.p, "a <tool b") && !q.in_calls, "no call=[%s]", q.content.p);
    }
    { /* stream: os deltas de content nao levam o texto da chamada */
        int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        OaOut o = {0}; o.alive = 1; o.calls_on = 1; o.stream = 1; o.s = sv[0]; o.id = "x"; o.model = "m";
        const char *p[] = {"Hi", " <|tool_call>call:f{}<tool_call|>", NULL};
        feed_all(&o, p);
        close(sv[0]);
        char rb[4096]; ssize_t k = read(sv[1], rb, sizeof rb - 1); rb[k > 0 ? k : 0] = 0; close(sv[1]);
        CHECK(strstr(rb, "\"delta\":{\"content\":\"Hi \"}") && !strstr(rb, "call:f"), "stream=[%s]", rb);
    }
    {   /* ferramentas: parser, validacao e renderizacao em cada formato */
        const char *tools = "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"description\":\"Weather now\","
            "\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"},\"days\":{\"type\":\"integer\"},\"metric\":{\"type\":\"boolean\"},"
            "\"ratio\":{\"type\":\"number\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"opt\":{\"type\":\"object\"},\"maybe\":{\"type\":[\"integer\",\"null\"]}},\"required\":[\"city\"]}}}]";
        char req[4096];
        snprintf(req, sizeof req, "{\"messages\":[{\"role\":\"system\",\"content\":\"S\"},{\"role\":\"user\",\"content\":\"u1\"},"
            "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"city\\\": \\\"Rio\\\", \\\"days\\\": 2}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"sunny\"}],%s}", tools);
        OaReq *r = xalloc(1, sizeof *r);
        const char *e = oa_parse(req, r);
        CHECK(!e && r->ntools == 1 && r->n == 4 && r->msg[3].role == 't' && r->msg[2].calls, "tools parse %s", e);
        struct { int fmt, xml, glm; const char *must[6]; } F[] = {
            {0, 1, 0, {"<|im_start|>system\nS\n\n# Tools\n", "{\"type\": \"function\", \"function\": {\"name\": \"get_weather\", \"description\": \"Weather now\"",
                       "<|im_start|>assistant\n<tool_call>\n<function=get_weather>\n<parameter=city>\nRio\n</parameter>\n<parameter=days>\n2\n</parameter>\n</function>\n</tool_call><|im_end|>\n",
                       "<|im_start|>user\n<tool_response>\n[resultado da ferramenta get_weather", "</tool_response><|im_end|>\n<|im_start|>assistant\n", "<function=example_function_name>"}},
            {0, 0, 0, {"<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>",
                       "<|im_start|>assistant\n<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"city\": \"Rio\", \"days\": 2}}\n</tool_call><|im_end|>", "<|im_start|>user\n<tool_response>\n", NULL}},
            {8, 0, 0, {"<|turn>system\nS<|tool>declaration:get_weather{description:<|\"|>Weather now<|\"|>", "<|turn>user\nu1<turn|>\n<|turn>model\n<|channel>thought\n<channel|><|tool_call>call:get_weather{city:<|\"|>Rio<|\"|>,days:2}<tool_call|>",
                       "<|tool_response>response:get_weather{value:<|\"|>[resultado da ferramenta get_weather", "sunny\n[fim do resultado da ferramenta get_weather]<|\"|>}<tool_response|>", NULL}},
            {10, 0, 1, {"[gMASK]<sop><|system|>S\n\n# Tools", "<arg_key>{arg-key-1}</arg_key>", "<|assistant|><think></think>\n<tool_call>get_weather<arg_key>city</arg_key><arg_value>Rio</arg_value><arg_key>days</arg_key><arg_value>2</arg_value></tool_call>",
                        "<|observation|><tool_response>\n[resultado", "</tool_response><|assistant|><think>", NULL}},
        };
        for (int f = 0; f < 4; f++) {
            Config c; memset(&c, 0, sizeof c); c.chat_fmt = F[f].fmt; c.tool_xml = F[f].xml; c.glm_args = F[f].glm; c.think = F[f].glm;
            Buf t = {0}, fo = {0}; int th = -1;
            const char *e2 = oa_prompt_tools(&c, r, &t, &fo, &th);
            int ok = !e2 && t.p && !fo.n;
            for (int k = 0; k < 6 && F[f].must[k]; k++) if (!t.p || !strstr(t.p, F[f].must[k])) { ok = 0; printf("  missing [%s]\n", F[f].must[k]); }
            CHECK(ok, "render fmt %d (err %s):\n%s", f, e2, t.p);
            CHECK(th == (F[f].glm ? 1 : 0), "think start fmt %d = %d", f, th);
            free(t.p); free(fo.p);
        }
        r->choice = 3; snprintf(r->choice_name, sizeof r->choice_name, "get_weather");
        const char *want[4] = {"<tool_call>\n<function=get_weather>\n", "<tool_call>\n{\"name\": \"get_weather\", \"arguments\": ", "<|tool_call>call:get_weather{", "</think><tool_call>get_weather"};
        for (int f = 0; f < 4; f++) {
            Config c; memset(&c, 0, sizeof c); c.chat_fmt = F[f].fmt; c.tool_xml = F[f].xml; c.glm_args = F[f].glm; c.think = F[f].glm;
            Buf t = {0}, fo = {0}; int th = -1;
            oa_prompt_tools(&c, r, &t, &fo, &th);
            const size_t wl = strlen(want[f]);
            CHECK(t.p && t.n >= wl && !strcmp(t.p + t.n - wl, want[f]) && th == 0 && fo.n, "forced fmt %d:\n[%s]", f, t.p ? t.p + (t.n > 200 ? t.n - 200 : 0) : "");
            free(t.p); free(fo.p);
        }
        r->choice = 1;                                 /* none: sem declaracoes */
        { Config c; memset(&c, 0, sizeof c); c.tool_xml = 1; Buf t = {0}, fo = {0}; int th; oa_prompt_tools(&c, r, &t, &fo, &th);
          CHECK(t.p && !strstr(t.p, "# Tools") && strstr(t.p, "<function=get_weather>"), "none declares:\n%s", t.p); free(t.p); free(fo.p); }
        r->choice = 0;
        /* leitura das chamadas e argumentos tipados, em cada formato */
        const char *raw[4] = {
            "<tool_call>\n<function=get_weather>\n<parameter=city>\nSao \"Paulo\"\n</parameter>\n<parameter=days>\n 3 \n</parameter>\n<parameter=metric>\ntrue\n</parameter>\n<parameter=ratio>\n-1.5e2\n</parameter>\n"
            "<parameter=tags>\n[\"a\", \"b\"]\n</parameter>\n<parameter=opt>\n{\"k\": 1}\n</parameter>\n<parameter=maybe>\nnull\n</parameter>\n</function>\n</tool_call>\n<tool_call>\n<function=get_weather>\n<parameter=days>\nmany\n</parameter>\n</function>\n</tool_call>",
            "<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"city\": \"Rio\", \"days\": \"4\"}}\n</tool_call>",
            "<|tool_call>call:get_weather{city:<|\"|>Rio<|\"|>,days:5,tags:[<|\"|>x<|\"|>]}<tool_call|>",
            "<tool_call>get_weather<arg_key>city</arg_key><arg_value>Rio</arg_value><arg_key>days</arg_key><arg_value>6</arg_value><arg_key>metric</arg_key><arg_value>False</arg_value></tool_call>"};
        const char *wargs[4] = {"{\"city\": \"Sao \\\"Paulo\\\"\", \"days\": 3, \"metric\": true, \"ratio\": -1.5e2, \"tags\": [\"a\", \"b\"], \"opt\": {\"k\": 1}, \"maybe\": null}",
                                "{\"city\": \"Rio\", \"days\": 4}", "{\"city\": \"Rio\", \"days\": 5, \"tags\": [\"x\"]}", "{\"city\": \"Rio\", \"days\": 6, \"metric\": false}"};
        for (int f = 0; f < 4; f++) {
            Config c; memset(&c, 0, sizeof c); c.chat_fmt = F[f].fmt; c.tool_xml = F[f].xml; c.glm_args = F[f].glm;
            OaCall cl[4]; const int n = oa_calls_parse(&c, r, raw[f], cl, 4);
            CHECK(n >= 1 && !strcmp(cl[0].name, "get_weather") && !strcmp(cl[0].args, wargs[f]), "calls fmt %d n=%d args=[%s]", f, n, n ? cl[0].args : "");
            if (f == 0) CHECK(n == 2 && !strcmp(cl[1].args, "{\"days\": \"many\"}"), "untyped kept as string [%s]", n > 1 ? cl[1].args : "");
            for (int i = 0; i < n; i++) free(cl[i].args);
        }
        oa_free(r); memset(r, 0, sizeof *r);
        /* erros de validacao */
        CHECK(oa_parse("{\"messages\":[{\"role\":\"robot\",\"content\":\"a\"}]}", r) != NULL, "unknown role accepted"); oa_free(r); memset(r, 0, sizeof *r);
        e = oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"},{\"role\":\"tool\",\"tool_call_id\":\"zz\",\"content\":\"r\"}]}", r);
        CHECK(e && strstr(e, "tool_call_id"), "tool id unmatched: %s", e); oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"bad name\"}}]}", r) != NULL, "bad name accepted"); oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}],\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"g\"}}}", r) != NULL, "unknown choice accepted"); oa_free(r); memset(r, 0, sizeof *r);
        CHECK(oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\"}}],\"tool_choice\":\"required\",\"parallel_tool_calls\":false}", r) == NULL && r->choice == 2 && !r->parallel, "required"); oa_free(r); free(r);
    }
    printf(fails ? "%d FAIL\n" : "all ok\n", fails);
    return fails != 0;
}
