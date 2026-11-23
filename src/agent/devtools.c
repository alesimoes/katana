/* ---------- utilitarios de execucao: aspas de shell, captura de saida, hash, linhas finais ---------- */


/* json: valor booleano (true, "true" ou 1) da chave */
static int json_bool(const char *js, const char *end, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(js, pat);
    if (!q || q >= end) return 0;
    q += strlen(pat);
    while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t' || *q == '\r')) q++;
    if (q < end && *q == '"') q++;
    return q < end && (strncmp(q, "true", 4) == 0 || *q == '1');
}

/* ---- sha256 (so libc) ---- */
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; size_t bl; } Sha2;
static void sha2_block(Sha2 *s, const unsigned char *p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a, b, c, d, e, f, g, h;
#define ROR2(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR2(w[i-15], 7) ^ ROR2(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR2(w[i-2], 17) ^ ROR2(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR2(e, 6) ^ ROR2(e, 11) ^ ROR2(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ROR2(a, 2) ^ ROR2(a, 13) ^ ROR2(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
#undef ROR2
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha2_init(Sha2 *s) {
    static const uint32_t I[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, I, sizeof I); s->n = 0; s->bl = 0;
}
static void sha2_add(Sha2 *s, const unsigned char *p, size_t n) {
    s->n += n;
    while (n) {
        size_t k = 64 - s->bl; if (k > n) k = n;
        memcpy(s->b + s->bl, p, k); s->bl += k; p += k; n -= k;
        if (s->bl == 64) { sha2_block(s, s->b); s->bl = 0; }
    }
}
static void sha2_hex(Sha2 *s, char *hex) {      /* hex: 65 bytes */
    const uint64_t bits = s->n * 8;
    unsigned char pad = 0x80;
    sha2_add(s, &pad, 1);
    pad = 0;
    while (s->bl != 56) sha2_add(s, &pad, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha2_add(s, lb, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}

#ifndef _WIN32
/* argumento entre aspas simples para o shell; 0 se nao couber */
static int shq(const char *s, char *o, size_t cap) {
    size_t n = 0;
    if (n + 2 >= cap) return 0;
    o[n++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') { if (n + 5 >= cap) return 0; memcpy(o + n, "'\\''", 4); n += 4; }
        else { if (n + 2 >= cap) return 0; o[n++] = *s; }
    }
    o[n++] = '\'';
    o[n] = 0;
    return 1;
}

/* executa o comando (com 2>&1 dentro dele) e devolve a saida (ate maxb bytes) num buffer novo; *rc = codigo de saida */
static char *run_capture(const char *cmd, size_t maxb, int *rc, size_t *nout) {
    FILE *f = popen(cmd, "r");
    char *b = (char *)malloc(maxb + 1);
    if (!f || !b) { if (f) pclose(f); free(b); *rc = -1; *nout = 0; return NULL; }
    size_t n = 0, r;
    while (n < maxb && (r = fread(b + n, 1, maxb - n, f)) > 0) n += r;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, f) > 0) ;
    const int st = pclose(f);
    *rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    b[n] = 0; *nout = n;
    return b;
}

/* inicio das ultimas `nl` linhas de s (no maximo maxb bytes) */
static const char *tail_lines(const char *s, size_t n, int nl, size_t maxb) {
    const char *e = s + n;
    while (e > s && (e[-1] == '\n' || e[-1] == '\r')) e--;
    const char *p = e;
    int k = 0;
    while (p > s && (size_t)(e - p) < maxb) { p--; if (*p == '\n' && ++k >= nl) { p++; break; } }
    return p;
}
#endif


static void code_check(const char *path, int partial, char *out, size_t cap);

static int sudo_prepare(const char *cmd, int force);
static char *run_collect(const char *cmd, int tmo, size_t maxb, int *rc);
