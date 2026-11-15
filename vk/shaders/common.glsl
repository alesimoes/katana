/* trecho comum: tipo do cache KV (float32 ou float16 conforme -DKV16=1/0) */
#if KV16
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define KVT float16_t
#define KVL(p, i) float(p[i])
#define KVS(v, i, p) p[i] = float16_t(v)
#else
#define KVT float
#define KVL(p, i) (p[i])
#define KVS(v, i, p) p[i] = (v)
#endif

/* cos/sin do angulo de RoPE (pos * freq, ate ~600 rad): a instrucao de hardware perde precisao longe de zero
   (erro ~1e-4 em 600 rad), entao reduz o argumento a [-pi, pi] com 2*pi em duas partes (fma) antes. */
void rope_cs(float ang, out float cs, out float sn) {
    float k = round(ang * 0.15915494309189535);
    float r = fma(-k, 6.2831854820251465, ang);
    r = fma(-k, -1.74845553e-7, r);
    cs = cos(r); sn = sin(r);
}
