    const uint kb = gl_WorkGroupID.z * pc.kper, ke = min(pc.cols, kb + pc.kper);
    for (uint k0 = kb; k0 < ke; k0 += BK) {
        // ---- A: decodifica os pesos
        const uint blk = k0 / 256, s = (k0 % 256) / 64;
        FILLA
        // ---- B: ativacoes
        for (uint i = 0; i < 8; i++) {
            const uint idx = id * 8 + i;
            const uint c = idx / (BN / 4), n = (idx % (BN / 4)) * 4;
            const f16vec4 v = xb.x[((k0 + c) * pc.tokens + ic + n) / 4];
            buf_b[n * STRIDE + c] = v.x; buf_b[(n + 1) * STRIDE + c] = v.y;
            buf_b[(n + 2) * STRIDE + c] = v.z; buf_b[(n + 3) * STRIDE + c] = v.w;
        }
        barrier();
        for (uint k = 0; k < BK; k += T) {
            for (uint cr = 0; cr < CR; cr++) {
                coopmat<float16_t, gl_ScopeSubgroup, T, T, gl_MatrixUseA> a;
                coopMatLoad(a, buf_a, (wr * WM + cr * T) * STRIDE + k, STRIDE, gl_CooperativeMatrixLayoutRowMajor);
                for (uint cc = 0; cc < CC; cc++) {
                    coopmat<float16_t, gl_ScopeSubgroup, T, T, gl_MatrixUseB> b;
                    coopMatLoad(b, buf_b, (wc * WN + cc * T) * STRIDE + k, STRIDE, gl_CooperativeMatrixLayoutColumnMajor);
                    acc[cc * CR + cr] = coopMatMulAdd(a, b, acc[cc * CR + cr]);
                }
            }
        }
        barrier();
    }
    for (uint cr = 0; cr < CR; cr++)
        for (uint cc = 0; cc < CC; cc++)
            coopMatStore(acc[cc * CR + cr], yb.y, gl_WorkGroupID.z * pc.slice + (ir + wr * WM + cr * T) * pc.tokens + ic + wc * WN + cc * T, pc.tokens,
                         gl_CooperativeMatrixLayoutRowMajor);
}
