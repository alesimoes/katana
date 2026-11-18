/* simulador offline de politicas de vagas de especialistas (registro de KATANA_EXPLOG do katana)
   uso: sim L E K S teste.bin P_teste [calib1.bin ...]
   P_teste = tokens de prompt do teste (acertos contados so na geracao, depois do prompt) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
static int L, E, K;
typedef struct { int n; uint16_t *sel; } Tr;   /* sel[(t*L+l)*K+k] */
static Tr load(const char *fn) {
    Tr t = {0}; FILE *f = fopen(fn, "rb"); if (!f) { perror(fn); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint16_t *r = malloc(n); fread(r, 1, n, f); fclose(f);
    long nr = n / 2 / (K + 1);
    int *cnt = calloc(L, sizeof(int)); int T = (int)(nr / L) + 2;
    t.sel = calloc((size_t)T * L * K, 2);
    for (long i = 0; i < nr; i++) {
        int l = r[i * (K + 1)]; int ti = cnt[l]++;
        if (ti < T) memcpy(t.sel + ((size_t)ti * L + l) * K, r + i * (K + 1) + 1, K * 2);
    }
    t.n = cnt[0]; for (int l = 0; l < L; l++) if (cnt[l] < t.n) t.n = cnt[l];
    free(r); free(cnt); return t;
}
/* calor medio de calibracao (contagem normalizada) */
static double *calib; static int ncal;
/* politica: 0 = calor EMA (atual), 1 = LRU, 2 = LFU acumulado, 3 = fixo pelo calib, 4 = oraculo fixo (top do proprio teste na geracao) */
typedef struct { int pol; float decay, margin; int pre; int delay; int prompt_swap; } Pol;
static long g_loads;
/* devolve acertos na geracao (tokens >= P) e uso total; sl = vagas por camada */
static double run(const Tr *t, int P, const int *sl, Pol p, int gen0, int gen1, double *hits_out, double *uses_out) {
    double hits = 0, uses = 0; long loads = 0;
    float *heat = calloc(E, sizeof(float)); int *own = malloc(64 * 4 * 64); int *slot = malloc(E * 4);
    long *last = calloc(E, sizeof(long)); double *freq = calloc(E, sizeof(double));
    int *pend = malloc(E * 4); int np;
    for (int l = 0; l < L; l++) {
        const int S = sl[l];
        for (int e = 0; e < E; e++) { slot[e] = -1; heat[e] = 0; last[e] = 0; freq[e] = 0; }
        for (int i = 0; i < S; i++) own[i] = -1;
        np = 0;
        if (p.pre || p.pol == 3 || p.pol == 4) {   /* pre-carrega os S mais quentes */
            double *src = calib + (size_t)l * E, *tmp = NULL;
            if (p.pol == 4) { tmp = calloc(E, sizeof(double)); for (int ti = gen0; ti < gen1; ti++) for (int k = 0; k < K; k++) tmp[t->sel[((size_t)ti * L + l) * K + k]]++; src = tmp; }
            for (int i = 0; i < S; i++) {
                int b = -1; for (int e = 0; e < E; e++) if (slot[e] < 0 && (b < 0 || src[e] > src[b])) b = e;
                own[i] = b; slot[b] = i;
            }
            if (p.pre && p.pol == 0) for (int e = 0; e < E; e++) heat[e] = (float)(src[e] * 0.5);   /* heat_load: calor do arquivo * 0,5 */
            free(tmp);
        }
        for (int ti = 0; ti < t->n && ti < gen1; ti++) {
            const uint16_t *s = t->sel + ((size_t)ti * L + l) * K;
            const int gen = ti >= gen0;
            /* instala as cargas pedidas no token anterior (thread carregadora) */
            for (int q = 0; q < np; q++) {
                int e = pend[q]; if (slot[e] >= 0) continue;
                int v = -1;
                for (int i = 0; i < S; i++) { if (own[i] < 0) { v = i; break; }
                    if (p.pol == 1) { if (v < 0 || last[own[i]] < last[own[v]]) v = i; }
                    else if (p.pol == 2) { if (v < 0 || freq[own[i]] < freq[own[v]]) v = i; }
                    else if (v < 0 || heat[own[i]] < heat[own[v]]) v = i; }
                if (own[v] >= 0) {
                    if (p.pol == 0 && heat[e] <= heat[own[v]]) continue;
                    if (p.pol == 2 && freq[e] <= freq[own[v]]) continue;
                    slot[own[v]] = -1;
                }
                own[v] = e; slot[e] = v; loads++;
            }
            np = 0;
            for (int e = 0; e < E; e++) heat[e] *= p.decay;
            for (int k = 0; k < K; k++) { heat[s[k]] += 1.0f; last[s[k]] = ti + 1; freq[s[k]] += 1; }
            for (int k = 0; k < K; k++) {
                const int e = s[k];
                if (gen) uses++;
                if (slot[e] >= 0) { if (gen) hits++; continue; }
                if (p.pol >= 3) continue;
                /* candidata: vaga livre ou a mais fria fora deste token */
                int cand = -1;
                for (int i = 0; i < S; i++) {
                    if (own[i] < 0) { cand = i; break; }
                    int inT = 0; for (int j = 0; j < K; j++) inT |= own[i] == s[j];
                    if (inT) continue;
                    if (p.pol == 1) { if (cand < 0 || last[own[i]] < last[own[cand]]) cand = i; }
                    else if (p.pol == 2) { if (cand < 0 || freq[own[i]] < freq[own[cand]]) cand = i; }
                    else if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
                }
                if (cand < 0) continue;
                int want = own[cand] < 0;
                if (!want && (gen || p.prompt_swap)) {
                    if (p.pol == 1) want = 1;
                    else if (p.pol == 2) want = freq[e] > freq[own[cand]] + p.margin;
                    else want = heat[e] > heat[own[cand]] + p.margin;
                }
                if (want) {
                    if (p.delay) pend[np++] = e;
                    else { if (own[cand] >= 0) slot[own[cand]] = -1; own[cand] = e; slot[e] = cand; loads++; }
                }
            }
        }
    }
    free(heat); free(own); free(slot); free(last); free(freq); free(pend);
    g_loads = loads;
    *hits_out = hits; *uses_out = uses;
    return uses ? hits / uses : 0;
}
/* Belady (oraculo, sem atraso) por camada, a partir do inicio da geracao com as vagas do fixo */
static double belady(const Tr *t, const int *sl, int gen0, int gen1) {
    double hits = 0, uses = 0;
    for (int l = 0; l < L; l++) {
        const int S = sl[l]; int *own = malloc(S * 4), n = 0;
        for (int ti = gen0; ti < gen1; ti++) {
            const uint16_t *s = t->sel + ((size_t)ti * L + l) * K;
            for (int k = 0; k < K; k++) {
                int e = s[k], in = 0; uses++;
                for (int i = 0; i < n; i++) if (own[i] == e) in = 1;
                if (in) { hits++; continue; }
                if (n < S) { own[n++] = e; continue; }
                /* tira o que sera usado mais tarde (ou nunca), exceto os do token atual */
                int vb = -1; long far = -1;
                for (int i = 0; i < n; i++) {
                    int inT = 0; for (int j = 0; j < K; j++) inT |= own[i] == s[j];
                    if (inT) continue;
                    long nx = 1L << 40;
                    for (int u = ti + 1; u < gen1; u++) { const uint16_t *s2 = t->sel + ((size_t)u * L + l) * K; int f = 0; for (int j = 0; j < K; j++) f |= s2[j] == own[i]; if (f) { nx = u; break; } }
                    if (nx > far) { far = nx; vb = i; }
                }
                long nxe = 1L << 40;
                for (int u = ti + 1; u < gen1; u++) { const uint16_t *s2 = t->sel + ((size_t)u * L + l) * K; int f = 0; for (int j = 0; j < K; j++) f |= s2[j] == e; if (f) { nxe = u; break; } }
                if (vb >= 0 && nxe < far) own[vb] = e;
            }
        }
        free(own);
    }
    return hits / uses;
}
int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "uso\n"); return 1; }
    L = atoi(argv[1]); E = atoi(argv[2]); K = atoi(argv[3]); const int S0 = atoi(argv[4]);
    Tr t = load(argv[5]); const int P = atoi(argv[6]);
    ncal = argc - 7;
    calib = calloc((size_t)L * E, sizeof(double));
    for (int c = 0; c < ncal; c++) {
        Tr ct = load(argv[7 + c]);
        for (int ti = 0; ti < ct.n; ti++) for (int l = 0; l < L; l++) for (int k = 0; k < K; k++) calib[(size_t)l * E + ct.sel[((size_t)ti * L + l) * K + k]] += 1.0 / ct.n / K;
        free(ct.sel);
    }
    const int gen0 = P, gen1 = t.n;
    printf("teste %s: %d tokens (%d de prompt, %d gerados), vagas %d por camada, calibracao com %d registros\n", argv[5], t.n, P, gen1 - gen0, S0, ncal);
    int *sl = malloc(L * 4); for (int l = 0; l < L; l++) sl[l] = S0;
    double h, u;
    Pol atual = { 0, 0.98f, 4.0f, 0, 1, 0 };
    #define R(name, pp, slv) do { double r = run(&t, P, slv, pp, gen0, gen1, &h, &u); printf("  %-44s %5.1f%%  cargas/token %.2f\n", name, 100 * r, (double)g_loads / (gen1 > 0 ? t.n : 1)); } while (0)
    R("atual (calor 0,98, margem 4, frio)", atual, sl);
    { Pol p = atual; p.pre = 1; R("atual + pre-carga do calor (calib)", p, sl); }
    float decs[] = { 0.9f, 0.95f, 0.99f, 0.995f, 0.999f };
    for (int i = 0; i < 5; i++) { Pol p = atual; p.decay = decs[i]; p.pre = 1; char nm[64]; snprintf(nm, 64, "calor %.3f margem 4 + pre", decs[i]); R(nm, p, sl); }
    float mg[] = { 0, 1, 2, 8 };
    for (int i = 0; i < 4; i++) { Pol p = atual; p.margin = mg[i]; p.pre = 1; char nm[64]; snprintf(nm, 64, "calor 0,98 margem %.0f + pre", mg[i]); R(nm, p, sl); }
    { Pol p = atual; p.pre = 1; p.prompt_swap = 1; R("atual + pre + trocas no prompt", p, sl); }
    { Pol p = atual; p.delay = 0; p.pre = 1; R("atual + pre, sem atraso de carga", p, sl); }
    { Pol p = { 1, 0.98f, 0, 1, 1, 1 }; R("LRU + pre", p, sl); }
    { Pol p = { 2, 0.98f, 0, 1, 1, 1 }; R("LFU acumulado (margem 0) + pre", p, sl); }
    { Pol p = { 2, 0.98f, 4, 1, 1, 1 }; R("LFU acumulado (margem 4) + pre", p, sl); }
    { Pol p = { 3, 0.98f, 0, 1, 1, 0 }; R("fixo pelo calor de calibracao", p, sl); }
    { Pol p = { 4, 0.98f, 0, 1, 1, 0 }; R("oraculo fixo (top S da propria geracao)", p, sl); }
    printf("  %-44s %5.1f%%\n", "Belady (oraculo dinamico, teto)", 100 * belady(&t, sl, gen0, gen1));
    /* vagas desiguais: total S0*L; guloso pelo ganho marginal no calor de calibracao (fixo) */
    {
        int *s2 = malloc(L * 4); double *srt = malloc(E * sizeof(double));
        double **top = malloc(L * sizeof(double *));
        for (int l = 0; l < L; l++) {
            top[l] = malloc(E * sizeof(double));
            for (int e = 0; e < E; e++) srt[e] = calib[(size_t)l * E + e];
            for (int i = 0; i < E; i++) { int b = i; for (int j = i + 1; j < E; j++) if (srt[j] > srt[b]) b = j; double x = srt[i]; srt[i] = srt[b]; srt[b] = x; top[l][i] = srt[i]; }
            s2[l] = K;     /* minimo: K vagas */
        }
        int left = S0 * L - K * L;
        while (left-- > 0) { int b = -1; for (int l = 0; l < L; l++) if (s2[l] < E && (b < 0 || top[l][s2[l]] > top[b][s2[b]])) b = l; s2[b]++; }
        printf("  vagas desiguais (min %d):", K); for (int l = 0; l < L; l++) printf(" %d", s2[l]); printf("\n");
        { Pol p = atual; p.pre = 1; R("desiguais + atual + pre", p, s2); }
        { Pol p = { 3, 0.98f, 0, 1, 1, 0 }; R("desiguais + fixo calib", p, s2); }
        { Pol p = { 2, 0.98f, 0, 1, 1, 1 }; R("desiguais + LFU + pre", p, s2); }
        /* aquecimento: acertos nos primeiros 32/64 tokens gerados com e sem pre-carga */
    }
    for (int w = 16; w <= 64; w *= 2) {
        double a, b;
        Pol p = atual; run(&t, P, sl, p, gen0, gen0 + w < gen1 ? gen0 + w : gen1, &h, &u); a = h / u;
        p.pre = 1; run(&t, P, sl, p, gen0, gen0 + w < gen1 ? gen0 + w : gen1, &h, &u); b = h / u;
        printf("  primeiros %d gerados: sem pre-carga %.1f%%, com pre-carga %.1f%%\n", w, 100 * a, 100 * b);
    }
    /* sem prompt (sessao retomada direto na geracao): acertos nos primeiros tokens a partir de vagas vazias */
    for (int w = 16; w <= 128; w *= 2) {
        double a, b; Pol p = atual;
        run(&t, 0, sl, p, 0, w, &h, &u); a = h / u;
        p.pre = 1; run(&t, 0, sl, p, 0, w, &h, &u); b = h / u;
        printf("  vagas vazias, primeiros %d tokens do registro: frio %.1f%%, pre-carga %.1f%%\n", w, 100 * a, 100 * b);
    }
    return 0;
}
