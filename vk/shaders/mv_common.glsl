// Parte comum dos kernels mv_*: porte dos kernels OpenCL do katana (NB=1, um token).
// Linhas entrelacadas de 32 em 32: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32].
// Especializacao: NR = linhas por thread, NSEG = segmentos de colunas (local_size_y).
// (o arquivo .comp que inclui este define #version e a extensao de include antes)
layout(local_size_x = 32, local_size_y_id = 1) in;
layout(constant_id = 0) const int NR = 1;
layout(constant_id = 1) const int NSEG = 1;
layout(std430, binding = 0) readonly buffer WB { uint W[]; };
layout(std430, binding = 1) readonly buffer XB { float x[]; };
layout(std430, binding = 2) buffer OB { float outv[]; };
layout(std430, binding = 3) readonly buffer BB { float bias[]; };
layout(push_constant) uniform PC { int cols; int mode; int rows; int gs; int go; int woff; int ooff;
    int mtp; int mxrow; int morow; int mcol; } pc;
// mtp > 0: varias matrizes num lancamento (especialistas do MoE). Os grupos sao divididos em faixas de mtp;
// a faixa k tem 6 entradas em bias (como uint); em mcol, mcol+1, mcol+2: inicio da matriz em W (palavras),
// indice do vetor de entrada, indice do vetor de saida. Entrada a partir de indice*mxrow, saida de indice*morow.

shared float lp[NR * NSEG * 32];
float total[NR];

uint gi;     // indice global da thread (lane = gi & 31)
int sgi;     // segmento de colunas desta thread
uint lane;
uint wof_;   // deslocamento dos pesos (woff ou da tabela)
int xoff_, oof_;

void ini() {
    gi = gl_GlobalInvocationID.x; sgi = int(gl_LocalInvocationID.y); lane = gi & 31u;
    wof_ = uint(pc.woff); xoff_ = 0; oof_ = pc.ooff;
    if (pc.mtp > 0) {
        uint k = gl_WorkGroupID.x / uint(pc.mtp);
        gi -= k * uint(pc.mtp) * 32u;
        uint t = 6u * k + uint(pc.mcol);
        wof_ = floatBitsToUint(bias[t]); xoff_ = int(floatBitsToUint(bias[t + 1u])) * pc.mxrow; oof_ = int(floatBitsToUint(bias[t + 2u])) * pc.morow;
    }
    for (int r = 0; r < NR; r++) total[r] = 0.0;
}

// ponteiro (em palavras) da linha r desta thread
uint rptr(int r, int wpr) {
    return ((((gi >> 5) * uint(NR) + uint(r)) * uint(pc.gs) + uint(pc.go)) * uint(wpr)) * 32u + lane + wof_;
}
int rrow(int r) { return int((((gi >> 5) * uint(NR) + uint(r)) << 5) + lane); }

// fecho: bias opcional (bit 4) e modo: 0 grava, 1 soma (residual), 2 SwiGLU com out
void fin(float t, int row) {
    if ((pc.mode & 4) != 0) t += bias[row];
    row += oof_;
    int mm = pc.mode & 3;
    if (mm == 1) t += outv[row];
    else if (mm == 2) { float g = outv[row]; t = g / (1.0 + exp(-g)) * t; }
    outv[row] = t;
}

// as duas metades (float16) de uma palavra
vec2 h2(uint w) { return unpackHalf2x16(w); }

void finish() {
    if (NSEG > 1) {
        for (int r = 0; r < NR; r++) lp[(r * NSEG + sgi) * 32 + int(lane)] = total[r];
        barrier();
        if (sgi != 0) return;
        for (int r = 0; r < NR; r++) {
            float t = 0.0;
            for (int q = 0; q < NSEG; q++) t += lp[(r * NSEG + q) * 32 + int(lane)];
            total[r] = t;
        }
    }
    // modo 8 (par): grupos pares = gate, impares = up; grava silu(gate)*up
    if (NR >= 2 && (pc.mode & 8) != 0) {
        for (int r = 0; r + 1 < NR; r += 2) {
            int row = int(((((gi >> 5) * uint(NR) + uint(r)) >> 1) << 5) + lane);
            if (row < pc.rows) { float g = total[r]; outv[row + oof_] = g / (1.0 + exp(-g)) * total[r + 1]; }
        }
        return;
    }
    for (int r = 0; r < NR; r++) { int row = rrow(r); if (row < pc.rows) fin(total[r], row); }
}
