/* ================= estruturas ================= */

typedef struct {
    int dim, n_layers, n_heads, n_kv, hd, hidden, ctx, vocab;
    float rope_base, eps;
    int bos, eos, add_bos;
    int rope_neox;      /* RoPE em metades (qwen2) em vez de pares adjacentes (llama) */
    int n_expert, n_used;   /* mistura de especialistas (MoE): quantos existem e quantos entram por token */
    int moe_norm;       /* renormaliza os pesos dos especialistas escolhidos (qwen3moe) */
    int qk_norm;        /* norma RMS em q e k: 1 = no vetor inteiro (olmoe), 2 = por cabeca (qwen3) */
    int hidden_exp;
    int hd_key;         /* largura de cada cabeca quando nao e dim/cabecas (qwen3moe) */
    int chat_fmt;       /* 0 = ChatML, 1 = Llama 3, 2 = Zephyr, 3 = [INST], 4 = texto simples, 5 = DeepSeek, 6 = Kimi, 7 = OLMoE, 8 = Gemma 4 */
    const char *def_sys;    /* prompt de sistema que o template do modelo poe quando o usuario nao manda (Qwen2.5) */
    int trim_msgs;          /* o template aplica |trim as mensagens */
    int llama_hdr;          /* template do Llama 3.1/3.2: sempre abre com o bloco de sistema com a data */
    char bos_text[48], eos_text[48];    /* texto dos tokens bos/eos (formatos de conversa que os escrevem) */
    /* atencao latente (MLA, deepseek2): o cache guarda so o latente c_kv e a parte rope de k */
    int mla;            /* largura do latente (kv_lora_rank); 0 = atencao comum */
    int q_lora;         /* q comprimida (q_lora_rank, V3/Kimi K2): q = q_b(norma(q_a x)) */
    int rope_dim;       /* dimensoes com RoPE em cada cabeca (o resto de q/k nao gira) */
    int hd_v;           /* largura de v por cabeca */
    int hd_mla_k, hd_mla_v;
    int n_dense;        /* camadas iniciais com feed-forward denso num MoE */
    int hidden_dense;   /* largura do feed-forward denso dessas camadas */
    int n_shared;       /* especialistas compartilhados (fundidos em *_shexp), sempre somados */
    int gate_sig;       /* roteador sigmoide (com vies so na escolha) em vez de softmax */
    float exp_scale;    /* escala dos pesos dos especialistas */
    float kq_mul;       /* multiplicador extra da escala q.k (yarn) */
    float yarn_factor, yarn_logmul; int yarn_orig, yarn_ext;   /* yarn_ext: escala pedida pelo usuario (nao veio do GGUF) */
    int ctx_train;      /* contexto treinado do modelo (do GGUF); ctx pode passar dele com escala de RoPE */
    int kv16_cpu;       /* cache KV da CPU em fp16 (hibrido DeltaNet) */
    int kv8_cpu;        /* cache KV da CPU em blocos Q8_0 (hibrido DeltaNet) */
    /* atencao linear Gated DeltaNet (qwen35moe): camadas recorrentes intercaladas com atencao completa */
    int ssm_conv, ssm_state, ssm_groups, ssm_rank, ssm_inner;  /* janela da convolucao, dk, grupos q/k, cabecas v, largura v */
    int attn_every;     /* (camada+1) % attn_every == 0: atencao completa; as outras sao DeltaNet */
    int attn_gated;     /* attn_q traz q e o portao da saida intercalados por cabeca */
    int hidden_shexp;   /* largura do especialista compartilhado */
    int think;          /* o assistente comeca com <think>\n */
    int tool_tmpl;      /* o template de conversa do modelo fala de ferramentas */
    int tool_xml;       /* o modelo chama ferramentas em XML (<function=..><parameter=..>) */
    int glm_args;       /* GLM 4.5 em diante: chamada nome<arg_key>k</arg_key><arg_value>v</arg_value>, marcas de turno sem quebra de linha */
    /* gemma4: camadas locais (janela deslizante) intercaladas com globais, largura de cabeca e cabecas kv por
       camada, feed-forward denso em paralelo com o MoE, normas depois de cada bloco */
    int g4;             /* arquitetura gemma4 */
    int go;             /* arquitetura gpt-oss */
    int glm;            /* arquitetura glm4 (dense): norma sanduiche, rope parcial, qkv com vies */
    int glmoe;          /* arquitetura glm4moe (MoE): pre-norm comum, rope NEOX parcial, qkv com vies, compartilhado */
    int n_nextn;        /* camadas MTP/nextn no fim do modelo (ignoradas) */
    int swa_win;        /* janela das camadas locais (posicoes vistas, contando a atual) */
    int hd_swa;         /* largura de cabeca das camadas locais (hd e a das globais) */
    float rope_base_swa;
    float softcap;      /* logits finais: softcap * tanh(logit / softcap); 0 = sem */
    float emb_scale;    /* escala do embedding (0 = 1) */
    char arch[64];
} Config;

/* matriz de pesos: float32 (f) ou quantizada (q aponta direto para o arquivo mapeado) */
typedef struct {
    float *f; const uint8_t *q; uint32_t type; size_t stride;
    void *gbuf;         /* buffer na VRAM (pode ser compartilhado com outra matriz) */
    int gs, go, pair;   /* matrizes intercaladas no buffer: quantas, qual; par gate/up fundido */
} Mat;

typedef struct {
    float *attn_norm, *ffn_norm;
    float *bq, *bk, *bv, *bo;           /* bias da atencao (opcional; qwen2 usa) */
    Mat wq, wk, wv, wo, w_gate, w_up, w_down;
    float *q_norm, *k_norm;             /* norma de q e k (olmoe, qwen3) */
    Mat router;                         /* MoE: [n_expert][dim], escolhe os especialistas do token */
    Mat *eg, *eu, *ed;                  /* MoE: gate/up/down de cada especialista (fatias do tensor empilhado) */
    Mat wkv_a;                          /* MLA: x -> [latente | k rope] */
    float *kv_a_norm;                   /* MLA: norma do latente */
    Mat wq_a, wq_b; float *q_a_norm;    /* MLA com q comprimida */
    float *wkb, *wvb;                   /* MLA: [cabeca][latente][nope] e [cabeca][hd_v][latente], em float */
    Mat sg, su, sd;                     /* especialistas compartilhados */
    float *exp_bias;                    /* vies do roteador (so na escolha) */
    float *shexp_gate;                  /* opcional [dim]: portao sigmoide escalar do compartilhado */
    /* DeltaNet: qkv misturados, portao z, alpha/beta por cabeca, convolucao, decaimento, norma e saida */
    Mat wqkv, wz, walpha, wbeta, wssm_out;
    float *conv_w, *ssm_a, *dt_bias, *ssm_norm;
    int lin, lin_i;                     /* 1 = camada DeltaNet; indice entre as camadas DeltaNet (estado) */
    /* gemma4 */
    int swa, nkv_l, hd_l;               /* camada local (janela); cabecas kv e largura de cabeca desta camada */
    float *post_attn_norm, *post_ffn_norm, *post_ffn1, *post_ffn2, *pre_ffn2;   /* normas extras */
    float *router_scale, *exp_w;        /* escala da entrada do roteador [dim]; peso de cada especialista [n_expert] */
    float out_scale;                    /* escala da saida da camada */
    size_t kv_off; int kv_cap;          /* inicio da camada no cache KV da CPU e posicoes guardadas (local = anel) */
    /* gpt-oss */
    float *sinks, *router_b, *eg_b, *eu_b, *ed_b;   /* sink por cabeca; vies do roteador; vies dos especialistas gate/up/down */
} Layer;

typedef struct {
    char **tok; int *toklen; int *toktype; int n;
    int spm; float *score;      /* SentencePiece: funde pares pela pontuacao do token; 2 = espacos como U+2581 e fusoes BPE (gemma4) */
    int digits;                 /* pre-tokenizacao legada: digitos seguidos agrupados ate este numero (0 = 1) */
    int pre;                    /* pre-tokenizador (PRE_*), pela chave tokenizer.ggml.pre */
    int nfc;                    /* normaliza o texto em NFC antes de tokenizar (Qwen, OLMo) */
    uint8_t spec1[256];         /* 1 = existe token especial que comeca com este byte */
    Map vocab, merges;
    int *special; int n_special;
    int byte2cp[256]; int cp2byte[512];
} Tokenizer;

typedef struct {
    Config cfg;
    Tokenizer tk;
    Mat tok_emb, lm_head;
    float *out_norm;
    Layer *layers;
    float *kcache, *vcache;             /* [L][ctx][kv_dim] */
    uint16_t *kh, *vh;                  /* cache KV fp16 da CPU (hibrido): [camada de atencao][ctx][kv_dim] */
    uint8_t *kq8, *vq8;                 /* cache KV Q8_0 da CPU (hibrido): mesma ordem, blocos de 34 bytes por 32 valores */
    float *ckv;                         /* MLA: [L][ctx][latente + rope] */
    float *ssm;                         /* DeltaNet: estado contiguo; por camada linear, S [cabeca v][dk][dv] e (janela-1) x canais da convolucao */
    int n_lin;                          /* camadas DeltaNet */
    int64_t fsize;                      /* tamanho do arquivo do modelo */
    uint64_t fid;                       /* identidade dos pesos: data e tamanho do arquivo e das especializacoes aplicadas (cache de prefixo) */
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *hb2, *logits, *rope_freq;
    float *rope_freq_swa;               /* gemma4: frequencias das camadas locais */
    size_t n_params;
} Model;

/* ================= leitura do GGUF ================= */

typedef struct { const uint8_t *p, *end; } Rd;

static void rd_need(Rd *r, size_t n) { if ((size_t)(r->end - r->p) < n) die("GGUF truncado"); }
static uint32_t rd_u32(Rd *r) { uint32_t v; rd_need(r, 4); memcpy(&v, r->p, 4); r->p += 4; return v; }
static uint64_t rd_u64(Rd *r) { uint64_t v; rd_need(r, 8); memcpy(&v, r->p, 8); r->p += 8; return v; }
static const char *rd_str(Rd *r, int *len) {
    uint64_t n = rd_u64(r);
    rd_need(r, (size_t)n);
    const char *s = (const char *)r->p;
    r->p += n; *len = (int)n;
    return s;
}

static const int GG_SIZE[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

static double rd_num(Rd *r, uint32_t t) {
    if (t > 12 || GG_SIZE[t] == 0) die("tipo numerico inesperado no GGUF");
    rd_need(r, GG_SIZE[t]);
    const uint8_t *p = r->p; r->p += GG_SIZE[t];
    switch (t) {
        case 0: case 7: return *p;
        case 1: return *(const int8_t *)p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 3: { int16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        case 5: { int32_t v; memcpy(&v, p, 4); return v; }
        case 6: { float v; memcpy(&v, p, 4); return v; }
        case 10: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
        case 11: { int64_t v; memcpy(&v, p, 8); return (double)v; }
        default: { double v; memcpy(&v, p, 8); return v; }
    }
}

static void rd_skip(Rd *r, uint32_t t) {
    int len;
    if (t == 8) { rd_str(r, &len); return; }
    if (t == 9) {
        uint32_t et = rd_u32(r); uint64_t n = rd_u64(r);
        if (et != 8 && et != 9) {
            if (et > 12) die("tipo de array invalido");
            rd_need(r, (size_t)n * GG_SIZE[et]); r->p += n * GG_SIZE[et];
        } else for (uint64_t i = 0; i < n; i++) rd_skip(r, et);
        return;
    }
    rd_num(r, t);
}

static int key_ends(const char *k, int kl, const char *suf) {
    int sl = (int)strlen(suf);
    return kl >= sl && memcmp(k + kl - sl, suf, sl) == 0;
}
static int key_is(const char *k, int kl, const char *s) {
    return kl == (int)strlen(s) && memcmp(k, s, kl) == 0;
}

typedef struct {
    const char *name; int namelen;
    uint64_t dims[4]; uint32_t ndims, type; uint64_t offset;
    int used;           /* alguem leu este tensor na carga */
} TInfo;
/* tensores do arquivo que a carga nao leu (selftest --deep) */
static struct { char (*name)[96]; uint64_t *bytes; int n; } g_unread;

typedef struct { TInfo *t; uint64_t n; const uint8_t *data, *end; } Tensors;

static float f16_to_f32(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u;
    int e = (h >> 10) & 0x1f;
    if (e == 0) {
        if (m == 0) u = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; e--; }
            m &= 0x3ff;
            u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) u = (s << 31) | 0x7f800000u | (m << 13);
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4);
    return f;
}

/* tipos de tensor do GGUF */
enum { T_F32 = 0, T_F16 = 1, T_Q4_0 = 2, T_Q4_1 = 3, T_Q5_0 = 6, T_Q5_1 = 7, T_Q8_0 = 8, T_Q2_K = 10, T_Q3_K = 11,
       T_Q4_K = 12, T_Q5_K = 13, T_Q6_K = 14, T_IQ2_XXS = 16, T_IQ2_XS = 17, T_IQ3_XXS = 18, T_IQ1_S = 19,
       T_IQ4_NL = 20, T_IQ3_S = 21, T_IQ2_S = 22, T_IQ4_XS = 23, T_IQ1_M = 29, T_BF16 = 30, T_TQ1_0 = 34,
       T_TQ2_0 = 35, T_MXFP4 = 39 };

/* bytes ocupados por n elementos (0 = tipo nao suportado ou n desalinhado) */
static size_t qbytes(uint32_t type, size_t n) {
    switch (type) {
        case T_F32:  return n * 4;
        case T_F16:  return n * 2;
        case T_Q4_0: return n % 32 ? 0 : n / 32 * 18;     /* f16 d + 32 nibbles */
        case T_Q8_0: return n % 32 ? 0 : n / 32 * 34;     /* f16 d + 32 x int8 */
        case T_Q5_0: return n % 32 ? 0 : n / 32 * 22;     /* f16 d + 32 bits altos + 32 nibbles */
        case T_Q3_K: return n % 256 ? 0 : n / 256 * 110;  /* 32 bits altos, 64 de 2 bits, 12 escalas, f16 d */
        case T_Q5_K: return n % 256 ? 0 : n / 256 * 176;  /* f16 d, f16 dmin, 12 escalas, 32 bits altos, 128 nibbles */
        case T_Q4_K: return n % 256 ? 0 : n / 256 * 144;  /* f16 d, f16 dmin, 12 escalas, 128 nibbles */
        case T_Q6_K: return n % 256 ? 0 : n / 256 * 210;  /* 128 ql, 64 qh, 16 escalas, f16 d */
        case T_BF16: return n * 2;
        case T_Q4_1: return n % 32 ? 0 : n / 32 * 20;     /* f16 d, f16 m, 32 nibbles */
        case T_Q5_1: return n % 32 ? 0 : n / 32 * 24;     /* f16 d, f16 m, 32 bits altos, 32 nibbles */
        case T_IQ4_NL: return n % 32 ? 0 : n / 32 * 18;   /* f16 d, 32 nibbles (indices da tabela nao linear) */
        case T_MXFP4: return n % 32 ? 0 : n / 32 * 17;    /* expoente E8M0, 32 valores E2M1 */
        case T_Q2_K: return n % 256 ? 0 : n / 256 * 84;   /* 16 escalas/minimos, 64 de 2 bits, f16 d, f16 dmin */
        case T_IQ4_XS: return n % 256 ? 0 : n / 256 * 136;  /* f16 d, escalas altas, 4 escalas baixas, 128 nibbles */
        case T_IQ2_XXS: return n % 256 ? 0 : n / 256 * 66;
        case T_IQ2_XS: return n % 256 ? 0 : n / 256 * 74;
        case T_IQ2_S: return n % 256 ? 0 : n / 256 * 82;
        case T_IQ3_XXS: return n % 256 ? 0 : n / 256 * 98;
        case T_IQ3_S: return n % 256 ? 0 : n / 256 * 110;
        case T_IQ1_S: return n % 256 ? 0 : n / 256 * 50;
        case T_IQ1_M: return n % 256 ? 0 : n / 256 * 56;
        case T_TQ1_0: return n % 256 ? 0 : n / 256 * 54;
        case T_TQ2_0: return n % 256 ? 0 : n / 256 * 66;
        default:     return 0;
    }
}

/* escala e minimo (6 bits cada) do sub-bloco j de um bloco Q4_K */
static void k4_scale_min(int j, const uint8_t *q, int *d, int *m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

/* bloco Q3_K -> 256 valores inteiros (-4..3) e 16 escalas (-32..31) */
static void q3k_unpack(const uint8_t *src, int8_t *val, int8_t *sc) {
    const uint32_t km1 = 0x03030303u, km2 = 0x0f0f0f0fu;
    const uint8_t *hm = src, *q = src + 32;
    uint32_t aux[4];
    memcpy(aux, src + 96, 12);
    const uint32_t tmp = aux[2];
    aux[2] = ((aux[0] >> 4) & km2) | (((tmp >> 4) & km1) << 4);
    aux[3] = ((aux[1] >> 4) & km2) | (((tmp >> 6) & km1) << 4);
    aux[0] = (aux[0] & km2) | (((tmp >> 0) & km1) << 4);
    aux[1] = (aux[1] & km2) | (((tmp >> 2) & km1) << 4);
    for (int i = 0; i < 16; i++) sc[i] = (int8_t)(((const uint8_t *)aux)[i] - 32);
    uint8_t m = 1;
    for (int j = 0; j < 2; j++, q += 32) {
        for (int k = 0, shift = 0; k < 4; k++, shift += 2, m <<= 1) {
            for (int l = 0; l < 16; l++) *val++ = (int8_t)(((q[l] >> shift) & 3) - ((hm[l] & m) ? 0 : 4));
            for (int l = 0; l < 16; l++) *val++ = (int8_t)(((q[l + 16] >> shift) & 3) - ((hm[l + 16] & m) ? 0 : 4));
        }
    }
}

/* elementos por bloco de um tipo (1 = sem blocos) */
static int qunit(uint32_t type) {
    switch (type) {
        case T_F32: case T_F16: case T_BF16: return 1;
        case T_Q4_0: case T_Q4_1: case T_Q5_0: case T_Q5_1: case T_Q8_0: case T_IQ4_NL: case T_MXFP4: return 32;
        default: return 256;
    }
}

static const int8_t q_iq4nl_vals[16] = { -127, -104, -83, -65, -49, -35, -22, -10, 1, 13, 25, 38, 53, 69, 89, 113 };
/* valores E2M1 dobrados (0, 0.5, 1, 1.5, 2, 3, 4, 6 vezes 2), com sinal no bit 3 */
static const int8_t q_fp4_vals[16] = { 0, 1, 2, 3, 4, 6, 8, 12, 0, -1, -2, -3, -4, -6, -8, -12 };

/* escala E8M0 dividida por 2 (compensa os valores E2M1 dobrados): 2^(e-128), subnormal para e < 2 */
static float e8m0_half(uint8_t e) {
    uint32_t b = e < 2 ? 0x00200000u << e : (uint32_t)(e - 1) << 23;
    float f; memcpy(&f, &b, 4);
    return f;
}
static float bf16_to_f32(uint16_t h) { uint32_t u = (uint32_t)h << 16; float f; memcpy(&f, &u, 4); return f; }
static int kv8_escolha(size_t ctx);
static float h2f(const uint8_t *p) { uint16_t h; memcpy(&h, p, 2); return f16_to_f32(h); }

/* escala de 6 bits do sub-bloco ib (0..7) de um bloco IQ4_XS (ja sem o deslocamento de 32) */
static int iq4xs_scale(const uint8_t *blk, int ib) {
    uint16_t sh; memcpy(&sh, blk + 2, 2);
    return (((blk[4 + ib / 2] >> 4 * (ib % 2)) & 0xF) | (((sh >> 2 * ib) & 3) << 4)) - 32;
}

/* tipos acrescentados depois (formatos com tabela, IQ, ternarios, BF16); 1 = tratado */
static int dequant_ext(uint32_t type, const uint8_t *src, float *out, size_t n) {
    static const uint8_t kmask[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    switch (type) {
    case T_BF16:
        for (size_t j = 0; j < n; j++) { uint16_t h; memcpy(&h, src + j * 2, 2); out[j] = bf16_to_f32(h); }
        return 1;
    case T_Q4_1:
        for (size_t b = 0; b < n / 32; b++, src += 20, out += 32) {
            const float d = h2f(src), m = h2f(src + 2);
            for (int j = 0; j < 16; j++) { out[j] = (src[4 + j] & 0xF) * d + m; out[j + 16] = (src[4 + j] >> 4) * d + m; }
        }
        return 1;
    case T_Q5_1:
        for (size_t b = 0; b < n / 32; b++, src += 24, out += 32) {
            const float d = h2f(src), m = h2f(src + 2);
            uint32_t qh; memcpy(&qh, src + 4, 4);
            const uint8_t *qs = src + 8;
            for (int j = 0; j < 16; j++) {
                out[j] = (int)((qs[j] & 0xF) | (((qh >> j) & 1) << 4)) * d + m;
                out[j + 16] = (int)((qs[j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) * d + m;
            }
        }
        return 1;
    case T_IQ4_NL:
        for (size_t b = 0; b < n / 32; b++, src += 18, out += 32) {
            const float d = h2f(src);
            for (int j = 0; j < 16; j++) { out[j] = d * q_iq4nl_vals[src[2 + j] & 0xF]; out[j + 16] = d * q_iq4nl_vals[src[2 + j] >> 4]; }
        }
        return 1;
    case T_MXFP4:
        for (size_t b = 0; b < n / 32; b++, src += 17, out += 32) {
            const float d = e8m0_half(src[0]);
            for (int j = 0; j < 16; j++) { out[j] = q_fp4_vals[src[1 + j] & 0xF] * d; out[j + 16] = q_fp4_vals[src[1 + j] >> 4] * d; }
        }
        return 1;
    case T_IQ4_XS:
        for (size_t b = 0; b < n / 256; b++, src += 136) {
            const float d = h2f(src);
            const uint8_t *qs = src + 8;
            for (int ib = 0; ib < 8; ib++, qs += 16, out += 32) {
                const float dl = d * iq4xs_scale(src, ib);
                for (int j = 0; j < 16; j++) { out[j] = dl * q_iq4nl_vals[qs[j] & 0xF]; out[j + 16] = dl * q_iq4nl_vals[qs[j] >> 4]; }
            }
        }
        return 1;
    case T_Q2_K:
        for (size_t b = 0; b < n / 256; b++, src += 84) {
            const uint8_t *sc = src, *q = src + 16;
            const float d = h2f(src + 80), dmin = h2f(src + 82);
            int is = 0;
            for (int c = 0; c < 2; c++, q += 32)
                for (int shift = 0; shift < 8; shift += 2)
                    for (int h = 0; h < 2; h++) {
                        const uint8_t s8 = sc[is++];
                        const float dl = d * (s8 & 0xF), ml = dmin * (s8 >> 4);
                        for (int l = 0; l < 16; l++) *out++ = dl * ((q[l + 16 * h] >> shift) & 3) - ml;
                    }
        }
        return 1;
    case T_IQ2_XXS:
        for (size_t b = 0; b < n / 256; b++, src += 66) {
            const float d = h2f(src);
            for (int ib = 0; ib < 8; ib++) {
                uint32_t a[2]; memcpy(a, src + 2 + 8 * ib, 8);
                const float db = d * (0.5f + (a[1] >> 28)) * 0.25f;
                for (int l = 0; l < 4; l++, out += 8) {
                    const uint8_t *g = (const uint8_t *)(q_iq2xxs_grid + ((const uint8_t *)a)[l]);
                    const uint8_t sg = q_ksigns[(a[1] >> 7 * l) & 127];
                    for (int j = 0; j < 8; j++) out[j] = db * g[j] * (sg & kmask[j] ? -1.f : 1.f);
                }
            }
        }
        return 1;
    case T_IQ2_XS:
        for (size_t b = 0; b < n / 256; b++, src += 74) {
            const float d = h2f(src);
            const uint8_t *scl = src + 66;
            for (int ib = 0; ib < 8; ib++) {
                const float db[2] = { d * (0.5f + (scl[ib] & 0xF)) * 0.25f, d * (0.5f + (scl[ib] >> 4)) * 0.25f };
                for (int l = 0; l < 4; l++, out += 8) {
                    uint16_t q; memcpy(&q, src + 2 + 2 * (4 * ib + l), 2);
                    const uint8_t *g = (const uint8_t *)(q_iq2xs_grid + (q & 511));
                    const uint8_t sg = q_ksigns[q >> 9];
                    for (int j = 0; j < 8; j++) out[j] = db[l / 2] * g[j] * (sg & kmask[j] ? -1.f : 1.f);
                }
            }
        }
        return 1;
    case T_IQ2_S:
        for (size_t b = 0; b < n / 256; b++, src += 82) {
            const float d = h2f(src);
            const uint8_t *qs = src + 2, *sgn = src + 2 + 32, *qh = src + 66, *scl = src + 74;
            for (int ib = 0; ib < 8; ib++, qs += 4, sgn += 4) {
                const float db[2] = { d * (0.5f + (scl[ib] & 0xF)) * 0.25f, d * (0.5f + (scl[ib] >> 4)) * 0.25f };
                for (int l = 0; l < 4; l++, out += 8) {
                    const uint8_t *g = (const uint8_t *)(q_iq2s_grid + (qs[l] | ((qh[ib] << (8 - 2 * l)) & 0x300)));
                    for (int j = 0; j < 8; j++) out[j] = db[l / 2] * g[j] * (sgn[l] & kmask[j] ? -1.f : 1.f);
                }
            }
        }
        return 1;
    case T_IQ3_XXS:
        for (size_t b = 0; b < n / 256; b++, src += 98) {
            const float d = h2f(src);
            const uint8_t *qs = src + 2, *ss = src + 2 + 64;
            for (int ib = 0; ib < 8; ib++, qs += 8) {
                uint32_t a; memcpy(&a, ss + 4 * ib, 4);
                const float db = d * (0.5f + (a >> 28)) * 0.5f;
                for (int l = 0; l < 4; l++, out += 8) {
                    const uint8_t sg = q_ksigns[(a >> 7 * l) & 127];
                    const uint8_t *g1 = (const uint8_t *)(q_iq3xxs_grid + qs[2 * l]), *g2 = (const uint8_t *)(q_iq3xxs_grid + qs[2 * l + 1]);
                    for (int j = 0; j < 4; j++) {
                        out[j] = db * g1[j] * (sg & kmask[j] ? -1.f : 1.f);
                        out[j + 4] = db * g2[j] * (sg & kmask[j + 4] ? -1.f : 1.f);
                    }
                }
            }
        }
        return 1;
    case T_IQ3_S:
        for (size_t b = 0; b < n / 256; b++, src += 110) {
            const float d = h2f(src);
            const uint8_t *qs = src + 2, *qh = src + 66, *sgn = src + 74, *scl = src + 106;
            for (int ib = 0; ib < 8; ib += 2) {
                for (int half = 0; half < 2; half++, qs += 8, sgn += 4) {
                    const float db = d * (1 + 2 * (half ? scl[ib / 2] >> 4 : scl[ib / 2] & 0xF));
                    for (int l = 0; l < 4; l++, out += 8) {
                        const uint8_t *g1 = (const uint8_t *)(q_iq3s_grid + (qs[2 * l] | ((qh[half] << (8 - 2 * l)) & 256)));
                        const uint8_t *g2 = (const uint8_t *)(q_iq3s_grid + (qs[2 * l + 1] | ((qh[half] << (7 - 2 * l)) & 256)));
                        for (int j = 0; j < 4; j++) {
                            out[j] = db * g1[j] * (sgn[l] & kmask[j] ? -1.f : 1.f);
                            out[j + 4] = db * g2[j] * (sgn[l] & kmask[j + 4] ? -1.f : 1.f);
                        }
                    }
                }
                qh += 2;
            }
        }
        return 1;
    case T_IQ1_S:
        for (size_t b = 0; b < n / 256; b++, src += 50) {
            const float d = h2f(src);
            const uint8_t *qs = src + 2;
            for (int ib = 0; ib < 8; ib++, qs += 4) {
                uint16_t qh; memcpy(&qh, src + 34 + 2 * ib, 2);
                const float dl = d * (2 * ((qh >> 12) & 7) + 1);
                const float delta = qh & 0x8000 ? -0.125f : 0.125f;
                for (int l = 0; l < 4; l++, out += 8) {
                    const int8_t *g = (const int8_t *)(q_iq1s_grid + (qs[l] | (((qh >> 3 * l) & 7) << 8)));
                    for (int j = 0; j < 8; j++) out[j] = dl * (g[j] + delta);
                }
            }
        }
        return 1;
    case T_IQ1_M:
        for (size_t b = 0; b < n / 256; b++, src += 56) {
            const uint8_t *qs = src, *qh = src + 32;
            uint16_t sc[4]; memcpy(sc, src + 48, 8);
            const uint16_t hs = (uint16_t)((sc[0] >> 12) | ((sc[1] >> 8) & 0x00f0) | ((sc[2] >> 4) & 0x0f00) | (sc[3] & 0xf000));
            const float d = f16_to_f32(hs);
            for (int ib = 0; ib < 8; ib++, qs += 4, qh += 2) {
                const float dl1 = d * (2 * ((sc[ib / 2] >> (6 * (ib % 2) + 0)) & 7) + 1);
                const float dl2 = d * (2 * ((sc[ib / 2] >> (6 * (ib % 2) + 3)) & 7) + 1);
                const int idx[4] = { qs[0] | ((qh[0] << 8) & 0x700), qs[1] | ((qh[0] << 4) & 0x700),
                                     qs[2] | ((qh[1] << 8) & 0x700), qs[3] | ((qh[1] << 4) & 0x700) };
                const float delta[4] = { qh[0] & 0x08 ? -0.125f : 0.125f, qh[0] & 0x80 ? -0.125f : 0.125f,
                                         qh[1] & 0x08 ? -0.125f : 0.125f, qh[1] & 0x80 ? -0.125f : 0.125f };
                for (int l = 0; l < 4; l++, out += 8) {
                    const int8_t *g = (const int8_t *)(q_iq1s_grid + idx[l]);
                    const float dl = l < 2 ? dl1 : dl2;
                    for (int j = 0; j < 8; j++) out[j] = dl * (g[j] + delta[l]);
                }
            }
        }
        return 1;
    case T_TQ1_0: {
        static const uint8_t pow3[6] = { 1, 3, 9, 27, 81, 243 };
        for (size_t b = 0; b < n / 256; b++, src += 54) {
            const float d = h2f(src + 52);
            const uint8_t *qs = src, *qh = src + 48;
            for (int j = 0; j < 32; j += 32)
                for (int k = 0; k < 5; k++)
                    for (int m = 0; m < 32; m++) { const uint8_t q = (uint8_t)(qs[j + m] * pow3[k]); *out++ = (float)((((uint16_t)q * 3) >> 8) - 1) * d; }
            for (int j = 32; j < 48; j += 16)
                for (int k = 0; k < 5; k++)
                    for (int m = 0; m < 16; m++) { const uint8_t q = (uint8_t)(qs[j + m] * pow3[k]); *out++ = (float)((((uint16_t)q * 3) >> 8) - 1) * d; }
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 4; j++) { const uint8_t q = (uint8_t)(qh[j] * pow3[k]); *out++ = (float)((((uint16_t)q * 3) >> 8) - 1) * d; }
        }
        return 1;
    }
    case T_TQ2_0:
        for (size_t b = 0; b < n / 256; b++, src += 66) {
            const float d = h2f(src + 64);
            for (int j = 0; j < 64; j += 32)
                for (int l = 0; l < 4; l++)
                    for (int m = 0; m < 32; m++) *out++ = (float)(((src[j + m] >> (l * 2)) & 3) - 1) * d;
        }
        return 1;
    default:
        return 0;
    }
}

static void dequant(uint32_t type, const uint8_t *src, float *out, size_t n) {
    if (dequant_ext(type, src, out, n)) return;
    if (type == T_Q4_0) {
        for (size_t b = 0; b < n / 32; b++, src += 18, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            const float d = f16_to_f32(h);
            for (int j = 0; j < 16; j++) {
                out[j] = d * ((src[2 + j] & 0xF) - 8);
                out[j + 16] = d * ((src[2 + j] >> 4) - 8);
            }
        }
        return;
    }
    if (type == T_Q5_0) {
        for (size_t b = 0; b < n / 32; b++, src += 22, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            uint32_t qh; memcpy(&qh, src + 2, 4);
            const float d = f16_to_f32(h);
            const uint8_t *qs = src + 6;
            for (int j = 0; j < 16; j++) {
                out[j] = d * ((int)((qs[j] & 0xF) | (((qh >> j) & 1) << 4)) - 16);
                out[j + 16] = d * ((int)((qs[j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) - 16);
            }
        }
        return;
    }
    if (type == T_Q3_K) {
        for (size_t b = 0; b < n / 256; b++, src += 110, out += 256) {
            int8_t val[256], sc[16];
            uint16_t h; memcpy(&h, src + 108, 2);
            const float d = f16_to_f32(h);
            q3k_unpack(src, val, sc);
            for (int i = 0; i < 256; i++) out[i] = d * sc[i / 16] * val[i];
        }
        return;
    }
    if (type == T_Q5_K) {
        for (size_t b = 0; b < n / 256; b++, src += 176) {
            uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
            const float d = f16_to_f32(h0), dmin = f16_to_f32(h1);
            const uint8_t *sc = src + 4, *qh = src + 16, *q = src + 48;
            for (int j = 0; j < 8; j += 2, q += 32) {
                int s1, m1, s2, m2;
                k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) *out++ = d * s1 * ((q[l] & 0xF) + (((qh[l] >> j) & 1) << 4)) - dmin * m1;
                for (int l = 0; l < 32; l++) *out++ = d * s2 * ((q[l] >> 4) + (((qh[l] >> (j + 1)) & 1) << 4)) - dmin * m2;
            }
        }
        return;
    }
    if (type == T_F32) memcpy(out, src, n * 4);
    else if (type == T_F16) {
        for (size_t j = 0; j < n; j++) { uint16_t h; memcpy(&h, src + j * 2, 2); out[j] = f16_to_f32(h); }
    } else if (type == T_Q8_0) {
        for (size_t b = 0; b < n / 32; b++, src += 34, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            const float d = f16_to_f32(h);
            const int8_t *qs = (const int8_t *)(src + 2);
            for (int j = 0; j < 32; j++) out[j] = d * qs[j];
        }
    } else if (type == T_Q4_K) {
        for (size_t b = 0; b < n / 256; b++, src += 144) {
            uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
            const float d = f16_to_f32(h0), dmin = f16_to_f32(h1);
            const uint8_t *sc = src + 4, *q = src + 16;
            for (int j = 0; j < 8; j += 2, q += 32) {
                int s1, m1, s2, m2;
                k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) *out++ = d * s1 * (q[l] & 0xF) - dmin * m1;
                for (int l = 0; l < 32; l++) *out++ = d * s2 * (q[l] >> 4) - dmin * m2;
            }
        }
    } else if (type == T_Q6_K) {
        for (size_t b = 0; b < n / 256; b++, src += 210) {
            const uint8_t *ql = src, *qh = src + 128;
            const int8_t *sc = (const int8_t *)(src + 192);
            uint16_t h; memcpy(&h, src + 208, 2);
            const float d = f16_to_f32(h);
            for (int c = 0; c < 2; c++, out += 128, ql += 64, qh += 32, sc += 8) {
                for (int l = 0; l < 32; l++) {
                    const int is = l / 16;
                    const int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                    const int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                    const int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                    const int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                    out[l]      = d * sc[is] * q1;
                    out[l + 32] = d * sc[is + 2] * q2;
                    out[l + 64] = d * sc[is + 4] * q3;
                    out[l + 96] = d * sc[is + 6] * q4;
                }
            }
        }
    }
}

/* devolve o tensor convertido para float32; NULL se nao existir e !required */
static float *get_tensor(const Tensors *ts, const char *name, size_t expect, int required, size_t *count) {
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        ((TInfo *)t)->used = 1;
        size_t n = 1;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != expect) { fprintf(stderr, "Tensor %s: %zu elementos, esperava %zu\n", name, n, expect); exit(1); }
        const uint8_t *src = ts->data + t->offset;
        float *out = xalloc(n, sizeof(float));
        size_t bytes = qbytes(t->type, n);
        if (!bytes) {
            fprintf(stderr, "Tensor %s: quantizacao tipo %u nao suportada (use F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q2_K..Q6_K, IQ1..IQ4, TQ1_0, TQ2_0 ou MXFP4)\n", name, t->type);
            exit(1);
        }
        if (src + bytes > ts->end) die("tensor fora do arquivo");
        dequant(t->type, src, out, n);
        if (count) *count += n;
        return out;
    }
    if (required) { fprintf(stderr, "Tensor %s nao encontrado\n", name); exit(1); }
    return NULL;
}

/* matriz [rows][cols]: usa os dados quantizados direto do arquivo quando cada linha
   comeca num bloco inteiro; senao converte para float32 */
static Mat get_mat(const Tensors *ts, const char *name, size_t rows, size_t cols, int required, size_t *count) {
    Mat m = {0};
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        if (t->type == T_F32 || t->type == T_F16 || t->type == T_BF16) break;
        ((TInfo *)t)->used = 1;
        size_t n = 1, stride = qbytes(t->type, cols);
        if (!stride) break;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != rows * cols) { fprintf(stderr, "Tensor %s: tamanho inesperado\n", name); exit(1); }
        if (ts->data + t->offset + rows * stride > ts->end) die("tensor fora do arquivo");
        m.q = ts->data + t->offset;
        m.type = t->type; m.stride = stride;
        if (count) *count += n;
        return m;
    }
    m.f = get_tensor(ts, name, rows * cols, required, count);
    return m;
}

static int g_vocab_only;    /* toktest: carrega so metadados e vocabulario, sem os pesos */
static int pre_kind(const char *sv, int sl);
static int pre_has_nfc(int kind);

static void tokenizer_finish(Tokenizer *tk, const char **merges, int *mlen, int n_merges) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174) tk->byte2cp[b] = b;
        else tk->byte2cp[b] = 256 + n++;
    }
    for (int i = 0; i < 512; i++) tk->cp2byte[i] = -1;
    for (int b = 0; b < 256; b++) tk->cp2byte[tk->byte2cp[b]] = b;

    map_init(&tk->vocab, tk->n);
    tk->special = xalloc(tk->n, sizeof(int));
    for (int i = 0; i < tk->n; i++) {
        map_put(&tk->vocab, tk->tok[i], tk->toklen[i], i);
        if (tk->toktype && (tk->toktype[i] == 3 || tk->toktype[i] == 4) && tk->toklen[i] > 0)
            tk->special[tk->n_special++] = i;
    }
    map_init(&tk->merges, n_merges);
    for (int i = 0; i < n_merges; i++) map_put(&tk->merges, merges[i], mlen[i], i);
    for (int j = 0; j < tk->n_special; j++) tk->spec1[(uint8_t)tk->tok[tk->special[j]][0]] = 1;
}

static void gpu_setup(Model *m);
static void mat_row(float *out, const Mat *W, int row, int cols);
static void embed_row(const Model *m, float *out, int tok);
static int g_gelu;      /* feed-forward com GELU (tanh) em vez de SiLU (gemma4) */
#define G4_NB 16        /* gemma4: tokens por lote no prompt (CPU; = GPU_BMAX, limite do moe_cpu_multi) */
static void shards_open(const char *path, int64_t fsz);

/* Aquecimento dos especialistas (MoE): no Windows a primeira leitura de cada pagina do arquivo
   mapeado custa caro, e num MoE cada token toca especialistas novos. Algumas threads de baixa
   prioridade percorrem os tensores de especialistas logo na carga, para esse custo nao cair
   no caminho dos primeiros tokens. KATANA_WARM=0 desliga. */
#define WARM_T 4
typedef struct { const Model *m; int t; } WarmArg;
static volatile long warm_sink;
#ifdef _WIN32
static DWORD WINAPI warm_thread(LPVOID a) {
    const WarmArg *w = a;
    const Config *c = &w->m->cfg;
    const size_t d = c->dim, H = c->hidden, E = c->n_expert;
    long acc = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int l = w->t; l < c->n_layers; l += WARM_T) {
        const Layer *L = &w->m->layers[l];
        const Mat *ms[3] = { &L->eg[0], &L->eu[0], &L->ed[0] };
        for (int k = 0; k < 3; k++) {
            if (!ms[k]->q) continue;
            const size_t len = E * (k < 2 ? H : d) * ms[k]->stride;
            for (size_t o = 0; o < len; o += 4096) acc += ms[k]->q[o];
        }
    }
    warm_sink += acc;
    return 0;
}
#endif
static void warm_start(const Model *m) {
#ifdef _WIN32
    if (getenv("KATANA_WARM") && atoi(getenv("KATANA_WARM")) == 0) return;
    for (int t = 0; t < WARM_T; t++) {
        WarmArg *w = xalloc(1, sizeof(WarmArg));
        w->m = m; w->t = t;
        HANDLE h = CreateThread(NULL, 0, warm_thread, w, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    (void)m;
#endif
}
static void prefetch_setup(int64_t fsz);

/* mapeia o arquivo em memoria (somente leitura): o SO carrega as paginas sob demanda
   e o mapeamento fica vivo ate o processo terminar */
static const uint8_t *map_file(const char *path, int64_t *size) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = xalloc(wn, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) return NULL;
    HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mp) return NULL;
    *size = sz.QuadPart;
    return MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0);
#else
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    *size = st.st_size;
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void sample_preset(const char *arch);
static void spec_apply(Model *m);
/* mistura data e tamanho de um arquivo na identidade dos pesos: duas fusoes da mesma base tem o mesmo tamanho, o mesmo
   numero de parametros e o mesmo output_norm, e o cache de prefixo de uma nao serve para a outra */
static void model_fid(Model *m, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    uint64_t v[3] = { (uint64_t)st.st_size, (uint64_t)st.st_mtime, 0 };
#if !defined(_WIN32) && !defined(__APPLE__)
    v[2] = (uint64_t)st.st_mtim.tv_nsec;
#endif
    for (int i = 0; i < 3; i++) m->fid = (m->fid ^ v[i]) * 0x9E3779B97F4A7C15ULL + (m->fid >> 29);
}
static int g_spec_tools = 0;        /* uma especializacao chamada tools.kspec foi aplicada: o modelo ja conhece as ferramentas do agente */
static void load_model(Model *m, const char *path) {
    int64_t fsz = 0;
    const uint8_t *buf = map_file(path, &fsz);
    if (!buf) { fprintf(stderr, "Nao consegui abrir %s\n", path); exit(1); }

    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) die("nao e um arquivo GGUF");
    uint32_t ver = rd_u32(&r);
    if (ver < 2) die("versao de GGUF nao suportada");
    uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);

    Config *c = &m->cfg;
    c->rope_base = 10000.0f; c->eps = 1e-5f; c->bos = -1; c->eos = -1; c->add_bos = -1;
    char *tmpl = NULL;
    uint64_t align = 32;
    const char **merges = NULL; int *mlen = NULL; int n_merges = 0;
    Tokenizer *tk = &m->tk;
    static int g4_nkv[1024], g4_swa[1024];      /* gemma4: cabecas kv e janela, por camada */
    int g4_nkv_n = 0, g4_swa_n = 0;

    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) {
            int sl; const char *s = rd_str(&r, &sl);
            if (sl > 63) sl = 63;
            memcpy(c->arch, s, sl);
        } else if (t == 8 && key_is(k, kl, "katana.no_tool_decl")) {   /* GGUF fundido de uma especializacao que conhece as ferramentas */
            int sl; const char *s = rd_str(&r, &sl);
            if (sl > 0 && s[0] == '1') g_spec_tools = 1;
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.tokens")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("tokens: tipo inesperado");
            tk->n = (int)n;
            tk->tok = xalloc(n, sizeof(char *)); tk->toklen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                tk->tok[j] = xalloc(sl + 1, 1); memcpy(tk->tok[j], s, sl); tk->toklen[j] = sl;
            }
        } else if (t == 8 && key_is(k, kl, "tokenizer.ggml.model")) {
            int sl; const char *sv = rd_str(&r, &sl);
            tk->spm = sl == 5 && memcmp(sv, "llama", 5) == 0;
            if (sl == 6 && memcmp(sv, "gemma4", 6) == 0) tk->spm = 2;
        } else if (t == 8 && key_is(k, kl, "tokenizer.ggml.pre")) {
            int sl; const char *sv = rd_str(&r, &sl);
            tk->pre = pre_kind(sv, sl);
            tk->nfc = pre_has_nfc(tk->pre);
        } else if (t == 8 && key_is(k, kl, "tokenizer.chat_template")) {
            int sl; const char *sv = rd_str(&r, &sl);
            tmpl = xalloc((size_t)sl + 1, 1); memcpy(tmpl, sv, sl);
            c->tool_xml = strstr(tmpl, "<function=") != NULL;
            c->glm_args = strstr(tmpl, "<arg_key>") != NULL;
            c->tool_tmpl = strstr(tmpl, "tool") != NULL;
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.scores")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            tk->score = xalloc(n, sizeof(float));
            for (uint64_t j = 0; j < n; j++) tk->score[j] = (float)rd_num(&r, et);
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.merges")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("merges: tipo inesperado");
            n_merges = (int)n;
            merges = xalloc(n, sizeof(char *)); mlen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                char *cp = xalloc(sl + 1, 1); memcpy(cp, s, sl);
                merges[j] = cp; mlen[j] = sl;
            }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.token_type")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            tk->toktype = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) tk->toktype[j] = (int)rd_num(&r, et);
        } else if (t == 9 && (key_ends(k, kl, ".attention.head_count_kv") || key_ends(k, kl, ".attention.sliding_window_pattern"))) {
            const int kv = key_ends(k, kl, ".attention.head_count_kv");
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et == 8 || et == 9) die("lista por camada: tipo inesperado");
            for (uint64_t j = 0; j < n; j++) { const int v = (int)rd_num(&r, et); if (j < 1024) (kv ? g4_nkv : g4_swa)[j] = v; }
            if (kv) { g4_nkv_n = n < 1024 ? (int)n : 1024; for (int j = 0; j < g4_nkv_n; j++) if (g4_nkv[j] > c->n_kv) c->n_kv = g4_nkv[j]; }
            else g4_swa_n = n < 1024 ? (int)n : 1024;
        } else if (t != 8 && t != 9) {
            double v = rd_num(&r, t);
            if (key_ends(k, kl, ".block_count")) c->n_layers = (int)v;
            else if (key_ends(k, kl, ".attention.sliding_window")) c->swa_win = (int)v;
            else if (key_ends(k, kl, ".attention.key_length_swa")) c->hd_swa = (int)v;
            else if (key_ends(k, kl, ".rope.freq_base_swa")) c->rope_base_swa = (float)v;
            else if (key_ends(k, kl, ".final_logit_softcapping")) c->softcap = (float)v;
            else if (key_ends(k, kl, ".context_length")) c->ctx = (int)v;
            else if (key_ends(k, kl, ".embedding_length")) c->dim = (int)v;
            else if (key_ends(k, kl, ".expert_feed_forward_length")) c->hidden_exp = (int)v;
            else if (key_ends(k, kl, ".expert_shared_feed_forward_length")) c->hidden_shexp = (int)v;
            else if (key_ends(k, kl, ".ssm.conv_kernel")) c->ssm_conv = (int)v;
            else if (key_ends(k, kl, ".ssm.state_size")) c->ssm_state = (int)v;
            else if (key_ends(k, kl, ".ssm.group_count")) c->ssm_groups = (int)v;
            else if (key_ends(k, kl, ".ssm.time_step_rank")) c->ssm_rank = (int)v;
            else if (key_ends(k, kl, ".ssm.inner_size")) c->ssm_inner = (int)v;
            else if (key_ends(k, kl, ".full_attention_interval")) c->attn_every = (int)v;
            else if (key_ends(k, kl, ".feed_forward_length")) c->hidden = (int)v;
            else if (key_ends(k, kl, ".expert_count")) c->n_expert = (int)v;
            else if (key_ends(k, kl, ".expert_used_count")) c->n_used = (int)v;
            else if (key_ends(k, kl, ".attention.head_count")) c->n_heads = (int)v;
            else if (key_ends(k, kl, ".attention.head_count_kv")) c->n_kv = (int)v;
            else if (key_ends(k, kl, ".attention.key_length")) c->hd_key = (int)v;
            else if (key_ends(k, kl, ".attention.value_length")) c->hd_v = (int)v;
            else if (key_ends(k, kl, ".attention.key_length_mla")) c->hd_mla_k = (int)v;
            else if (key_ends(k, kl, ".attention.value_length_mla")) c->hd_mla_v = (int)v;
            else if (key_ends(k, kl, ".attention.kv_lora_rank")) c->mla = (int)v;
            else if (key_ends(k, kl, ".attention.q_lora_rank")) c->q_lora = (int)v;
            else if (key_ends(k, kl, ".rope.dimension_count")) c->rope_dim = (int)v;
            else if (key_ends(k, kl, ".leading_dense_block_count")) c->n_dense = (int)v;
            else if (key_ends(k, kl, ".nextn_predict_layers")) c->n_nextn = (int)v;
            else if (key_ends(k, kl, ".expert_shared_count")) c->n_shared = (int)v;
            else if (key_ends(k, kl, ".expert_weights_scale")) c->exp_scale = (float)v;
            else if (key_ends(k, kl, ".expert_weights_norm")) c->moe_norm = (int)v;
            else if (key_ends(k, kl, ".expert_gating_func")) c->gate_sig = (int)v == 2;
            else if (key_ends(k, kl, ".rope.scaling.factor")) c->yarn_factor = (float)v;
            else if (key_ends(k, kl, ".rope.scaling.original_context_length")) c->yarn_orig = (int)v;
            else if (key_ends(k, kl, ".rope.scaling.yarn_log_multiplier")) c->yarn_logmul = (float)v;
            else if (key_ends(k, kl, ".rope.freq_base")) c->rope_base = (float)v;
            else if (key_ends(k, kl, ".layer_norm_rms_epsilon")) c->eps = (float)v;
            else if (key_is(k, kl, "tokenizer.ggml.bos_token_id")) c->bos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.eos_token_id")) c->eos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.add_bos_token")) c->add_bos = (int)v;
            else if (key_is(k, kl, "general.alignment")) align = (uint64_t)v;
        } else rd_skip(&r, t);
    }

    if (strcmp(c->arch, "qwen2") == 0) c->rope_neox = 1;
    else if (strcmp(c->arch, "olmoe") == 0) { c->rope_neox = 1; c->qk_norm = 1; }
    else if (strcmp(c->arch, "qwen3") == 0) { c->rope_neox = 1; c->qk_norm = 2; }
    else if (strcmp(c->arch, "qwen3moe") == 0) { c->rope_neox = 1; c->qk_norm = 2; c->moe_norm = 1; }
    else if (strcmp(c->arch, "deepseek2") == 0) c->rope_neox = 0;
    else if (strcmp(c->arch, "qwen35moe") == 0) { c->rope_neox = 1; c->qk_norm = 2; c->moe_norm = 1; c->attn_gated = 1; c->think = 1; }
    else if (strcmp(c->arch, "qwen35") == 0) { c->rope_neox = 1; c->qk_norm = 2; c->attn_gated = 1; c->think = 1; }   /* Qwen3.8 denso: DeltaNet + atencao com portao + FFN denso */
    else if (strcmp(c->arch, "gemma4") == 0) {
        c->g4 = 1; c->rope_neox = 1; c->moe_norm = 1; g_gelu = 1;
        c->emb_scale = sqrtf((float)c->dim);
        if (!c->hd_swa) c->hd_swa = c->hd_key;
        if (!c->rope_base_swa) c->rope_base_swa = 10000.0f;
        if (!c->swa_win) c->swa_win = 1 << 30;
        if (!c->hd_key || c->hd_key % 2 || c->hd_swa % 2) die("gemma4: largura de cabeca invalida");
    }
    else if (strcmp(c->arch, "gpt-oss") == 0) {
        c->go = 1; c->rope_neox = 1; c->moe_norm = 1;
        if (!c->yarn_logmul) c->yarn_logmul = 0.1f;   /* mscale padrao do yarn do gpt-oss */
        if (!c->swa_win) c->swa_win = 128;
    }
    else if (strcmp(c->arch, "glm4") == 0) { c->glm = 1; }       /* RoPE NORM (pares consecutivos), parcial */
    else if (strcmp(c->arch, "glm4moe") == 0) { c->glmoe = 1; c->rope_neox = 1; c->moe_norm = 1; }   /* RoPE NEOX parcial; roteador sigmoide+vies, compartilhado */
    else if (strcmp(c->arch, "llama") != 0)
        fprintf(stderr, "Aviso: arquitetura '%s' nao testada (so 'llama' e 'qwen2' por enquanto)\n", c->arch);
    if (c->n_nextn > 0 && c->n_nextn < c->n_layers) c->n_layers -= c->n_nextn;   /* camadas MTP/nextn: tensores existem mas nao sao processados */
    if (getenv("KATANA_ROPE")) c->rope_neox = atoi(getenv("KATANA_ROPE"));
    c->kq_mul = 1.0f;
    if (!c->exp_scale) c->exp_scale = 1.0f;
    if (c->mla) {       /* cabeca de q/k = nope + rope; o cache e o latente, entao kv generico fica minimo */
        if (c->hd_mla_k) c->hd_key = c->hd_mla_k;
        if (c->hd_mla_v) c->hd_v = c->hd_mla_v;
        if (!c->rope_dim) c->rope_dim = 64;
        c->n_kv = 1;
        if (!c->hd_v || c->hd_key <= c->rope_dim || c->rope_dim % 2) die("MLA: dimensoes invalidas");
    }
    if (c->ssm_inner) {
        if (!c->attn_every || !c->ssm_conv || !c->ssm_state || !c->ssm_groups || !c->ssm_rank || c->ssm_rank % c->ssm_groups
            || c->ssm_inner % c->ssm_rank || c->ssm_conv > 8) die("DeltaNet: dimensoes invalidas");
    }
    c->hidden_dense = c->hidden;
    if (c->n_expert) {
        if (c->hidden_exp) c->hidden = c->hidden_exp;      /* daqui em diante, hidden = largura de UM especialista */
        if (c->hidden_shexp && !c->n_shared && c->hidden_shexp % c->hidden == 0) c->n_shared = c->hidden_shexp / c->hidden;
        if (c->n_used < 1 || c->n_used > 64 || c->n_used > c->n_expert || c->n_expert > 1024) die("MoE: contagem de especialistas invalida");
    }
    if (!c->dim || !c->n_layers || !c->n_heads || !c->hidden || !tk->n) die("metadados incompletos no GGUF");
    if (!c->n_kv) c->n_kv = c->n_heads;
    c->vocab = tk->n;
    c->hd = c->hd_key > 0 ? c->hd_key : c->dim / c->n_heads;       /* q tem n_heads*hd, que pode passar de dim */
    if ((!c->hd_key && c->dim % c->n_heads) || c->hd % 2 || c->n_heads % c->n_kv) die("dimensoes de atencao invalidas");
    const int model_ctx = c->ctx > 0 ? c->ctx : MAX_CTX;
    c->ctx = model_ctx < MAX_CTX ? model_ctx : MAX_CTX;
    c->ctx_train = model_ctx;
    if (getenv("KATANA_CTX")) {     /* menor = cache KV menor; maior que o treinado = estende o RoPE (yarn) */
        const long want = atol(getenv("KATANA_CTX"));
        if (want >= 64) c->ctx = want < (1L << 24) ? (int)want : (1 << 24);
    }
    {   /* escala de RoPE: KATANA_ROPE_SCALE=f (1 desliga); sem ela, automatica quando ctx passa do treinado */
        float f = c->ctx > model_ctx ? (float)c->ctx / model_ctx : 1.0f;
        if (getenv("KATANA_ROPE_SCALE")) f = (float)atof(getenv("KATANA_ROPE_SCALE"));
        if (f > 1.0f && !(c->yarn_factor > 1.0f)) {
            c->yarn_factor = f; c->yarn_orig = model_ctx;
            if (!c->yarn_logmul) c->yarn_logmul = 0.1f;
            c->yarn_ext = 1;
        }
    }
    if (!merges && !tk->spm) die("GGUF sem merges BPE nem SentencePiece (tokenizador nao suportado)");
    if (tk->spm == 2 && !merges) die("gemma4: GGUF sem merges");
    if (tk->spm && !tk->score) tk->score = xalloc(tk->n, sizeof(float));
    if (c->add_bos < 0) c->add_bos = tk->spm;      /* SentencePiece comeca com BOS por padrao */
    tokenizer_finish(tk, merges, mlen, n_merges);
    /* formato de conversa: pelo template do proprio modelo; sem template, pelo vocabulario */
    if (tmpl && strstr(tmpl, "[gMASK]") && strstr(tmpl, "<sop>")) c->chat_fmt = 10;   /* glm4 */
    else if (tmpl && strstr(tmpl, "<|turn>")) c->chat_fmt = 8;
    else if (tmpl && strstr(tmpl, "<|im_start|>")) c->chat_fmt = 0;
    else if (tmpl && strstr(tmpl, "<|start_header_id|>")) c->chat_fmt = 1;
    else if (tmpl && strstr(tmpl, "<|user|>")) c->chat_fmt = 2;
    else if (tmpl && strstr(tmpl, "[INST]")) c->chat_fmt = 3;
    else if (tmpl && strstr(tmpl, "<|im_user|>")) c->chat_fmt = 6;
    else if (tmpl && strstr(tmpl, "'User: '")) c->chat_fmt = 5;
    else if (map_get(&tk->vocab, "<|im_start|>", 12) >= 0) c->chat_fmt = 0;
    else if (map_get(&tk->vocab, "<|start_header_id|>", 19) >= 0) c->chat_fmt = 1;
    else c->chat_fmt = 4;
    if (tmpl && strcmp(c->arch, "olmoe") == 0 && strstr(tmpl, "<|user|>")) c->chat_fmt = 7;
    if (c->go) c->chat_fmt = 9;     /* gpt-oss: formato harmony */
    if (c->chat_fmt != 10) c->glm_args = 0;
    if (c->glm_args && strstr(tmpl, "</think>")) c->think = 1;     /* a resposta abre com <think> (ou </think>, sem raciocinio) */
    if (tmpl && strstr(tmpl, "You are Qwen, created by Alibaba Cloud. You are a helpful assistant."))
        c->def_sys = "You are Qwen, created by Alibaba Cloud. You are a helpful assistant.";
    if (tmpl && c->chat_fmt == 8 && strstr(tmpl, "| trim")) c->trim_msgs = 1;
    if (tmpl && strstr(tmpl, "|trim")) c->trim_msgs = 1;
    if (tmpl && strstr(tmpl, "Cutting Knowledge Date")) c->llama_hdr = 1;
    if (c->bos >= 0 && c->bos < tk->n && tk->toklen[c->bos] < 48) { memcpy(c->bos_text, tk->tok[c->bos], tk->toklen[c->bos]); }
    if (c->eos >= 0 && c->eos < tk->n && tk->toklen[c->eos] < 48) { memcpy(c->eos_text, tk->tok[c->eos], tk->toklen[c->eos]); }
    free(tmpl);
    if (g_vocab_only) return;

    Tensors ts;
    ts.n = n_tensors; ts.t = xalloc(n_tensors, sizeof(TInfo));
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts.t[i];
        t->name = rd_str(&r, &t->namelen);
        t->ndims = rd_u32(&r);
        if (t->ndims > 4) die("tensor com dimensoes demais");
        for (uint32_t d = 0; d < t->ndims; d++) t->dims[d] = rd_u64(&r);
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
    }
    uint64_t pos = (uint64_t)(r.p - buf);
    pos = (pos + align - 1) / align * align;
    ts.data = buf + pos; ts.end = buf + fsz;

    size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t qd = (size_t)c->n_heads * c->hd, dq = qd > d ? qd : d;
    size_t *np = &m->n_params;
    m->tok_emb = get_mat(&ts, "token_embd.weight", V, d, 1, np);
    m->out_norm = get_tensor(&ts, "output_norm.weight", d, 1, np);
    m->lm_head = get_mat(&ts, "output.weight", V, d, 0, np);
    if (!m->lm_head.f && !m->lm_head.q) m->lm_head = m->tok_emb;   /* pesos amarrados */
    m->layers = xalloc(c->n_layers, sizeof(Layer));
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        char nm[96];
#define V1(field, suffix) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_tensor(&ts, nm, d, 1, np)
#define M2(field, suffix, rows, cols) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_mat(&ts, nm, rows, cols, 1, np)
        if (c->g4) {
            L->swa = l < g4_swa_n ? g4_swa[l] : 0;
            L->nkv_l = l < g4_nkv_n && g4_nkv[l] > 0 ? g4_nkv[l] : c->n_kv;
            L->hd_l = L->swa ? c->hd_swa : c->hd;
            if (c->n_heads % L->nkv_l) die("gemma4: cabecas kv invalidas");
            const size_t hq = (size_t)c->n_heads * L->hd_l, hk = (size_t)L->nkv_l * L->hd_l, Hd = c->hidden_dense;
            V1(attn_norm, "attn_norm.weight");
            M2(wq, "attn_q.weight", hq, d);
            M2(wk, "attn_k.weight", hk, d);
            snprintf(nm, sizeof nm, "blk.%d.attn_v.weight", l); L->wv = get_mat(&ts, nm, hk, d, 0, np);   /* sem attn_v: v sai de k */
            M2(wo, "attn_output.weight", d, hq);
            snprintf(nm, sizeof nm, "blk.%d.attn_q_norm.weight", l); L->q_norm = get_tensor(&ts, nm, L->hd_l, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.attn_k_norm.weight", l); L->k_norm = get_tensor(&ts, nm, L->hd_l, 1, np);
            V1(post_attn_norm, "post_attention_norm.weight");
            V1(ffn_norm, "ffn_norm.weight");
            M2(w_gate, "ffn_gate.weight", Hd, d);
            M2(w_up, "ffn_up.weight", Hd, d);
            M2(w_down, "ffn_down.weight", d, Hd);
            V1(post_ffn_norm, "post_ffw_norm.weight");
            {
                snprintf(nm, sizeof nm, "blk.%d.layer_output_scale.weight", l);
                float *os = get_tensor(&ts, nm, 1, 0, np);
                L->out_scale = os ? os[0] : 1.0f;
                free(os);
            }
            if (c->n_expert) {
                const size_t E = c->n_expert;
                snprintf(nm, sizeof nm, "blk.%d.ffn_gate_inp.weight", l); L->router = get_mat(&ts, nm, E, d, 0, np);
            }
            if (L->router.q || L->router.f) {
                const size_t E = c->n_expert;
                V1(pre_ffn2, "pre_ffw_norm_2.weight");
                V1(post_ffn1, "post_ffw_norm_1.weight");
                V1(post_ffn2, "post_ffw_norm_2.weight");
                V1(router_scale, "ffn_gate_inp.scale");
                snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.scale", l); L->exp_w = get_tensor(&ts, nm, E, 0, np);
                /* gate e up fundidos: em cada especialista, H linhas de gate e depois H de up */
                Mat gu, dn;
                snprintf(nm, sizeof nm, "blk.%d.ffn_gate_up_exps.weight", l); gu = get_mat(&ts, nm, E * 2 * H, d, 1, np);
                snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.weight", l); dn = get_mat(&ts, nm, E * d, H, 1, np);
                L->eg = xalloc(E, sizeof(Mat)); L->eu = xalloc(E, sizeof(Mat)); L->ed = xalloc(E, sizeof(Mat));
                for (size_t e = 0; e < E; e++) {
                    L->eg[e] = gu; L->eu[e] = gu; L->ed[e] = dn;
                    if (gu.q) { L->eg[e].q += e * 2 * H * gu.stride; L->eu[e].q += (e * 2 + 1) * H * gu.stride; }
                    else { L->eg[e].f += e * 2 * H * d; L->eu[e].f += (e * 2 + 1) * H * d; }
                    if (dn.q) L->ed[e].q += e * d * dn.stride; else L->ed[e].f += e * d * H;
                }
            }
            continue;
        }
        V1(attn_norm, "attn_norm.weight");
        L->lin = c->ssm_inner && (l + 1) % c->attn_every != 0;
        if (L->lin) L->lin_i = m->n_lin++;
        if (L->lin) {
            const size_t nv = c->ssm_rank, C = 2 * (size_t)c->ssm_groups * c->ssm_state + c->ssm_inner;
            M2(wqkv, "attn_qkv.weight", C, d);
            M2(wz, "attn_gate.weight", (size_t)c->ssm_inner, d);
            M2(walpha, "ssm_alpha.weight", nv, d);
            M2(wbeta, "ssm_beta.weight", nv, d);
            M2(wssm_out, "ssm_out.weight", d, (size_t)c->ssm_inner);
            snprintf(nm, sizeof nm, "blk.%d.ssm_conv1d.weight", l); L->conv_w = get_tensor(&ts, nm, C * c->ssm_conv, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ssm_a", l);             L->ssm_a = get_tensor(&ts, nm, nv, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ssm_dt.bias", l);       L->dt_bias = get_tensor(&ts, nm, nv, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ssm_norm.weight", l);   L->ssm_norm = get_tensor(&ts, nm, (size_t)c->ssm_inner / nv, 1, np);
        } else if (c->attn_gated) {
            M2(wq, "attn_q.weight", 2 * qd, d);
            M2(wk, "attn_k.weight", kvd, d);
            M2(wv, "attn_v.weight", kvd, d);
            M2(wo, "attn_output.weight", d, qd);
        } else if (c->mla && c->q_lora) {
            M2(wq_a, "attn_q_a.weight", (size_t)c->q_lora, d);
            snprintf(nm, sizeof nm, "blk.%d.attn_q_a_norm.weight", l); L->q_a_norm = get_tensor(&ts, nm, c->q_lora, 1, np);
            M2(wq_b, "attn_q_b.weight", qd, (size_t)c->q_lora);
        } else { M2(wq, "attn_q.weight", qd, d); }
        if (c->ssm_inner) ;
        else if (c->mla) {
            const size_t R = c->mla, nr = c->rope_dim, nope = c->hd - c->rope_dim, hv = c->hd_v, nh = c->n_heads;
            M2(wkv_a, "attn_kv_a_mqa.weight", R + nr, d);
            snprintf(nm, sizeof nm, "blk.%d.attn_kv_a_norm.weight", l); L->kv_a_norm = get_tensor(&ts, nm, R, 1, np);
            M2(wo, "attn_output.weight", d, nh * hv);
            /* k_b e v_b viram float: q e absorvida por k_b e a saida latente passa por v_b */
            L->wkb = xalloc(nh * R * nope, sizeof(float)); L->wvb = xalloc(nh * hv * R, sizeof(float));
            snprintf(nm, sizeof nm, "blk.%d.attn_kv_b.weight", l);
            Mat kvb = get_mat(&ts, nm, nh * (nope + hv), R, 0, np);
            if (kvb.q || kvb.f) {   /* forma unica: por cabeca, nope linhas de k e depois hd_v linhas de v */
                float *row = xalloc(R, sizeof(float));
                for (size_t h = 0; h < nh; h++) {
                    for (size_t i = 0; i < nope; i++) {
                        mat_row(row, &kvb, (int)(h * (nope + hv) + i), (int)R);
                        for (size_t r2 = 0; r2 < R; r2++) L->wkb[(h * R + r2) * nope + i] = row[r2];
                    }
                    for (size_t i = 0; i < hv; i++) mat_row(L->wvb + (h * hv + i) * R, &kvb, (int)(h * (nope + hv) + nope + i), (int)R);
                }
                free(row);
            } else {                /* forma separada: k_b [cabeca][latente][nope], v_b [cabeca][hd_v][latente] */
                Mat kb, vb;
                snprintf(nm, sizeof nm, "blk.%d.attn_k_b.weight", l); kb = get_mat(&ts, nm, nh * R, nope, 1, np);
                snprintf(nm, sizeof nm, "blk.%d.attn_v_b.weight", l); vb = get_mat(&ts, nm, nh * hv, R, 1, np);
                for (size_t j = 0; j < nh * R; j++) mat_row(L->wkb + j * nope, &kb, (int)j, (int)nope);
                for (size_t j = 0; j < nh * hv; j++) mat_row(L->wvb + j * R, &vb, (int)j, (int)R);
            }
        } else {
            M2(wk, "attn_k.weight", kvd, d);
            M2(wv, "attn_v.weight", kvd, d);
            M2(wo, "attn_output.weight", d, qd);
        }
        if (c->ssm_inner || c->glmoe || c->go) { V1(ffn_norm, "post_attention_norm.weight"); }
        else { V1(ffn_norm, "ffn_norm.weight"); }
        if (c->n_expert && l >= c->n_dense) {
            /* tensores empilhados: o especialista e ocupa a fatia e, com as linhas inteiras */
            const size_t E = c->n_expert;
            M2(router, "ffn_gate_inp.weight", E, d);
            Mat sg, su, sd;
            snprintf(nm, sizeof nm, "blk.%d.ffn_gate_exps.weight", l); sg = get_mat(&ts, nm, E * H, d, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_up_exps.weight", l);   su = get_mat(&ts, nm, E * H, d, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.weight", l); sd = get_mat(&ts, nm, E * d, H, 1, np);
            L->eg = xalloc(E, sizeof(Mat)); L->eu = xalloc(E, sizeof(Mat)); L->ed = xalloc(E, sizeof(Mat));
            for (size_t e = 0; e < E; e++) {
                L->eg[e] = sg; L->eu[e] = su; L->ed[e] = sd;
                if (sg.q) L->eg[e].q += e * H * sg.stride; else L->eg[e].f += e * H * d;
                if (su.q) L->eu[e].q += e * H * su.stride; else L->eu[e].f += e * H * d;
                if (sd.q) L->ed[e].q += e * d * sd.stride; else L->ed[e].f += e * d * H;
            }
            L->w_gate = L->eg[0]; L->w_up = L->eu[0]; L->w_down = L->ed[0];    /* para os testes por matriz */
            if (c->n_shared) {
                const size_t Hs = (size_t)c->n_shared * H;
                M2(sg, "ffn_gate_shexp.weight", Hs, d);
                M2(su, "ffn_up_shexp.weight", Hs, d);
                M2(sd, "ffn_down_shexp.weight", d, Hs);
            }
            snprintf(nm, sizeof nm, "blk.%d.exp_probs_b.bias", l); L->exp_bias = get_tensor(&ts, nm, E, 0, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_gate_inp_shexp.weight", l); L->shexp_gate = get_tensor(&ts, nm, d, 0, np);
            if (c->go) {
                snprintf(nm, sizeof nm, "blk.%d.ffn_gate_inp.bias", l);  L->router_b = get_tensor(&ts, nm, E, 0, np);
                snprintf(nm, sizeof nm, "blk.%d.ffn_gate_exps.bias", l); L->eg_b = get_tensor(&ts, nm, E * H, 0, np);
                snprintf(nm, sizeof nm, "blk.%d.ffn_up_exps.bias", l);   L->eu_b = get_tensor(&ts, nm, E * H, 0, np);
                snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.bias", l); L->ed_b = get_tensor(&ts, nm, E * d, 0, np);
            }
        } else if (c->glm) {
            V1(post_attn_norm, "post_attention_norm.weight");
            V1(post_ffn_norm, "post_ffw_norm.weight");
            /* gate e up fundidos em ffn_up: primeiras H linhas sao o gate, as H seguintes o up */
            Mat gu;
            snprintf(nm, sizeof nm, "blk.%d.ffn_up.weight", l); gu = get_mat(&ts, nm, 2 * H, d, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_down.weight", l); L->w_down = get_mat(&ts, nm, d, H, 1, np);
            L->w_gate = gu; L->w_up = gu;
            if (gu.q) L->w_up.q += (size_t)H * gu.stride; else L->w_up.f += (size_t)H * d;
        } else {
            const size_t Hd = c->n_expert ? (size_t)c->hidden_dense : H;
            M2(w_gate, "ffn_gate.weight", Hd, d);
            M2(w_up, "ffn_up.weight", Hd, d);
            M2(w_down, "ffn_down.weight", d, Hd);
        }
        if (c->qk_norm && !L->lin) {
            snprintf(nm, sizeof nm, "blk.%d.attn_q_norm.weight", l); L->q_norm = get_tensor(&ts, nm, c->qk_norm == 1 ? d : (size_t)c->hd, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.attn_k_norm.weight", l); L->k_norm = get_tensor(&ts, nm, c->qk_norm == 1 ? kvd : (size_t)c->hd, 1, np);
        }
        snprintf(nm, sizeof nm, "blk.%d.attn_q.bias", l);      L->bq = get_tensor(&ts, nm, qd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_k.bias", l);      L->bk = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_v.bias", l);      L->bv = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_output.bias", l); L->bo = get_tensor(&ts, nm, d, 0, np);
        if (c->go) { snprintf(nm, sizeof nm, "blk.%d.attn_sinks.weight", l); L->sinks = get_tensor(&ts, nm, c->n_heads, 0, np); L->swa = (l % 2 == 0); }
#undef V1
#undef M2
    }
    float *g4_ff = c->g4 ? get_tensor(&ts, "rope_freqs.weight", c->hd / 2, 0, np) : NULL;     /* divisores das frequencias globais */
    g_unread.name = xalloc(n_tensors + 1, 96); g_unread.bytes = xalloc(n_tensors + 1, sizeof(uint64_t)); g_unread.n = 0;
    for (uint64_t i = 0; i < n_tensors; i++) if (!ts.t[i].used) {
        size_t ne = 1; for (uint32_t k = 0; k < ts.t[i].ndims; k++) ne *= (size_t)ts.t[i].dims[k];
        snprintf(g_unread.name[g_unread.n], 96, "%.*s", ts.t[i].namelen < 95 ? ts.t[i].namelen : 95, ts.t[i].name);
        g_unread.bytes[g_unread.n++] = qbytes(ts.t[i].type, ne);
    }
    free(ts.t);   /* o mapeamento do arquivo continua vivo: os pesos apontam para ele */

    size_t ctx = c->ctx, L = c->n_layers;
    if (c->g4) {    /* cache por camada: as locais guardam so a janela (anel), as globais o contexto todo */
        size_t tot = 0;
        for (int l = 0; l < c->n_layers; l++) {
            Layer *Ly = &m->layers[l];
            Ly->kv_cap = Ly->swa && (size_t)c->swa_win + G4_NB < ctx ? c->swa_win + G4_NB : (int)ctx;   /* folga: um lote grava antes de atender */
            Ly->kv_off = tot;
            tot += (size_t)Ly->kv_cap * Ly->nkv_l * Ly->hd_l;
        }
        const char *e16 = getenv("KATANA_CPU_KV16");      /* padrao: fp16 a partir de 64k de contexto */
        c->kv16_cpu = e16 ? atoi(e16) != 0 : ctx > 65536;
        if (c->kv16_cpu) { m->kh = xalloc(tot, 2); m->vh = xalloc(tot, 2); }
        else { m->kcache = xalloc(tot, sizeof(float)); m->vcache = xalloc(tot, sizeof(float)); }
    } else if (c->ssm_inner) {    /* hibrido: cache so nas camadas de atencao completa */
        size_t na = 0;
        for (int l = 0; l < c->n_layers; l++) {
            Layer *Ly = &m->layers[l];
            Ly->kv_off = Ly->lin ? 0 : na++ * ctx * kvd;
        }
        if (!na) na = 1;
        const char *e16 = getenv("KATANA_CPU_KV16");      /* padrao: fp16 a partir de 64k de contexto */
        c->kv16_cpu = e16 ? atoi(e16) != 0 : ctx > 65536;
        c->kv8_cpu = kv8_escolha(ctx) && kvd % 32 == 0 && c->hd % 32 == 0;
        if (c->kv8_cpu) { c->kv16_cpu = 0; m->kq8 = xalloc(na * ctx * kvd / 32, 34); m->vq8 = xalloc(na * ctx * kvd / 32, 34); }
        else if (c->kv16_cpu) { m->kh = xalloc(na * ctx * kvd, 2); m->vh = xalloc(na * ctx * kvd, 2); }
        else { m->kcache = xalloc(na * ctx * kvd, sizeof(float)); m->vcache = xalloc(na * ctx * kvd, sizeof(float)); }
    } else {
        for (int l = 0; l < c->n_layers; l++) m->layers[l].kv_off = (size_t)l * ctx * kvd;
        m->kcache = xalloc(L * ctx * kvd, sizeof(float));
        m->vcache = xalloc(L * ctx * kvd, sizeof(float));
    }
    m->x = xalloc(d, sizeof(float));   m->xb = xalloc(dq, sizeof(float));
    m->xb2 = xalloc(dq, sizeof(float)); m->q = xalloc(dq, sizeof(float));
    m->k = xalloc(kvd, sizeof(float)); m->v = xalloc(kvd, sizeof(float));
    m->att = xalloc(ctx, sizeof(float));
    {
        size_t Hm = H;
        if ((size_t)c->hidden_dense > Hm) Hm = c->hidden_dense;
        if ((size_t)c->n_shared * H > Hm) Hm = (size_t)c->n_shared * H;
        if ((size_t)c->q_lora > Hm) Hm = c->q_lora;
        m->hb = xalloc(Hm, sizeof(float));  m->hb2 = xalloc(Hm, sizeof(float));
    }
    if (c->mla) m->ckv = xalloc(L * ctx * (size_t)(c->mla + c->rope_dim), sizeof(float));
    if (c->ssm_inner) {
        const size_t C = 2 * (size_t)c->ssm_groups * c->ssm_state + c->ssm_inner;
        m->ssm = xalloc((size_t)m->n_lin * ((size_t)c->ssm_state * c->ssm_inner + (size_t)(c->ssm_conv - 1) * C), sizeof(float));
    }
    m->logits = xalloc(V, sizeof(float));
    m->rope_freq = xalloc(c->hd / 2, sizeof(float));
    for (int i = 0; i < c->hd / 2; i++)
        m->rope_freq[i] = powf(c->rope_base, -2.0f * i / c->hd);
    if (c->g4) {        /* globais: base propria e divisores do arquivo (rope proporcional); locais: base da janela */
        for (int i = 0; i < c->hd / 2; i++) m->rope_freq[i] = powf(c->rope_base, -2.0f * i / c->hd) / (g4_ff ? g4_ff[i] : 1.0f);
        m->rope_freq_swa = xalloc(c->hd_swa / 2, sizeof(float));
        for (int i = 0; i < c->hd_swa / 2; i++) m->rope_freq_swa[i] = powf(c->rope_base_swa, -2.0f * i / c->hd_swa);
        free(g4_ff);
    }
    if ((c->attn_gated || c->glm || c->glmoe) && c->rope_dim > 0 && c->rope_dim < c->hd)     /* RoPE parcial: so as primeiras rope_dim dimensoes giram */
        for (int i = 0; i < c->rope_dim / 2; i++) m->rope_freq[i] = powf(c->rope_base, -2.0f * i / c->rope_dim);
    if (c->mla) {       /* MLA: so rope_dim dimensoes giram; com yarn, mistura interpolacao e extrapolacao */
        const int nr = c->rope_dim;
        for (int i = 0; i < nr / 2; i++) m->rope_freq[i] = powf(c->rope_base, -2.0f * i / nr);
        if (c->yarn_factor > 1.0f && c->yarn_orig > 0) {
            const float fs = 1.0f / c->yarn_factor, tw = 6.28318530718f;
            float lo = floorf(nr * logf(c->yarn_orig / (32.0f * tw)) / (2.0f * logf(c->rope_base)));
            float hi = ceilf(nr * logf(c->yarn_orig / (1.0f * tw)) / (2.0f * logf(c->rope_base)));
            if (lo < 0) lo = 0;
            if (hi > nr - 1) hi = (float)(nr - 1);
            for (int i = 0; i < nr / 2; i++) {
                float y = (i - lo) / fmaxf(0.001f, hi - lo);
                y = y < 0 ? 0 : y > 1 ? 1 : y;
                const float ramp = 1.0f - y;      /* 1 = extrapola (frequencia original) */
                m->rope_freq[i] *= fs * (1.0f - ramp) + ramp;
            }
            const float ms = 1.0f + c->yarn_logmul * logf(c->yarn_factor);
            c->kq_mul = ms * ms;
        }
    }
    if (c->go && c->yarn_factor > 1.0f && c->yarn_orig > 0) {   /* yarn: mistura interpolacao/extrapolacao por dimensao */
        const int nr = c->hd;
        const float fs = 1.0f / c->yarn_factor, tw = 6.28318530718f;
        float lo = floorf(nr * logf(c->yarn_orig / (32.0f * tw)) / (2.0f * logf(c->rope_base)));
        float hi = ceilf(nr * logf(c->yarn_orig / (1.0f * tw)) / (2.0f * logf(c->rope_base)));
        if (lo < 0) lo = 0;
        if (hi > nr - 1) hi = (float)(nr - 1);
        for (int i = 0; i < nr / 2; i++) {
            float y = (i - lo) / fmaxf(0.001f, hi - lo);
            y = y < 0 ? 0 : y > 1 ? 1 : y;
            const float ramp = 1.0f - y;
            m->rope_freq[i] *= fs * (1.0f - ramp) + ramp;
        }
        const float ms = 1.0f + c->yarn_logmul * logf(c->yarn_factor);
        c->kq_mul = ms * ms;
    }
    if (c->yarn_ext && !c->mla && !c->go && !c->g4) {     /* yarn pedido pelo usuario: so as dimensoes que giram */
        const int nr = c->rope_dim > 0 && c->rope_dim < c->hd ? c->rope_dim : c->hd;
        const float fs = 1.0f / c->yarn_factor, tw = 6.28318530718f;
        float lo = floorf(nr * logf(c->yarn_orig / (32.0f * tw)) / (2.0f * logf(c->rope_base)));
        float hi = ceilf(nr * logf(c->yarn_orig / (1.0f * tw)) / (2.0f * logf(c->rope_base)));
        if (lo < 0) lo = 0;
        if (hi > nr - 1) hi = (float)(nr - 1);
        for (int i = 0; i < nr / 2; i++) {
            float y = (i - lo) / fmaxf(0.001f, hi - lo);
            y = y < 0 ? 0 : y > 1 ? 1 : y;
            const float ramp = 1.0f - y;
            m->rope_freq[i] *= fs * (1.0f - ramp) + ramp;
        }
        const float ms = 1.0f + c->yarn_logmul * logf(c->yarn_factor);
        c->kq_mul = ms * ms;
    }

    /* por ultimo: a GPU precisa de tudo acima ja preenchido (ex.: rope_freq) */
    m->fsize = fsz;
    model_fid(m, path);
    shards_open(path, fsz);
    prefetch_setup(fsz);
    if (c->n_expert) warm_start(m);
    sample_preset(c->arch);
    spec_apply(m);                      /* KATANA_SPEC: especializacao por cima do modelo base (app/train.c) */
    if (!getenv("KATANA_CPU")) gpu_setup(m);
}

