/* ============================ render: conversor do codificador de texto do FLUX.2 klein ===============================
 * katana render flux-convert-text <pasta FLUX.2-klein-4B> <saida.gguf>
 *
 * O FLUX.2 klein le o prompt com um Qwen3 de 36 camadas (text_encoder/, pesos bf16, embedding amarrado) e usa os estados ocultos das
 * camadas 9, 18 e 27. Camadas acima da 27 nao entram na conta, entao o GGUF leva so o embedding e as camadas 0 a 26 (sem a norma final,
 * que so atua na ultima saida). Matrizes viram F16, normas F32 (a mesma politica do conversor do Stable). Os nomes dos tensores sao os
 * do checkpoint com o prefixo "txt." ("txt.model.layers.3.self_attn.q_proj.weight"). O tokenizador (BPE de bytes, pre-tokenizador
 * qwen2) vem do tokenizer.json, no formato tokenizer.ggml.* que o tokenizador do motor ja le. */

#define FXT_KEEP 27                                /* camadas guardadas: hidden_states[27] e a saida da camada de indice 26 */

static void fxt_kv_tokenizer(const char *dir, GBuf *kv, uint64_t *nkv) {
    char path[1200];
    JV tj; snprintf(path, sizeof path, "%s/tokenizer/tokenizer.json", dir);
    if (!cv_json_file(path, &tj)) { fprintf(stderr, "render: nao consegui ler %s\n", path); exit(1); }
    const JV *model = jget(&tj, "model"), *vocab = jget(model, "vocab"), *merges = jget(model, "merges"), *added = jget(&tj, "added_tokens");
    if (!model || !vocab || vocab->t != J_OBJ || !merges || merges->t != J_ARR) { fprintf(stderr, "render: tokenizer.json sem model.vocab e model.merges\n"); exit(1); }
    int64_t maxid = -1;
    for (size_t i = 0; i < vocab->n; i++) if (vocab->v[i].i > maxid) maxid = vocab->v[i].i;
    if (added) for (size_t i = 0; i < added->n; i++) { const int64_t id = (int64_t)jnum(&added->v[i], "id", -1); if (id > maxid) maxid = id; }
    const int V = (int)maxid + 1;
    char **toks = xalloc((size_t)V, sizeof(char *));
    int32_t *types = xalloc((size_t)V, 4);
    for (int i = 0; i < V; i++) types[i] = 5;
    for (size_t i = 0; i < vocab->n; i++) { const int64_t id = vocab->v[i].i; toks[id] = vocab->k[i]; types[id] = 1; }
    if (added) for (size_t i = 0; i < added->n; i++) {
        const JV *e = &added->v[i]; const int64_t id = (int64_t)jnum(e, "id", -1); const char *ct = jstr(e, "content");
        if (id < 0 || !ct) continue;
        toks[id] = (char *)ct;
        const JV *sp = jget(e, "special");
        types[id] = sp && sp->t == J_BOOL && sp->i ? 3 : 4;
    }
    for (int i = 0; i < V; i++) if (!toks[i]) { char tmp[32]; snprintf(tmp, sizeof tmp, "[PAD%d]", i); toks[i] = strdup(tmp); }
    gkv_str(kv, "tokenizer.ggml.model", "gpt2"); (*nkv)++;
    gkv_str(kv, "tokenizer.ggml.pre", "qwen2"); (*nkv)++;
    gbuf_gstr(kv, "tokenizer.ggml.tokens", strlen("tokenizer.ggml.tokens")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)V);
    for (int i = 0; i < V; i++) gbuf_gstr(kv, toks[i], strlen(toks[i]));
    (*nkv)++;
    gbuf_gstr(kv, "tokenizer.ggml.token_type", strlen("tokenizer.ggml.token_type")); gbuf_u32(kv, 9); gbuf_u32(kv, 5); gbuf_u64(kv, (uint64_t)V);
    gbuf_bytes(kv, types, (size_t)V * 4);
    (*nkv)++;
    gbuf_gstr(kv, "tokenizer.ggml.merges", strlen("tokenizer.ggml.merges")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)merges->n);
    for (size_t i = 0; i < merges->n; i++) {
        const JV *m = &merges->v[i];
        if (m->t == J_STR) gbuf_gstr(kv, m->s, m->slen);
        else if (m->t == J_ARR && m->n == 2 && m->v[0].t == J_STR && m->v[1].t == J_STR) {
            gbuf_u64(kv, m->v[0].slen + 1 + m->v[1].slen);
            gbuf_bytes(kv, m->v[0].s, m->v[0].slen); gbuf_bytes(kv, " ", 1); gbuf_bytes(kv, m->v[1].s, m->v[1].slen);
        } else { fprintf(stderr, "render: merge %zu invalido\n", i); exit(1); }
    }
    (*nkv)++;
    int pad = -1, eos = -1;
    for (int i = 0; i < V; i++) { if (!strcmp(toks[i], "<|endoftext|>")) pad = i; if (!strcmp(toks[i], "<|im_end|>")) eos = i; }
    if (pad < 0 || eos < 0) { fprintf(stderr, "render: o vocabulario nao tem <|endoftext|> e <|im_end|>\n"); exit(1); }
    gkv_u32(kv, "tokenizer.ggml.padding_token_id", (uint32_t)pad); (*nkv)++;      /* tokenizer_config.json: pad_token = <|endoftext|> */
    gkv_u32(kv, "tokenizer.ggml.eos_token_id", (uint32_t)eos); (*nkv)++;          /* eos_token = <|im_end|> */
    free(toks); free(types);
}

static int fxt_convert(const char *dir, const char *out_path) {
    char p[1200], q[1200];
    snprintf(p, sizeof p, "%s/text_encoder/config.json", dir);
    JV cj; if (!cv_json_file(p, &cj)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
    snprintf(p, sizeof p, "%s/text_encoder/model.safetensors.index.json", dir);
    JV ij; if (!cv_json_file(p, &ij)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
    const JV *wm = jget(&ij, "weight_map");
    if (!wm || wm->t != J_OBJ) { fprintf(stderr, "render: %s sem weight_map\n", p); return 1; }
    CvSrcSet src; memset(&src, 0, sizeof src);
    char *opened[8]; int nop = 0;
    for (size_t i = 0; i < wm->n; i++) {                                     /* abre cada arquivo citado no indice, uma vez */
        const char *f = wm->v[i].s; int seen = 0;
        for (int k = 0; k < nop; k++) if (!strcmp(opened[k], f)) seen = 1;
        if (seen) continue;
        if (nop >= 8) { fprintf(stderr, "render: arquivos demais no indice\n"); return 1; }
        snprintf(q, sizeof q, "%s/text_encoder/%s", dir, f);
        cv_open_shard(&src, q); opened[nop++] = (char *)f;
    }
    qsort(src.st, src.nst, sizeof(CvSt), cv_st_cmp);
    SdSet S; memset(&S, 0, sizeof S);
    int dropped = 0;
    for (size_t i = 0; i < src.nst; i++) {
        const CvSt *t = &src.st[i];
        int layer = -1;
        if (!strncmp(t->name, "model.layers.", 13)) layer = atoi(t->name + 13);
        const int keep = !strcmp(t->name, "model.embed_tokens.weight") || (layer >= 0 && layer < FXT_KEEP);
        if (!keep) { dropped++; continue; }
        sd_add(&S, "txt.", t->name, &src, t, 0);
    }
    qsort(S.t, (size_t)S.n, sizeof(SdT), sd_t_cmp);
    printf("katana render flux-convert-text: %s\n  %d tensores guardados (embedding + camadas 0 a %d), %d descartados (camadas acima e norma final)\n", dir, S.n, FXT_KEEP - 1, dropped);
    GBuf kv = {0}; uint64_t nkv = 0;
    gkv_str(&kv, "general.architecture", "fluxtext"); nkv++;
    gkv_str(&kv, "general.type", "model"); nkv++;
    gkv_str(&kv, "general.name", "FLUX.2 klein text encoder (Qwen3-4B, camadas 0-26)"); nkv++;
    gkv_str(&kv, "general.quantized_by", "katana"); nkv++;
#define KU(key, name, def) do { gkv_u32(&kv, key, (uint32_t)jnum(&cj, name, def)); nkv++; } while (0)
    KU("fluxtext.hidden_size", "hidden_size", 2560); KU("fluxtext.intermediate_size", "intermediate_size", 9728);
    KU("fluxtext.num_attention_heads", "num_attention_heads", 32); KU("fluxtext.num_key_value_heads", "num_key_value_heads", 8);
    KU("fluxtext.head_dim", "head_dim", 128); KU("fluxtext.vocab_size", "vocab_size", 151936);
#undef KU
    gkv_u32(&kv, "fluxtext.num_layers", FXT_KEEP); nkv++;
    cv_kv_f32(&kv, "fluxtext.rms_eps", (float)jnum(&cj, "rms_norm_eps", 1e-6)); nkv++;
    cv_kv_f32(&kv, "fluxtext.rope_theta", (float)jnum(&cj, "rope_theta", 1000000.0)); nkv++;
    fxt_kv_tokenizer(dir, &kv, &nkv);
    return sd_write(out_path, &S, &kv, nkv);
}
