/* ============================ CUDA: kernels em PTX, emitidos por C ===============================
 * Nao ha compilador de CUDA nesta maquina nem no repositorio: os kernels sao escritos como PTX (o assembly virtual da NVIDIA) e emitidos
 * por funcoes C que desenrolam os lacos (a forma de cada ladrilho e parametro). O driver compila o PTX para a placa (cuModuleLoadDataEx).
 *
 * gemm: Y[n][rows] = X[n][K] * W^T, X em F16 (linhas preenchidas ate multiplo de 128), W quantizado [rows][K] (Q4_K, Q5_K) ou BF16.
 *   Ladrilho 128 x 128 x 64, 8 warps (2 x 4, cada um 64 x 32), mma.sync m16n8k16 (f16 -> f32), ldmatrix, A por cp.async em S estagios,
 *   B desquantizado em registradores e gravado em F16 na memoria compartilhada (os pesos nunca existem em F16 na VRAM); trocas de
 *   banco evitadas por troca de blocos de 16 bytes (XOR com a linha). Epilogos: 0 grava F32, 1 R += G[col] * acc (residual com portao),
 *   2 grava F16. */

static char *cpx_b; static int cpx_r, cpx_f, cpx_d, cpx_p;
#define PP(...) (cpx_b += sprintf(cpx_b, __VA_ARGS__))
static int cpx_R(void) { return cpx_r++; }
static int cpx_F(void) { return cpx_f++; }
static int cpx_D(void) { return cpx_d++; }
static int cpx_P(void) { return cpx_p++; }

enum { GQ_BF16 = 0, GQ_Q4K = 4, GQ_Q5K = 5 };
#define GBM 128
#define GBN 128
#define GBK 64

typedef struct { int st, sb; int qs[4], hd[4], sx[4], sy[4], sz[4], qh[4], bf[4][4], qlo[4], qhi[4], hlo[4], hhi[4]; } CpB;

/* carrega (para os registradores) os dados do ladrilho de k `tr` (registrador) dos 4 itens deste thread; pr[i] = linha valida */
static void cpx_load_b(int qt, int tr, const int rowp[4], const int pr[4], int jj, int hi, int jf, CpB *B) {
    const int blk = qt == GQ_Q4K ? 144 : 176;
    int tq = cpx_R(), tb = cpx_R(), off = cpx_R(), sb = cpx_R();
    if (qt != GQ_BF16) {
        PP("and.b32 %%r%d,%%r%d,3;\n", tq, tr);                     /* t&3 */
        PP("shr.u32 %%r%d,%%r%d,2;\n", tb, tr);                     /* t>>2 */
        PP("shl.b32 %%r%d,%%r%d,1;\n", sb, tq);                     /* 2*(t&3) */
        { const int h1 = cpx_R(); PP("shr.u32 %%r%d,%%r%d,2;\n", h1, hi); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sb, sb, h1); }   /* sub-bloco */
    }
    for (int i = 0; i < 4; i++) {
        const int a = cpx_D(), a2 = cpx_D(), t64 = cpx_D();
        if (qt == GQ_BF16) {
            /* 16 bytes a partir da coluna t*64 + j*8 */
            PP("shl.b32 %%r%d,%%r%d,6;\n", off, tr); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", off, off, jf);   /* jf = j*8 */
            PP("mul.wide.u32 %%d%d,%%r%d,2;\n", t64, off);
            PP("add.u64 %%d%d,%%d%d,%%d%d;\n", a, rowp[i], t64);
            for (int c = 0; c < 4; c++) PP("mov.u32 %%r%d,0;\n", B->bf[i][c] = cpx_R());
            PP("@%%p%d ld.global.v4.u32 {%%r%d,%%r%d,%%r%d,%%r%d},[%%d%d];\n", pr[i], B->bf[i][0], B->bf[i][1], B->bf[i][2], B->bf[i][3], a);
            continue;
        }
        /* inicio do super-bloco: rowp + (t>>2)*blk */
        PP("mul.wide.u32 %%d%d,%%r%d,%d;\n", t64, tb, blk);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", a, rowp[i], t64);                 /* a = cabecalho do super-bloco */
        B->hd[i] = cpx_R(); B->sx[i] = cpx_R(); B->sy[i] = cpx_R(); B->sz[i] = cpx_R();
        PP("mov.u32 %%r%d,0;\n", B->hd[i]); PP("mov.u32 %%r%d,0;\n", B->sx[i]); PP("mov.u32 %%r%d,0;\n", B->sy[i]); PP("mov.u32 %%r%d,0;\n", B->sz[i]);
        PP("@%%p%d ld.global.u32 %%r%d,[%%d%d];\n", pr[i], B->hd[i], a);
        /* bytes de escala: x = s[sb+4], y = s[sb], z = s[sb&3] (a partir de a+4) */
        const int ix = cpx_R(), iy = cpx_R(), iz = cpx_R(); const int dx = cpx_D(), dy = cpx_D(), dz = cpx_D();
        PP("add.u32 %%r%d,%%r%d,4;\n", ix, sb); PP("mov.u32 %%r%d,%%r%d;\n", iy, sb); PP("and.b32 %%r%d,%%r%d,3;\n", iz, sb);
        PP("cvt.u64.u32 %%d%d,%%r%d;\n", dx, ix); PP("cvt.u64.u32 %%d%d,%%r%d;\n", dy, iy); PP("cvt.u64.u32 %%d%d,%%r%d;\n", dz, iz);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dx, dx, a); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dy, dy, a); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dz, dz, a);
        PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sx[i], dx); PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sy[i], dy); PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sz[i], dz);
        /* quantizados: Q4 em +16 + 32*(t&3) + 8*jj; Q5 em +48 + ... e qh em +16 + 8*jj */
        const int qo = cpx_R(); const int dq = cpx_D();
        PP("shl.b32 %%r%d,%%r%d,5;\n", qo, tq); PP("shl.b32 %%r%d,%%r%d,3;\n", off, jj); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", qo, qo, off);
        PP("cvt.u64.u32 %%d%d,%%r%d;\n", dq, qo); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dq, dq, a);
        B->qlo[i] = cpx_R(); B->qhi[i] = cpx_R();
        PP("mov.u32 %%r%d,0;\n", B->qlo[i]); PP("mov.u32 %%r%d,0;\n", B->qhi[i]);
        PP("@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+%d];\n", pr[i], B->qlo[i], B->qhi[i], dq, qt == GQ_Q4K ? 16 : 48);
        if (qt == GQ_Q5K) {
            const int dh = cpx_D(); B->hlo[i] = cpx_R(); B->hhi[i] = cpx_R();
            PP("mov.u32 %%r%d,0;\n", B->hlo[i]); PP("mov.u32 %%r%d,0;\n", B->hhi[i]);
            PP("cvt.u64.u32 %%d%d,%%r%d;\n", dh, off); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dh, dh, a);
            PP("@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+16];\n", pr[i], B->hlo[i], B->hhi[i], dh);
        }
    }
    B->st = 1; B->sb = sb; (void)jf;
}

/* converte os registradores de B em 8 valores F16 por item e grava na memoria compartilhada em (soff[i] + base) */
static void cpx_store_b(int qt, int soff[4], int sbase, int hi, const CpB *B) {
    const int sbr = B->sb;
    for (int i = 0; i < 4; i++) {
        int pk[4];
        for (int c = 0; c < 4; c++) pk[c] = cpx_R();
        if (qt == GQ_BF16) {
            for (int c = 0; c < 4; c++) {
                const int lo = cpx_R(), hh = cpx_R(), fl = cpx_F(), fh = cpx_F();
                PP("shl.b32 %%r%d,%%r%d,16;\n", lo, B->bf[i][c]); PP("and.b32 %%r%d,%%r%d,0xffff0000;\n", hh, B->bf[i][c]);
                PP("mov.b32 %%f%d,%%r%d;\n", fl, lo); PP("mov.b32 %%f%d,%%r%d;\n", fh, hh);
                PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", pk[c], fh, fl);
            }
        } else {
            /* d, dmin */
            const int dh = cpx_R(), mh = cpx_R(), fd = cpx_F(), fm = cpx_F();
            PP("and.b32 %%r%d,%%r%d,0xffff;\n", dh, B->hd[i]); PP("shr.u32 %%r%d,%%r%d,16;\n", mh, B->hd[i]);
            PP("{ .reg .b16 hh, hm; cvt.u16.u32 hh,%%r%d; cvt.u16.u32 hm,%%r%d; cvt.f32.f16 %%f%d,hh; cvt.f32.f16 %%f%d,hm; }\n", dh, mh, fd, fm);
            /* escala e minimo de 6 bits: sb < 4 (p): sc = y&63, m = x&63; senao sc = (x&15)|((z>>6)<<4), m = (x>>4)|((y>>6)<<4) */
            const int sc = cpx_R(), mn = cpx_R(), t1 = cpx_R(), t2 = cpx_R(), t3 = cpx_R(), t4 = cpx_R(), pp = cpx_P();
            PP("setp.lt.u32 %%p%d,%%r%d,4;\n", pp, sbr);
            PP("and.b32 %%r%d,%%r%d,63;\n", t1, B->sy[i]); PP("and.b32 %%r%d,%%r%d,63;\n", t2, B->sx[i]);
            PP("and.b32 %%r%d,%%r%d,15;\n", t3, B->sx[i]); PP("shr.u32 %%r%d,%%r%d,6;\n", t4, B->sz[i]); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", sc, t1, t3, pp);
            PP("shr.u32 %%r%d,%%r%d,4;\n", t3, B->sx[i]); PP("shr.u32 %%r%d,%%r%d,6;\n", t4, B->sy[i]); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", mn, t2, t3, pp);
            const int fs = cpx_F(), fmm = cpx_F(), fdsc = cpx_F(), fndm = cpx_F();
            PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fs, sc); PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fmm, mn);
            PP("mul.f32 %%f%d,%%f%d,%%f%d;\n", fdsc, fd, fs); PP("mul.f32 %%f%d,%%f%d,%%f%d;\n", fndm, fm, fmm); PP("neg.f32 %%f%d,%%f%d;\n", fndm, fndm);
            const int w0 = cpx_R(), w1 = cpx_R(), h0 = cpx_R(), h1 = cpx_R();
            PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", w0, B->qlo[i], hi); PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", w1, B->qhi[i], hi);    /* hi = 0 ou 4 */
            if (qt == GQ_Q5K) { PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", h0, B->hlo[i], sbr); PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", h1, B->hhi[i], sbr); }
            int fprev = 0;
            for (int e = 0; e < 8; e++) {
                const int q = cpx_R(), fq = cpx_F(), fw = cpx_F();
                PP("bfe.u32 %%r%d,%%r%d,%d,4;\n", q, e < 4 ? w0 : w1, 8 * (e & 3));
                if (qt == GQ_Q5K) { const int bt = cpx_R(); PP("bfe.u32 %%r%d,%%r%d,%d,1;\n", bt, e < 4 ? h0 : h1, 8 * (e & 3)); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,1;\n", q, bt, q); }
                PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fq, q);
                PP("fma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\n", fw, fq, fdsc, fndm);
                if (e & 1) PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", pk[e >> 1], fw, fprev);   /* (alto, baixo) */
                fprev = fw;
            }
        }
        PP("st.shared.v4.u32 [%%r%d+%d],{%%r%d,%%r%d,%%r%d,%%r%d};\n", soff[i], sbase, pk[0], pk[1], pk[2], pk[3]);
    }
}

/* epilogo comum: acc[16][4] (4 x 4 ladrilhos m16n8) -> Y (modo 0 F32, 1 Y += G[col]*acc, 2 F16) */
static void cpx_epilogue(int acc[16][4], int lane, int wm, int wn, int m0, int n0, int pn, int prow, int pmode, int dY, int dG) {
    /* epilogo */
    const int er = cpx_R(), ec = cpx_R(), q = cpx_R();
    PP("shr.u32 %%r%d,%%r%d,2;\nshl.b32 %%r%d,%%r%d,6;\nadd.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", er, lane, q, wm, er, er, q, er, er, m0);   /* m0 + wm*64 + lane/4 */
    PP("and.b32 %%r%d,%%r%d,3;\nshl.b32 %%r%d,%%r%d,1;\nshl.b32 %%r%d,%%r%d,5;\nadd.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", ec, lane, ec, ec, q, wn, ec, ec, q, ec, ec, n0);
    for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) for (int hh = 0; hh < 2; hh++) {
        const int *a = acc[mi * 4 + ni];
        const int tok = cpx_R(), col = cpx_R(), pp = cpx_P(), pq = cpx_P(), o = cpx_D(), dd = cpx_D(), idx = cpx_R();
        PP("add.u32 %%r%d,%%r%d,%d;\nadd.u32 %%r%d,%%r%d,%d;\n", tok, er, mi * 16 + hh * 8, col, ec, ni * 8);
        PP("setp.lt.u32 %%p%d,%%r%d,%%r%d;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\nand.pred %%p%d,%%p%d,%%p%d;\n", pp, tok, pn, pq, col, prow, pp, pp, pq);
        PP("mad.lo.u32 %%r%d,%%r%d,%%r%d,%%r%d;\n", idx, tok, prow, col);
        const int v0 = a[hh * 2], v1 = a[hh * 2 + 1];
        PP("setp.eq.u32 %%p%d,%%r%d,0;\n", pq, pmode);
        PP("mul.wide.u32 %%d%d,%%r%d,4;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dd, idx, o, dY, dd);
        PP("and.pred %%p%d,%%p%d,%%p%d;\n@%%p%d st.global.v2.f32 [%%d%d],{%%f%d,%%f%d};\n", pq, pq, pp, pq, o, v0, v1);
        /* modo 1: R[idx] += G[col] * v (R = Y) */
        const int g0 = cpx_F(), g1 = cpx_F(), r0 = cpx_F(), r1 = cpx_F(), dgg = cpx_D(), pm1 = cpx_P();
        PP("setp.eq.u32 %%p%d,%%r%d,1;\nand.pred %%p%d,%%p%d,%%p%d;\n", pm1, pmode, pm1, pm1, pp);
        PP("mul.wide.u32 %%d%d,%%r%d,4;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dgg, col, dgg, dG, dgg);
        PP("mov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\n", g0, g1, r0, r1);
        PP("@%%p%d ld.global.v2.f32 {%%f%d,%%f%d},[%%d%d];\n@%%p%d ld.global.v2.f32 {%%f%d,%%f%d},[%%d%d];\n", pm1, g0, g1, dgg, pm1, r0, r1, o);
        PP("fma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\nfma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\n", r0, g0, v0, r0, r1, g1, v1, r1);
        PP("@%%p%d st.global.v2.f32 [%%d%d],{%%f%d,%%f%d};\n", pm1, o, r0, r1);
        /* modo 2: F16 em Y (2 bytes por valor) */
        const int pm2 = cpx_P(), hv = cpx_R(), d2 = cpx_D(), d3 = cpx_D();
        PP("setp.eq.u32 %%p%d,%%r%d,2;\nand.pred %%p%d,%%p%d,%%p%d;\n", pm2, pmode, pm2, pm2, pp);
        PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\nmul.wide.u32 %%d%d,%%r%d,2;\nadd.u64 %%d%d,%%d%d,%%d%d;\n@%%p%d st.global.b32 [%%d%d],%%r%d;\n", hv, v1, v0, d2, idx, d3, dY, d2, pm2, d3, hv);
    }
}

/* emite o modulo PTX inteiro: gemm_bf16, gemm_q4k, gemm_q5k com `stages` estagios. Devolve o tamanho. */
static void cpx_gemm(int qt, int S, const char *name) {
    char *body = malloc(1 << 20), *save = cpx_b; cpx_b = body;
    cpx_r = 0; cpx_f = 0; cpx_d = 0; cpx_p = 0;
    const int tid = cpx_R(), lane = cpx_R(), warp = cpx_R(), wm = cpx_R(), wn = cpx_R(), m0 = cpx_R(), n0 = cpx_R(), smb = cpx_R();
    const int dX = cpx_D(), dW = cpx_D(), dY = cpx_D(), dG = cpx_D(), pn = cpx_R(), prow = cpx_R(), pK = cpx_R(), pmode = cpx_R();
    PP("ld.param.u64 %%d%d,[X]; ld.param.u64 %%d%d,[W]; ld.param.u64 %%d%d,[Y]; ld.param.u64 %%d%d,[G];\n", dX, dW, dY, dG);
    PP("ld.param.u32 %%r%d,[n]; ld.param.u32 %%r%d,[rows]; ld.param.u32 %%r%d,[K]; ld.param.u32 %%r%d,[mode];\n", pn, prow, pK, pmode);
    PP("mov.u32 %%r%d,%%tid.x; and.b32 %%r%d,%%r%d,31; shr.u32 %%r%d,%%r%d,5;\n", tid, lane, tid, warp, tid);
    PP("shr.u32 %%r%d,%%r%d,2; and.b32 %%r%d,%%r%d,3;\n", wm, warp, wn, warp);
    PP("mov.u32 %%r%d,%%ctaid.y; shl.b32 %%r%d,%%r%d,7; mov.u32 %%r%d,%%ctaid.x; shl.b32 %%r%d,%%r%d,7;\n", m0, m0, m0, n0, n0, n0);
    PP("mov.u32 %%r%d,sm;\n", smb);
    /* acumuladores */
    int acc[16][4];
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { acc[i][c] = cpx_F(); PP("mov.f32 %%f%d,0f00000000;\n", acc[i][c]); }
    /* numero de ladrilhos de k e bytes por linha do peso */
    const int nt = cpx_R(), rowbytes = cpx_R(), kk256 = cpx_R();
    PP("shr.u32 %%r%d,%%r%d,6;\n", nt, pK);
    if (qt == GQ_BF16) PP("shl.b32 %%r%d,%%r%d,1;\n", rowbytes, pK);
    else { PP("shr.u32 %%r%d,%%r%d,8;\n", kk256, pK); PP("mul.lo.u32 %%r%d,%%r%d,%d;\n", rowbytes, kk256, qt == GQ_Q4K ? 144 : 176); }
    /* A: 4 pedacos de 16 bytes por thread */
    const int rA = cpx_R(), cA = cpx_R(), swz = cpx_R(), sAoff = cpx_R(), t7 = cpx_R();
    PP("shr.u32 %%r%d,%%r%d,3;\n", rA, tid);                 /* linha dentro do ladrilho (0..31) */
    PP("and.b32 %%r%d,%%r%d,7;\n", cA, tid);                 /* pedaco 0..7 */
    PP("and.b32 %%r%d,%%r%d,7;\n", t7, rA);
    PP("xor.b32 %%r%d,%%r%d,%%r%d;\n", swz, cA, t7); PP("shl.b32 %%r%d,%%r%d,4;\n", swz, swz);
    PP("shl.b32 %%r%d,%%r%d,7;\n", sAoff, rA); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sAoff, sAoff, swz);   /* deslocamento na memoria compartilhada */
    PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sAoff, sAoff, smb);
    int gA[4]; const int dstrideA = cpx_D();
    PP("mul.wide.u32 %%d%d,%%r%d,2;\n", dstrideA, pK);   /* bytes por linha de X */
    for (int i = 0; i < 4; i++) {
        gA[i] = cpx_D(); const int d1 = cpx_D(), d2 = cpx_D(), rr = cpx_R();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rr, m0, rA); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", rr, rr, 32 * i);
        PP("mul.wide.u32 %%d%d,%%r%d,2;\n", d1, pK); PP("mul.wide.u32 %%d%d,%%r%d,%%r%d;\n", d1, rr, pK); PP("shl.b64 %%d%d,%%d%d,1;\n", d1, d1);
        PP("shl.b32 %%r%d,%%r%d,4;\n", t7, cA); PP("cvt.u64.u32 %%d%d,%%r%d;\n", d2, t7);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", d1, d1, d2); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", gA[i], dX, d1);
    }
    /* B: 4 itens (linha do peso r = tid/8 + 32 i, pedaco j = tid&7) */
    const int jj = cpx_R(), hi = cpx_R(), jf = cpx_R(), sBoff = cpx_R();
    PP("and.b32 %%r%d,%%r%d,3;\n", jj, cA);                   /* j&3 */
    PP("shr.u32 %%r%d,%%r%d,2;\n", hi, cA); PP("shl.b32 %%r%d,%%r%d,2;\n", hi, hi);   /* 0 ou 4: deslocamento do nibble */
    PP("shl.b32 %%r%d,%%r%d,3;\n", jf, cA);                   /* j*8 */
    PP("add.u32 %%r%d,%%r%d,%d;\n", sBoff, sAoff, GBM * 128 / 1);   /* B depois de A no mesmo estagio */
    int rowp[4], prB[4], soffB[4];
    for (int i = 0; i < 4; i++) {
        rowp[i] = cpx_D(); prB[i] = cpx_P(); const int wr = cpx_R(), d1 = cpx_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", wr, n0, rA); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", wr, wr, 32 * i);
        PP("setp.lt.u32 %%p%d,%%r%d,%%r%d;\n", prB[i], wr, prow);
        PP("mul.wide.u32 %%d%d,%%r%d,%%r%d;\n", d1, wr, rowbytes); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", rowp[i], dW, d1);
        soffB[i] = cpx_R(); PP("add.u32 %%r%d,%%r%d,%d;\n", soffB[i], sBoff, 4096 * i);
    }
    /* o A e o B de cada estagio: A em [0, 16K), B em [16K, 32K): sAoff aponta o A do estagio 0 e soffB[i] o B (estagio 0) */
    const int STG = 32768;
    int tpred = cpx_P();
    CpB B;
    /* prologo: estagios 0..S-2 */
    for (int s = 0; s < S - 1; s++) {
        PP("setp.lt.u32 %%p%d,%d,%%r%d;\n", tpred, s, nt);
        for (int i = 0; i < 4; i++) {
            const int sa = cpx_R(); PP("add.u32 %%r%d,%%r%d,%d;\n", sa, sAoff, s * STG + 4096 * i);
            const int dg = cpx_D(); PP("add.u64 %%d%d,%%d%d,%d;\n", dg, gA[i], s * 128);
            PP("@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", tpred, sa, dg);
        }
        PP("cp.async.commit_group;\n");
        const int tr = cpx_R(); PP("mov.u32 %%r%d,%d;\n", tr, s);
        int pr[4]; for (int i = 0; i < 4; i++) { pr[i] = cpx_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], tpred); }
        cpx_load_b(qt, tr, rowp, pr, jj, hi, jf, &B);
        cpx_store_b(qt, soffB, s * STG, hi, &B);
    }
    /* laco principal: t = 0..nt-1; sRd = deslocamento do estagio de leitura, sWr = o do estagio de escrita (t+S-1) */
    const int t = cpx_R(), sRd = cpx_R(), sWr = cpx_R(), tl = cpx_R(), ptl = cpx_P(), ploop = cpx_P();
    PP("mov.u32 %%r%d,0; mov.u32 %%r%d,0; mov.u32 %%r%d,%d;\n", t, sRd, sWr, (S - 1) * STG);
    PP("LOOP:\n");
    PP("cp.async.wait_group %d;\nbar.sync 0;\n", S - 2);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n", tl, t, S - 1, ptl, tl, nt);
    for (int i = 0; i < 4; i++) {
        const int sa = cpx_R(), dg = cpx_D(), dd = cpx_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sa, sAoff, sWr); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", sa, sa, 4096 * i);
        PP("mul.wide.u32 %%d%d,%%r%d,128;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dd, tl, dg, gA[i], dd);
        PP("@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", ptl, sa, dg);
    }
    PP("cp.async.commit_group;\n");
    int pr[4]; for (int i = 0; i < 4; i++) { pr[i] = cpx_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], ptl); }
    CpB L; cpx_load_b(qt, tl, rowp, pr, jj, hi, jf, &L);
    /* computo do ladrilho t */
    const int rdA = cpx_R(), lA = cpx_R(), lB = cpx_R(), rowA = cpx_R(), rowB = cpx_R();
    PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rdA, smb, sRd);       /* base do estagio de leitura (A); B = +16384 */
    PP("and.b32 %%r%d,%%r%d,7;\n", lA, lane);                /* l&7 */
    {   const int a1 = cpx_R(), a2 = cpx_R();
        PP("shr.u32 %%r%d,%%r%d,3; and.b32 %%r%d,%%r%d,1; shl.b32 %%r%d,%%r%d,3;\n", a1, lane, a1, a1, a1, a1);  /* ((l>>3)&1)*8 */
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowA, lA, a1); PP("shl.b32 %%r%d,%%r%d,6;\n", a2, wm); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowA, rowA, a2);   /* wm*64 */
        PP("shl.b32 %%r%d,%%r%d,7;\n", rowA, rowA);
        PP("shr.u32 %%r%d,%%r%d,4;\n", a1, lane); PP("shl.b32 %%r%d,%%r%d,3;\n", a1, a1);   /* (l>>4)*8 */
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowB, lA, a1); PP("shl.b32 %%r%d,%%r%d,5;\n", a2, wn); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowB, rowB, a2); PP("shl.b32 %%r%d,%%r%d,7;\n", rowB, rowB); }
    (void)lB;
    for (int kk = 0; kk < 4; kk++) {
        int af[4][4], bf[2][4];
        const int ca = cpx_R(), cb = cpx_R(), bA = cpx_R(), bB = cpx_R();
        /* pedaco de A: (2kk + (l>>4)) ^ (l&7); de B: (2kk + ((l>>3)&1)) ^ (l&7) */
        { const int u = cpx_R(), v = cpx_R();
          PP("shr.u32 %%r%d,%%r%d,4;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", u, lane, u, u, 2 * kk, ca, u, lA, ca, ca);
          PP("shr.u32 %%r%d,%%r%d,3;\nand.b32 %%r%d,%%r%d,1;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", v, lane, v, v, v, v, 2 * kk, cb, v, lA, cb, cb); }
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bA, rdA, rowA, bA, bA, ca);
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bB, rdA, rowB, bB, bB, cb);
        for (int mi = 0; mi < 4; mi++) {
            for (int c = 0; c < 4; c++) af[mi][c] = cpx_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", af[mi][0], af[mi][1], af[mi][2], af[mi][3], bA, mi * 2048);
        }
        for (int nj = 0; nj < 2; nj++) {
            for (int c = 0; c < 4; c++) bf[nj][c] = cpx_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", bf[nj][0], bf[nj][1], bf[nj][2], bf[nj][3], bB, 16384 + nj * 2048);
        }
        for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) {
            const int *a = acc[mi * 4 + ni], nj = ni >> 1, h = (ni & 1) * 2;
            PP("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%%f%d,%%f%d,%%f%d,%%f%d},{%%r%d,%%r%d,%%r%d,%%r%d},{%%r%d,%%r%d},{%%f%d,%%f%d,%%f%d,%%f%d};\n",
               a[0], a[1], a[2], a[3], af[mi][0], af[mi][1], af[mi][2], af[mi][3], bf[nj][h], bf[nj][h + 1], a[0], a[1], a[2], a[3]);
        }
    }
    /* desquantiza o ladrilho t+S-1 para o estagio de escrita */
    { int so[4]; for (int i = 0; i < 4; i++) { so[i] = cpx_R(); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", so[i], soffB[i], sWr); }
      cpx_store_b(qt, so, 0, hi, &L); }
    /* avanca */
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sRd, sRd, STG, ploop, sRd, S * STG, ploop, sRd);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sWr, sWr, STG, ploop, sWr, S * STG, ploop, sWr);
    PP("add.u32 %%r%d,%%r%d,1;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n@%%p%d bra LOOP;\n", t, t, ploop, t, nt, ploop);
    PP("cp.async.wait_group 0;\n");
    cpx_epilogue(acc, lane, wm, wn, m0, n0, pn, prow, pmode, dY, dG);
    PP("ret;\n");
    /* cabecalho com as declaracoes */
    char *out = save;
    out += sprintf(out, ".visible .entry %s(.param .u64 X,.param .u64 W,.param .u64 Y,.param .u64 G,.param .u32 n,.param .u32 rows,.param .u32 K,.param .u32 mode){\n", name);
    out += sprintf(out, ".reg .b32 %%r<%d>; .reg .b64 %%d<%d>; .reg .f32 %%f<%d>; .reg .pred %%p<%d>;\n", cpx_r + 1, cpx_d + 1, cpx_f + 1, cpx_p + 1);
    out += sprintf(out, "%s}\n", body);
    free(body); cpx_b = out;
}

/* ---- gemm "preciso": X = Xh + Xl (duas metades F16 de cada ativacao) e W = Wh + Wl (idem, o residuo so nos tipos quantizados); o produto soma
 * Xh*Wh + Xl*Wh + Xh*Wl em F32, ~22 bits de mantissa (o F16 simples erra ~2e-4 por produto e ~2e-3 em 25 blocos, acima da tolerancia de 1e-3).
 * Ladrilho 128 x 128 x 32, S estagios; `fl` bit 0 = usa Xl, bit 1 = usa Wl (para medir cada termo). Parametros: X, XL, W, Y, G, n, rows, K, mode. */
typedef struct { int hd[2], sx[2], sy[2], sz[2], qlo[2], qhi[2], hlo[2], hhi[2], bf[2][4], sb, hi; } CpxBX;

static void cpx_load_bx(int qt, int tr, const int rowp[2], const int pr[2], int j, CpxBX *B) {
    const int blk = qt == GQ_Q4K ? 144 : 176;
    const int sb = cpx_R(), tb = cpx_R(), off = cpx_R(), hi = cpx_R();
    PP("and.b32 %%r%d,%%r%d,7;\nshr.u32 %%r%d,%%r%d,3;\n", sb, tr, tb, tr);
    PP("and.b32 %%r%d,%%r%d,1;\nshl.b32 %%r%d,%%r%d,2;\n", hi, sb, hi, hi);                /* deslocamento do nibble: 0 ou 4 */
    B->sb = sb; B->hi = hi;
    for (int i = 0; i < 2; i++) {
        const int a = cpx_D(), t64 = cpx_D();
        if (qt == GQ_BF16) {
            const int jf = cpx_R();
            PP("shl.b32 %%r%d,%%r%d,5;\nshl.b32 %%r%d,%%r%d,3;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", off, tr, jf, j, off, off, jf);
            PP("mul.wide.u32 %%d%d,%%r%d,2;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", t64, off, a, rowp[i], t64);
            for (int c = 0; c < 4; c++) PP("mov.u32 %%r%d,0;\n", B->bf[i][c] = cpx_R());
            PP("@%%p%d ld.global.v4.u32 {%%r%d,%%r%d,%%r%d,%%r%d},[%%d%d];\n", pr[i], B->bf[i][0], B->bf[i][1], B->bf[i][2], B->bf[i][3], a);
            continue;
        }
        PP("mul.wide.u32 %%d%d,%%r%d,%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", t64, tb, blk, a, rowp[i], t64);
        B->hd[i] = cpx_R(); B->sx[i] = cpx_R(); B->sy[i] = cpx_R(); B->sz[i] = cpx_R();
        PP("mov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\n", B->hd[i], B->sx[i], B->sy[i], B->sz[i]);
        PP("@%%p%d ld.global.u32 %%r%d,[%%d%d];\n", pr[i], B->hd[i], a);
        const int ix = cpx_R(), iz = cpx_R(); const int dx = cpx_D(), dy = cpx_D(), dz = cpx_D();
        PP("add.u32 %%r%d,%%r%d,4;\nand.b32 %%r%d,%%r%d,3;\n", ix, sb, iz, sb);
        PP("cvt.u64.u32 %%d%d,%%r%d;\ncvt.u64.u32 %%d%d,%%r%d;\ncvt.u64.u32 %%d%d,%%r%d;\n", dx, ix, dy, sb, dz, iz);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dx, dx, a, dy, dy, a, dz, dz, a);
        PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sx[i], dx, pr[i], B->sy[i], dy, pr[i], B->sz[i], dz);
        /* quantizados: 32 bytes por par de sub-blocos (sb>>1), 8 bytes por item */
        const int qo = cpx_R(), dq = cpx_D();
        PP("shr.u32 %%r%d,%%r%d,1;\nshl.b32 %%r%d,%%r%d,5;\nshl.b32 %%r%d,%%r%d,3;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", qo, sb, qo, qo, off, j, qo, qo, off);
        PP("cvt.u64.u32 %%d%d,%%r%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dq, qo, dq, dq, a);
        B->qlo[i] = cpx_R(); B->qhi[i] = cpx_R();
        PP("mov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\n@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+%d];\n", B->qlo[i], B->qhi[i], pr[i], B->qlo[i], B->qhi[i], dq, qt == GQ_Q4K ? 16 : 48);
        if (qt == GQ_Q5K) {
            const int dh = cpx_D(); B->hlo[i] = cpx_R(); B->hhi[i] = cpx_R();
            PP("mov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\n", B->hlo[i], B->hhi[i]);
            PP("shl.b32 %%r%d,%%r%d,3;\ncvt.u64.u32 %%d%d,%%r%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", off, j, dh, off, dh, dh, a);
            PP("@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+16];\n", pr[i], B->hlo[i], B->hhi[i], dh);
        }
    }
}

/* converte e grava Wh (e Wl se wl) das duas itens na memoria compartilhada: soffh[i] (Wh) e soffl[i] (Wl), enderecos absolutos */
static void cpx_store_bx(int qt, const int soffh[2], const int soffl[2], int wl, const CpxBX *B) {
    for (int i = 0; i < 2; i++) {
        int ph[4], pl[4];
        for (int c = 0; c < 4; c++) { ph[c] = cpx_R(); pl[c] = cpx_R(); }
        if (qt == GQ_BF16) {
            for (int c = 0; c < 4; c++) {
                const int lo = cpx_R(), hh = cpx_R(), fl = cpx_F(), fh = cpx_F();
                PP("shl.b32 %%r%d,%%r%d,16;\nand.b32 %%r%d,%%r%d,0xffff0000;\nmov.b32 %%f%d,%%r%d;\nmov.b32 %%f%d,%%r%d;\n", lo, B->bf[i][c], hh, B->bf[i][c], fl, lo, fh, hh);
                PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", ph[c], fh, fl);
            }
        } else {
            const int dh = cpx_R(), mh = cpx_R(), fd = cpx_F(), fm = cpx_F();
            PP("and.b32 %%r%d,%%r%d,0xffff;\nshr.u32 %%r%d,%%r%d,16;\n", dh, B->hd[i], mh, B->hd[i]);
            PP("{ .reg .b16 hh, hm; cvt.u16.u32 hh,%%r%d; cvt.u16.u32 hm,%%r%d; cvt.f32.f16 %%f%d,hh; cvt.f32.f16 %%f%d,hm; }\n", dh, mh, fd, fm);
            const int sc = cpx_R(), mn = cpx_R(), t1 = cpx_R(), t2 = cpx_R(), t3 = cpx_R(), t4 = cpx_R(), pp = cpx_P();
            PP("setp.lt.u32 %%p%d,%%r%d,4;\n", pp, B->sb);
            PP("and.b32 %%r%d,%%r%d,63;\nand.b32 %%r%d,%%r%d,63;\n", t1, B->sy[i], t2, B->sx[i]);
            PP("and.b32 %%r%d,%%r%d,15;\nshr.u32 %%r%d,%%r%d,6;\nbfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, B->sx[i], t4, B->sz[i], t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", sc, t1, t3, pp);
            PP("shr.u32 %%r%d,%%r%d,4;\nshr.u32 %%r%d,%%r%d,6;\nbfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, B->sx[i], t4, B->sy[i], t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", mn, t2, t3, pp);
            const int fs = cpx_F(), fmm = cpx_F(), fdsc = cpx_F(), fndm = cpx_F();
            PP("cvt.rn.f32.u32 %%f%d,%%r%d;\ncvt.rn.f32.u32 %%f%d,%%r%d;\nmul.f32 %%f%d,%%f%d,%%f%d;\nmul.f32 %%f%d,%%f%d,%%f%d;\nneg.f32 %%f%d,%%f%d;\n", fs, sc, fmm, mn, fdsc, fd, fs, fndm, fm, fmm, fndm, fndm);
            const int w0 = cpx_R(), w1 = cpx_R(), h0 = cpx_R(), h1 = cpx_R();
            PP("shr.u32 %%r%d,%%r%d,%%r%d;\nshr.u32 %%r%d,%%r%d,%%r%d;\n", w0, B->qlo[i], B->hi, w1, B->qhi[i], B->hi);
            if (qt == GQ_Q5K) PP("shr.u32 %%r%d,%%r%d,%%r%d;\nshr.u32 %%r%d,%%r%d,%%r%d;\n", h0, B->hlo[i], B->sb, h1, B->hhi[i], B->sb);
            int fprev = 0;
            for (int e = 0; e < 8; e++) {
                const int q = cpx_R(), fq = cpx_F(), fw = cpx_F();
                PP("bfe.u32 %%r%d,%%r%d,%d,4;\n", q, e < 4 ? w0 : w1, 8 * (e & 3));
                if (qt == GQ_Q5K) { const int bt = cpx_R(); PP("bfe.u32 %%r%d,%%r%d,%d,1;\nbfi.b32 %%r%d,%%r%d,%%r%d,4,1;\n", bt, e < 4 ? h0 : h1, 8 * (e & 3), q, bt, q); }
                PP("cvt.rn.f32.u32 %%f%d,%%r%d;\nfma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\n", fq, q, fw, fq, fdsc, fndm);
                if (e & 1) {
                    PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", ph[e >> 1], fw, fprev);
                    if (wl) {
                        const int l16 = cpx_R(), fa = cpx_F(), fb = cpx_F();
                        PP("{ .reg .b16 a, b; mov.b32 {a, b}, %%r%d; cvt.f32.f16 %%f%d,a; cvt.f32.f16 %%f%d,b; }\n", ph[e >> 1], fa, fb);   /* fa baixo (= fprev), fb alto (= fw) */
                        PP("sub.f32 %%f%d,%%f%d,%%f%d;\nsub.f32 %%f%d,%%f%d,%%f%d;\n", fa, fprev, fa, fb, fw, fb);
                        PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", pl[e >> 1], fb, fa); (void)l16;
                    }
                }
                fprev = fw;
            }
        }
        PP("st.shared.v4.u32 [%%r%d],{%%r%d,%%r%d,%%r%d,%%r%d};\n", soffh[i], ph[0], ph[1], ph[2], ph[3]);
        if (wl && qt != GQ_BF16) PP("st.shared.v4.u32 [%%r%d],{%%r%d,%%r%d,%%r%d,%%r%d};\n", soffl[i], pl[0], pl[1], pl[2], pl[3]);
    }
}

static void cpx_gemmx(int qt, int S, int fl, const char *name) {
    char *body = malloc(1 << 20), *save = cpx_b; cpx_b = body;
    cpx_r = 0; cpx_f = 0; cpx_d = 0; cpx_p = 0;
    const int xl = fl & 1, wl = (fl & 2) && qt != GQ_BF16, fl2 = (fl & 4) != 0;      /* fl2: acumulador parcial por ladrilho somado em F32 (o mma acumula com menos precisao que a soma IEEE) */
    const int SA = 8192, OAL = SA, OBH = SA * (1 + xl), OBL = OBH + SA, ST = SA * (2 + xl + wl);
    const int tid = cpx_R(), lane = cpx_R(), warp = cpx_R(), wm = cpx_R(), wn = cpx_R(), m0 = cpx_R(), n0 = cpx_R(), smb = cpx_R();
    const int dX = cpx_D(), dXL = cpx_D(), dW = cpx_D(), dY = cpx_D(), dG = cpx_D(), pn = cpx_R(), prow = cpx_R(), pK = cpx_R(), pmode = cpx_R();
    PP("ld.param.u64 %%d%d,[X]; ld.param.u64 %%d%d,[XL]; ld.param.u64 %%d%d,[W]; ld.param.u64 %%d%d,[Y]; ld.param.u64 %%d%d,[G];\n", dX, dXL, dW, dY, dG);
    PP("ld.param.u32 %%r%d,[n]; ld.param.u32 %%r%d,[rows]; ld.param.u32 %%r%d,[K]; ld.param.u32 %%r%d,[mode];\n", pn, prow, pK, pmode);
    PP("mov.u32 %%r%d,%%tid.x; and.b32 %%r%d,%%r%d,31; shr.u32 %%r%d,%%r%d,5;\n", tid, lane, tid, warp, tid);
    PP("shr.u32 %%r%d,%%r%d,2; and.b32 %%r%d,%%r%d,3;\n", wm, warp, wn, warp);
    PP("mov.u32 %%r%d,%%ctaid.y; shl.b32 %%r%d,%%r%d,7; mov.u32 %%r%d,%%ctaid.x; shl.b32 %%r%d,%%r%d,7;\n", m0, m0, m0, n0, n0, n0);
    PP("mov.u32 %%r%d,sm;\n", smb);
    int acc[16][4];
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { acc[i][c] = cpx_F(); PP("mov.f32 %%f%d,0f00000000;\n", acc[i][c]); }
    const int nt = cpx_R(), rowbytes = cpx_R(), kk256 = cpx_R();
    PP("shr.u32 %%r%d,%%r%d,5;\n", nt, pK);
    if (qt == GQ_BF16) PP("shl.b32 %%r%d,%%r%d,1;\n", rowbytes, pK);
    else PP("shr.u32 %%r%d,%%r%d,8;\nmul.lo.u32 %%r%d,%%r%d,%d;\n", kk256, pK, rowbytes, kk256, qt == GQ_Q4K ? 144 : 176);
    /* A: 2 pedacos de 16 bytes por thread (linha (tid>>2) + 64 i, pedaco tid&3); smem: linha*64 + ((pedaco ^ ((linha>>1)&3)) << 4) */
    const int rA = cpx_R(), cA = cpx_R(), swz = cpx_R(), sAoff = cpx_R(), t7 = cpx_R();
    PP("shr.u32 %%r%d,%%r%d,2;\nand.b32 %%r%d,%%r%d,3;\nshr.u32 %%r%d,%%r%d,1;\nand.b32 %%r%d,%%r%d,3;\n", rA, tid, cA, tid, t7, rA, t7, t7);
    PP("xor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\nshl.b32 %%r%d,%%r%d,6;\nadd.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", swz, cA, t7, swz, swz, sAoff, rA, sAoff, sAoff, swz, sAoff, sAoff, smb);
    int gA[2], gAL[2];
    for (int i = 0; i < 2; i++) {
        gA[i] = cpx_D(); gAL[i] = cpx_D(); const int d1 = cpx_D(), d2 = cpx_D(), rr = cpx_R();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rr, m0, rA); if (i) PP("add.u32 %%r%d,%%r%d,64;\n", rr, rr);
        PP("mul.wide.u32 %%d%d,%%r%d,%%r%d;\nshl.b64 %%d%d,%%d%d,1;\nshl.b32 %%r%d,%%r%d,4;\ncvt.u64.u32 %%d%d,%%r%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", d1, rr, pK, d1, d1, t7, cA, d2, t7, d1, d1, d2);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", gA[i], dX, d1, gAL[i], dXL, d1);
    }
    /* B: 2 itens (linha da tile (tid>>2) + 64 i, pedaco j = tid&3) */
    const int jj = cpx_R();
    PP("mov.u32 %%r%d,%%r%d;\n", jj, cA);
    int rowp[2], prB[2], soffH[2], soffL[2];
    for (int i = 0; i < 2; i++) {
        rowp[i] = cpx_D(); prB[i] = cpx_P(); const int wr = cpx_R(), d1 = cpx_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", wr, n0, rA); if (i) PP("add.u32 %%r%d,%%r%d,64;\n", wr, wr);
        PP("setp.lt.u32 %%p%d,%%r%d,%%r%d;\nmul.wide.u32 %%d%d,%%r%d,%%r%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", prB[i], wr, prow, d1, wr, rowbytes, rowp[i], dW, d1);
        soffH[i] = cpx_R(); soffL[i] = cpx_R();
        PP("add.u32 %%r%d,%%r%d,%d;\nadd.u32 %%r%d,%%r%d,%d;\n", soffH[i], sAoff, OBH + 4096 * i, soffL[i], sAoff, OBL + 4096 * i);
    }
    const int tpred = cpx_P();
    CpxBX B;
    /* prologo: estagios 0..S-2 */
    for (int s = 0; s < S - 1; s++) {
        PP("setp.lt.u32 %%p%d,%d,%%r%d;\n", tpred, s, nt);
        for (int i = 0; i < 2; i++) {
            const int sa = cpx_R(), dg = cpx_D();
            PP("add.u32 %%r%d,%%r%d,%d;\nadd.u64 %%d%d,%%d%d,%d;\n@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", sa, sAoff, s * ST + 4096 * i, dg, gA[i], s * 64, tpred, sa, dg);
            if (xl) { const int sl = cpx_R(), dl = cpx_D(); PP("add.u32 %%r%d,%%r%d,%d;\nadd.u64 %%d%d,%%d%d,%d;\n@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", sl, sAoff, s * ST + OAL + 4096 * i, dl, gAL[i], s * 64, tpred, sl, dl); }
        }
        PP("cp.async.commit_group;\n");
        const int tr = cpx_R(); PP("mov.u32 %%r%d,%d;\n", tr, s);
        int pr[2]; for (int i = 0; i < 2; i++) { pr[i] = cpx_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], tpred); }
        cpx_load_bx(qt, tr, rowp, pr, jj, &B);
        int sh[2], sl[2]; for (int i = 0; i < 2; i++) { sh[i] = cpx_R(); sl[i] = cpx_R(); PP("add.u32 %%r%d,%%r%d,%d;\nadd.u32 %%r%d,%%r%d,%d;\n", sh[i], soffH[i], s * ST, sl[i], soffL[i], s * ST); }
        cpx_store_bx(qt, sh, sl, wl, &B);
    }
    const int t = cpx_R(), sRd = cpx_R(), sWr = cpx_R(), tl = cpx_R(), ptl = cpx_P(), ploop = cpx_P();
    PP("mov.u32 %%r%d,0;\nmov.u32 %%r%d,0;\nmov.u32 %%r%d,%d;\n", t, sRd, sWr, (S - 1) * ST);
    PP("LOOP:\ncp.async.wait_group %d;\nbar.sync 0;\n", S - 2);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n", tl, t, S - 1, ptl, tl, nt);
    for (int i = 0; i < 2; i++) {
        const int sa = cpx_R(), dg = cpx_D(), dd = cpx_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%d;\nmul.wide.u32 %%d%d,%%r%d,64;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", sa, sAoff, sWr, sa, sa, 4096 * i, dd, tl, dg, gA[i], dd);
        PP("@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", ptl, sa, dg);
        if (xl) { const int sl = cpx_R(), dl = cpx_D(); PP("add.u32 %%r%d,%%r%d,%d;\nadd.u64 %%d%d,%%d%d,%%d%d;\n@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", sl, sa, OAL, dl, gAL[i], dd, ptl, sl, dl); }
    }
    PP("cp.async.commit_group;\n");
    int pr[2]; for (int i = 0; i < 2; i++) { pr[i] = cpx_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], ptl); }
    CpxBX L; cpx_load_bx(qt, tl, rowp, pr, jj, &L);
    /* computo do ladrilho t: 2 passos de k16 */
    const int rdA = cpx_R(), lA = cpx_R(), rowA = cpx_R(), rowB = cpx_R(), sw = cpx_R();
    PP("add.u32 %%r%d,%%r%d,%%r%d;\nand.b32 %%r%d,%%r%d,7;\nshr.u32 %%r%d,%%r%d,1;\nand.b32 %%r%d,%%r%d,3;\n", rdA, smb, sRd, lA, lane, sw, lane, sw, sw);
    { const int a1 = cpx_R(), a2 = cpx_R();
      PP("shr.u32 %%r%d,%%r%d,3;\nand.b32 %%r%d,%%r%d,1;\nshl.b32 %%r%d,%%r%d,3;\nadd.u32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,6;\nadd.u32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,6;\n", a1, lane, a1, a1, a1, a1, rowA, lA, a1, a2, wm, rowA, rowA, a2, rowA, rowA);
      PP("shr.u32 %%r%d,%%r%d,4;\nshl.b32 %%r%d,%%r%d,3;\nadd.u32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,5;\nadd.u32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,6;\n", a1, lane, a1, a1, rowB, lA, a1, a2, wn, rowB, rowB, a2, rowB, rowB); }
    int tmpacc[16][4];
    if (fl2) for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { tmpacc[i][c] = cpx_F(); PP("mov.f32 %%f%d,0f00000000;\n", tmpacc[i][c]); }
    for (int kk = 0; kk < 2; kk++) {
        int af[4][4], al[4][4], bh[2][4], bl[2][4];
        const int ca = cpx_R(), cb = cpx_R(), bA = cpx_R(), bB = cpx_R(), u = cpx_R(), v = cpx_R();
        PP("shr.u32 %%r%d,%%r%d,4;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", u, lane, u, u, 2 * kk, ca, u, sw, ca, ca);
        PP("shr.u32 %%r%d,%%r%d,3;\nand.b32 %%r%d,%%r%d,1;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", v, lane, v, v, v, v, 2 * kk, cb, v, sw, cb, cb);
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bA, rdA, rowA, bA, bA, ca);
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bB, rdA, rowB, bB, bB, cb);
        for (int mi = 0; mi < 4; mi++) {
            for (int c = 0; c < 4; c++) af[mi][c] = cpx_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", af[mi][0], af[mi][1], af[mi][2], af[mi][3], bA, mi * 1024);
            if (xl) { for (int c = 0; c < 4; c++) al[mi][c] = cpx_R();
                PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", al[mi][0], al[mi][1], al[mi][2], al[mi][3], bA, mi * 1024 + OAL); }
        }
        for (int nj = 0; nj < 2; nj++) {
            for (int c = 0; c < 4; c++) bh[nj][c] = cpx_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", bh[nj][0], bh[nj][1], bh[nj][2], bh[nj][3], bB, OBH + nj * 1024);
            if (wl) { for (int c = 0; c < 4; c++) bl[nj][c] = cpx_R();
                PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", bl[nj][0], bl[nj][1], bl[nj][2], bl[nj][3], bB, OBL + nj * 1024); }
        }
        for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) {
            const int *a = fl2 ? tmpacc[mi * 4 + ni] : acc[mi * 4 + ni], nj = ni >> 1, h = (ni & 1) * 2;
#define MMAX(A_, B_) PP("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%%f%d,%%f%d,%%f%d,%%f%d},{%%r%d,%%r%d,%%r%d,%%r%d},{%%r%d,%%r%d},{%%f%d,%%f%d,%%f%d,%%f%d};\n", \
               a[0], a[1], a[2], a[3], A_[mi][0], A_[mi][1], A_[mi][2], A_[mi][3], B_[nj][h], B_[nj][h + 1], a[0], a[1], a[2], a[3])
            MMAX(af, bh);
            if (xl) MMAX(al, bh);
            if (wl) MMAX(af, bl);
#undef MMAX
        }
    }
    if (fl2) for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) PP("add.f32 %%f%d,%%f%d,%%f%d;\nmov.f32 %%f%d,0f00000000;\n", acc[i][c], acc[i][c], tmpacc[i][c], tmpacc[i][c]);
    { int sh[2], sl[2]; for (int i = 0; i < 2; i++) { sh[i] = cpx_R(); sl[i] = cpx_R(); PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", sh[i], soffH[i], sWr, sl[i], soffL[i], sWr); }
      cpx_store_bx(qt, sh, sl, wl, &L); }
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sRd, sRd, ST, ploop, sRd, S * ST, ploop, sRd);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sWr, sWr, ST, ploop, sWr, S * ST, ploop, sWr);
    PP("add.u32 %%r%d,%%r%d,1;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n@%%p%d bra LOOP;\n", t, t, ploop, t, nt, ploop);
    PP("cp.async.wait_group 0;\n");
    cpx_epilogue(acc, lane, wm, wn, m0, n0, pn, prow, pmode, dY, dG);
    PP("ret;\n");
    char *out = save;
    out += sprintf(out, ".visible .entry %s(.param .u64 X,.param .u64 XL,.param .u64 W,.param .u64 Y,.param .u64 G,.param .u32 n,.param .u32 rows,.param .u32 K,.param .u32 mode){\n", name);
    out += sprintf(out, ".reg .b32 %%r<%d>; .reg .b64 %%d<%d>; .reg .f32 %%f<%d>; .reg .pred %%p<%d>;\n", cpx_r + 1, cpx_d + 1, cpx_f + 1, cpx_p + 1);
    out += sprintf(out, "%s}\n", body);
    free(body); cpx_b = out;
}
