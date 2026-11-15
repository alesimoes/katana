#version 450
#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require

// Y[rows][tokens] = W[rows][cols] * X[cols][tokens]; pesos quantizados no layout do katana,
// decodificados ao encher o ladrilho compartilhado. Grupo de 128 threads = 4 subgrupos.
layout(local_size_x = 128) in;
const uint BM = 64, BN = 64, BK = 64;
const uint T = 16;
const uint WM = BM / 2, WN = BN / 2;
const uint CR = WM / T, CC = WN / T;
const uint STRIDE = BK + 8;
#ifdef ACC16
#define ACCT float16_t
#else
#define ACCT float
#endif

layout(set = 0, binding = 0) readonly buffer WB { uint w[]; } wb;       // entrelacado de 32 em 32 linhas
layout(set = 0, binding = 1) readonly buffer XB { f16vec4 x[]; } xb;    // x[col*tokens + token]
layout(set = 0, binding = 2) buffer YB { ACCT y[]; } yb;                 // y[row*tokens + token]
layout(push_constant) uniform PC { uint rows, cols, tokens, wpr, kper, slice; } pc;  // kper: colunas por fatia de K; slice: elementos por fatia de saida

shared float16_t buf_a[BM * STRIDE];
shared float16_t buf_b[BN * STRIDE];
