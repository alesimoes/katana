/* testes de unidade da API OpenAI (openai.c), sem modelo:
     gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/servertest/oa_unit.c -o /tmp/oa_unit -lm -lpthread -ldl && /tmp/oa_unit */
#define main katana_main
#include "src/katana.c"
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
        oa_parse("{\"messages\":[{\"role\":\"system\",\"content\":\"S\"},{\"role\":\"user\",\"content\":\"u1\"},{\"role\":\"assistant\",\"content\":\"a1\"},{\"role\":\"user\",\"content\":\"u2\"},{\"role\":\"tool\",\"content\":\"t\"}]}", r);
        Buf t = {0};
        const char *e = oa_prompt(&c, r, &t);
        const char *want = "<|im_start|>system\nS<|im_end|>\n<|im_start|>user\nu1<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\na1<|im_end|>\n<|im_start|>user\nu2\n\nt<|im_end|>\n<|im_start|>assistant\n<think>\n";
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
    printf(fails ? "%d FAIL\n" : "all ok\n", fails);
    return fails != 0;
}
