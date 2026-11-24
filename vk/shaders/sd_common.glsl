// Stable Diffusion 1.5 na GPU (Vulkan): uma arena de memoria so (um buffer, visto como palavras de 32 bits); os "ponteiros" sao
// deslocamentos em bytes (0 = nulo). F32 em palavras inteiras, F16 em pares dentro da palavra (o par comeca em byte multiplo de 4).
layout(std430, binding = 0) buffer Arena { uint D[]; };
float ldf(uint b) { return uintBitsToFloat(D[b >> 2]); }
float ldh(uint b) { return unpackHalf2x16(D[b >> 2])[(b >> 1) & 1u]; }
void stf(uint b, float v) { D[b >> 2] = floatBitsToUint(v); }
void sth2(uint b, float v0, float v1) { D[b >> 2] = packHalf2x16(vec2(v0, v1)); }
shared float sd_red[256];
float sd_total(float v) {
    const uint l = gl_LocalInvocationID.x;
    barrier(); sd_red[l] = v; barrier();
    for (uint s = 128; s > 0; s >>= 1) { if (l < s) sd_red[l] += sd_red[l + s]; barrier(); }
    const float r = sd_red[0];
    return r;
}
float sd_maxall(float v) {
    const uint l = gl_LocalInvocationID.x;
    barrier(); sd_red[l] = v; barrier();
    for (uint s = 128; s > 0; s >>= 1) { if (l < s) sd_red[l] = max(sd_red[l], sd_red[l + s]); barrier(); }
    const float r = sd_red[0];
    return r;
}
