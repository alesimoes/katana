        for (uint h = 0; h < 2; h++) {
            const uint r = r0 + 32 * h;
            const uint base = (ir / 32 + h) * pc.wpr * 32 + r0 + blk * 53 * 32;
            const float d = uintBitsToFloat(wb.w[base + 52 * 32]);
            const int scw = int(wb.w[base + (48 + s) * 32]);
            const float dsc = d * float((scw << (24 - 8 * int(grp))) >> 24);
            const uint q = base + (s * 4 + grp) * 3 * 32;
            const uint w0 = wb.w[q], w1 = wb.w[q + 32], w2 = wb.w[q + 64];
            const uint o = r * STRIDE + grp * 16;
            for (uint i = 0; i < 16; i++) {
                const uint bit = 6 * i, wi = bit / 32, off = bit % 32;
                uint v = (wi == 0 ? w0 : (wi == 1 ? w1 : w2)) >> off;
                if (off > 26) v |= (wi == 0 ? w1 : w2) << (32 - off);
                buf_a[o + i] = float16_t(dsc * float(int(v & 63) - 32));
            }
        }
