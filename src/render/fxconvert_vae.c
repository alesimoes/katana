/* ============================ katana render flux-vae convert ===============================
 * katana render flux-vae convert <pasta-do-vae> <saida.gguf>
 *
 * Converte o VAE do FLUX.2 klein (formato diffusers: config.json + diffusion_pytorch_model.safetensors, tudo em BF16) para GGUF
 * (arquitetura "flux2vae"). Nomes: os do diffusers com o prefixo "vae." (encoder.*, decoder.*, quant_conv, post_quant_conv), mais
 * "vae.bn.running_mean" e "vae.bn.running_var" (as estatisticas de lote dos 128 canais do latente patchificado, 2x2 x 32);
 * "bn.num_batches_tracked" (inteiro) nao entra. Tipos: tudo em F32 (BF16 -> F32 e exato; o VAE e pequeno e sensivel, como o do
 * Stable Diffusion). Dimensoes do GGUF = as do safetensors na ordem inversa (ne0 contiguo), nada e transposto.
 * Chaves: flux2vae.{latent_channels, in_channels, out_channels, layers_per_block, norm_num_groups, patch_size, block_out_channels,
 * batch_norm_eps}. Reaproveita o escritor de render/sdconvert.c (sd_add, sd_write). */

static int fxv_convert(const char *dir, const char *out) {
    char p[1100]; JV j; int32_t v[8]; int n;
    snprintf(p, sizeof p, "%s/diffusion_pytorch_model.safetensors", dir);
    FILE *ex = fopen(p, "rb");
    if (!ex) { fprintf(stderr, "render: nao achei %s\n", p); return 1; }
    fclose(ex);
    CvSrcSet src; memset(&src, 0, sizeof src);
    g_cv_skip_int = 1;
    cv_open_shard(&src, p);
    SdSet S; memset(&S, 0, sizeof S);
    for (size_t i = 0; i < src.nst; i++) sd_add(&S, "vae.", src.st[i].name, &src, &src.st[i], 1);
    qsort(S.t, (size_t)S.n, sizeof(SdT), sd_t_cmp);
    for (int i = 1; i < S.n; i++) if (!strcmp(S.t[i].name, S.t[i - 1].name)) { fprintf(stderr, "render: nome repetido: %s\n", S.t[i].name); return 1; }
    snprintf(p, sizeof p, "%s/config.json", dir);
    if (!cv_json_file(p, &j)) { fprintf(stderr, "render: nao consegui ler %s\n", p); return 1; }
    GBuf kv = {0}; uint64_t nkv = 0; char key[96];
    gkv_str(&kv, "general.architecture", "flux2vae"); nkv++;
    gkv_str(&kv, "general.type", "model"); nkv++;
    gkv_str(&kv, "general.name", "FLUX.2 klein VAE"); nkv++;
    gkv_str(&kv, "general.quantized_by", "katana"); nkv++;
#define KU(name, def) do { snprintf(key, sizeof key, "flux2vae.%s", name); gkv_u32(&kv, key, (uint32_t)jnum(&j, name, def)); nkv++; } while (0)
    KU("in_channels", 3); KU("out_channels", 3); KU("latent_channels", 32); KU("layers_per_block", 2); KU("norm_num_groups", 32);
#undef KU
    gkv_u32(&kv, "flux2vae.patch_size", 2); nkv++;
    if ((n = sd_ints(&j, "block_out_channels", v, 8)) > 0) { cv_kv_arr_i32(&kv, "flux2vae.block_out_channels", v, n); nkv++; }
    cv_kv_f32(&kv, "flux2vae.batch_norm_eps", (float)jnum(&j, "batch_norm_eps", 1e-4)); nkv++;
    printf("katana render flux-vae convert: %s\n", dir);
    return sd_write(out, &S, &kv, nkv);
}
