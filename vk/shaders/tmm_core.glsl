// Nucleo do produto do treino (tmm.comp). E GLSL e tambem C++ valido: tools/traintest/tmm_emu.cpp roda este mesmo
// codigo na CPU, com 256 threads e barreiras de verdade, e confere contra o dequant do motor.
uint u8(uint b) { return (W[b >> 2] >> ((b & 3u) * 8u)) & 255u; }
uint u16(uint b) { return (W[b >> 2] >> ((b & 2u) * 8u)) & 65535u; }      // b par
float f16(uint b) { return unpackHalf2x16(u16(b)).x; }
int i8(uint b) { return (int(u8(b)) << 24) >> 24; }

// escala (x) e minimo (y) do sub-bloco j (0..7) dos formatos K de 4 e 5 bits (12 bytes em s)
vec2 scmin(uint s, uint j) {
    if (j < 4u) return vec2(float(u8(s + j) & 63u), float(u8(s + j + 4u) & 63u));
    return vec2(float((u8(s + j + 4u) & 15u) | ((u8(s + j - 4u) >> 6) << 4)), float((u8(s + j + 4u) >> 4) | ((u8(s + j) >> 6) << 4)));
}

// elemento (r, c) do peso
float wq(uint r, uint c) {
    uint base = pc.wo + r * pc.ws;
    if (WT == 0) return uintBitsToFloat(W[(base >> 2) + c]);
    if (WT == 1) return f16(base + 2u * c);
    if (WT == 6) return uintBitsToFloat(u16(base + 2u * c) << 16);
    if (WT == 2) {                                   // Q8_0: 34 bytes = d (f16) + 32 int8
        uint b = base + (c >> 5) * 34u;
        return f16(b) * float(i8(b + 2u + (c & 31u)));
    }
    uint j = c & 255u, j64 = j >> 6, w = j & 63u, l = w & 31u, hi = w >> 5;
    if (WT == 3) {                                   // Q4_K: 144 bytes = d, dmin, escalas[12], qs[128]
        uint b = base + (c >> 8) * 144u;
        vec2 sm = scmin(b + 4u, 2u * j64 + hi);
        uint q = (u8(b + 16u + j64 * 32u + l) >> (4u * hi)) & 15u;
        return f16(b) * sm.x * float(q) - f16(b + 2u) * sm.y;
    }
    if (WT == 4) {                                   // Q5_K: 176 bytes = d, dmin, escalas[12], qh[32], qs[128]
        uint b = base + (c >> 8) * 176u;
        vec2 sm = scmin(b + 4u, 2u * j64 + hi);
        uint q = ((u8(b + 48u + j64 * 32u + l) >> (4u * hi)) & 15u) | (((u8(b + 16u + l) >> (2u * j64 + hi)) & 1u) << 4);
        return f16(b) * sm.x * float(q) - f16(b + 2u) * sm.y;
    }
    // Q6_K: 210 bytes = ql[128], qh[64], escalas int8[16], d (f16)
    uint b = base + (c >> 8) * 210u;
    uint h = j >> 7, part = (j & 127u) >> 5, l6 = j & 31u;
    uint lo = (u8(b + h * 64u + l6 + (part & 1u) * 32u) >> (4u * (part >> 1))) & 15u;
    uint up = (u8(b + 128u + h * 32u + l6) >> (2u * part)) & 3u;
    int q = int(lo | (up << 4)) - 32;
    return f16(b + 208u) * float(i8(b + 192u + h * 8u + l6 / 16u + 2u * part)) * float(q);
}

// 8 elementos seguidos (r, c..c+7), c multiplo de 8: as mesmas contas de wq, mas o cabecalho do bloco e as palavras de
// quantizados sao lidos uma vez (menos cargas encadeadas por elemento). Os formatos de 34 e 210 bytes por bloco so
// garantem alinhamento par, entao leem de 16 em 16 bits.
struct F8 { float v[8]; };
uint byt(uint w0, uint w1, uint i) { return ((i < 4u ? w0 : w1) >> ((i & 3u) * 8u)) & 255u; }
F8 dec8(uint r, uint c) {
    F8 o;
    if (WT <= 1 || WT == 6) { for (uint i = 0u; i < 8u; i++) o.v[i] = wq(r, c + i); return o; }
    uint base = pc.wo + r * pc.ws;
    uint j = c & 255u, j64 = j >> 6, hi = (j >> 5) & 1u, l = j & 31u;
    if (WT == 2) {
        uint b = base + (c >> 5) * 34u;
        float d = f16(b);
        uint a = b + 2u + (c & 31u);
        for (uint i = 0u; i < 4u; i++) {
            uint h = u16(a + 2u * i);
            o.v[2u * i] = d * float((int(h & 255u) << 24) >> 24);
            o.v[2u * i + 1u] = d * float((int((h >> 8) & 255u) << 24) >> 24);
        }
        return o;
    }
    if (WT == 3 || WT == 4) {
        uint b = base + (c >> 8) * (WT == 3 ? 144u : 176u);
        vec2 sm = scmin(b + 4u, 2u * j64 + hi);
        float d1 = f16(b) * sm.x, d2 = f16(b + 2u) * sm.y;
        uint a = (WT == 3 ? b + 16u : b + 48u) + j64 * 32u + l;
        uint w0 = W[a >> 2], w1 = W[(a >> 2) + 1u];
        if (WT == 3) {
            for (uint i = 0u; i < 8u; i++) o.v[i] = d1 * float((byt(w0, w1, i) >> (4u * hi)) & 15u) - d2;
        } else {
            uint h = b + 16u + l, h0 = W[h >> 2], h1 = W[(h >> 2) + 1u];
            for (uint i = 0u; i < 8u; i++) {
                uint q = ((byt(w0, w1, i) >> (4u * hi)) & 15u) | (((byt(h0, h1, i) >> (2u * j64 + hi)) & 1u) << 4);
                o.v[i] = d1 * float(q) - d2;
            }
        }
        return o;
    }
    uint b = base + (c >> 8) * 210u;
    uint h = j >> 7, part = (j & 127u) >> 5, l6 = j & 31u;
    float d = f16(b + 208u) * float(i8(b + 192u + h * 8u + l6 / 16u + 2u * part));
    uint a = b + h * 64u + l6 + (part & 1u) * 32u, a2 = b + 128u + h * 32u + l6;
    for (uint i = 0u; i < 4u; i++) {
        uint x = u16(a + 2u * i), y = u16(a2 + 2u * i);
        for (uint e = 0u; e < 2u; e++) {
            uint lo = ((x >> (8u * e)) >> (4u * (part >> 1))) & 15u;
            uint up = ((y >> (8u * e)) >> (2u * part)) & 3u;
            o.v[2u * i + e] = d * float(int(lo | (up << 4)) - 32);
        }
    }
    return o;
}

float aget(uint m, uint k) {
    if (m >= pc.M || k >= pc.K) return 0.0;
    if (MODE == 0) return wq(m, k);
    if (MODE == 1) return wq(k, m);
    return uintBitsToFloat(W[k * pc.M + m]);
}
float bget(uint n, uint k) {
    if (n >= pc.N || k >= pc.K) return 0.0;
    if (MODE == 2) return X[k * pc.N + n];
    return X[n * pc.K + k];
}

// ---- N pequeno (ate 8 tokens por passada; o laco de n repete a passada): o ladrilho de 64 tokens desperdicava quase tudo
// e deixava poucos grupos (um peso 512x2048 virava 8 grupos). Aqui o grupo de 256 threads e 16 x 16 e le o peso uma vez.
// MODE 3 (forward):    O[n][m] = soma_k W[m][k] X[n][k]; 16 linhas por grupo, 16 threads por linha (cada uma 8 colunas
//                      seguidas por vez), soma das 16 por memoria compartilhada. Precisa de K multiplo de 8.
// MODE 4 (transposto): O[n][m] = soma_k W[k][m] X[n][k]; cada thread tem 8 colunas m seguidas e 1 de 16 fatias das linhas k;
//                      soma das 16 fatias por memoria compartilhada. Precisa de M multiplo de 8.
void tmv_fwd() {
    uint tx = gl_LocalInvocationID.x, ty = gl_LocalInvocationID.y, tid = ty * 16u + tx;
    uint m = gl_WorkGroupID.x * 16u + ty;
    for (uint n0 = 0u; n0 < pc.N; n0 += 8u) {
        float acc[8];
        for (int j = 0; j < 8; j++) acc[j] = 0.0;
        if (m < pc.M) {
            for (uint c = tx * 8u; c < pc.K; c += 128u) {
                F8 w = dec8(m, c);
                for (uint i = 0u; i < 8u; i++) {
                    for (uint j = 0u; j < 8u; j++) if (n0 + j < pc.N) acc[j] = fma(w.v[i], X[(n0 + j) * pc.K + c + i], acc[j]);
                }
            }
        }
        for (int j = 0; j < 8; j++) Rd[tid * 8u + uint(j)] = acc[j];
        barrier();
        if (tx < 8u && m < pc.M && n0 + tx < pc.N) {
            float t = 0.0;
            for (uint l = 0u; l < 16u; l++) t += Rd[(ty * 16u + l) * 8u + tx];
            O[(n0 + tx) * pc.M + m] = t;
        }
        barrier();
    }
}
void tmv_tr() {
    uint tx = gl_LocalInvocationID.x, ty = gl_LocalInvocationID.y, tid = ty * 16u + tx;
    uint m0 = (gl_WorkGroupID.x * 16u + tx) * 8u;
    for (uint n0 = 0u; n0 < pc.N; n0 += 8u) {
        float acc[8][8];
        for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) acc[j][i] = 0.0;
        if (m0 < pc.M) {
            for (uint k = ty; k < pc.K; k += 16u) {
                F8 w = dec8(k, m0);
                for (uint j = 0u; j < 8u; j++) if (n0 + j < pc.N) {
                    float x = X[(n0 + j) * pc.K + k];
                    for (int i = 0; i < 8; i++) acc[j][i] = fma(w.v[i], x, acc[j][i]);
                }
            }
        }
        for (uint j = 0u; j < 8u; j++) {
            if (n0 + j >= pc.N) break;
            for (int i = 0; i < 8; i++) Rd[tid * 8u + uint(i)] = acc[j][i];
            barrier();
            if (tid < 128u) {
                uint u = tid >> 3, i = tid & 7u;
                uint mm = (gl_WorkGroupID.x * 16u + u) * 8u + i;
                if (mm < pc.M) {
                    float t = 0.0;
                    for (uint sl = 0u; sl < 16u; sl++) t += Rd[(sl * 16u + u) * 8u + i];
                    O[(n0 + j) * pc.M + mm] = t;
                }
            }
            barrier();
        }
    }
}

void tmm_main() {
    if (MODE == 3) { tmv_fwd(); return; }
    if (MODE == 4) { tmv_tr(); return; }
    uint tx = gl_LocalInvocationID.x, ty = gl_LocalInvocationID.y, tid = ty * 16u + tx;
    uint m0 = gl_WorkGroupID.x * 64u, n0 = gl_WorkGroupID.y * 64u;
    float acc[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) acc[i][j] = 0.0;
    for (uint k0 = 0u; k0 < pc.K; k0 += 16u) {
        for (uint e = tid; e < 1024u; e += 256u) {
            // A: no forward a linha do peso e contigua em k; no transposto e no gradiente, em m
            uint am, ak;
            if (MODE == 0) { am = e >> 4; ak = e & 15u; } else { ak = e >> 6; am = e & 63u; }
            As[ak][am] = aget(m0 + am, k0 + ak);
            uint bn, bk;
            if (MODE == 2) { bk = e >> 6; bn = e & 63u; } else { bn = e >> 4; bk = e & 15u; }
            Bs[bk][bn] = bget(n0 + bn, k0 + bk);
        }
        barrier();
        for (uint kk = 0u; kk < 16u; kk++) {
            float a[4], bb[4];
            for (int i = 0; i < 4; i++) a[i] = As[kk][tx + 16u * uint(i)];
            for (int j = 0; j < 4; j++) bb[j] = Bs[kk][ty + 16u * uint(j)];
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) acc[i][j] = fma(a[i], bb[j], acc[i][j]);
        }
        barrier();
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        uint m = m0 + tx + 16u * uint(i), n = n0 + ty + 16u * uint(j);
        if (m >= pc.M || n >= pc.N) continue;
        if (MODE == 2) O[m * pc.N + n] = acc[i][j];
        else O[n * pc.M + m] = acc[i][j];
    }
}
