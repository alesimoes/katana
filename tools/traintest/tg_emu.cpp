// GPU de mentira para testar o treino na GPU sem placa: roda o codigo do shader do produto (vk/shaders/tmm_core.glsl)
// na CPU. Cada workgroup vira 256 corrotinas (ucontext) e barrier() passa a vez para a proxima, entao as barreiras
// valem de verdade. Usado por tools/traintest/tg_emu.h (katana compilado com -DKATANA_TG_EMU).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ucontext.h>

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

static int MODE, WT;
static const uint *W;
static const float *X;
static float *O;
static struct { uint M, N, K, ws, wo; } pc;
static float As[16][64], Bs[16][64];
static float Rd[2048];
static uvec3 gl_WorkGroupID, gl_LocalInvocationID;

static ucontext_t sched, co[256];
static int cur;
static void barrier() { swapcontext(&co[cur], &sched); }

#include "tmm_core.glsl"

static bool fin[256];
static void entry() { tmm_main(); fin[cur] = true; }

extern "C" void tg_emu_run(int mode, int wt, const void *w, const float *x, float *o,
                           uint32_t M, uint32_t N, uint32_t K, uint32_t ws, uint32_t wo, uint32_t gx, uint32_t gy) {
    static char *stacks;
    const size_t SS = 64 * 1024;
    if (!stacks) stacks = (char *)std::malloc(SS * 256);
    MODE = mode; WT = wt; W = (const uint *)w; X = x; O = o; pc = { M, N, K, ws, wo };
    for (uint wy = 0; wy < gy; wy++) for (uint wx = 0; wx < gx; wx++) {
        gl_WorkGroupID = { wx, wy, 0 };
        bool done[256];
        for (int t = 0; t < 256; t++) {
            getcontext(&co[t]);
            co[t].uc_stack.ss_sp = stacks + SS * t; co[t].uc_stack.ss_size = SS; co[t].uc_link = &sched;
            makecontext(&co[t], entry, 0);
            done[t] = false; fin[t] = false;
        }
        // cada rodada leva todas as threads ate a proxima barreira (ou ate o fim)
        int alive = 256;
        while (alive) {
            for (int t = 0; t < 256; t++) {
                if (done[t]) continue;
                cur = t; gl_LocalInvocationID = { (uint)t % 16u, (uint)t / 16u, 0 };
                swapcontext(&sched, &co[t]);           // volta na barreira, ou no fim (uc_link)
                if (fin[t]) { done[t] = true; alive--; }
            }
        }
    }
}
