// Roda o codigo do shader do produto do treino (vk/shaders/tmm_core.glsl) na CPU, sem GPU: cada workgroup com 256
// threads e barreiras de verdade, e compara com as respostas do dequant da CPU (tools/traintest/tmm_ref.c).
//   g++ -std=c++20 -O1 -pthread -I vk/shaders tools/traintest/tmm_emu.cpp -o /tmp/tmm_emu && /tmp/tmm_emu /tmp/tmm_cases.bin
// Confere indices, bordas, desquantizacao de cada formato e os tres modos; nao confere o compilador do driver.
#include <barrier>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

typedef unsigned int uint;
struct vec2 { float x, y; vec2() : x(0), y(0) {} vec2(float a, float b) : x(a), y(b) {} };
struct uvec3 { uint x, y, z; };
static float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
static vec2 unpackHalf2x16(uint u) {
    const uint h = u & 0xffffu, s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024u), (int)e - 25);
    return vec2(s ? -v : v, 0.0f);
}
static float fma(float a, float b, float c) { return std::fma(a, b, c); }

// estado do "dispositivo"
static int MODE, WT;
static const uint *W;
static const float *X;
static float *O;
static struct { uint M, N, K, ws, wo; } pc;
static float As[16][64], Bs[16][64];
static uvec3 gl_WorkGroupID;
static thread_local uvec3 gl_LocalInvocationID;
static std::barrier<> *g_bar;
static void barrier() { g_bar->arrive_and_wait(); }

#include "tmm_core.glsl"

static void run(uint gx, uint gy) {
    std::barrier<> bar(256);
    g_bar = &bar;
    for (uint wy = 0; wy < gy; wy++) for (uint wx = 0; wx < gx; wx++) {
        gl_WorkGroupID = { wx, wy, 0 };
        std::vector<std::thread> th;
        for (uint t = 0; t < 256; t++) th.emplace_back([t] { gl_LocalInvocationID = { t % 16u, t / 16u, 0 }; tmm_main(); });
        for (auto &x : th) x.join();
    }
}

static double cmp(const float *a, const float *b, size_t n) {   // maior erro relativo ao maior valor
    double worst = 0, mx = 1e-30;
    for (size_t i = 0; i < n; i++) mx = std::fmax(mx, std::fabs(b[i]));
    for (size_t i = 0; i < n; i++) { const double d = std::fabs((double)a[i] - b[i]); if (!(d <= worst)) worst = d; }
    return worst / mx;
}

int main(int argc, char **argv) {
    if (argc != 2) { std::fprintf(stderr, "uso: tmm_emu <casos>\n"); return 1; }
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "nao abriu %s\n", argv[1]); return 1; }
    auto rd = [&](void *p, size_t n) { if (std::fread(p, 1, n, f) != n) { std::fprintf(stderr, "arquivo cortado\n"); std::exit(1); } };
    const char *nm[7] = { "F32", "F16", "Q8_0", "Q4_K", "Q5_K", "Q6_K", "BF16" };
    uint32_t n; rd(&n, 4);
    int fails = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h[6], qn; rd(h, sizeof h); rd(&qn, 4);
        const uint R = h[1], C = h[2], N = h[3];
        std::vector<uint> q((qn + 3) / 4 + 1, 0u); rd(q.data(), qn);
        std::vector<float> x((size_t)N * C), dy((size_t)N * R), y((size_t)N * R), dx((size_t)N * C), g((size_t)R * C);
        rd(x.data(), x.size() * 4); rd(dy.data(), dy.size() * 4); rd(y.data(), y.size() * 4); rd(dx.data(), dx.size() * 4); rd(g.data(), g.size() * 4);
        WT = (int)h[0];
        std::vector<float> o;
        double e[3];
        // forward: Y[N][R] = X W^T
        MODE = 0; W = q.data(); X = x.data(); o.assign((size_t)N * R, NAN); O = o.data();
        pc = { R, N, C, h[4], h[5] }; run((R + 63) / 64, (N + 63) / 64); e[0] = cmp(o.data(), y.data(), o.size());
        // transposto: dX[N][C] = dY W
        MODE = 1; X = dy.data(); o.assign((size_t)N * C, NAN); O = o.data();
        pc = { C, N, R, h[4], h[5] }; run((C + 63) / 64, (N + 63) / 64); e[1] = cmp(o.data(), dx.data(), o.size());
        // gradiente: G[R][C] = dY^T X (dY no binding 0, como palavras)
        MODE = 2; W = reinterpret_cast<const uint *>(dy.data()); X = x.data(); o.assign((size_t)R * C, NAN); O = o.data();
        pc = { R, C, N, 0, 0 }; run((R + 63) / 64, (C + 63) / 64); e[2] = cmp(o.data(), g.data(), o.size());
        const bool ok = e[0] < 1e-5 && e[1] < 1e-5 && e[2] < 1e-5;
        std::printf("%-5s forward %.1e  transposto %.1e  gradiente %.1e  %s\n", nm[WT], e[0], e[1], e[2], ok ? "ok" : "FALHOU");
        fails += !ok;
    }
    std::printf(fails ? "tmm_emu: FALHOU\n" : "tmm_emu: ok\n");
    return fails != 0;
}
