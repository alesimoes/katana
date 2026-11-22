/* visao: testes de unidade e de estado com um modelo minusculo (tools/visiontest/run.sh monta os arquivos)
     unit MODELO.gguf PROJETOR.gguf IMAGEM.png IMAGEM2.png PASTA */
#define main katana_main
#include "src/katana.c"
#undef main
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static char *b64(const uint8_t *b, size_t n) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = xalloc(n / 3 * 4 + 8, 1), *p = o;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        *p++ = A[v >> 18]; *p++ = A[v >> 12 & 63]; *p++ = i + 1 < n ? A[v >> 6 & 63] : '='; *p++ = i + 2 < n ? A[v & 63] : '=';
    }
    *p = 0;
    return o;
}
static double lg_diff(const float *a, const float *b, int n) { double m = 0; for (int i = 0; i < n; i++) if (fabs(a[i] - b[i]) > m) m = fabs(a[i] - b[i]); return m; }
int main(int argc, char **argv) {
    if (argc < 6) return 2;
    pool_init(); vnni_init();
    char err[300], mk[80], mk2[80];
    setenv("KATANA_MMPROJ", argv[2], 1); setenv("KATANA_IMAGE_TOKENS", "64", 1); setenv("KATANA_CACHE_DIR", argv[5], 1); setenv("HOME", argv[5], 1);
    Model m; memset(&m, 0, sizeof m);
    load_model(&m, argv[1]);
    size_t fn; uint8_t *file = vis_slurp(argv[3], &fn, err, sizeof err);
    char *B = b64(file, fn);

    /* sem visao: tudo como antes */
    { static char out[4000]; char js[1200]; snprintf(js, sizeof js, "\"arguments\": {\"file_path\": \"%s\"}", argv[3]);
      ct_read(js, js + strlen(js), out, sizeof out);
      CHECK(strstr(out, "no vision input"), "Read without vision: %s", out);
      McpB t = {0}; mcp_bput(&t, "", 0); mcp_block("{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"QUJD\"}", &t);
      CHECK(!strcmp(t.p, "[image image/png, 3 bytes: binary content not shown]"), "MCP without vision: %s", t.p); free(t.p); }

    g_vis.store = 1;
    vis_init(&m, argv[1]);
    CHECK(g_vis.on, "vision on");
    { static char out[4000], safe[8000]; char js[1200]; snprintf(js, sizeof js, "\"arguments\": {\"file_path\": \"%s\"}", argv[3]);
      agent_tool("Read", js, js + strlen(js), out, sizeof out);
      CHECK(strstr(out, VIS_TAG) && strstr(out, "tokens]"), "Read with vision: %s", out);
      defang(out, safe, sizeof safe);
      CHECK(strstr(safe, VIS_TAG), "the image of the tool result passes: %s", safe);
      const char *j2 = "\"arguments\": {\"file_path\": \"/nonexistent/x.txt\"}";
      agent_tool("Read", j2, j2 + strlen(j2), out, sizeof out);
      defang(strstr(safe, VIS_TAG), out, sizeof out);
      CHECK(!strstr(out, VIS_TAG) && strstr(out, "< |katana_image"), "a mark inside text of another tool is neutralised: %s", out); }
    { McpB t = {0}; mcp_bput(&t, "", 0); Buf j = {0}; buf_str(&j, "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\""); buf_str(&j, B); buf_str(&j, "\"}");
      mcp_block(j.p, &t);
      CHECK(strstr(t.p, VIS_TAG) && g_vis.n == 1, "MCP image, same slot: %s", t.p); free(t.p);
      McpB u = {0}; mcp_bput(&u, "", 0); mcp_block("{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"iVBORw0KGgoAAAA!\"}", &u);
      CHECK(strstr(u.p, "not shown"), "bad MCP image: %s", u.p); free(u.p); free(j.p); }
    { OaReq *r = xalloc(1, sizeof *r); Buf j = {0};
      buf_str(&j, "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"look\"},{\"type\":\"image_url\",\"image_url\":{\"detail\":\"low\",\"url\":\"data:image/png;base64,"); buf_str(&j, B); buf_str(&j, "\"}},{\"type\":\"text\",\"text\":\"what\"}]}]}");
      const char *e = oa_parse(j.p, r);
      CHECK(!e && r->n == 1 && strstr(r->msg[0].content, VIS_TAG) && strstr(r->msg[0].content, "look") && strstr(r->msg[0].content, "what"), "image_url: %s", e ? e : r->msg[0].content);
      oa_free(r); memset(r, 0, sizeof *r);
      e = oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"http://x/y.png\"}}]}]}", r);
      CHECK(e && strstr(e, "data:"), "remote image refused: %s", e ? e : "accepted");
      oa_free(r); memset(r, 0, sizeof *r);
      e = oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}}]}]}", r);
      CHECK(e, "bad image refused");
      oa_free(r); memset(r, 0, sizeof *r);
      e = oa_parse("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,iVBO", r);
      CHECK(e || r->n == 0, "cut request refused");
      oa_free(r); free(r); free(j.p); }
    vis_mark(0, mk, sizeof mk);
    const int k2 = vis_add_file(argv[4], err, sizeof err);
    CHECK(k2 == 1, "second image");
    vis_mark(1, mk2, sizeof mk2);
    CHECK(vis_add_file(argv[3], err, sizeof err) == 0, "same file, same slot");

    /* marca -> ids; texto com marca desconhecida fica texto */
    Ids a = {0}, b = {0}; char t[600];
    snprintf(t, sizeof t, "the %s hello", mk); encode(&m.tk, t, &a);
    int nv = 0; for (int i = 0; i < a.n; i++) nv += a.ids[i] >= VIS_BASE;
    CHECK(nv == g_vis.v[0].nt, "image ids: %d of %d", nv, g_vis.v[0].nt);
    snprintf(t, sizeof t, "the %.*s%s hello", VIS_TAG_N, VIS_TAG, "00000000000000000000000000000000|>"); encode(&m.tk, t, &b);
    nv = 0; for (int i = 0; i < b.n; i++) nv += b.ids[i] >= VIS_BASE;
    CHECK(nv == 0, "unknown image stays text");
    /* identidade em disco: pelo conteudo, nao pelo lugar */
    const uint64_t h0 = vis_ids_hash(1, a.ids, a.n);
    VisImg x = g_vis.v[0]; g_vis.v[0] = g_vis.v[1]; g_vis.v[1] = x;
    Ids a2 = {0}; snprintf(t, sizeof t, "the %s hello", mk); encode(&m.tk, t, &a2);
    CHECK(a2.n == a.n && memcmp(a2.ids, a.ids, sizeof(int) * (size_t)a.n) && vis_ids_hash(1, a2.ids, a2.n) == h0, "the disk key follows the content, not the slot");
    x = g_vis.v[0]; g_vis.v[0] = g_vis.v[1]; g_vis.v[1] = x;
    a2.n = 0; snprintf(t, sizeof t, "the %s hello", mk2); encode(&m.tk, t, &a2);
    CHECK(a2.n == a.n && vis_ids_hash(1, a2.ids, a2.n) != h0, "another image, another key");
    /* compactacao: a imagem pesa pelos tokens */
    CHECK(vis_weight(mk, mk + strlen(mk)) == (size_t)g_vis.v[0].nt * 4, "weight");
    /* guardada: outro processo acha pelo hash */
    { VisImg *sv = g_vis.v; const int sn = g_vis.n; g_vis.v = NULL; g_vis.n = 0; Ids c = {0};
      snprintf(t, sizeof t, "%s", mk); encode(&m.tk, t, &c); CHECK(c.n == a.n - (a.n - sv[0].nt - 2) && g_vis.n == 1, "resumed session finds the stored file");
      free(g_vis.v); g_vis.v = sv; g_vis.n = sn; free(c.ids); }

    /* estado: prefixo com imagem gravado, outra conversa por cima, carregado e continuado = sem interrupcao */
    const int V = m.cfg.vocab;
    float *ref = xalloc((size_t)V, 4);
    char *full = xalloc(2000, 1);
    snprintf(full, 2000, "the hello %s other in the end and an ou", mk);
    Ids f = {0}; encode(&m.tk, full, &f);
    const int cut = f.n - 5;
    memcpy(ref, forward_seq(&m, f.ids, f.n, 0, 1), (size_t)V * 4);
    forward_seq(&m, f.ids, cut, 0, 0);
    memcpy(ref, forward_seq(&m, f.ids + cut, f.n - cut, cut, 1), (size_t)V * 4);
    int ngl, esz, be;
    if (pfx_geom(&m, &ngl, &esz, &be)) {
        forward_seq(&m, f.ids, cut, 0, 0);
        const uint64_t key = pfx_key(&m, f.ids, cut, ngl, esz, be);
        CHECK(pfx_save(&m, key, cut, ngl, esz, be), "prefix saved");
        Ids o = {0}; snprintf(full, 2000, "ou an and %s%s in the", mk2, mk2); encode(&m.tk, full, &o);
        forward_seq(&m, o.ids, o.n, 0, 0);
        CHECK(pfx_load(&m, key, cut, ngl, esz, be), "prefix loaded"); vis_seq(&m, f.ids, cut, 0);
        const double d = lg_diff(ref, forward_seq(&m, f.ids + cut, f.n - cut, cut, 1), V);
        CHECK(d < 1e-4, "prefix with an image restored: %.2e", d);
        /* prefixo de texto restaurado por cima de uma conversa com imagem: posicoes de texto */
        Ids p = {0}; encode(&m.tk, "the hello and other in the end and then an ou in the hello", &p);
        forward_seq(&m, o.ids, o.n, 0, 0);
        memcpy(ref, forward_seq(&m, p.ids, p.n, 0, 1), (size_t)V * 4);
        for (int i = 0; i < p.n; i++) CHECK(vis_rp[3 * i] == i && vis_rp[3 * i + 2] == i, "text position %d is %d", i, vis_rp[3 * i]);
        int32_t *keep = vis_rp; vis_rp = NULL;      /* como se nunca tivesse havido imagem */
        const double d2 = lg_diff(ref, forward_seq(&m, p.ids, p.n, 0, 1), V);
        vis_rp = keep;
        CHECK(d2 == 0, "text after image conversations is bit identical: %.2e", d2);
    }
    CHECK(g_vis.nemb > 0, "images encoded");
    printf("vision unit: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
