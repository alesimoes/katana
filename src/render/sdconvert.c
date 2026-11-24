/* ============================ katana render convert ===============================
 * katana render convert <pasta> <base.gguf> <controle.gguf>
 * katana render names   <pasta>
 *
 * Converte os pesos do Stable Diffusion 1.5 para GGUF (docs/render.md, etapa 2). <pasta> tem as tres pastas baixadas:
 *   realistic-vision/   um *.safetensors no formato original (LDM): UNet, codificador de texto CLIP e VAE juntos, mais
 *                       tokenizer/{vocab.json,merges.txt,tokenizer_config.json}, unet/config.json, text_encoder/config.json,
 *                       scheduler/scheduler_config.json
 *   vae-ft-mse/         o VAE recomendado (formato diffusers), diffusion_pytorch_model.safetensors e config.json
 *   controlnet-canny/   o ControlNet (formato diffusers), um *.safetensors e config.json
 * Saidas: base.gguf (arquitetura "sd15": UNet, CLIP, VAE e tokenizador) e controle.gguf (arquitetura "sd15controlnet").
 *
 * Nomes: todos no formato do diffusers, com um prefixo por parte: unet., clip., vae., ctrl.. A UNet vem no formato original
 * (input_blocks, middle_block, output_blocks) e e renomeada aqui; o ControlNet e o VAE ja vem no formato do diffusers (o VAE
 * antigo chama a atencao do meio de query/key/value/proj_attn: vira to_q/to_k/to_v/to_out.0). O VAE do checkpoint (first_stage_model)
 * nao e usado: vale o vae-ft-mse, que e o que o README do Realistic Vision recomenda.
 *   UNet:  time_embed.0/2 -> time_embedding.linear_1/2; input_blocks.0.0 -> conv_in; out.0/2 -> conv_norm_out/conv_out;
 *          input_blocks.N (N >= 1): bloco (N-1)/3, camada (N-1)%3; sub 0 = resnet (ou op = downsampler), sub 1 = atencao;
 *          middle_block.0/1/2 -> mid_block.resnets.0 / attentions.0 / resnets.1;
 *          output_blocks.N: bloco N/3, camada N%3; sub 0 = resnet, sub 1 = atencao ou conv (upsampler), sub 2 = conv.
 *          Dentro de um resnet: in_layers.0/2 -> norm1/conv1, out_layers.0/3 -> norm2/conv2, emb_layers.1 -> time_emb_proj,
 *          skip_connection -> conv_shortcut.
 * Tipos: pesos de 2 ou mais dimensoes em F16 (UNet, CLIP, ControlNet) e F32 no VAE (o VAE e pequeno e sensivel); vetores
 * (vies, normas) em F32. Dimensoes do GGUF = as do safetensors na ordem inversa (ne0 contiguo): uma convolucao
 * [saida, entrada, kh, kw] vira [kw, kh, entrada, saida]. Nada e transposto. */

typedef struct { char name[160]; const uint8_t *p; int dt; int nd; uint64_t shape[8]; uint32_t otype; uint64_t nbytes, off; } SdT;
typedef struct { SdT *t; int n, cap; } SdSet;

static int sd_pfx(const char *s, const char *p, const char **rest) {
    const size_t l = strlen(p);
    if (strncmp(s, p, l) != 0) return 0;
    *rest = s + l;
    return 1;
}

/* dentro de um resnet da UNet: nomes do formato original -> diffusers */
static void sd_rn(const char *r, char *out, size_t cap) {
    static const char *const m[][2] = { { "in_layers.0.", "norm1." }, { "in_layers.2.", "conv1." }, { "out_layers.0.", "norm2." },
                                        { "out_layers.3.", "conv2." }, { "emb_layers.1.", "time_emb_proj." }, { "skip_connection.", "conv_shortcut." } };
    for (int i = 0; i < 6; i++) {
        const char *q;
        if (sd_pfx(r, m[i][0], &q)) { snprintf(out, cap, "%s%s", m[i][1], q); return; }
    }
    snprintf(out, cap, "%s", r);
}

/* UNet do formato original -> diffusers (sem prefixo). 0 = o nome nao e da UNet ou nao tem regra */
static int sd_unet_name(const char *name, char *out, size_t cap) {
    const char *r, *q; char *e; char rn[160];
    if (!sd_pfx(name, "model.diffusion_model.", &r)) return 0;
    if (sd_pfx(r, "time_embed.0.", &q)) { snprintf(out, cap, "time_embedding.linear_1.%s", q); return 1; }
    if (sd_pfx(r, "time_embed.2.", &q)) { snprintf(out, cap, "time_embedding.linear_2.%s", q); return 1; }
    if (sd_pfx(r, "input_blocks.0.0.", &q)) { snprintf(out, cap, "conv_in.%s", q); return 1; }
    if (sd_pfx(r, "out.0.", &q)) { snprintf(out, cap, "conv_norm_out.%s", q); return 1; }
    if (sd_pfx(r, "out.2.", &q)) { snprintf(out, cap, "conv_out.%s", q); return 1; }
    if (sd_pfx(r, "middle_block.", &q)) {
        const long s = strtol(q, &e, 10);
        if (*e != '.' || s < 0 || s > 2) return 0;
        q = e + 1;
        if (s == 1) snprintf(out, cap, "mid_block.attentions.0.%s", q);
        else { sd_rn(q, rn, sizeof rn); snprintf(out, cap, "mid_block.resnets.%d.%s", s == 0 ? 0 : 1, rn); }
        return 1;
    }
    const int down = sd_pfx(r, "input_blocks.", &q), up = !down && sd_pfx(r, "output_blocks.", &q);
    if (!down && !up) return 0;
    const long n = strtol(q, &e, 10);
    if (*e != '.' || n < 0) return 0;
    q = e + 1;
    const long s = strtol(q, &e, 10);
    if (*e != '.' || s < 0 || s > 2) return 0;
    const char *rest = e + 1;
    if (down) {
        if (n < 1) return 0;
        const long bid = (n - 1) / 3, lid = (n - 1) % 3;
        if (s == 0 && sd_pfx(rest, "op.", &q)) { snprintf(out, cap, "down_blocks.%ld.downsamplers.0.conv.%s", bid, q); return 1; }
        if (s == 0) { sd_rn(rest, rn, sizeof rn); snprintf(out, cap, "down_blocks.%ld.resnets.%ld.%s", bid, lid, rn); return 1; }
        if (s == 1) { snprintf(out, cap, "down_blocks.%ld.attentions.%ld.%s", bid, lid, rest); return 1; }
        return 0;
    }
    const long bid = n / 3, lid = n % 3;
    if (s == 0) { sd_rn(rest, rn, sizeof rn); snprintf(out, cap, "up_blocks.%ld.resnets.%ld.%s", bid, lid, rn); return 1; }
    if (sd_pfx(rest, "conv.", &q)) { snprintf(out, cap, "up_blocks.%ld.upsamplers.0.conv.%s", bid, q); return 1; }
    if (s == 1) { snprintf(out, cap, "up_blocks.%ld.attentions.%ld.%s", bid, lid, rest); return 1; }
    return 0;
}

/* VAE antigo: a atencao do meio usa query/key/value/proj_attn; o resto ja esta no formato do diffusers */
static void sd_vae_name(const char *name, char *out, size_t cap) {
    static const char *const m[][2] = { { ".attentions.0.query.", ".attentions.0.to_q." }, { ".attentions.0.key.", ".attentions.0.to_k." },
                                        { ".attentions.0.value.", ".attentions.0.to_v." }, { ".attentions.0.proj_attn.", ".attentions.0.to_out.0." } };
    for (int i = 0; i < 4; i++) {
        const char *at = strstr(name, m[i][0]);
        if (at) { snprintf(out, cap, "%.*s%s%s", (int)(at - name), name, m[i][1], at + strlen(m[i][0])); return; }
    }
    snprintf(out, cap, "%s", name);
}

static SdT *sd_add(SdSet *S, const char *prefix, const char *name, const CvSrcSet *src, const CvSt *st, int force32) {
    if (S->n == S->cap) { S->cap = S->cap ? S->cap * 2 : 512; S->t = realloc(S->t, (size_t)S->cap * sizeof(SdT)); if (!S->t) die("render: sem memoria"); }
    SdT *t = &S->t[S->n++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s%s", prefix, name);
    const CvShard *sh = &src->sh[st->shard];
    t->p = sh->base + sh->data0 + st->b;
    t->dt = st->dt; t->nd = st->nd;
    uint64_t cnt = 1;
    for (int d = 0; d < st->nd; d++) { t->shape[d] = st->shape[d]; cnt *= st->shape[d]; }
    t->otype = (st->nd >= 2 && !force32) ? 1u : 0u;          /* 1 = F16, 0 = F32 */
    t->nbytes = cnt * (t->otype ? 2u : 4u);
    return t;
}

static int sd_t_cmp(const void *a, const void *b) { return strcmp(((const SdT *)a)->name, ((const SdT *)b)->name); }

/* o primeiro *.safetensors de uma pasta (em ordem de nome); 0 se nao ha */
static int sd_find_st(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    char best[512] = "";
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        const size_t l = strlen(e->d_name);
        if (l > 12 && !strcmp(e->d_name + l - 12, ".safetensors") && !strstr(e->d_name, "inpainting") && (!best[0] || strcmp(e->d_name, best) < 0)) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, cap, "%s/%s", dir, best);
    return 1;
}

typedef struct { CvSrcSet ck, vae, cn; SdSet base, ctrl; } SdCtx;

/* le as tres fontes e monta as duas listas de tensores (ordenadas por nome); 0 = ok */
static int sd_plan(const char *dir, SdCtx *c) {
    char p[1100], q[1100];
    memset(c, 0, sizeof *c);
    snprintf(p, sizeof p, "%s/realistic-vision", dir);
    if (!sd_find_st(p, q, sizeof q)) { fprintf(stderr, "render: nao achei um *.safetensors em %s\n", p); return 1; }
    cv_open_shard(&c->ck, q);
    snprintf(p, sizeof p, "%s/vae-ft-mse", dir);
    if (!sd_find_st(p, q, sizeof q)) { fprintf(stderr, "render: nao achei um *.safetensors em %s\n", p); return 1; }
    cv_open_shard(&c->vae, q);
    snprintf(p, sizeof p, "%s/controlnet-canny", dir);
    if (!sd_find_st(p, q, sizeof q)) { fprintf(stderr, "render: nao achei um *.safetensors em %s\n", p); return 1; }
    cv_open_shard(&c->cn, q);
    int skipped = 0;
    char nm[220];
    for (size_t i = 0; i < c->ck.nst; i++) {
        const CvSt *t = &c->ck.st[i];
        const char *r;
        if (sd_unet_name(t->name, nm, sizeof nm)) sd_add(&c->base, "unet.", nm, &c->ck, t, 0);
        else if (sd_pfx(t->name, "cond_stage_model.transformer.text_model.", &r)) {
            if (strstr(r, "position_ids")) { skipped++; continue; }
            sd_add(&c->base, "clip.", r, &c->ck, t, 0);
        } else if (!strncmp(t->name, "first_stage_model.", 18)) skipped++;
        else { fprintf(stderr, "render: tensor do checkpoint sem regra: %s\n", t->name); return 1; }
    }
    for (size_t i = 0; i < c->vae.nst; i++) { sd_vae_name(c->vae.st[i].name, nm, sizeof nm); sd_add(&c->base, "vae.", nm, &c->vae, &c->vae.st[i], 1); }
    for (size_t i = 0; i < c->cn.nst; i++) sd_add(&c->ctrl, "ctrl.", c->cn.st[i].name, &c->cn, &c->cn.st[i], 0);
    qsort(c->base.t, (size_t)c->base.n, sizeof(SdT), sd_t_cmp);
    qsort(c->ctrl.t, (size_t)c->ctrl.n, sizeof(SdT), sd_t_cmp);
    for (int i = 1; i < c->base.n; i++) if (!strcmp(c->base.t[i].name, c->base.t[i - 1].name)) { fprintf(stderr, "render: nome repetido: %s\n", c->base.t[i].name); return 1; }
    for (int i = 1; i < c->ctrl.n; i++) if (!strcmp(c->ctrl.t[i].name, c->ctrl.t[i - 1].name)) { fprintf(stderr, "render: nome repetido: %s\n", c->ctrl.t[i].name); return 1; }
    printf("  checkpoint: %d tensores usados (UNet + CLIP), %d ignorados (VAE embutido e position_ids)\n", c->base.n, skipped);
    return 0;
}

/* ---------- metadados ---------- */
static int sd_ints(const JV *o, const char *key, int32_t *out, int max) {
    const JV *a = jget(o, key);
    if (!a || a->t != J_ARR) return 0;
    int n = 0;
    for (size_t i = 0; i < a->n && n < max; i++) out[n++] = (int32_t)a->v[i].i;
    return n;
}

static void sd_kv_unet(const char *dir, const char *prefix, GBuf *kv, uint64_t *nkv) {
    char p[1100]; JV j; char key[96]; int32_t v[8]; int n;
    snprintf(p, sizeof p, "%s", dir);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); exit(1); }
#define KU(name, def) do { snprintf(key, sizeof key, "%s." name, prefix); gkv_u32(kv, key, (uint32_t)jnum(&j, name, def)); (*nkv)++; } while (0)
    KU("in_channels", 4); KU("out_channels", 4); KU("layers_per_block", 2); KU("cross_attention_dim", 768);
    KU("attention_head_dim", 8); KU("norm_num_groups", 32); KU("sample_size", 64);
#undef KU
    if ((n = sd_ints(&j, "block_out_channels", v, 8)) > 0) { snprintf(key, sizeof key, "%s.block_out_channels", prefix); cv_kv_arr_i32(kv, key, v, n); (*nkv)++; }
    snprintf(key, sizeof key, "%s.norm_eps", prefix); cv_kv_f32(kv, key, 1e-5f); (*nkv)++;
}

/* vocab.json (token -> id) e merges.txt do CLIP para dentro do GGUF; os ids especiais vem de tokenizer_config.json */
static void sd_kv_tokenizer(const char *dir, GBuf *kv, uint64_t *nkv) {
    char p[1100]; JV vj;
    snprintf(p, sizeof p, "%s/realistic-vision/tokenizer/vocab.json", dir);
    if (!cv_json_file(p, &vj) || vj.t != J_OBJ) { fprintf(stderr, "render: nao consegui ler %s\n", p); exit(1); }
    int64_t maxid = -1;
    for (size_t i = 0; i < vj.n; i++) if (vj.v[i].i > maxid) maxid = vj.v[i].i;
    const int V = (int)maxid + 1;
    const char **toks = xalloc((size_t)V, sizeof(char *));
    for (size_t i = 0; i < vj.n; i++) { const int64_t id = vj.v[i].i; if (id >= 0 && id < V) toks[id] = vj.k[i]; }
    for (int i = 0; i < V; i++) if (!toks[i]) { fprintf(stderr, "render: vocab.json sem o id %d\n", i); exit(1); }
    gkv_str(kv, "tokenizer.clip.model", "clip-bpe"); (*nkv)++;
    gbuf_gstr(kv, "tokenizer.clip.tokens", strlen("tokenizer.clip.tokens")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)V);
    for (int i = 0; i < V; i++) gbuf_gstr(kv, toks[i], strlen(toks[i]));
    (*nkv)++;
    snprintf(p, sizeof p, "%s/realistic-vision/tokenizer/merges.txt", dir);
    char *mt = cv_read_text(p);
    if (!mt) { fprintf(stderr, "render: nao consegui ler %s\n", p); exit(1); }
    /* so a primeira linha ("#version: 0.2") e cabecalho: seis regras de fusao de verdade comecam com '#' (por exemplo "# #</w>") */
    char *body = mt;
    if (!strncmp(body, "#version", 8)) { char *nl = strchr(body, '\n'); body = nl ? nl + 1 : body + strlen(body); }
    size_t nm = 0;
    for (char *s = body, *e; *s; s = e + 1) { e = strchr(s, '\n'); if (!e) e = s + strlen(s); size_t l = (size_t)(e - s); if (l && s[l - 1] == '\r') l--; if (l) nm++; if (!*e) break; }
    gbuf_gstr(kv, "tokenizer.clip.merges", strlen("tokenizer.clip.merges")); gbuf_u32(kv, 9); gbuf_u32(kv, 8); gbuf_u64(kv, (uint64_t)nm);
    for (char *s = body, *e; *s; s = e + 1) {
        e = strchr(s, '\n'); if (!e) e = s + strlen(s);
        size_t l = (size_t)(e - s); if (l && s[l - 1] == '\r') l--;
        if (l) gbuf_gstr(kv, s, l);
        if (!*e) break;
    }
    (*nkv)++;
    free(mt);
    int bos = -1, eos = -1;
    for (int i = 0; i < V; i++) { if (!strcmp(toks[i], "<|startoftext|>")) bos = i; if (!strcmp(toks[i], "<|endoftext|>")) eos = i; }
    if (bos < 0 || eos < 0) { fprintf(stderr, "render: o vocabulario nao tem <|startoftext|> e <|endoftext|>\n"); exit(1); }
    gkv_u32(kv, "tokenizer.clip.bos_token_id", (uint32_t)bos); (*nkv)++;
    gkv_u32(kv, "tokenizer.clip.eos_token_id", (uint32_t)eos); (*nkv)++;
    gkv_u32(kv, "tokenizer.clip.pad_token_id", (uint32_t)eos); (*nkv)++;      /* tokenizer_config.json: pad_token = <|endoftext|> */
    gkv_u32(kv, "tokenizer.clip.lowercase", 1); (*nkv)++;                     /* do_lower_case = true */
    free(toks);
}

/* ---------- LoRA (fusao nos pesos da UNet, ao converter) ----------
 * katana render convert <pasta> <base.gguf> <controle.gguf|-> --lora ARQ [--lora-scale S]
 * O LoRA (formato kohya: lora_unet_<caminho com _>.lora_down.weight / .lora_up.weight / .alpha) e somado ao peso W da camada
 * correspondente da UNet: W += S * (alpha / r) * (up x down), com up [saida, r] e down [r, entrada (x kh x kw)]. O nome da camada vem do
 * nome do tensor no GGUF (os pontos viram "_"), entao nao ha ambiguidade com os "_" dos nomes. Serve ao LCM-LoRA do SD 1.5. "-" no lugar
 * do GGUF do ControlNet nao o grava. */
static CvSrcSet g_sd_lora; static int g_sd_lora_on, g_sd_lora_merged; static double g_sd_lora_scale = 1.0;
typedef struct { float *W; const float *up, *down; int out, r, inn; float scale; } SdLoraJob;
static void sd_lora_job(void *cx, int o0, int o1) {
    const SdLoraJob *j = cx;
    for (int o = o0; o < o1; o++) {
        float *w = j->W + (size_t)o * j->inn;
        for (int r = 0; r < j->r; r++) {
            const float u = j->up[(size_t)o * j->r + r] * j->scale;
            if (u == 0.0f) continue;
            const float *d = j->down + (size_t)r * j->inn;
            for (int i = 0; i < j->inn; i++) w[i] += u * d[i];
        }
    }
}
static const uint8_t *sd_lora_ptr(const CvSt *t) { const CvShard *sh = &g_sd_lora.sh[t->shard]; return sh->base + sh->data0 + t->b; }
static void sd_lora_merge(const SdT *t, float *fl, size_t n) {
    const size_t nl = strlen(t->name);
    if (strncmp(t->name, "unet.", 5) != 0 || nl < 12 || strcmp(t->name + nl - 7, ".weight") != 0) return;
    char key[300]; size_t k = (size_t)snprintf(key, sizeof key, "lora_unet_");
    for (size_t i = 5; i < nl - 7 && k < sizeof key - 40; i++) key[k++] = t->name[i] == '.' ? '_' : t->name[i];
    key[k] = 0;
    char nm[360];
    snprintf(nm, sizeof nm, "%s.lora_down.weight", key); CvSt *dn = cv_find(&g_sd_lora, nm);
    snprintf(nm, sizeof nm, "%s.lora_up.weight", key); CvSt *up = cv_find(&g_sd_lora, nm);
    if (!dn || !up) return;
    dn->used = up->used = 1;
    snprintf(nm, sizeof nm, "%s.alpha", key); CvSt *al = cv_find(&g_sd_lora, nm);
    size_t dne = 1, upe = 1;
    for (int d = 0; d < dn->nd; d++) dne *= dn->shape[d];
    for (int d = 0; d < up->nd; d++) upe *= up->shape[d];
    const int r = (int)dn->shape[0], out = (int)up->shape[0];
    const size_t inn = dne / (size_t)r;
    if (up->nd < 2 || (int)up->shape[1] != r || (size_t)out * inn != n) { fprintf(stderr, "render: o LoRA de %s nao casa com o peso (%zu elementos)\n", key, n); exit(1); }
    float alpha = (float)r;
    if (al) { al->used = 1; cv_load(al->dt, sd_lora_ptr(al), &alpha, 1); }
    float *D = xalloc(dne, 4), *U = xalloc(upe, 4);
    cv_load(dn->dt, sd_lora_ptr(dn), D, dne); cv_load(up->dt, sd_lora_ptr(up), U, upe);
    SdLoraJob j = { fl, U, D, out, r, (int)inn, (float)(g_sd_lora_scale * alpha / (double)r) };
    pool_for(sd_lora_job, &j, out, 8);
    free(D); free(U);
    g_sd_lora_merged++;
}

/* ---------- gravacao ---------- */
static int sd_write(const char *out_path, SdSet *S, GBuf *kv, uint64_t nkv) {
    const uint64_t align = 32;
    uint64_t off = 0, params = 0;
    for (int i = 0; i < S->n; i++) {
        SdT *t = &S->t[i];
        off = (off + align - 1) / align * align;
        t->off = off; off += t->nbytes;
        params += t->nbytes / (t->otype ? 2u : 4u);
    }
    GBuf tib = {0};
    for (int i = 0; i < S->n; i++) {
        const SdT *t = &S->t[i];
        gbuf_gstr(&tib, t->name, strlen(t->name)); gbuf_u32(&tib, (uint32_t)t->nd);
        for (int d = t->nd - 1; d >= 0; d--) gbuf_u64(&tib, t->shape[d]);        /* ordem inversa: ne0 = coluna contigua */
        gbuf_u32(&tib, t->otype); gbuf_u64(&tib, t->off);
    }
    FILE *ex = fopen(out_path, "rb");
    if (ex) { fclose(ex); fprintf(stderr, "render: %s ja existe; escolha outro nome.\n", out_path); return 1; }
    char tmpp[2100]; snprintf(tmpp, sizeof tmpp, "%s.part", out_path);
    FILE *f = fopen(tmpp, "wb");
    if (!f) { fprintf(stderr, "render: nao consegui gravar %s\n", tmpp); return 1; }
    uint8_t hdr[24]; const uint32_t magic = 0x46554747u, v3 = 3; const uint64_t nt = (uint64_t)S->n;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &v3, 4); memcpy(hdr + 8, &nt, 8); memcpy(hdr + 16, &nkv, 8);
    fwrite(hdr, 1, 24, f); fwrite(kv->p, 1, kv->n, f); fwrite(tib.p, 1, tib.n, f);
    const uint64_t hpos = 24 + kv->n + tib.n, dstart = (hpos + align - 1) / align * align;
    for (uint64_t p = hpos; p < dstart; p++) fputc(0, f);
    uint64_t cur = 0;
    for (int i = 0; i < S->n; i++) {
        const SdT *t = &S->t[i];
        while (cur < t->off) { fputc(0, f); cur++; }
        const size_t n = (size_t)(t->nbytes / (t->otype ? 2u : 4u));
        float *fl = xalloc(n, sizeof(float));
        cv_load(t->dt, t->p, fl, n);
        if (g_sd_lora_on) sd_lora_merge(t, fl, n);
        if (t->otype == 1) {
            uint16_t *h = xalloc(n, 2);
            for (size_t k = 0; k < n; k++) h[k] = f32_to_f16(fl[k]);
            if (fwrite(h, 2, n, f) != n) { fprintf(stderr, "render: erro de escrita (disco cheio?)\n"); fclose(f); remove(tmpp); return 1; }
            free(h);
        } else if (fwrite(fl, 4, n, f) != n) { fprintf(stderr, "render: erro de escrita (disco cheio?)\n"); fclose(f); remove(tmpp); return 1; }
        free(fl);
        cur = t->off + t->nbytes;
    }
    free(tib.p);
    if (fclose(f) != 0) { fprintf(stderr, "render: erro ao fechar %s\n", tmpp); remove(tmpp); return 1; }
    if (rename(tmpp, out_path) != 0) { fprintf(stderr, "render: nao consegui renomear %s\n", tmpp); return 1; }
    printf("  %s: %d tensores, %.1f M parametros, %.2f GB\n", out_path, S->n, (double)params / 1e6, (double)(dstart + off) / 1073741824.0);
    return 0;
}

static int sd_convert(const char *dir, const char *out_base, const char *out_ctrl) {
    SdCtx *c = xalloc(1, sizeof *c);
    if (sd_plan(dir, c)) return 1;
    char p[1100]; JV j; char key[96]; int32_t v[8]; int n;
    GBuf kv = {0}; uint64_t nkv = 0;
    gkv_str(&kv, "general.architecture", "sd15"); nkv++;
    gkv_str(&kv, "general.type", "model"); nkv++;
    gkv_str(&kv, "general.name", "Realistic Vision V6.0 B1 + sd-vae-ft-mse"); nkv++;
    gkv_str(&kv, "general.quantized_by", "katana"); nkv++;
    snprintf(p, sizeof p, "%s/realistic-vision/unet/config.json", dir);
    sd_kv_unet(p, "sd15.unet", &kv, &nkv);
    snprintf(p, sizeof p, "%s/realistic-vision/text_encoder/config.json", dir);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
#define KU(name, def) do { snprintf(key, sizeof key, "sd15.clip.%s", name); gkv_u32(&kv, key, (uint32_t)jnum(&j, name, def)); nkv++; } while (0)
    KU("hidden_size", 768); KU("intermediate_size", 3072); KU("num_attention_heads", 12); KU("num_hidden_layers", 12);
    KU("max_position_embeddings", 77); KU("vocab_size", 49408);
#undef KU
    cv_kv_f32(&kv, "sd15.clip.layer_norm_eps", (float)jnum(&j, "layer_norm_eps", 1e-5)); nkv++;
    gkv_str(&kv, "sd15.clip.hidden_act", jstr(&j, "hidden_act") ? jstr(&j, "hidden_act") : "quick_gelu"); nkv++;
    snprintf(p, sizeof p, "%s/vae-ft-mse/config.json", dir);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
#define KU(name, def) do { snprintf(key, sizeof key, "sd15.vae.%s", name); gkv_u32(&kv, key, (uint32_t)jnum(&j, name, def)); nkv++; } while (0)
    KU("in_channels", 3); KU("out_channels", 3); KU("latent_channels", 4); KU("layers_per_block", 2); KU("norm_num_groups", 32);
#undef KU
    if ((n = sd_ints(&j, "block_out_channels", v, 8)) > 0) { cv_kv_arr_i32(&kv, "sd15.vae.block_out_channels", v, n); nkv++; }
    cv_kv_f32(&kv, "sd15.vae.scaling_factor", (float)jnum(&j, "scaling_factor", 0.18215)); nkv++;
    snprintf(p, sizeof p, "%s/realistic-vision/scheduler/scheduler_config.json", dir);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
    gkv_u32(&kv, "sd15.sched.num_train_timesteps", (uint32_t)jnum(&j, "num_train_timesteps", 1000)); nkv++;
    cv_kv_f32(&kv, "sd15.sched.beta_start", (float)jnum(&j, "beta_start", 0.00085)); nkv++;
    cv_kv_f32(&kv, "sd15.sched.beta_end", (float)jnum(&j, "beta_end", 0.012)); nkv++;
    gkv_str(&kv, "sd15.sched.beta_schedule", jstr(&j, "beta_schedule") ? jstr(&j, "beta_schedule") : "scaled_linear"); nkv++;
    sd_kv_tokenizer(dir, &kv, &nkv);
    printf("katana render convert: %s\n", dir);
    if (sd_write(out_base, &c->base, &kv, nkv)) return 1;
    if (g_sd_lora_on) {
        int pairs = 0, unmatched = 0;
        for (size_t i = 0; i < g_sd_lora.nst; i++) if (strstr(g_sd_lora.st[i].name, ".lora_down.weight")) { pairs++; if (!g_sd_lora.st[i].used) unmatched++; }
        printf("  LoRA fundido em %d tensores da UNet (o arquivo tem %d camadas; %d sem par)\n", g_sd_lora_merged, pairs, unmatched);
        if (unmatched) { fprintf(stderr, "render: %d camadas do LoRA nao acharam a camada da UNet\n", unmatched); return 1; }
    }
    if (!strcmp(out_ctrl, "-")) return 0;
    GBuf kc = {0}; uint64_t nkc = 0;
    gkv_str(&kc, "general.architecture", "sd15controlnet"); nkc++;
    gkv_str(&kc, "general.type", "model"); nkc++;
    gkv_str(&kc, "general.name", "ControlNet v1.1 canny (SD 1.5)"); nkc++;
    gkv_str(&kc, "general.quantized_by", "katana"); nkc++;
    snprintf(p, sizeof p, "%s/controlnet-canny/config.json", dir);
    sd_kv_unet(p, "sd15controlnet", &kc, &nkc);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
    if ((n = sd_ints(&j, "conditioning_embedding_out_channels", v, 8)) > 0) { cv_kv_arr_i32(&kc, "sd15controlnet.cond_embedding_channels", v, n); nkc++; }
    if (sd_write(out_ctrl, &c->ctrl, &kc, nkc)) return 1;
    return 0;
}

/* "katana render names <pasta>": uma linha por tensor de saida (nome e dimensoes na ordem do safetensors), para comparar com a referencia */
static int sd_names(const char *dir) {
    SdCtx *c = xalloc(1, sizeof *c);
    if (sd_plan(dir, c)) return 1;
    SdSet *sets[2] = { &c->base, &c->ctrl };
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < sets[s]->n; i++) {
            const SdT *t = &sets[s]->t[i];
            printf("%s", t->name);
            for (int d = 0; d < t->nd; d++) printf(" %llu", (unsigned long long)t->shape[d]);
            printf("\n");
        }
    return 0;
}

static int sdt_main(const char *refdir, const char *mdir);      /* render/sdtest.c */
static int sdp_main(int argc, char **argv);                      /* render/sdgen.c */
static int sdb_main(int argc, char **argv);                      /* render/sdbrief.c */
static int fxt_convert(const char *dir, const char *out_path);       /* render/fxconvert_text.c */
static int fxt_main(int argc, char **argv);                          /* render/fxtext.c */
static int sds_main(int argc, char **argv);                      /* render/sdspec.c */
static int fxv_main(int argc, char **argv);                      /* render/fxvae.c */
static int fxd_main(int argc, char **argv);                      /* render/fxdit.c */
static int fxg_main(int argc, char **argv);                      /* render/fxgen.c */
static int sdl_main(int argc, char **argv);                      /* render/sdlogo.c */
static int render_main(int argc, char **argv) {
    g_cv_skip_int = 1;      /* position_ids do CLIP e outros tensores de inteiros: nao entram */
    if (argc >= 2 && !strcmp(argv[0], "names")) return sd_names(argv[1]);
    if (argc >= 3 && !strcmp(argv[0], "selftest")) return sdt_main(argv[1], argv[2]);
    if (argc >= 1 && !strcmp(argv[0], "generate")) return sdp_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "brief")) return sdb_main(argc - 1, argv + 1);
    if (argc >= 3 && !strcmp(argv[0], "flux-convert-text")) return fxt_convert(argv[1], argv[2]);
    if (argc >= 1 && !strcmp(argv[0], "flux-text")) return fxt_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "flux-dit")) return fxd_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "flux-gen")) return fxg_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "spec")) return sds_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "logo")) return sdl_main(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "flux-vae")) return fxv_main(argc - 1, argv + 1);
    if (argc >= 4 && !strcmp(argv[0], "convert")) {
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--lora") && i + 1 < argc) { cv_open_shard(&g_sd_lora, argv[++i]); qsort(g_sd_lora.st, g_sd_lora.nst, sizeof(CvSt), cv_st_cmp); g_sd_lora_on = 1; }
            else if (!strcmp(argv[i], "--lora-scale") && i + 1 < argc) g_sd_lora_scale = atof(argv[++i]);
            else { fprintf(stderr, "render convert: opcao desconhecida: %s\n", argv[i]); return 1; }
        }
        return sd_convert(argv[1], argv[2], argv[3]);
    }
    fprintf(stderr, "uso: katana render convert <pasta> <base.gguf> <controle.gguf>\n"
                    "     katana render names   <pasta>\n"
                    "     katana render selftest <referencia> <pasta>\n"
                    "     katana render brief <briefing.md> --image1 vista.png --out saida.png [--image2 linhas.png] [--show]   |   katana render brief selftest [briefing.md]\n"
                    "     katana render logo --image render.png --logo logo.png --box x,y,w,h [--perspective x1,y1,...,x4,y4] [--blend]   |   katana render logo selftest\n"
                    "     katana render spec --brief briefing.md --engine flux|stable   (imprime o prompt do briefing, sem gerar)\n"
                    "     katana render generate --prompt \"...\" --out saida.png [opcoes]   (katana render generate sem argumentos lista as opcoes)\n"
                    "  <pasta> = models/render (com realistic-vision/, vae-ft-mse/ e controlnet-canny/); docs/render.md\n");
    return 1;
}
