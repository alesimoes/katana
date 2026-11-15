        for (uint h = 0; h < 2; h++) {
            const uint r = r0 + 32 * h;
            const uint base = (ir / 32 + h) * pc.wpr * 32 + r0 + blk * 36 * 32;
            const uint h0 = wb.w[base];
            uint hw[3] = uint[3](wb.w[base + 32], wb.w[base + 64], wb.w[base + 96]);
            const float d = unpackHalf2x16(h0).x, dmin = unpackHalf2x16(h0).y;
            // escala e minimo dos sub-blocos j = 2s e 2s+1
            float dsc[2], dm[2];
            for (uint u = 0; u < 2; u++) {
                const uint j = 2 * s + u;
                uint sc, mn;
                if (j < 4) { sc = SB(j) & 63; mn = SB(j + 4) & 63; }
                else { sc = (SB(j + 4) & 15) | ((SB(j - 4) >> 6) << 4); mn = (SB(j + 4) >> 4) | ((SB(j) >> 6) << 4); }
                dsc[u] = d * float(sc); dm[u] = dmin * float(mn);
            }
            for (uint u = 0; u < 2; u++) {
                const uint l = grp * 2 + u;
                const uint wd = wb.w[base + (4 + s * 8 + l) * 32];
                const uint o = r * STRIDE + l * 4;
                for (uint b = 0; b < 4; b++) {
                    buf_a[o + b] = float16_t(dsc[0] * float((wd >> (8 * b)) & 15) - dm[0]);
                    buf_a[o + 32 + b] = float16_t(dsc[1] * float((wd >> (8 * b + 4)) & 15) - dm[1]);
                }
            }
        }
