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

void tmm_main() {
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
