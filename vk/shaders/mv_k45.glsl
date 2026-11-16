// Q4_K (FIVE=0, 36 palavras/bloco) e Q5_K (FIVE=1, 44 palavras/bloco); incluido por mv_q4k.comp / mv_q5k.comp
#include "mv_common.glsl"
const int WPB = FIVE != 0 ? 44 : 36;
const int Q0 = FIVE != 0 ? 12 : 4;
uint sb(uvec3 hw, int k) { return (hw[k >> 2] >> ((k & 3) * 8)) & 255u; }
void k4(int j, uvec3 hw, out int d, out int m) {
    if (j < 4) { d = int(sb(hw, j) & 63u); m = int(sb(hw, j + 4) & 63u); }
    else { d = int((sb(hw, j + 4) & 0xFu) | ((sb(hw, j - 4) >> 6) << 4)); m = int((sb(hw, j + 4) >> 4) | ((sb(hw, j) >> 6) << 4)); }
}
void main() {
    ini();
    int nb = pc.cols >> 8, bps = nb / NSEG, b0 = sgi * bps;
    uint p[NR];
    for (int r = 0; r < NR; r++) p[r] = rptr(r, nb * WPB) + uint(b0) * uint(WPB * 32);
    int xb = b0 * 256;
    for (int b = 0; b < bps; b++) {
        float d[NR], dmin[NR]; uvec3 hw[NR];
        for (int r = 0; r < NR; r++) {
            uint h0 = W[p[r]];
            hw[r] = uvec3(W[p[r] + 32u], W[p[r] + 64u], W[p[r] + 96u]);
            vec2 dd = h2(h0); d[r] = dd.x; dmin[r] = dd.y;
        }
        for (int j = 0; j < 8; j += 2, xb += 64) {
            float a[NR], c[NR];
            for (int r = 0; r < NR; r++) { a[r] = 0.0; c[r] = 0.0; }
            float xa = 0.0, xc = 0.0;
            for (int l = 0; l < 8; l++) {
                float x0 = x[xoff_ + xb + 4 * l], x1 = x[xoff_ + xb + 4 * l + 1], x2 = x[xoff_ + xb + 4 * l + 2], x3 = x[xoff_ + xb + 4 * l + 3];
                float y0 = x[xoff_ + xb + 32 + 4 * l], y1 = x[xoff_ + xb + 33 + 4 * l], y2 = x[xoff_ + xb + 34 + 4 * l], y3 = x[xoff_ + xb + 35 + 4 * l];
                xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;
                for (int r = 0; r < NR; r++) {
                    uint w = W[p[r] + uint(Q0 + j * 4 + l) * 32u];
#if FIVE
                    uint hb = W[p[r] + uint(4 + l) * 32u] >> j;
                    a[r] += float((w & 15u) | ((hb & 1u) << 4)) * x0 + float(((w >> 8) & 15u) | (((hb >> 8) & 1u) << 4)) * x1
                          + float(((w >> 16) & 15u) | (((hb >> 16) & 1u) << 4)) * x2 + float(((w >> 24) & 15u) | (((hb >> 24) & 1u) << 4)) * x3;
                    c[r] += float(((w >> 4) & 15u) | (((hb >> 1) & 1u) << 4)) * y0 + float(((w >> 12) & 15u) | (((hb >> 9) & 1u) << 4)) * y1
                          + float(((w >> 20) & 15u) | (((hb >> 17) & 1u) << 4)) * y2 + float((w >> 28) | (((hb >> 25) & 1u) << 4)) * y3;
#else
                    a[r] += float(w & 15u) * x0 + float((w >> 8) & 15u) * x1 + float((w >> 16) & 15u) * x2 + float((w >> 24) & 15u) * x3;
                    c[r] += float((w >> 4) & 15u) * y0 + float((w >> 12) & 15u) * y1 + float((w >> 20) & 15u) * y2 + float(w >> 28) * y3;
#endif
                }
            }
            for (int r = 0; r < NR; r++) {
                int s1, m1, s2, m2;
                k4(j, hw[r], s1, m1); k4(j + 1, hw[r], s2, m2);
                total[r] += d[r] * (float(s1) * a[r] + float(s2) * c[r]) - dmin[r] * (float(m1) * xa + float(m2) * xc);
            }
        }
        for (int r = 0; r < NR; r++) p[r] += uint(WPB * 32);
    }
    finish();
}
