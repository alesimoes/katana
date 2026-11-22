/* ---------- toktest: teste dourado do tokenizador e dos formatos de conversa (ver tools/tokref) ----------
 * Entrada: um caso por linha em hexa UTF-8 (modo texto) ou "sistema usuario assistente usuario2" em hexa, com '-' para
 * ausente (modo --chat). Saida: modo texto = uma linha com os ids; modo --chat = duas linhas por caso (conversa de um
 * turno e de dois turnos), cada uma "texto-em-hexa<TAB>ids", com o BOS que o motor poria. */
static int hex_val(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

static char *hex_dec(const char *h, size_t n) {
    char *o = xalloc(n / 2 + 1, 1);
    for (size_t i = 0; i + 1 < n; i += 2) o[i / 2] = (char)(hex_val(h[i]) * 16 + hex_val(h[i + 1]));
    return o;
}

static void tt_print(const Model *m, const char *text, int with_bos) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    Ids ids = {0};
    if (with_bos && c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text, &ids);
    for (int i = 0; i < ids.n; i++) printf(i ? " %d" : "%d", ids.ids[i]);
    printf("\n");
    free(ids.ids);
}

static void tt_hexline(const char *pre, const char *t) {
    for (const uint8_t *p = (const uint8_t *)pre; *p; p++) printf("%02x", *p);
    for (const uint8_t *p = (const uint8_t *)t; *p; p++) printf("%02x", *p);
    printf("\t");
}

static int toktest(Model *m, const char *path, int chat) {
    const Config *c = &m->cfg;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "nao abri %s\n", path); return 1; }
    char *line = xalloc(1 << 22, 1);
    while (fgets(line, 1 << 22, f)) {
        size_t ln = strlen(line);
        while (ln && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (!chat) {
            char *t = hex_dec(line, ln);
            tt_print(m, t, 0);
            free(t);
            continue;
        }
        char *fld[4] = {0}; int nf = 0;
        for (char *p = line; nf < 4 && p; ) {
            char *sp = strchr(p, ' ');
            size_t l = sp ? (size_t)(sp - p) : strlen(p);
            fld[nf++] = (l == 1 && p[0] == '-') ? NULL : (l == 1 && p[0] == '~') ? hex_dec("", 0) : hex_dec(p, l);
            p = sp ? sp + 1 : NULL;
        }
        char *t1 = xalloc(strlen(fld[1]) + 8192, 1), *t2 = xalloc(strlen(fld[1]) + strlen(fld[2]) + strlen(fld[3]) + 16384, 1);
        char *tmp = xalloc(strlen(fld[3]) + 8192, 1);
        turn_text(c, t1, strlen(fld[1]) + 8192, fld[0], fld[1], 1);
        /* a resposta simulada: o que o modelo geraria. O DeepSeek gera um espaco logo depois de "Assistant:" */
        snprintf(t2, strlen(fld[1]) + strlen(fld[2]) + strlen(fld[3]) + 16384, "%s%s%s", t1, c->chat_fmt == 5 ? " " : "", fld[2]);
        turn_text(c, tmp, strlen(fld[3]) + 8192, NULL, fld[3], 0);
        strcat(t2, tmp);
        const char *pre = (c->add_bos && c->bos >= 0) ? c->bos_text : "";
        tt_hexline(pre, t1); tt_print(m, t1, 1);
        tt_hexline(pre, t2); tt_print(m, t2, 1);
        free(t1); free(t2); free(tmp);
        for (int i = 0; i < 4; i++) free(fld[i]);
    }
    free(line); fclose(f);
    return 0;
}

/* katana tokenize <modelo> [arquivo|-] [--ids]: conta os tokens de um texto (so o tokenizador, sem pesos).
   Se todas as linhas nao vazias forem "papel<TAB>texto" (system, user, assistant), monta a conversa no formato
   do modelo (como o chat: termina abrindo a resposta se a ultima for do usuario) e conta por mensagem.
   Nas mensagens, "\n" e "\t" no texto viram quebra de linha e tabulacao. */
static void tk_unesc(char *s) {
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (p[0] == '\\' && (p[1] == 'n' || p[1] == 't' || p[1] == '\\')) { *o++ = p[1] == 'n' ? '\n' : p[1] == 't' ? '\t' : '\\'; p++; }
        else *o++ = *p;
    }
    *o = 0;
}
static int tokenize_cmd(Model *m, const char *path, int show_ids) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    FILE *f = (!path || strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!f) { fprintf(stderr, "nao abri %s\n", path); return 1; }
    Buf in = {0};
    char blk[65536];
    size_t r;
    while ((r = fread(blk, 1, sizeof blk, f)) > 0) buf_add(&in, blk, r);
    if (f != stdin) fclose(f);
    if (!in.p) buf_str(&in, "");
    int nmsg = 0, conv = 1;
    for (char *p = in.p; *p; ) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && !(l == 1 && p[0] == '\r')) {
            nmsg++;
            if (!((l > 7 && strncmp(p, "system\t", 7) == 0) || (l > 5 && strncmp(p, "user\t", 5) == 0) || (l > 10 && strncmp(p, "assistant\t", 10) == 0))) conv = 0;
        }
        p = e ? e + 1 : p + l;
    }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    if (!conv || !nmsg) {
        encode(tk, in.p, &ids);
        printf("%d tokens (texto, %zu bytes%s)\n", ids.n, in.n, c->add_bos && c->bos >= 0 ? ", inclui o BOS" : "");
    } else {
        Buf t = {0};
        const char *sys = NULL;
        int first = 1, prev = ids.n;
        static const char *FMT[] = {"ChatML", "Llama 3", "Zephyr", "[INST]", "texto", "DeepSeek", "Kimi", "OLMoE", "Gemma 4", "harmony", "GLM-4"};
        printf("conversa, formato %s\n", c->chat_fmt >= 0 && c->chat_fmt <= 10 ? FMT[c->chat_fmt] : "?");
        for (char *p = in.p; *p; ) {
            char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
            if (e) *e = 0;
            if (l && p[l - 1] == '\r') p[--l] = 0;
            char *tab = l ? strchr(p, '\t') : NULL;
            if (tab) {
                *tab = 0;
                char *msg = tab + 1;
                tk_unesc(msg);
                buf_clear(&t);
                if (strcmp(p, "system") == 0) sys = msg;
                else {
                    if (strcmp(p, "user") == 0) {
                        size_t cap = strlen(msg) + (sys ? strlen(sys) : 0) + 8192;
                        char *o = xalloc(cap, 1);
                        turn_text(c, o, cap, first ? sys : NULL, msg, first);
                        buf_str(&t, o); free(o);
                        first = 0;
                    } else buf_str(&t, msg);   /* resposta do assistente: o texto que o modelo geraria (fecho no proximo turno) */
                    encode(tk, t.p, &ids);
                    printf("%-9s %6d tokens (acumulado %d)\n", p, ids.n - prev, ids.n);
                    prev = ids.n;
                }
            }
            p = e ? e + 1 : p + l;
        }
        printf("%d tokens no total%s\n", ids.n, c->add_bos && c->bos >= 0 ? " (inclui o BOS)" : "");
        free(t.p);
    }
    if (show_ids) { for (int i = 0; i < ids.n; i++) printf(i ? " %d" : "%d", ids.ids[i]); printf("\n"); }
    free(ids.ids); free(in.p);
    return 0;
}

/* nome do algoritmo de quantizacao a partir de general.file_type do GGUF */
static const char *quant_name(int ft) {
    switch (ft) {
        case 0: return "F32"; case 1: return "F16"; case 2: return "Q4_0"; case 3: return "Q4_1";
        case 7: return "Q8_0"; case 8: return "Q5_0"; case 9: return "Q5_1"; case 10: return "Q2_K";
        case 11: return "Q3_K_S"; case 12: return "Q3_K_M"; case 13: return "Q3_K_L"; case 14: return "Q4_K_S";
        case 15: return "Q4_K_M"; case 16: return "Q5_K_S"; case 17: return "Q5_K_M"; case 18: return "Q6_K";
        case 19: return "IQ2_XXS"; case 20: return "IQ2_XS"; case 21: return "Q2_K_S"; case 23: return "IQ3_XXS";
        case 24: return "IQ1_S"; case 25: return "IQ4_NL"; case 26: return "IQ3_S"; case 27: return "IQ2_S";
        case 28: return "IQ4_XS"; case 29: return "IQ1_M"; case 30: return "BF16"; case 32: return "MXFP4";
        default: return "?";
    }
}

/* le so os metadados de um GGUF (sem carregar tensores): arquitetura, quantizacao, camadas, especialistas */
static int gguf_meta(const char *path, char *arch, int *ft, int *n_layers, int *n_expert) {
    int64_t fsz = 0;
    const uint8_t *buf = map_file(path, &fsz);
    if (!buf) return 0;
    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) return 0;
    if (rd_u32(&r) < 2) return 0;
    rd_u64(&r); uint64_t n_kv = rd_u64(&r);
    arch[0] = 0; *ft = -1; *n_layers = 0; *n_expert = 0;
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) {
            int sl; const char *sv = rd_str(&r, &sl); if (sl > 47) sl = 47;
            memcpy(arch, sv, sl); arch[sl] = 0;
        } else if (t != 8 && t != 9 && key_is(k, kl, "general.file_type")) {
            *ft = (int)rd_num(&r, t);
        } else if (t != 8 && t != 9 && key_ends(k, kl, ".block_count")) {
            *n_layers = (int)rd_num(&r, t);
        } else if (t != 8 && t != 9 && key_ends(k, kl, ".expert_count")) {
            *n_expert = (int)rd_num(&r, t);
        } else rd_skip(&r, t);
    }
#ifndef _WIN32
    munmap((void *)buf, (size_t)fsz);
#endif
    return 1;
}

/* arquiteturas que o katana suporta (carrega e roda) */
static int arch_suportada(const char *a) {
    static const char *ok[] = { "llama", "qwen2", "qwen3", "qwen3moe", "qwen35moe", "qwen35",
                                "deepseek2", "olmoe", "gemma4", "qwen4exp", "gpt-oss", "glm4", "glm4moe" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(a, ok[i])) return 1;
    return 0;
}

/* como o modelo roda nesta maquina, por arquitetura e tamanho (estimativa; prepare mede de fato) */
static const char *como_roda(const char *arch, int n_expert, double gb) {
    if (!arch_suportada(arch)) return "nao suportado";
    if (!strcmp(arch, "qwen4exp")) return "CPU";
    if (!strcmp(arch, "gpt-oss") || !strcmp(arch, "glm4") || !strcmp(arch, "glm4moe"))
        return "CPU (Vulkan pendente)";
    if (!strcmp(arch, "gemma4")) return "GPU hibrido (cache esp.)";
    if (!strcmp(arch, "deepseek2")) return "GPU hibrido (MLA)";
    if (n_expert > 0) return "GPU (cache especialistas)";
    return gb < 3.4 ? "GPU (Vulkan, inteiro)" : "GPU parcial / CPU";
}

/* lista os GGUF de uma pasta numa tabela: data, tamanho, algoritmo (arquitetura + quantizacao), como roda */
static int modelos_cmd(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "modelos: nao abriu a pasta %s\n", dir); return 1; }
    printf("%-42s  %-10s  %8s  %-22s  %s\n", "modelo", "data", "tamanho", "algoritmo", "como roda");
    printf("%-42s  %-10s  %8s  %-22s  %s\n", "------", "----", "-------", "---------", "---------");
    struct dirent *e;
    char path[4096], arch[64], algo[64], data[16], tam[16];
    int n = 0; double soma = 0;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 6 || strcmp(nm + l - 5, ".gguf")) continue;   /* so .gguf (pula .ktn/.heat/.prof) */
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        const double gb = st.st_size / 1073741824.0;
        struct tm *tm = localtime(&st.st_mtime);
        strftime(data, sizeof data, "%Y-%m-%d", tm);
        snprintf(tam, sizeof tam, "%.1f GB", gb);
        int ft, nl, ne;
        if (gguf_meta(path, arch, &ft, &nl, &ne)) {
            snprintf(algo, sizeof algo, "%s %s%s", arch[0] ? arch : "?", quant_name(ft), ne > 0 ? " MoE" : "");
        } else snprintf(algo, sizeof algo, "(nao e GGUF valido)");
        printf("%-42s  %-10s  %8s  %-22s  %s\n", nm, data, tam, algo,
               gguf_meta(path, arch, &ft, &nl, &ne) ? como_roda(arch, ne, gb) : "-");
        n++; soma += gb;
    }
    closedir(d);
    printf("\n%d modelos, %.1f GB no total. Data = data do arquivo (download). 'como roda' e estimativa; use 'katana prepare' para medir.\n", n, soma);
    return 0;
}

/* ============================ katana quantize =============================
 * Le um GGUF e grava outro com os tensores requantizados no tipo alvo. Os pesos 2D grandes
 * (attn_*, ffn_*, token_embd, output) sao dequantizados para F32 e recodificados; normas,
 * vieses, tensores 1D e os que ja sao <= ao alvo sao copiados como estao. Quantizacao simples,
 * round-to-nearest, sem matriz de importancia (imatrix) por enquanto. */

/* float32 -> half (arredondamento ao mais proximo, empate para par) */
static uint16_t f32_to_f16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e = (x >> 23) & 0xffu;
    uint32_t mant = x & 0x7fffffu;
    if (e == 0xff) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0u));   /* inf/nan */
    int exp = (int)e - 127 + 15;
    if (exp >= 0x1f) return (uint16_t)(sign | 0x7c00u);                        /* overflow -> inf */
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;                                  /* underflow -> 0 */
        mant |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t m = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (m & 1u))) m++;
        return (uint16_t)(sign | m);
    }
    uint32_t m = mant >> 13;
    const uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u))) {
        m++;
        if (m == 0x400u) { m = 0; exp++; if (exp >= 0x1f) return (uint16_t)(sign | 0x7c00u); }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | m);
}

static int qnear(float x) { return (int)lrintf(x); }

/* ---- codificadores: produzem exatamente o layout que o dequant() correspondente le ---- */

/* Q8_0: bloco de 32 = f16 d + 32 int8 */
static void enc_q8_0(const float *x, uint8_t *dst, size_t n) {
    for (size_t b = 0; b < n / 32; b++, x += 32, dst += 34) {
        float amax = 0;
        for (int j = 0; j < 32; j++) { float a = fabsf(x[j]); if (a > amax) amax = a; }
        const float d = amax / 127.0f, id = d > 0 ? 1.0f / d : 0.0f;
        const uint16_t h = f32_to_f16(d); memcpy(dst, &h, 2);
        int8_t *q = (int8_t *)(dst + 2);
        for (int j = 0; j < 32; j++) { int v = qnear(x[j] * id); v = v > 127 ? 127 : v < -127 ? -127 : v; q[j] = (int8_t)v; }
    }
}

/* empacota as 8 escalas e 8 minimos de 6 bits no formato que k4_scale_min le (Q4_K/Q5_K) */
static void pack_k_scales(uint8_t *sc, const uint8_t *ls, const uint8_t *lm) {
    for (int j = 0; j < 8; j++) {
        if (j < 4) { sc[j] = ls[j]; sc[j + 4] = lm[j]; }
        else {
            sc[j + 4] = (uint8_t)((ls[j] & 0xF) | ((lm[j] & 0xF) << 4));
            sc[j - 4] = (uint8_t)(sc[j - 4] | ((ls[j] >> 4) << 6));
            sc[j]     = (uint8_t)(sc[j]     | ((lm[j] >> 4) << 6));
        }
    }
}

/* escala/minimo por sub-bloco (quantizacao assimetrica de 'bits' bits, valores 0..nmax) */
static void k_subscale(const float *xb, int cnt, int nmax, float *scale, float *vmin) {
    float mn = xb[0], mx = xb[0];
    for (int i = 1; i < cnt; i++) { if (xb[i] < mn) mn = xb[i]; if (xb[i] > mx) mx = xb[i]; }
    if (mn > 0) mn = 0;                 /* garante que o zero seja representavel */
    *scale = (mx - mn) / (float)nmax;
    *vmin = -mn;                        /* minimo guardado como magnitude positiva */
}

/* Q4_K: bloco de 256 = f16 d, f16 dmin, 12 escalas, 128 nibbles (8 sub-blocos de 32, 4 bits) */
static void enc_q4_k(const float *x, uint8_t *dst, size_t n) {
    for (size_t b = 0; b < n / 256; b++, x += 256, dst += 144) {
        float scales[8], mins[8], max_scale = 0, max_min = 0;
        for (int sb = 0; sb < 8; sb++) {
            k_subscale(x + 32 * sb, 32, 15, &scales[sb], &mins[sb]);
            if (scales[sb] > max_scale) max_scale = scales[sb];
            if (mins[sb] > max_min) max_min = mins[sb];
        }
        const float inv_s = max_scale > 0 ? 63.0f / max_scale : 0, inv_m = max_min > 0 ? 63.0f / max_min : 0;
        uint8_t ls[8], lm[8];
        for (int sb = 0; sb < 8; sb++) {
            int s = qnear(inv_s * scales[sb]); if (s < 0) s = 0; if (s > 63) s = 63; ls[sb] = (uint8_t)s;
            int m = qnear(inv_m * mins[sb]);   if (m < 0) m = 0; if (m > 63) m = 63; lm[sb] = (uint8_t)m;
        }
        const uint16_t hd = f32_to_f16(max_scale / 63.0f), hm = f32_to_f16(max_min / 63.0f);
        const float df = f16_to_f32(hd), dmf = f16_to_f32(hm);
        memset(dst, 0, 144);
        memcpy(dst, &hd, 2); memcpy(dst + 2, &hm, 2);
        pack_k_scales(dst + 4, ls, lm);
        uint8_t *q = dst + 16;
        for (int sb = 0; sb < 8; sb++) {
            const float d_sb = df * ls[sb], m_sb = dmf * lm[sb];
            const float *xb = x + 32 * sb;
            uint8_t *qp = q + 32 * (sb / 2);
            for (int i = 0; i < 32; i++) {
                int v = d_sb > 0 ? qnear((xb[i] + m_sb) / d_sb) : 0;
                v = v < 0 ? 0 : v > 15 ? 15 : v;
                if (sb % 2 == 0) qp[i] = (uint8_t)((qp[i] & 0xF0) | v);
                else qp[i] = (uint8_t)((qp[i] & 0x0F) | (v << 4));
            }
        }
    }
}

/* Q5_K: bloco de 256 = f16 d, f16 dmin, 12 escalas, 32 bits altos, 128 nibbles (5 bits) */
static void enc_q5_k(const float *x, uint8_t *dst, size_t n) {
    for (size_t b = 0; b < n / 256; b++, x += 256, dst += 176) {
        float scales[8], mins[8], max_scale = 0, max_min = 0;
        for (int sb = 0; sb < 8; sb++) {
            k_subscale(x + 32 * sb, 32, 31, &scales[sb], &mins[sb]);
            if (scales[sb] > max_scale) max_scale = scales[sb];
            if (mins[sb] > max_min) max_min = mins[sb];
        }
        const float inv_s = max_scale > 0 ? 63.0f / max_scale : 0, inv_m = max_min > 0 ? 63.0f / max_min : 0;
        uint8_t ls[8], lm[8];
        for (int sb = 0; sb < 8; sb++) {
            int s = qnear(inv_s * scales[sb]); if (s < 0) s = 0; if (s > 63) s = 63; ls[sb] = (uint8_t)s;
            int m = qnear(inv_m * mins[sb]);   if (m < 0) m = 0; if (m > 63) m = 63; lm[sb] = (uint8_t)m;
        }
        const uint16_t hd = f32_to_f16(max_scale / 63.0f), hm = f32_to_f16(max_min / 63.0f);
        const float df = f16_to_f32(hd), dmf = f16_to_f32(hm);
        memset(dst, 0, 176);
        memcpy(dst, &hd, 2); memcpy(dst + 2, &hm, 2);
        pack_k_scales(dst + 4, ls, lm);
        uint8_t *qh = dst + 16, *q = dst + 48;
        for (int sb = 0; sb < 8; sb++) {
            const float d_sb = df * ls[sb], m_sb = dmf * lm[sb];
            const float *xb = x + 32 * sb;
            uint8_t *qp = q + 32 * (sb / 2);
            for (int i = 0; i < 32; i++) {
                int v = d_sb > 0 ? qnear((xb[i] + m_sb) / d_sb) : 0;
                v = v < 0 ? 0 : v > 31 ? 31 : v;
                const uint8_t nib = (uint8_t)(v & 0xF);
                if (sb % 2 == 0) qp[i] = (uint8_t)((qp[i] & 0xF0) | nib);
                else qp[i] = (uint8_t)((qp[i] & 0x0F) | (nib << 4));
                if (v & 0x10) qh[i] = (uint8_t)(qh[i] | (1 << sb));
            }
        }
    }
}

/* Q6_K: bloco de 256 = 128 ql, 64 qh, 16 escalas int8, f16 d (16 sub-blocos de 16, 6 bits) */
static void enc_q6_k(const float *x, uint8_t *dst, size_t n) {
    for (size_t b = 0; b < n / 256; b++, x += 256, dst += 210) {
        float scales[16], max_scale = 0, max_abs = 0;
        for (int ib = 0; ib < 16; ib++) {
            const float *xb = x + 16 * ib;
            float amax = 0, vmax = 0;
            for (int i = 0; i < 16; i++) { float a = fabsf(xb[i]); if (a > amax) { amax = a; vmax = xb[i]; } }
            scales[ib] = amax < 1e-30f ? 0.0f : vmax / -32.0f;
            const float as = fabsf(scales[ib]);
            if (as > max_abs) { max_abs = as; max_scale = scales[ib]; }
        }
        int8_t qsc[16];
        float d;
        if (max_abs < 1e-30f) { d = 0; memset(qsc, 0, 16); }
        else {
            const float iscale = -128.0f / max_scale;
            for (int ib = 0; ib < 16; ib++) { int l = qnear(iscale * scales[ib]); l = l < -128 ? -128 : l > 127 ? 127 : l; qsc[ib] = (int8_t)l; }
            d = max_scale / -128.0f;
        }
        const uint16_t h = f32_to_f16(d);
        const float df = f16_to_f32(h);
        int8_t L[256];
        for (int ib = 0; ib < 16; ib++) {
            const float dl = df * qsc[ib];
            const float *xb = x + 16 * ib;
            for (int i = 0; i < 16; i++) {
                int l = dl != 0 ? qnear(xb[i] / dl) : 0;
                l = l < -32 ? -32 : l > 31 ? 31 : l;
                L[16 * ib + i] = (int8_t)(l + 32);
            }
        }
        memset(dst, 0, 210);
        uint8_t *ql = dst, *qh = dst + 128;
        const int8_t *Lp = L;
        for (int half = 0; half < 2; half++, ql += 64, qh += 32, Lp += 128) {
            for (int l = 0; l < 32; l++) {
                const uint8_t q1 = Lp[l] & 0xF, q2 = Lp[l + 32] & 0xF, q3 = Lp[l + 64] & 0xF, q4 = Lp[l + 96] & 0xF;
                ql[l] = (uint8_t)(q1 | (q3 << 4));
                ql[l + 32] = (uint8_t)(q2 | (q4 << 4));
                qh[l] = (uint8_t)((Lp[l] >> 4) | ((Lp[l + 32] >> 4) << 2) | ((Lp[l + 64] >> 4) << 4) | ((Lp[l + 96] >> 4) << 6));
            }
        }
        memcpy(dst + 192, qsc, 16);
        memcpy(dst + 208, &h, 2);
    }
}

/* Q2_K: bloco de 256 = 16 escalas/minimos, 64 de 2 bits, f16 d, f16 dmin (16 sub-blocos de 16) */
static void enc_q2_k(const float *x, uint8_t *dst, size_t n) {
    for (size_t b = 0; b < n / 256; b++, x += 256, dst += 84) {
        float scales[16], mins[16], max_scale = 0, max_min = 0;
        for (int is = 0; is < 16; is++) {
            k_subscale(x + 16 * is, 16, 3, &scales[is], &mins[is]);
            if (scales[is] > max_scale) max_scale = scales[is];
            if (mins[is] > max_min) max_min = mins[is];
        }
        const float inv_s = max_scale > 0 ? 15.0f / max_scale : 0, inv_m = max_min > 0 ? 15.0f / max_min : 0;
        uint8_t s4[16], m4[16];
        for (int is = 0; is < 16; is++) {
            int s = qnear(inv_s * scales[is]); if (s < 0) s = 0; if (s > 15) s = 15; s4[is] = (uint8_t)s;
            int m = qnear(inv_m * mins[is]);   if (m < 0) m = 0; if (m > 15) m = 15; m4[is] = (uint8_t)m;
        }
        const uint16_t hd = f32_to_f16(max_scale / 15.0f), hm = f32_to_f16(max_min / 15.0f);
        const float df = f16_to_f32(hd), dmf = f16_to_f32(hm);
        memset(dst, 0, 84);
        for (int is = 0; is < 16; is++) dst[is] = (uint8_t)(s4[is] | (m4[is] << 4));
        uint8_t *q = dst + 16;
        for (int is = 0; is < 16; is++) {
            const int c = is / 8, local = is % 8, shift = (local / 2) * 2, hh = local % 2;
            const float dl = df * s4[is], ml = dmf * m4[is];
            const float *xb = x + 16 * is;
            for (int i = 0; i < 16; i++) {
                int v = dl > 0 ? qnear((xb[i] + ml) / dl) : 0;
                v = v < 0 ? 0 : v > 3 ? 3 : v;
                q[c * 32 + hh * 16 + i] = (uint8_t)(q[c * 32 + hh * 16 + i] | (v << shift));
            }
        }
        memcpy(dst + 80, &hd, 2); memcpy(dst + 82, &hm, 2);
    }
}

static void enc_block(uint32_t t, const float *x, uint8_t *dst, size_t n) {
    switch (t) {
        case T_Q8_0: enc_q8_0(x, dst, n); break;
        case T_Q6_K: enc_q6_k(x, dst, n); break;
        case T_Q5_K: enc_q5_k(x, dst, n); break;
        case T_Q4_K: enc_q4_k(x, dst, n); break;
        case T_Q2_K: enc_q2_k(x, dst, n); break;
        default: die("quantize: tipo de codificacao nao suportado");
    }
}

/* ---- auxiliares de GGUF: buffer crescente e nomes ---- */
typedef struct { uint8_t *p; size_t n, cap; } GBuf;
static void gbuf_need(GBuf *b, size_t add) {
    if (b->n + add > b->cap) { b->cap = (b->n + add) * 2 + 256; b->p = realloc(b->p, b->cap); if (!b->p) die("quantize: sem memoria"); }
}
static void gbuf_bytes(GBuf *b, const void *s, size_t k) { gbuf_need(b, k); memcpy(b->p + b->n, s, k); b->n += k; }
static void gbuf_u32(GBuf *b, uint32_t v) { gbuf_bytes(b, &v, 4); }
static void gbuf_u64(GBuf *b, uint64_t v) { gbuf_bytes(b, &v, 8); }
static void gbuf_gstr(GBuf *b, const char *s, uint64_t len) { gbuf_u64(b, len); gbuf_bytes(b, s, (size_t)len); }
static void gkv_str(GBuf *b, const char *key, const char *val) { gbuf_gstr(b, key, strlen(key)); gbuf_u32(b, 8); gbuf_gstr(b, val, strlen(val)); }
static void gkv_u32(GBuf *b, const char *key, uint32_t v) { gbuf_gstr(b, key, strlen(key)); gbuf_u32(b, 4); gbuf_u32(b, v); }

/* substring numa string sem terminador */
static int name_has(const char *s, int sl, const char *sub) {
    const int subl = (int)strlen(sub);
    for (int i = 0; i + subl <= sl; i++) if (memcmp(s + i, sub, subl) == 0) return 1;
    return 0;
}
/* compara ignorando caixa; s pode ter 'sl' chars, suf e terminado em 0 */
static int ci_eq(const char *s, int sl, const char *suf) {
    if (sl != (int)strlen(suf)) return 0;
    for (int i = 0; i < sl; i++) { char a = s[i], b = suf[i]; if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; if (a != b) return 0; }
    return 1;
}
static int ci_tail(const char *s, int sl, const char *suf) {
    const int fl = (int)strlen(suf);
    return sl >= fl && ci_eq(s + sl - fl, fl, suf);
}

/* tipo alvo a partir do nome passado na linha de comando */
static int qtype_from_name(const char *s, uint32_t *t, int *ft, int *blk) {
    static const struct { const char *n; uint32_t t; int ft; int blk; } M[] = {
        { "Q8_0", T_Q8_0, 7, 32 }, { "Q6_K", T_Q6_K, 18, 256 }, { "Q5_K", T_Q5_K, 17, 256 },
        { "Q5_K_M", T_Q5_K, 17, 256 }, { "Q5_K_S", T_Q5_K, 16, 256 }, { "Q4_K", T_Q4_K, 15, 256 },
        { "Q4_K_M", T_Q4_K, 15, 256 }, { "Q4_K_S", T_Q4_K, 14, 256 }, { "Q2_K", T_Q2_K, 10, 256 },
        { "Q2_K_M", T_Q2_K, 10, 256 } };
    const int sl = (int)strlen(s);
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (ci_eq(s, sl, M[i].n)) { *t = M[i].t; *ft = M[i].ft; *blk = M[i].blk; return 1; }
    return 0;
}

/* rotulo canonico do alvo, para o nome do arquivo e os avisos */
static const char *qtgt_label(uint32_t t) {
    switch (t) {
        case T_Q8_0: return "Q8_0"; case T_Q6_K: return "Q6_K"; case T_Q5_K: return "Q5_K_M";
        case T_Q4_K: return "Q4_K_M"; case T_Q2_K: return "Q2_K"; default: return "Q?";
    }
}

/* gera o nome de saida: basename sem o sufixo de quantizacao conhecido + -<LABEL>.gguf, na mesma pasta */
static void gen_out_name(const char *in, const char *label, char *out, size_t on) {
    static const char *toks[] = {
        "F32", "F16", "BF16", "Q8_0", "Q6_K", "Q5_K_M", "Q5_K_S", "Q5_K", "Q4_K_M", "Q4_K_S", "Q4_K",
        "Q4_0", "Q4_1", "Q5_0", "Q5_1", "Q3_K_M", "Q3_K_S", "Q3_K_L", "Q3_K", "Q2_K_S", "Q2_K",
        "IQ4_NL", "IQ4_XS", "IQ3_XXS", "IQ3_S", "IQ2_XXS", "IQ2_XS", "IQ2_S", "IQ1_S", "IQ1_M", "MXFP4" };
    const char *slash = strrchr(in, '/');
    char dir[1024]; dir[0] = 0;
    const char *base = in;
    if (slash) { size_t dl = (size_t)(slash - in) + 1; if (dl < sizeof dir) { memcpy(dir, in, dl); dir[dl] = 0; } base = slash + 1; }
    char nb[512]; snprintf(nb, sizeof nb, "%s", base);
    int bl = (int)strlen(nb);
    if (bl >= 5 && ci_tail(nb, bl, ".gguf")) { bl -= 5; nb[bl] = 0; }
    for (size_t i = 0; i < sizeof toks / sizeof toks[0]; i++) {
        const int tl = (int)strlen(toks[i]);
        if (bl > tl + 1 && (nb[bl - tl - 1] == '-' || nb[bl - tl - 1] == '.') && ci_tail(nb, bl, toks[i])) { bl -= tl + 1; nb[bl] = 0; break; }
    }
    snprintf(out, on, "%s%s-%s.gguf", dir, nb, label);
    FILE *e = fopen(out, "rb");
    if (e) {
        fclose(e);
        for (int k = 2; k < 1000; k++) {
            snprintf(out, on, "%s%s-%s-%d.gguf", dir, nb, label, k);
            e = fopen(out, "rb");
            if (!e) break;
            fclose(e);
        }
        fprintf(stderr, "quantize: aviso: o nome padrao ja existia; usando %s\n", out);
    }
}

/* tipo por tensor: nos alvos baixos, mantem embeddings/saida em precisao maior (convencao _M),
   o que melhora muito modelos pequenos sem imatrix. Fora disso, usa o alvo base. */
static uint32_t tensor_target(const TInfo *t, uint32_t base, size_t n) {
    (void)n;
    if (name_has(t->name, t->namelen, "token_embd")) {
        if (base == T_Q2_K) return T_Q4_K;
    } else if (name_has(t->name, t->namelen, "output.weight") && !name_has(t->name, t->namelen, "_exps")) {
        if (base == T_Q2_K || base == T_Q4_K || base == T_Q5_K) return T_Q6_K;
    }
    return base;
}

static int quantize_cmd(const char *in, const char *out_arg, const char *type_arg) {
    int64_t fsz = 0;
    const uint8_t *buf = map_file(in, &fsz);
    if (!buf) { fprintf(stderr, "quantize: nao consegui abrir %s\n", in); return 1; }
    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) { fprintf(stderr, "quantize: %s nao e GGUF\n", in); return 1; }
    if (rd_u32(&r) < 2) { fprintf(stderr, "quantize: versao de GGUF nao suportada\n"); return 1; }
    const uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);

    /* passo 1: pula os KV, captura arquitetura e alinhamento */
    uint64_t align = 32;
    char arch[64]; arch[0] = 0;
    const uint8_t *kv_start = r.p;
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        const uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) { int sl; const char *s = rd_str(&r, &sl); if (sl > 63) sl = 63; memcpy(arch, s, sl); arch[sl] = 0; }
        else if (t != 8 && t != 9 && key_is(k, kl, "general.alignment")) align = (uint64_t)rd_num(&r, t);
        else rd_skip(&r, t);
    }
    if (align == 0) align = 32;

    /* passo 2: tabela de tensores, parametros, se e MoE e o tipo representativo da origem */
    TInfo *ts = xalloc(n_tensors, sizeof(TInfo));
    size_t n_params = 0; int is_moe = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts[i];
        t->name = rd_str(&r, &t->namelen);
        t->ndims = rd_u32(&r);
        if (t->ndims > 4) { fprintf(stderr, "quantize: tensor com dimensoes demais\n"); return 1; }
        for (uint32_t d = 0; d < t->ndims; d++) t->dims[d] = rd_u64(&r);
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
        size_t n = 1; for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        n_params += n;
        if (!is_moe && name_has(t->name, t->namelen, "_exps")) is_moe = 1;
    }
    const uint64_t info_end = (uint64_t)(r.p - buf);
    const uint8_t *in_data = buf + (info_end + align - 1) / align * align;
    uint32_t rep = T_F16; int rep_set = 0;
    for (uint64_t i = 0; i < n_tensors && !rep_set; i++)
        if (is_moe && ts[i].ndims >= 2 && name_has(ts[i].name, ts[i].namelen, "_exps")) { rep = ts[i].type; rep_set = 1; }
    for (uint64_t i = 0; i < n_tensors && !rep_set; i++)
        if (ts[i].ndims >= 2 && name_has(ts[i].name, ts[i].namelen, "ffn_") && name_has(ts[i].name, ts[i].namelen, ".weight")) { rep = ts[i].type; rep_set = 1; }
    for (uint64_t i = 0; i < n_tensors && !rep_set; i++)
        if (ts[i].ndims >= 2) { rep = ts[i].type; rep_set = 1; }

    printf("quantize: %s  (arquitetura=%s%s, %.1fM parametros, origem %s ~%.2f bits/peso, %.2f GB)\n",
           in, arch[0] ? arch : "?", is_moe ? " MoE" : "", n_params / 1e6, qtype_name(rep), bpw_fmt(rep), fsz / 1073741824.0);

    /* decide o tipo alvo */
    uint32_t tgt; int ft = 0, blk = 0;
    if (type_arg) {
        if (!qtype_from_name(type_arg, &tgt, &ft, &blk)) {
            fprintf(stderr, "quantize: tipo '%s' nao suportado. Use Q8_0, Q6_K, Q5_K(_M), Q4_K(_M) ou Q2_K.\n", type_arg);
            return 1;
        }
        printf("Tipo alvo: %s (escolhido pelo usuario)\n", qtgt_label(tgt));
        if (bpw_fmt(tgt) >= bpw_fmt(rep) - 1e-6)
            printf("  aviso: a origem (~%.2f bits/peso) nao e maior que o alvo; o tamanho pode nao diminuir (os tensores menores sao copiados)\n", bpw_fmt(rep));
    } else {
        printf("Escolhendo o tipo para esta maquina (RTX 3050 4 GB, ~43 GB RAM util; estimativa):\n");
        const uint32_t s = recomendar_quant(n_params, is_moe, rep);
        if (s == 0) { free(ts); return 0; }
        qtype_from_name(qtgt_label(s), &tgt, &ft, &blk);
    }
    const char *label = qtgt_label(tgt);

    /* nome de saida */
    char outbuf[2048];
    const char *out;
    if (out_arg) {
        out = out_arg;
        FILE *ex = fopen(out, "rb");
        if (ex) { fclose(ex); fprintf(stderr, "quantize: %s ja existe; escolha outro nome.\n", out); free(ts); return 1; }
    } else {
        gen_out_name(in, label, outbuf, sizeof outbuf);
        out = outbuf;
    }
    printf("Saida: %s\n", out);

    /* passo 3: KV de saida (substitui file_type; carimba quantized_by/author/quantization_version) */
    GBuf kv = {0}; uint64_t nkv_out = 0;
    Rd r2 = { kv_start, buf + fsz };
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r2, &kl);
        const uint32_t t = rd_u32(&r2);
        if ((t != 8 && t != 9 && key_is(k, kl, "general.file_type")) ||
            key_is(k, kl, "general.quantized_by") || key_is(k, kl, "general.author") ||
            (t != 8 && t != 9 && key_is(k, kl, "general.quantization_version"))) {
            rd_skip(&r2, t);
            continue;
        }
        const uint8_t *vs = r2.p; rd_skip(&r2, t);
        gbuf_gstr(&kv, k, (uint64_t)kl); gbuf_u32(&kv, t); gbuf_bytes(&kv, vs, (size_t)(r2.p - vs));
        nkv_out++;
    }
    gkv_u32(&kv, "general.file_type", (uint32_t)ft);
    gkv_u32(&kv, "general.quantization_version", 2);
    gkv_str(&kv, "general.quantized_by", "katana");
    gkv_str(&kv, "general.author", "Ale Simoes (alesimoes)");
    nkv_out += 4;

    /* passo 4: decide por tensor e monta a info de saida com os novos deslocamentos */
    GBuf tib = {0};
    int *req = xalloc(n_tensors, sizeof(int));
    uint32_t *otype = xalloc(n_tensors, 4);
    uint64_t *new_off = xalloc(n_tensors, 8);
    uint64_t data_off = 0;
    int requant_plan = 0;
    (void)blk;
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts[i];
        size_t n = 1; for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        uint32_t ot = t->type; int rq = 0;
        if (t->ndims >= 2) {
            const uint32_t cand = tensor_target(t, tgt, n);
            const uint64_t cblk = (uint64_t)qunit(cand);
            if ((t->dims[0] % cblk) == 0 && (n % (size_t)cblk) == 0 && qbytes(t->type, n)
                && bpw_fmt(cand) < bpw_fmt(t->type) - 1e-6) { ot = cand; rq = 1; requant_plan++; }
        }
        otype[i] = ot;
        const size_t ob = rq ? qbytes(ot, n) : qbytes(t->type, n);
        if (ob == 0) { fprintf(stderr, "quantize: nao sei o tamanho do tensor %.*s (tipo %u)\n", t->namelen, t->name, t->type); return 1; }
        data_off = (data_off + align - 1) / align * align;
        new_off[i] = data_off; req[i] = rq;
        data_off += ob;
        gbuf_gstr(&tib, t->name, (uint64_t)t->namelen); gbuf_u32(&tib, t->ndims);
        for (uint32_t d = 0; d < t->ndims; d++) gbuf_u64(&tib, t->dims[d]);
        gbuf_u32(&tib, ot); gbuf_u64(&tib, new_off[i]);
    }

    /* passo 5: grava */
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "quantize: nao consegui gravar %s\n", out); return 1; }
    uint8_t hdr[24];
    const uint32_t magic = 0x46554747u, v3 = 3;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &v3, 4); memcpy(hdr + 8, &n_tensors, 8); memcpy(hdr + 16, &nkv_out, 8);
    fwrite(hdr, 1, 24, f);
    fwrite(kv.p, 1, kv.n, f);
    fwrite(tib.p, 1, tib.n, f);
    const uint64_t hpos = 24 + kv.n + tib.n;
    const uint64_t dstart = (hpos + align - 1) / align * align;
    for (uint64_t p = hpos; p < dstart; p++) fputc(0, f);
    uint64_t cur = 0; int requant_count = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        while (cur < new_off[i]) { fputc(0, f); cur++; }
        size_t n = 1; for (uint32_t d = 0; d < ts[i].ndims; d++) n *= (size_t)ts[i].dims[d];
        const uint8_t *src = in_data + ts[i].offset;
        if (req[i]) {
            const size_t ob = qbytes(otype[i], n);
            float *fx = xalloc(n, sizeof(float));
            dequant(ts[i].type, src, fx, n);
            uint8_t *o = xalloc(ob, 1);
            enc_block(otype[i], fx, o, n);
            fwrite(o, 1, ob, f);
            free(fx); free(o);
            cur = new_off[i] + ob; requant_count++;
        } else {
            const size_t ob = qbytes(ts[i].type, n);
            fwrite(src, 1, ob, f);
            cur = new_off[i] + ob;
        }
    }
    fclose(f);
    struct stat st;
    const double outgb = (stat(out, &st) == 0) ? st.st_size / 1073741824.0 : 0;
    printf("Pronto: %llu tensores, %d requantizados para %s (resto copiado). Tamanho %.2f GB -> %.2f GB (%.0f%%). "
           "Metadados: author=\"Ale Simoes (alesimoes)\", quantized_by=\"katana\".\n",
           (unsigned long long)n_tensors, requant_count, label, fsz / 1073741824.0, outgb,
           fsz ? outgb * 1073741824.0 * 100.0 / (double)fsz : 0);
    (void)requant_plan;
    free(ts); free(req); free(otype); free(new_off); free(kv.p); free(tib.p);
    return 0;
}


/* katana merge <modelo.gguf> <espec.kspec> <saida.gguf>: grava um GGUF novo com a especializacao dentro (para levar
   o modelo especializado para outra maquina ou outro programa, sem o .kspec). Tensores inteiros substituem o
   original; adaptadores viram W + s B A; especialistas treinados trocam as linhas deles no tensor empilhado. Cada
   tensor alterado volta no tipo original quando o katana sabe codificar esse tipo (F32, F16, Q8_0, Q6_K, Q5_K, Q4_K,
   Q2_K); senao vai em Q8_0 (ou F16 se as colunas nao fecham blocos de 32). O resto e copiado byte a byte. */
typedef struct { char name[128]; uint32_t kind; uint64_t rows, cols; int32_t rank; float s; float *w, *a, *b; } MSpec;

static int merge_cmd(const char *in, const char *spec, const char *out) {
    FILE *ex = fopen(out, "rb");
    if (ex) { fclose(ex); fprintf(stderr, "merge: %s ja existe; escolha outro nome.\n", out); return 1; }
    /* especializacao */
    FILE *fs = fopen(spec, "rb");
    if (!fs) { fprintf(stderr, "merge: nao abriu %s\n", spec); return 1; }
    char mg[8]; int64_t sfs = 0; uint64_t snp = 0; int ns = 0;
    if (fread(mg, 1, 8, fs) != 8 || memcmp(mg, KSPEC_MAGIC, 8) || fread(&sfs, 8, 1, fs) != 1 || fread(&snp, 8, 1, fs) != 1 || fread(&ns, 4, 1, fs) != 1 || ns < 0 || ns > 1000000)
        { fprintf(stderr, "merge: %s nao e uma especializacao valida\n", spec); return 1; }
    MSpec *sp = xalloc((size_t)ns + 1, sizeof(MSpec));
    for (int i = 0; i < ns; i++) {
        MSpec *p = &sp[i]; uint32_t nl = 0;
        if (fread(&nl, 4, 1, fs) != 1 || nl == 0 || nl >= sizeof p->name || fread(p->name, 1, nl, fs) != nl || fread(&p->kind, 4, 1, fs) != 1
            || fread(&p->rows, 8, 1, fs) != 1 || fread(&p->cols, 8, 1, fs) != 1) { fprintf(stderr, "merge: especializacao cortada\n"); return 1; }
        p->name[nl] = 0;
        const size_t n = (size_t)(p->rows * p->cols);
        if (p->kind == 1) {
            if (fread(&p->rank, 4, 1, fs) != 1 || fread(&p->s, 4, 1, fs) != 1 || p->rank < 1 || p->rank > 4096) { fprintf(stderr, "merge: adaptador invalido\n"); return 1; }
            p->a = xalloc((size_t)p->rank * p->cols, 4); p->b = xalloc((size_t)p->rows * p->rank, 4);
            if (fread(p->a, 4, (size_t)p->rank * p->cols, fs) != (size_t)p->rank * p->cols || fread(p->b, 4, (size_t)(p->rows * p->rank), fs) != (size_t)(p->rows * p->rank))
                { fprintf(stderr, "merge: especializacao cortada\n"); return 1; }
        } else {
            p->w = xalloc(n, 4);
            if (fread(p->w, 4, n, fs) != n) { fprintf(stderr, "merge: especializacao cortada\n"); return 1; }
        }
    }
    fclose(fs);
    /* modelo base */
    int64_t fsz = 0;
    const uint8_t *buf = map_file(in, &fsz);
    if (!buf) { fprintf(stderr, "merge: nao abriu %s\n", in); return 1; }
    if (fsz != sfs) { fprintf(stderr, "merge: a especializacao foi treinada sobre outro modelo base (tamanho do arquivo nao confere)\n"); return 1; }
    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u || rd_u32(&r) < 2) { fprintf(stderr, "merge: %s nao e GGUF\n", in); return 1; }
    const uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);
    uint64_t align = 32;
    const uint8_t *kv_start = r.p;
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        const uint32_t t = rd_u32(&r);
        if (t != 8 && t != 9 && key_is(k, kl, "general.alignment")) align = (uint64_t)rd_num(&r, t);
        else rd_skip(&r, t);
    }
    if (!align) align = 32;
    const uint8_t *kv_end = r.p;
    TInfo *ts = xalloc(n_tensors, sizeof(TInfo));
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts[i];
        t->name = rd_str(&r, &t->namelen); t->ndims = rd_u32(&r);
        if (t->ndims > 4) { fprintf(stderr, "merge: tensor com dimensoes demais\n"); return 1; }
        for (uint32_t d = 0; d < t->ndims; d++) t->dims[d] = rd_u64(&r);
        t->type = rd_u32(&r); t->offset = rd_u64(&r);
    }
    const uint64_t info_end = (uint64_t)(r.p - buf);
    const uint8_t *in_data = buf + (info_end + align - 1) / align * align;
    /* tipo de saida e tamanho de cada tensor */
    int *touch = xalloc(n_tensors, sizeof(int));
    uint32_t *otype = xalloc(n_tensors, 4);
    uint64_t *off = xalloc(n_tensors, 8), data = 0;
    int used = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts[i];
        size_t n = 1; for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        for (int j = 0; j < ns; j++) {
            const size_t ln = (size_t)t->namelen;
            if ((strlen(sp[j].name) == ln && !memcmp(sp[j].name, t->name, ln)) ||
                (strlen(sp[j].name) > ln + 2 && !memcmp(sp[j].name, t->name, ln) && !strncmp(sp[j].name + ln, ".e", 2))) { touch[i] = 1; break; }
        }
        uint32_t ot = t->type;
        if (touch[i]) {
            const int enc = ot == T_F32 || ot == T_F16 || ot == T_Q8_0 || ot == T_Q6_K || ot == T_Q5_K || ot == T_Q4_K || ot == T_Q2_K;
            const uint64_t cols = t->dims[0];
            if (!enc || (ot != T_F32 && ot != T_F16 && cols % (uint64_t)qunit(ot))) ot = cols % 32 == 0 ? T_Q8_0 : T_F16;
        }
        otype[i] = ot;
        data = (data + align - 1) / align * align;
        off[i] = data;
        data += qbytes(ot, n);
    }
    /* grava: KV iguais, mais quem especializou; info com os novos tipos e deslocamentos; dados */
    GBuf kv = {0}; uint64_t nkv = 0;
    Rd r2 = { kv_start, kv_end };
    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r2, &kl);
        const uint32_t t = rd_u32(&r2);
        const uint8_t *vs = r2.p; rd_skip(&r2, t);
        if (key_is(k, kl, "katana.specialization") || key_is(k, kl, "katana.no_tool_decl")) continue;
        gbuf_gstr(&kv, k, (uint64_t)kl); gbuf_u32(&kv, t); gbuf_bytes(&kv, vs, (size_t)(r2.p - vs)); nkv++;
    }
    {
        const char *b = strrchr(spec, '/'); b = b ? b + 1 : spec;
        gkv_str(&kv, "katana.specialization", b); nkv++;
        if (spec_flags(spec) & KSPEC_NO_TOOL_DECL) { gkv_str(&kv, "katana.no_tool_decl", "1"); nkv++; }   /* o modelo fundido roda sem a secao de ferramentas */
    }
    GBuf tib = {0};
    for (uint64_t i = 0; i < n_tensors; i++) {
        gbuf_gstr(&tib, ts[i].name, (uint64_t)ts[i].namelen); gbuf_u32(&tib, ts[i].ndims);
        for (uint32_t d = 0; d < ts[i].ndims; d++) gbuf_u64(&tib, ts[i].dims[d]);
        gbuf_u32(&tib, otype[i]); gbuf_u64(&tib, off[i]);
    }
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "merge: nao consegui gravar %s\n", out); return 1; }
    uint8_t hdr[24];
    const uint32_t magic = 0x46554747u, v3 = 3;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &v3, 4); memcpy(hdr + 8, &n_tensors, 8); memcpy(hdr + 16, &nkv, 8);
    fwrite(hdr, 1, 24, f); fwrite(kv.p, 1, kv.n, f); fwrite(tib.p, 1, tib.n, f);
    const uint64_t hpos = 24 + kv.n + tib.n, dstart = (hpos + align - 1) / align * align;
    for (uint64_t p = hpos; p < dstart; p++) fputc(0, f);
    uint64_t cur = 0;
    for (uint64_t i = 0; i < n_tensors; i++) {
        while (cur < off[i]) { fputc(0, f); cur++; }
        TInfo *t = &ts[i];
        size_t n = 1; for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        const uint8_t *src = in_data + t->offset;
        const size_t ob = qbytes(otype[i], n);
        if (!touch[i]) { fwrite(src, 1, ob, f); cur = off[i] + ob; continue; }
        float *x = xalloc(n, 4);
        dequant(t->type, src, x, n);
        const size_t cols = (size_t)t->dims[0], ln = (size_t)t->namelen;
        for (int j = 0; j < ns; j++) {
            MSpec *p = &sp[j];
            size_t row0 = 0;
            if (strlen(p->name) == ln && !memcmp(p->name, t->name, ln)) row0 = 0;
            else if (strlen(p->name) > ln + 2 && !memcmp(p->name, t->name, ln) && !strncmp(p->name + ln, ".e", 2)) {
                char *end = NULL;
                const size_t e = (size_t)strtoul(p->name + ln + 2, &end, 10);
                /* gemma4: gate e up de cada especialista no mesmo tensor, gate nas linhas 2eH e up nas (2e+1)H */
                if (end && !strcmp(end, ".gate")) row0 = 2 * e * (size_t)p->rows;
                else if (end && !strcmp(end, ".up")) row0 = (2 * e + 1) * (size_t)p->rows;
                else row0 = e * (size_t)p->rows;
            } else continue;
            if (p->rows == 1 && p->kind == 0 && p->cols == n && row0 == 0) { memcpy(x, p->w, n * 4); used++; continue; }   /* vetor (normas, convolucao...) */
            if (p->cols != cols || row0 + p->rows > n / cols) { fprintf(stderr, "merge: %s nao confere com o tensor do modelo\n", p->name); return 1; }
            float *w = x + row0 * cols;
            if (p->kind == 0) memcpy(w, p->w, (size_t)(p->rows * p->cols) * 4);
            else for (uint64_t rr = 0; rr < p->rows; rr++) for (int k = 0; k < p->rank; k++) {
                const float bk = p->s * p->b[rr * p->rank + k];
                if (bk == 0.0f) continue;
                const float *ak = p->a + (size_t)k * p->cols;
                for (uint64_t c2 = 0; c2 < p->cols; c2++) w[rr * cols + c2] += bk * ak[c2];
            }
            used++;
        }
        uint8_t *o = xalloc(ob, 1);
        if (otype[i] == T_F32) memcpy(o, x, n * 4);
        else if (otype[i] == T_F16) { uint16_t *h = (uint16_t *)o; for (size_t q = 0; q < n; q++) h[q] = _cvtss_sh(x[q], 0); }
        else enc_block(otype[i], x, o, n);
        fwrite(o, 1, ob, f);
        free(o); free(x);
        cur = off[i] + ob;
    }
    fclose(f);
    int nt = 0; for (uint64_t i = 0; i < n_tensors; i++) nt += touch[i];
    printf("merge: %s + %s -> %s (%d tensores alterados, %d partes da especializacao aplicadas)\n", in, spec, out, nt, used);
    if (used != ns) printf("merge: aviso: %d partes da especializacao nao acharam tensor no modelo\n", ns - used);
    return used == ns ? 0 : 1;
}

/* katana calibrar <modelo> [arquivo de texto]: passa textos variados (conversa em portugues e ingles, codigo, perguntas
   de ferramentas) token a token e grava como .heat a media do calor de cada especialista ao longo de todos os tokens.
   Assim a primeira sessao ja comeca com os especialistas mais usados nas vagas da VRAM (o .heat normal guarda so o
   calor do fim da ultima sessao). Sem cache de especialistas (modelo cabe inteiro, ou so CPU) nao ha o que gravar. */
static int calibrar(Model *m, const char *file) {
    const Config *c = &m->cfg;
    static const char *txt[] = {
        "Voce e um assistente util. Usuario: Pode me explicar como funciona a fotossintese e por que as folhas sao verdes? "
        "Assistente: Claro. A fotossintese e o processo em que as plantas usam a luz do sol, a agua e o gas carbonico para produzir "
        "acucar e liberar oxigenio. A clorofila absorve sobretudo o azul e o vermelho e reflete o verde, por isso as folhas parecem verdes.",
        "User: Write a Python function that reads a CSV file and returns the average of the column 'price'.\nAssistant: ```python\n"
        "import csv\n\ndef average_price(path):\n    total, n = 0.0, 0\n    with open(path, newline='') as f:\n        for row in csv.DictReader(f):\n"
        "            total += float(row['price'])\n            n += 1\n    return total / n if n else 0.0\n```\nThis opens the file, sums the prices and divides by the count.",
        "Usuario: liste os arquivos da pasta atual e me diga qual e o maior.\nAssistente: <tool_call>\n{\"name\": \"list_dir\", \"arguments\": {\"path\": \".\"}}\n</tool_call>\n"
        "Resultado: README.md 4 KB, src/ (pasta), build.sh 1 KB, dados.csv 2,3 MB.\nAssistente: O maior arquivo e dados.csv, com 2,3 MB.",
        "Usuario: Qual a diferenca entre memoria RAM e armazenamento? Responda de forma curta.\nAssistente: A RAM e rapida e temporaria: guarda o que o "
        "computador esta usando agora e se apaga ao desligar. O armazenamento (SSD ou HD) e mais lento, porem permanente: guarda arquivos e programas.",
        "User: Summarize the main causes of the French Revolution in three bullet points.\nAssistant: - Fiscal crisis: debt from wars and an unfair tax system.\n"
        "- Social inequality: the Third Estate carried the burden while clergy and nobility kept privileges.\n- Enlightenment ideas questioning absolute monarchy.",
        "Usuario: escreva um script em bash que conte quantas linhas tem cada arquivo .c da pasta src.\nAssistente: ```bash\nfor f in src/*.c; do\n  printf '%s %s\\n' \"$(wc -l < \"$f\")\" \"$f\"\ndone | sort -n\n```",
    };
    const int NL = c->n_layers, E = c->n_expert;
    if (!gx.on || !gx.heat) { fprintf(stderr, "calibrar: sem cache de especialistas neste modelo/maquina; nada a gravar\n"); return 1; }
    double *acc = xalloc((size_t)NL * E, sizeof(double));
    long ntok = 0;
    char *ext = NULL;
    if (file) { FILE *f = fopen(file, "rb"); if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); ext = xalloc((size_t)n + 1, 1); if (fread(ext, 1, (size_t)n, f) != (size_t)n) n = 0; ext[n] = 0; fclose(f); } }
    const int nt = ext ? 1 : (int)(sizeof txt / sizeof txt[0]);
    const double t0 = now_sec();
    for (int k = 0; k < nt; k++) {
        Ids ids = {0};
        if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
        encode(&m->tk, ext ? ext : txt[k], &ids);
        memset(gx.heat, 0, (size_t)NL * E * sizeof(float));
        for (int i = 0; i < ids.n && i < c->ctx - 1; i++) {
            forward(m, ids.ids[i], i, 0);
            for (size_t j = 0; j < (size_t)NL * E; j++) acc[j] += gx.heat[j];
            ntok++;
        }
        fprintf(stderr, "calibrar: texto %d de %d (%d tokens)\n", k + 1, nt, ids.n);
        free(ids.ids);
    }
    for (size_t j = 0; j < (size_t)NL * E; j++) gx.heat[j] = (float)(acc[j] / (double)(ntok ? ntok : 1));
    heat_save(m);
    char fn[1100]; heat_path(fn, sizeof fn);
    fprintf(stderr, "calibrar: %ld tokens em %.1f s; calor medio gravado em %s\n", ntok, now_sec() - t0, fn);
    free(acc); free(ext);
    return 0;
}

static int run_main(int argc, char **argv) {
    { const int lr = launch_try(argc, argv); if (lr >= 0) return lr; }        /* katana sem subcomando: console do agente; install, adopt (app/launch.c) */
    g_orig_argc = argc;                    /* copia da linha de comando: o agente pode se recarregar (agent_regrow) */
    g_orig_argv = xalloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_orig_argv[i] = argv[i];
    for (int i = 2; i + 1 < argc; ) {      /* sessoes do agente: --retomar <arquivo>, --sessao <arquivo> */
        if (strcmp(argv[i], "--retomar") == 0 || strcmp(argv[i], "--sessao") == 0) {
            snprintf(argv[i][2] == 'r' ? g_resume_path : g_sess_path, sizeof g_sess_path, "%s", argv[i + 1]);
            for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
            argc -= 2;
        } else i++;
    }
    if (argc >= 3 && strcmp(argv[1], "serve") == 0)       /* --agent: o agente pela rede; o modelo fica so no agente filho */
        for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--agent") == 0) { g_srv_agent = 1; for (int j = i; j < argc; j++) argv[j] = argv[j + 1]; argc--; break; }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "serve") == 0) {
        Model m; memset(&m, 0, sizeof m);
        if (g_srv_agent) { if (!realpath(argv[2], g_srv_model_path)) die("katana serve --agent: modelo nao encontrado"); }
        else { perfil_apply(argv[2], 0); load_model(&m, argv[2]); print_info(&m); }
        {   /* nome do modelo na API: o arquivo sem pasta e sem .gguf */
            const char *b = argv[2], *sl = strrchr(b, '/'), *bs = strrchr(b, '\\');
            if (bs && (!sl || bs > sl)) sl = bs;
            if (sl) b = sl + 1;
            snprintf(g_srv_name, sizeof g_srv_name, "%s", b);
            const size_t ln = strlen(g_srv_name);
            if (ln > 5 && strcmp(g_srv_name + ln - 5, ".gguf") == 0) g_srv_name[ln - 5] = 0;
        }
        return serve(g_srv_agent ? NULL : &m, argc == 4 ? atoi(argv[3]) : 8080);
    }
    if (argc >= 2 && strcmp(argv[1], "mcp") == 0) return mcp_cli(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "skill") == 0) return skill_cli(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "init") == 0) return init_cli(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "brief") == 0) return brief_cli(argc - 2, argv + 2);
    if (argc == 2 && strcmp(argv[1], "jsontest") == 0) return jsontest();
    if (argc == 2 && strcmp(argv[1], "calltest") == 0) return calltest();
    if (argc == 2 && strcmp(argv[1], "toolrun") == 0) {      /* teste: le linhas "<ferramenta> <json>" da entrada e executa em sequencia, no mesmo processo (sessoes de terminal persistem) */
        static char res[TOOL_WEB + 512], ln[20000];
        g_web = !(getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0);
        while (fgets(ln, sizeof ln, stdin)) {
            ln[strcspn(ln, "\r\n")] = 0;
            char *sp = strchr(ln, ' ');
            if (!sp) continue;
            *sp = 0;
            agent_tool(ln, sp + 1, sp + 1 + strlen(sp + 1), res, sizeof res);
            printf("%s\n----\n", res);
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "tool") == 0) {         /* teste: executa uma ferramenta do agente sem modelo */
        static char res[TOOL_WEB + 512];
        g_web = !(getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0);
        agent_tool(argv[2], argv[3], argv[3] + strlen(argv[3]), res, sizeof res);
        printf("%s\n", res);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "agentsys") == 0) {      /* teste: imprime o prompt de sistema do agente; formato: qwen (XML, padrao), json ou gemma4; "code": o de katana code */
        static char sys[32000];
        const char *fmt = argc >= 3 && strcmp(argv[2], "code") ? argv[2] : "qwen";
        g_code = (argc >= 3 && !strcmp(argv[2], "code")) || (argc >= 4 && !strcmp(argv[3], "code"));
        g_web = 1; g_tool_xml = strcmp(fmt, "json") != 0; g_tool_glm = strcmp(fmt, "glm") == 0;
        agent_build_sys(sys, sizeof sys, "");
        mcp_init(0); mcp_sys(sys, sizeof sys);       /* como o laco do agente: os cartoes dos servidores MCP configurados */
        if (strcmp(fmt, "gemma4") == 0) g4_agent_sys(sys, sizeof sys);
        printf("%s\n[%zu bytes]\n", sys, strlen(sys));
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "toolparse") == 0) {     /* teste: converte uma chamada (formato "xml" ou "gemma4") em nome + JSON e executa a ferramenta */
        static char nm[128], js[16384], res[TOOL_WEB + 512];
        const char *tc = argv[3];
        const int ok = strcmp(argv[2], "gemma4") == 0 ? g4_call_parse(tc, tc + strlen(tc), nm, sizeof nm, js, sizeof js)
                     : strcmp(argv[2], "glm") == 0 ? glm_call_parse(tc, tc + strlen(tc), nm, sizeof nm, js, sizeof js)
                                                       : tool_xml_parse(tc, tc + strlen(tc), nm, sizeof nm, js, sizeof js);
        if (!ok) { printf("nao e uma chamada\n"); return 1; }
        printf("nome=%s\n%s\n---\n", nm, js);
        g_web = 1;
        agent_tool(nm, js, js + strlen(js), res, sizeof res);
        printf("%s\n", res);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "calibrar") == 0) {    /* calor padrao de especialistas (.heat) a partir de textos variados */
        perfil_apply(argv[2], 1);
        if (!getenv("KATANA_CTX")) putenv("KATANA_CTX=2048");
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return calibrar(&m, argc > 3 ? argv[3] : NULL);
    }
    if (argc == 3 && (strcmp(argv[1], "agent") == 0 || strcmp(argv[1], "code") == 0)) {
        g_code = strcmp(argv[1], "code") == 0;      /* modo de programacao: prompt e ferramentas enxutos */
        perfil_apply(argv[2], 1);
        /* resultados de ferramentas ocupam muito contexto: 131072. Qwen3.6 (10 camadas de atencao) e gemma4 (anel de
           janela nas 25 locais, contexto so nas 5 globais) ficam em ~2,7-2,9 GB de KV fp16 a 128k */
        if (!getenv("KATANA_CTX")) putenv("KATANA_CTX=131072");
        if (!getenv("KATANA_THINK")) putenv("KATANA_THINK=0");     /* agente responde direto; KATANA_THINK=1 liga o raciocinio */
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        if (!m.cfg.tool_tmpl)
            fprintf(stderr, "Aviso: este modelo nao foi treinado para chamar ferramentas (o template dele nao tem ferramentas);\n"
                            "  ele pode ignorar a internet e os arquivos. Para o agente, prefira um modelo com ferramentas (ex.: Qwen3.6).\n");
        return agent(&m, getenv("KATANA_MAXTOK") ? atoi(getenv("KATANA_MAXTOK")) : 32768,
                     getenv("KATANA_TEMP") ? (float)atof(getenv("KATANA_TEMP")) : 0.3f);
    }
    if (argc >= 5 && strcmp(argv[1], "routing") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        print_info(&m);
        return routing(&m, argv[3], argv[4]);
    }
    if (argc >= 5 && strcmp(argv[1], "sparsity") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return sparsity(&m, argv[3], argv[4]);
    }
    if (argc >= 3 && strcmp(argv[1], "tune") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return tune(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "verify") == 0) return verify(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "pack") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return pack(&m, argv[2], m.fsize);
    }
    if (argc >= 3 && strcmp(argv[1], "prepare") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return prepare(&m, argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "micro") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return micro(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "vkmicro") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_BACKEND=vulkan");
        load_model(&m, argv[2]);
        return vkmicro(&m);
    }
    gpu.debug = getenv("KATANA_DEBUG") != NULL;
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return bench(&m, argc > 3 ? atoi(argv[3]) : 256, argc > 4 ? atoi(argv[4]) : 64);
    }
    if (argc >= 4 && strcmp(argv[1], "vkcheck") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return vkcheck(&m, argv[3], argc > 4 ? atoi(argv[4]) : 40, argc > 5 ? atoi(argv[5]) : 600);
    }
    if (argc >= 2 && strcmp(argv[1], "qtest") == 0) return qtest();
    if (argc >= 2 && strcmp(argv[1], "lbtest") == 0) return lowbit_test(argc - 2, argv + 2);
    if (strcmp(argv[1], "quantize") == 0) {
        if (argc < 3) { fprintf(stderr, "uso: katana quantize <entrada.gguf> [saida.gguf] [tipo]\n"); return 1; }
        const char *in = argv[2], *out = NULL, *type = NULL;
        if (argc >= 5) { out = argv[3]; type = argv[4]; }
        else if (argc == 4) {
            uint32_t tt; int ft, blk;
            if (qtype_from_name(argv[3], &tt, &ft, &blk)) type = argv[3];   /* quantize entrada tipo */
            else out = argv[3];                                             /* quantize entrada saida */
        }
        return quantize_cmd(in, out, type);
    }
    if (argc >= 3 && strcmp(argv[1], "knowledge") == 0) return kn_main(argc - 2, argv + 2);                  /* notas das sessoes anteriores */
    if (argc >= 5 && strcmp(argv[1], "import") == 0) return import_main(argv[2], argc - 3, argv + 3);    /* conversas -> dados de treino */
    if (argc == 5 && strcmp(argv[1], "merge") == 0) return merge_cmd(argv[2], argv[3], argv[4]);          /* GGUF novo com a especializacao */
    if (argc >= 3 && strcmp(argv[1], "distill") == 0) return distill_main(argc - 2, argv + 2);         /* professor -> sessoes verificaveis */
    if (argc >= 4 && strcmp(argv[1], "tool") == 0) return ct_main(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "agentlog") == 0) return agentlog_main(argc - 2, argv + 2);       /* leitura do registro de acoes do agente */                  /* roda ferramentas de src/tools/ sem modelo */
    if (argc >= 3 && strcmp(argv[1], "export") == 0) return export_main(argc - 2, argv + 2);           /* sessoes do agente -> dados de treino */
    if (argc >= 3 && strcmp(argv[1], "learn") == 0) return learn_main(argv[0], argc - 2, argv + 2);    /* coleta, filtra, treina e prova, em rodadas */
    if (argc >= 3 && strcmp(argv[1], "curriculum") == 0) return curriculum_main(argc - 2, argv + 2);   /* convencoes -> dados de treino */
    if (argc >= 3 && strcmp(argv[1], "specialize") == 0) return specialize_main(argv[0], argc - 2, argv + 2);   /* versao especialista de ponta a ponta */
    if (argc >= 2 && strcmp(argv[1], "kerneltest") == 0) return argc > 2 && strcmp(argv[2], "bench") == 0 ? tk_bench(argc > 3 ? atoi(argv[3]) : 4, 2000) : tk_selftest();     /* treino: kernels da CPU x lacos simples, formatos reais */
    if (argc >= 3 && strcmp(argv[1], "gradcheck") == 0) {     /* treino: forward de treino x motor e gradientes x diferencas finitas */
        Model m; memset(&m, 0, sizeof m);
        putenv_dup("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return train_gradcheck(&m, argc > 3 ? atoi(argv[3]) : 12);
    }
    if (argc >= 4 && strcmp(argv[1], "train") == 0) {         /* especializacao: grava um .kspec */
        Model m; memset(&m, 0, sizeof m);
        putenv_dup("KATANA_CPU=1");
        putenv_dup("KATANA_SPEC=");
        load_model(&m, argv[2]);
        print_info(&m);
        return train_main(&m, argv[2], argc - 3, argv + 3);
    }
    if (argc == 4 && strcmp(argv[1], "q4check") == 0) {      /* qwen4exp contra estados e logits de referencia (tools/tinymodel/qwen4exp_ref.py) */
        static Model m;
        load_model(&m, argv[2]);
        return q4_check(&m, argv[3]);
    }
    if (argc >= 3 && strcmp(argv[1], "selftest") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        if (argc >= 4 && strcmp(argv[3], "--deep") == 0) return selftest_deep(&m, argc - 4, argv + 4);
        return selftest(&m);
    }
    if (argc >= 4 && strcmp(argv[1], "toktest") == 0) {     /* teste dourado do tokenizador (tools/tokref) */
        g_vocab_only = 1;
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return toktest(&m, argv[3], argc > 4 && strcmp(argv[4], "--chat") == 0);
    }
    if (argc >= 3 && strcmp(argv[1], "tokenize") == 0) {    /* contagem de tokens de texto ou conversa, sem os pesos */
        g_vocab_only = 1;
        const char *src = NULL; int ids = 0;
        for (int i = 3; i < argc; i++) { if (strcmp(argv[i], "--ids") == 0) ids = 1; else src = argv[i]; }
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return tokenize_cmd(&m, src, ids);
    }
    if (strcmp(argv[1], "hw") == 0) return hw_cmd(argc >= 3 ? argv[2] : models_dir());
    if (strcmp(argv[1], "catalog") == 0) return catalog_cmd(argc >= 3 ? argv[2] : models_dir());
    if (argc >= 3 && strcmp(argv[1], "pull") == 0) return pull_cmd(argv[2], argc >= 4 ? argv[3] : models_dir());
    if (argc >= 3 && strcmp(argv[1], "sha256") == 0) { char hx[65]; if (!sha_file(argv[2], hx, 0)) return 1; printf("%s\n", hx); return 0; }
    if (strcmp(argv[1], "models") == 0 || strcmp(argv[1], "modelos") == 0) {
        return modelos_cmd(argc >= 3 ? argv[2] : models_dir());
    }
    if (argc >= 3 && strcmp(argv[1], "info") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "chat") == 0) {       /* sem pergunta: conversa interativa */
        Model m; memset(&m, 0, sizeof m);
        perfil_apply(argv[2], 0);
        load_model(&m, argv[2]);
        print_info(&m);
        return session(&m, getenv("KATANA_SYSTEM"), getenv("KATANA_MAXTOK") ? atoi(getenv("KATANA_MAXTOK")) : 8192,
                       getenv("KATANA_TEMP") ? (float)atof(getenv("KATANA_TEMP")) : 0.7f);
    }
    if (argc >= 4 && (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "chat") == 0)) {
        Model m; memset(&m, 0, sizeof m);
        perfil_apply(argv[2], 0);
        load_model(&m, argv[2]);
        print_info(&m);
        int max_tokens = argc > 4 ? atoi(argv[4]) : 200;
        float temp = argc > 5 ? (float)atof(argv[5]) : 0.7f;
        if (argv[3][0] == '@') {      /* prompt lido de arquivo (prompts longos nao cabem na linha de comando) */
            FILE *pf = fopen(argv[3] + 1, "rb");
            if (!pf) die("nao abriu o arquivo do prompt");
            fseek(pf, 0, SEEK_END); const long n = ftell(pf); fseek(pf, 0, SEEK_SET);
            char *t = xalloc((size_t)n + 1, 1);
            if (fread(t, 1, (size_t)n, pf) != (size_t)n) die("falha lendo o prompt");
            fclose(pf); t[n] = 0;
            argv[3] = t;
        }
        if (strcmp(argv[1], "run") == 0) return generate(&m, argv[3], max_tokens, temp, getenv("KATANA_BENCH_POS") ? 0 : 1);

        size_t n = strlen(argv[3]) + 8192;
        char *p = xalloc(n, 1);
        turn_text(&m.cfg, p, n, getenv("KATANA_SYSTEM"), argv[3], 1);
        return generate(&m, p, max_tokens, temp, 0);
    }
    fprintf(stderr,
        "Uso:\n"
        "  katana [opcoes] [\"mensagem\"]   console do agente na pasta atual (katana --help; docs/console.md); katana -p \"pedido\" e o modo headless\n"
        "  katana install | uninstall     coloca o comando katana no PATH; katana adopt traz a configuracao de outro projeto\n"
        "  katana info <modelo.gguf>\n"
        "  katana selftest <modelo.gguf>\n"
        "  katana toktest <modelo.gguf> <casos.hex> [--chat]   ids do tokenizador (ou do formato de conversa) de cada caso\n"
        "  katana qtest                   confere a decodificacao e os produtos de cada tipo de quantizacao\n"
        "  katana quantize <entrada.gguf> [saida.gguf] [tipo]   grava um GGUF requantizado (tipo: Q8_0, Q6_K, Q5_K_M, Q4_K_M, Q2_K)\n"
        "                                 sem tipo, escolhe o melhor para esta maquina; sem saida, gera o nome a partir da entrada\n"
        "  katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]\n"
        "  katana tune <modelo.gguf>      mede e grava o melhor ajuste para a GPU desta maquina\n"
        "  katana pack <modelo.gguf>      gera os shards no layout da GPU (<modelo>.gguf.ktn)\n"
        "  katana prepare <modelo.gguf>   inspeciona, decide o plano, prepara (pack/tune/calor) e grava o perfil do modelo\n"
        "  katana verify <modelo.gguf>    confere os hashes dos shards\n"
        "  katana sparsity <modelo.gguf> <calib.txt> <aval.txt>   esparsidade por assunto (modelo denso)\n"
        "  katana routing  <modelo.gguf> <calib.txt> <aval.txt>   especialistas por assunto (modelo MoE)\n"
        "  katana run  <modelo.gguf> \"texto\" [max_tokens] [temperatura]\n"
        "  katana chat <modelo.gguf> \"pergunta\" [max_tokens] [temperatura]\n"
        "  katana train <modelo.gguf> <dados.jsonl> [--train padroes] [--rank R] [--experts N] [--steps N] [--lr X] [--seq T] [--out f.kspec]\n"
        "                                       especializa o modelo (grava so os tensores treinados; KATANA_SPEC=f.kspec aplica)\n"
        "  katana import <modelo.gguf> <entrada.jsonl> <saida.jsonl> [--system keep|ksess:ARQ|file:ARQ|none] [--only-known-tools]\n"
        "                                       conversas (mensagens ou ShareGPT) -> dados de treino no formato exato do modelo\n"
        "  katana distill --url URL --model M [--format qwen|json|gemma4|glm] [--out f.jsonl] \"tarefa\"\n"
        "  katana export <saida.jsonl> <sessoes.ksess ou pastas> [--bench resultados.jsonl]   sessoes do agente -> dados de treino\n"
        "  katana learn <modelo.gguf> [--rounds N] [--repeat K] [--teacher URL --teacher-model M] [-- opcoes do train]\n"
        "                                 aprende com o proprio uso: coleta no codebench, treina no que passou, prova e guarda o melhor\n"
        "  katana specialize <modelo.gguf> [all|teacher|tools|code|knowledge] [--from PASTA] [--sessions PASTA] [--hours H] [--wait-idle [H]] [--dry-run]\n"
        "                                 a rotina da noite: conhecimento, dados, plano de memoria, treino e relatorio (padroes no katana.cfg)\n"
        "  katana specialize --status           relatorio das noites (janelas vistas, perda, comandos para testar e ativar)\n"
        "  katana knowledge quiz <modelo.gguf> <holdout.jsonl> [--limit N] [--spec ARQ.kspec]   o conhecimento entrou? (nota por sobreposicao)\n"
        "                                       um modelo professor (API no formato da OpenAI) resolve a tarefa com as ferramentas do katana\n"
        "  katana merge <modelo.gguf> <espec.kspec> <saida.gguf>   GGUF novo com a especializacao dentro\n"
        "  katana gradcheck <modelo.gguf> [T]   confere o passo para tras do treino (forward x motor, gradientes x diferencas finitas)\n"
        "  katana serve <modelo.gguf> [porta]   conversa no navegador e API da OpenAI em http://127.0.0.1:porta (padrao 8080;\n"
        "                                       so conversa). Rede: KATANA_HOST=0.0.0.0 KATANA_API_KEY=<chave de 16+ caracteres>\n"
        "  katana serve <modelo.gguf> [porta] --agent   o agente pela rede (/v1/agent/sessions, docs/api.md): quem tem a chave\n"
        "                                       executa comandos nesta maquina\n"
        "  katana code <modelo.gguf>      agente em modo de programacao (prompt enxuto: ler, editar, executar, conferir)\n"
        "  katana agent <modelo.gguf>     conversa com ferramentas (ler, listar, gravar e executar; KATANA_CONFIRM=1 pede confirmacao)\n"
        "  katana chat <modelo.gguf>      conversa interativa de varios turnos (KATANA_SYSTEM = prompt de sistema)\n");
    return 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* argumentos e saida em UTF-8 no Windows */
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char **u8 = xalloc(wargc + 1, sizeof(char *));
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            u8[i] = xalloc(n, 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8[i], n, NULL, NULL);
        }
        argc = wargc; argv = u8;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    reap_start(argc, argv);
    pool_init();
    vnni_init();
    moe_q5k_old = getenv("KATANA_MOE_OLD") && atoi(getenv("KATANA_MOE_OLD")) != 0;
    return run_main(argc, argv);
}
