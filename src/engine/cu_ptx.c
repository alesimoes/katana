/* ============================ CUDA: kernels em PTX, emitidos por C ===============================
 * Nao ha compilador de CUDA nesta maquina nem no repositorio: os kernels sao escritos como PTX (o assembly virtual da NVIDIA) e emitidos
 * por funcoes C que desenrolam os lacos (a forma de cada ladrilho e parametro). O driver compila o PTX para a placa (cuModuleLoadDataEx).
 *
 * gemm: Y[n][rows] = X[n][K] * W^T, X em F16 (linhas preenchidas ate multiplo de 128), W quantizado [rows][K] (Q4_K, Q5_K) ou BF16.
 *   Ladrilho 128 x 128 x 64, 8 warps (2 x 4, cada um 64 x 32), mma.sync m16n8k16 (f16 -> f32), ldmatrix, A por cp.async em S estagios,
 *   B desquantizado em registradores e gravado em F16 na memoria compartilhada (os pesos nunca existem em F16 na VRAM); trocas de
 *   banco evitadas por troca de blocos de 16 bytes (XOR com a linha). Epilogos: 0 grava F32, 1 R += G[col] * acc (residual com portao),
 *   2 grava F16. */

static char *cp_b; static int cp_r, cp_f, cp_d, cp_p;
#define PP(...) (cp_b += sprintf(cp_b, __VA_ARGS__))
static int cp_R(void) { return cp_r++; }
static int cp_F(void) { return cp_f++; }
static int cp_D(void) { return cp_d++; }
static int cp_P(void) { return cp_p++; }

enum { GQ_BF16 = 0, GQ_Q4K = 4, GQ_Q5K = 5 };
#define GBM 128
#define GBN 128
#define GBK 64

typedef struct { int st, sb; int qs[4], hd[4], sx[4], sy[4], sz[4], qh[4], bf[4][4], qlo[4], qhi[4], hlo[4], hhi[4]; } CpB;

/* carrega (para os registradores) os dados do ladrilho de k `tr` (registrador) dos 4 itens deste thread; pr[i] = linha valida */
static void cp_load_b(int qt, int tr, const int rowp[4], const int pr[4], int jj, int hi, int jf, CpB *B) {
    const int blk = qt == GQ_Q4K ? 144 : 176;
    int tq = cp_R(), tb = cp_R(), off = cp_R(), sb = cp_R();
    if (qt != GQ_BF16) {
        PP("and.b32 %%r%d,%%r%d,3;\n", tq, tr);                     /* t&3 */
        PP("shr.u32 %%r%d,%%r%d,2;\n", tb, tr);                     /* t>>2 */
        PP("shl.b32 %%r%d,%%r%d,1;\n", sb, tq);                     /* 2*(t&3) */
        { const int h1 = cp_R(); PP("shr.u32 %%r%d,%%r%d,2;\n", h1, hi); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sb, sb, h1); }   /* sub-bloco */
    }
    for (int i = 0; i < 4; i++) {
        const int a = cp_D(), a2 = cp_D(), t64 = cp_D();
        if (qt == GQ_BF16) {
            /* 16 bytes a partir da coluna t*64 + j*8 */
            PP("shl.b32 %%r%d,%%r%d,6;\n", off, tr); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", off, off, jf);   /* jf = j*8 */
            PP("mul.wide.u32 %%d%d,%%r%d,2;\n", t64, off);
            PP("add.u64 %%d%d,%%d%d,%%d%d;\n", a, rowp[i], t64);
            for (int c = 0; c < 4; c++) PP("mov.u32 %%r%d,0;\n", B->bf[i][c] = cp_R());
            PP("@%%p%d ld.global.v4.u32 {%%r%d,%%r%d,%%r%d,%%r%d},[%%d%d];\n", pr[i], B->bf[i][0], B->bf[i][1], B->bf[i][2], B->bf[i][3], a);
            continue;
        }
        /* inicio do super-bloco: rowp + (t>>2)*blk */
        PP("mul.wide.u32 %%d%d,%%r%d,%d;\n", t64, tb, blk);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", a, rowp[i], t64);                 /* a = cabecalho do super-bloco */
        B->hd[i] = cp_R(); B->sx[i] = cp_R(); B->sy[i] = cp_R(); B->sz[i] = cp_R();
        PP("mov.u32 %%r%d,0;\n", B->hd[i]); PP("mov.u32 %%r%d,0;\n", B->sx[i]); PP("mov.u32 %%r%d,0;\n", B->sy[i]); PP("mov.u32 %%r%d,0;\n", B->sz[i]);
        PP("@%%p%d ld.global.u32 %%r%d,[%%d%d];\n", pr[i], B->hd[i], a);
        /* bytes de escala: x = s[sb+4], y = s[sb], z = s[sb&3] (a partir de a+4) */
        const int ix = cp_R(), iy = cp_R(), iz = cp_R(); const int dx = cp_D(), dy = cp_D(), dz = cp_D();
        PP("add.u32 %%r%d,%%r%d,4;\n", ix, sb); PP("mov.u32 %%r%d,%%r%d;\n", iy, sb); PP("and.b32 %%r%d,%%r%d,3;\n", iz, sb);
        PP("cvt.u64.u32 %%d%d,%%r%d;\n", dx, ix); PP("cvt.u64.u32 %%d%d,%%r%d;\n", dy, iy); PP("cvt.u64.u32 %%d%d,%%r%d;\n", dz, iz);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dx, dx, a); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dy, dy, a); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dz, dz, a);
        PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sx[i], dx); PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sy[i], dy); PP("@%%p%d ld.global.u8 %%r%d,[%%d%d+4];\n", pr[i], B->sz[i], dz);
        /* quantizados: Q4 em +16 + 32*(t&3) + 8*jj; Q5 em +48 + ... e qh em +16 + 8*jj */
        const int qo = cp_R(); const int dq = cp_D();
        PP("shl.b32 %%r%d,%%r%d,5;\n", qo, tq); PP("shl.b32 %%r%d,%%r%d,3;\n", off, jj); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", qo, qo, off);
        PP("cvt.u64.u32 %%d%d,%%r%d;\n", dq, qo); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dq, dq, a);
        B->qlo[i] = cp_R(); B->qhi[i] = cp_R();
        PP("mov.u32 %%r%d,0;\n", B->qlo[i]); PP("mov.u32 %%r%d,0;\n", B->qhi[i]);
        PP("@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+%d];\n", pr[i], B->qlo[i], B->qhi[i], dq, qt == GQ_Q4K ? 16 : 48);
        if (qt == GQ_Q5K) {
            const int dh = cp_D(); B->hlo[i] = cp_R(); B->hhi[i] = cp_R();
            PP("mov.u32 %%r%d,0;\n", B->hlo[i]); PP("mov.u32 %%r%d,0;\n", B->hhi[i]);
            PP("cvt.u64.u32 %%d%d,%%r%d;\n", dh, off); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", dh, dh, a);
            PP("@%%p%d ld.global.v2.u32 {%%r%d,%%r%d},[%%d%d+16];\n", pr[i], B->hlo[i], B->hhi[i], dh);
        }
    }
    B->st = 1; B->sb = sb; (void)jf;
}

/* converte os registradores de B em 8 valores F16 por item e grava na memoria compartilhada em (soff[i] + base) */
static void cp_store_b(int qt, int soff[4], int sbase, int hi, const CpB *B) {
    const int sbr = B->sb;
    for (int i = 0; i < 4; i++) {
        int pk[4];
        for (int c = 0; c < 4; c++) pk[c] = cp_R();
        if (qt == GQ_BF16) {
            for (int c = 0; c < 4; c++) {
                const int lo = cp_R(), hh = cp_R(), fl = cp_F(), fh = cp_F();
                PP("shl.b32 %%r%d,%%r%d,16;\n", lo, B->bf[i][c]); PP("and.b32 %%r%d,%%r%d,0xffff0000;\n", hh, B->bf[i][c]);
                PP("mov.b32 %%f%d,%%r%d;\n", fl, lo); PP("mov.b32 %%f%d,%%r%d;\n", fh, hh);
                PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", pk[c], fh, fl);
            }
        } else {
            /* d, dmin */
            const int dh = cp_R(), mh = cp_R(), fd = cp_F(), fm = cp_F();
            PP("and.b32 %%r%d,%%r%d,0xffff;\n", dh, B->hd[i]); PP("shr.u32 %%r%d,%%r%d,16;\n", mh, B->hd[i]);
            PP("{ .reg .b16 hh, hm; cvt.u16.u32 hh,%%r%d; cvt.u16.u32 hm,%%r%d; cvt.f32.f16 %%f%d,hh; cvt.f32.f16 %%f%d,hm; }\n", dh, mh, fd, fm);
            /* escala e minimo de 6 bits: sb < 4 (p): sc = y&63, m = x&63; senao sc = (x&15)|((z>>6)<<4), m = (x>>4)|((y>>6)<<4) */
            const int sc = cp_R(), mn = cp_R(), t1 = cp_R(), t2 = cp_R(), t3 = cp_R(), t4 = cp_R(), pp = cp_P();
            PP("setp.lt.u32 %%p%d,%%r%d,4;\n", pp, sbr);
            PP("and.b32 %%r%d,%%r%d,63;\n", t1, B->sy[i]); PP("and.b32 %%r%d,%%r%d,63;\n", t2, B->sx[i]);
            PP("and.b32 %%r%d,%%r%d,15;\n", t3, B->sx[i]); PP("shr.u32 %%r%d,%%r%d,6;\n", t4, B->sz[i]); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", sc, t1, t3, pp);
            PP("shr.u32 %%r%d,%%r%d,4;\n", t3, B->sx[i]); PP("shr.u32 %%r%d,%%r%d,6;\n", t4, B->sy[i]); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,2;\n", t3, t4, t3);
            PP("selp.b32 %%r%d,%%r%d,%%r%d,%%p%d;\n", mn, t2, t3, pp);
            const int fs = cp_F(), fmm = cp_F(), fdsc = cp_F(), fndm = cp_F();
            PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fs, sc); PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fmm, mn);
            PP("mul.f32 %%f%d,%%f%d,%%f%d;\n", fdsc, fd, fs); PP("mul.f32 %%f%d,%%f%d,%%f%d;\n", fndm, fm, fmm); PP("neg.f32 %%f%d,%%f%d;\n", fndm, fndm);
            const int w0 = cp_R(), w1 = cp_R(), h0 = cp_R(), h1 = cp_R();
            PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", w0, B->qlo[i], hi); PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", w1, B->qhi[i], hi);    /* hi = 0 ou 4 */
            if (qt == GQ_Q5K) { PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", h0, B->hlo[i], sbr); PP("shr.u32 %%r%d,%%r%d,%%r%d;\n", h1, B->hhi[i], sbr); }
            int fprev = 0;
            for (int e = 0; e < 8; e++) {
                const int q = cp_R(), fq = cp_F(), fw = cp_F();
                PP("bfe.u32 %%r%d,%%r%d,%d,4;\n", q, e < 4 ? w0 : w1, 8 * (e & 3));
                if (qt == GQ_Q5K) { const int bt = cp_R(); PP("bfe.u32 %%r%d,%%r%d,%d,1;\n", bt, e < 4 ? h0 : h1, 8 * (e & 3)); PP("bfi.b32 %%r%d,%%r%d,%%r%d,4,1;\n", q, bt, q); }
                PP("cvt.rn.f32.u32 %%f%d,%%r%d;\n", fq, q);
                PP("fma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\n", fw, fq, fdsc, fndm);
                if (e & 1) PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\n", pk[e >> 1], fw, fprev);   /* (alto, baixo) */
                fprev = fw;
            }
        }
        PP("st.shared.v4.u32 [%%r%d+%d],{%%r%d,%%r%d,%%r%d,%%r%d};\n", soff[i], sbase, pk[0], pk[1], pk[2], pk[3]);
    }
}

/* emite o modulo PTX inteiro: gemm_bf16, gemm_q4k, gemm_q5k com `stages` estagios. Devolve o tamanho. */
static void cp_gemm(int qt, int S, const char *name) {
    char *body = malloc(1 << 20), *save = cp_b; cp_b = body;
    cp_r = 0; cp_f = 0; cp_d = 0; cp_p = 0;
    const int tid = cp_R(), lane = cp_R(), warp = cp_R(), wm = cp_R(), wn = cp_R(), m0 = cp_R(), n0 = cp_R(), smb = cp_R();
    const int dX = cp_D(), dW = cp_D(), dY = cp_D(), dG = cp_D(), pn = cp_R(), prow = cp_R(), pK = cp_R(), pmode = cp_R();
    PP("ld.param.u64 %%d%d,[X]; ld.param.u64 %%d%d,[W]; ld.param.u64 %%d%d,[Y]; ld.param.u64 %%d%d,[G];\n", dX, dW, dY, dG);
    PP("ld.param.u32 %%r%d,[n]; ld.param.u32 %%r%d,[rows]; ld.param.u32 %%r%d,[K]; ld.param.u32 %%r%d,[mode];\n", pn, prow, pK, pmode);
    PP("mov.u32 %%r%d,%%tid.x; and.b32 %%r%d,%%r%d,31; shr.u32 %%r%d,%%r%d,5;\n", tid, lane, tid, warp, tid);
    PP("shr.u32 %%r%d,%%r%d,2; and.b32 %%r%d,%%r%d,3;\n", wm, warp, wn, warp);
    PP("mov.u32 %%r%d,%%ctaid.y; shl.b32 %%r%d,%%r%d,7; mov.u32 %%r%d,%%ctaid.x; shl.b32 %%r%d,%%r%d,7;\n", m0, m0, m0, n0, n0, n0);
    PP("mov.u32 %%r%d,sm;\n", smb);
    /* acumuladores */
    int acc[16][4];
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) { acc[i][c] = cp_F(); PP("mov.f32 %%f%d,0f00000000;\n", acc[i][c]); }
    /* numero de ladrilhos de k e bytes por linha do peso */
    const int nt = cp_R(), rowbytes = cp_R(), kk256 = cp_R();
    PP("shr.u32 %%r%d,%%r%d,6;\n", nt, pK);
    if (qt == GQ_BF16) PP("shl.b32 %%r%d,%%r%d,1;\n", rowbytes, pK);
    else { PP("shr.u32 %%r%d,%%r%d,8;\n", kk256, pK); PP("mul.lo.u32 %%r%d,%%r%d,%d;\n", rowbytes, kk256, qt == GQ_Q4K ? 144 : 176); }
    /* A: 4 pedacos de 16 bytes por thread */
    const int rA = cp_R(), cA = cp_R(), swz = cp_R(), sAoff = cp_R(), t7 = cp_R();
    PP("shr.u32 %%r%d,%%r%d,3;\n", rA, tid);                 /* linha dentro do ladrilho (0..31) */
    PP("and.b32 %%r%d,%%r%d,7;\n", cA, tid);                 /* pedaco 0..7 */
    PP("and.b32 %%r%d,%%r%d,7;\n", t7, rA);
    PP("xor.b32 %%r%d,%%r%d,%%r%d;\n", swz, cA, t7); PP("shl.b32 %%r%d,%%r%d,4;\n", swz, swz);
    PP("shl.b32 %%r%d,%%r%d,7;\n", sAoff, rA); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sAoff, sAoff, swz);   /* deslocamento na memoria compartilhada */
    PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sAoff, sAoff, smb);
    int gA[4]; const int dstrideA = cp_D();
    PP("mul.wide.u32 %%d%d,%%r%d,2;\n", dstrideA, pK);   /* bytes por linha de X */
    for (int i = 0; i < 4; i++) {
        gA[i] = cp_D(); const int d1 = cp_D(), d2 = cp_D(), rr = cp_R();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rr, m0, rA); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", rr, rr, 32 * i);
        PP("mul.wide.u32 %%d%d,%%r%d,2;\n", d1, pK); PP("mul.wide.u32 %%d%d,%%r%d,%%r%d;\n", d1, rr, pK); PP("shl.b64 %%d%d,%%d%d,1;\n", d1, d1);
        PP("shl.b32 %%r%d,%%r%d,4;\n", t7, cA); PP("cvt.u64.u32 %%d%d,%%r%d;\n", d2, t7);
        PP("add.u64 %%d%d,%%d%d,%%d%d;\n", d1, d1, d2); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", gA[i], dX, d1);
    }
    /* B: 4 itens (linha do peso r = tid/8 + 32 i, pedaco j = tid&7) */
    const int jj = cp_R(), hi = cp_R(), jf = cp_R(), sBoff = cp_R();
    PP("and.b32 %%r%d,%%r%d,3;\n", jj, cA);                   /* j&3 */
    PP("shr.u32 %%r%d,%%r%d,2;\n", hi, cA); PP("shl.b32 %%r%d,%%r%d,2;\n", hi, hi);   /* 0 ou 4: deslocamento do nibble */
    PP("shl.b32 %%r%d,%%r%d,3;\n", jf, cA);                   /* j*8 */
    PP("add.u32 %%r%d,%%r%d,%d;\n", sBoff, sAoff, GBM * 128 / 1);   /* B depois de A no mesmo estagio */
    int rowp[4], prB[4], soffB[4];
    for (int i = 0; i < 4; i++) {
        rowp[i] = cp_D(); prB[i] = cp_P(); const int wr = cp_R(), d1 = cp_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", wr, n0, rA); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", wr, wr, 32 * i);
        PP("setp.lt.u32 %%p%d,%%r%d,%%r%d;\n", prB[i], wr, prow);
        PP("mul.wide.u32 %%d%d,%%r%d,%%r%d;\n", d1, wr, rowbytes); PP("add.u64 %%d%d,%%d%d,%%d%d;\n", rowp[i], dW, d1);
        soffB[i] = cp_R(); PP("add.u32 %%r%d,%%r%d,%d;\n", soffB[i], sBoff, 4096 * i);
    }
    /* o A e o B de cada estagio: A em [0, 16K), B em [16K, 32K): sAoff aponta o A do estagio 0 e soffB[i] o B (estagio 0) */
    const int STG = 32768;
    int tpred = cp_P();
    CpB B;
    /* prologo: estagios 0..S-2 */
    for (int s = 0; s < S - 1; s++) {
        PP("setp.lt.u32 %%p%d,%d,%%r%d;\n", tpred, s, nt);
        for (int i = 0; i < 4; i++) {
            const int sa = cp_R(); PP("add.u32 %%r%d,%%r%d,%d;\n", sa, sAoff, s * STG + 4096 * i);
            const int dg = cp_D(); PP("add.u64 %%d%d,%%d%d,%d;\n", dg, gA[i], s * 128);
            PP("@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", tpred, sa, dg);
        }
        PP("cp.async.commit_group;\n");
        const int tr = cp_R(); PP("mov.u32 %%r%d,%d;\n", tr, s);
        int pr[4]; for (int i = 0; i < 4; i++) { pr[i] = cp_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], tpred); }
        cp_load_b(qt, tr, rowp, pr, jj, hi, jf, &B);
        cp_store_b(qt, soffB, s * STG, hi, &B);
    }
    /* laco principal: t = 0..nt-1; sRd = deslocamento do estagio de leitura, sWr = o do estagio de escrita (t+S-1) */
    const int t = cp_R(), sRd = cp_R(), sWr = cp_R(), tl = cp_R(), ptl = cp_P(), ploop = cp_P();
    PP("mov.u32 %%r%d,0; mov.u32 %%r%d,0; mov.u32 %%r%d,%d;\n", t, sRd, sWr, (S - 1) * STG);
    PP("LOOP:\n");
    PP("cp.async.wait_group %d;\nbar.sync 0;\n", S - 2);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n", tl, t, S - 1, ptl, tl, nt);
    for (int i = 0; i < 4; i++) {
        const int sa = cp_R(), dg = cp_D(), dd = cp_D();
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", sa, sAoff, sWr); if (i) PP("add.u32 %%r%d,%%r%d,%d;\n", sa, sa, 4096 * i);
        PP("mul.wide.u32 %%d%d,%%r%d,128;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dd, tl, dg, gA[i], dd);
        PP("@%%p%d cp.async.cg.shared.global [%%r%d],[%%d%d],16;\n", ptl, sa, dg);
    }
    PP("cp.async.commit_group;\n");
    int pr[4]; for (int i = 0; i < 4; i++) { pr[i] = cp_P(); PP("and.pred %%p%d,%%p%d,%%p%d;\n", pr[i], prB[i], ptl); }
    CpB L; cp_load_b(qt, tl, rowp, pr, jj, hi, jf, &L);
    /* computo do ladrilho t */
    const int rdA = cp_R(), lA = cp_R(), lB = cp_R(), rowA = cp_R(), rowB = cp_R();
    PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rdA, smb, sRd);       /* base do estagio de leitura (A); B = +16384 */
    PP("and.b32 %%r%d,%%r%d,7;\n", lA, lane);                /* l&7 */
    {   const int a1 = cp_R(), a2 = cp_R();
        PP("shr.u32 %%r%d,%%r%d,3; and.b32 %%r%d,%%r%d,1; shl.b32 %%r%d,%%r%d,3;\n", a1, lane, a1, a1, a1, a1);  /* ((l>>3)&1)*8 */
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowA, lA, a1); PP("shl.b32 %%r%d,%%r%d,6;\n", a2, wm); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowA, rowA, a2);   /* wm*64 */
        PP("shl.b32 %%r%d,%%r%d,7;\n", rowA, rowA);
        PP("shr.u32 %%r%d,%%r%d,4;\n", a1, lane); PP("shl.b32 %%r%d,%%r%d,3;\n", a1, a1);   /* (l>>4)*8 */
        PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowB, lA, a1); PP("shl.b32 %%r%d,%%r%d,5;\n", a2, wn); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", rowB, rowB, a2); PP("shl.b32 %%r%d,%%r%d,7;\n", rowB, rowB); }
    (void)lB;
    for (int kk = 0; kk < 4; kk++) {
        int af[4][4], bf[2][4];
        const int ca = cp_R(), cb = cp_R(), bA = cp_R(), bB = cp_R();
        /* pedaco de A: (2kk + (l>>4)) ^ (l&7); de B: (2kk + ((l>>3)&1)) ^ (l&7) */
        { const int u = cp_R(), v = cp_R();
          PP("shr.u32 %%r%d,%%r%d,4;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", u, lane, u, u, 2 * kk, ca, u, lA, ca, ca);
          PP("shr.u32 %%r%d,%%r%d,3;\nand.b32 %%r%d,%%r%d,1;\nadd.u32 %%r%d,%%r%d,%d;\nxor.b32 %%r%d,%%r%d,%%r%d;\nshl.b32 %%r%d,%%r%d,4;\n", v, lane, v, v, v, v, 2 * kk, cb, v, lA, cb, cb); }
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bA, rdA, rowA, bA, bA, ca);
        PP("add.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", bB, rdA, rowB, bB, bB, cb);
        for (int mi = 0; mi < 4; mi++) {
            for (int c = 0; c < 4; c++) af[mi][c] = cp_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", af[mi][0], af[mi][1], af[mi][2], af[mi][3], bA, mi * 2048);
        }
        for (int nj = 0; nj < 2; nj++) {
            for (int c = 0; c < 4; c++) bf[nj][c] = cp_R();
            PP("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%%r%d,%%r%d,%%r%d,%%r%d},[%%r%d+%d];\n", bf[nj][0], bf[nj][1], bf[nj][2], bf[nj][3], bB, 16384 + nj * 2048);
        }
        for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) {
            const int *a = acc[mi * 4 + ni], nj = ni >> 1, h = (ni & 1) * 2;
            PP("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%%f%d,%%f%d,%%f%d,%%f%d},{%%r%d,%%r%d,%%r%d,%%r%d},{%%r%d,%%r%d},{%%f%d,%%f%d,%%f%d,%%f%d};\n",
               a[0], a[1], a[2], a[3], af[mi][0], af[mi][1], af[mi][2], af[mi][3], bf[nj][h], bf[nj][h + 1], a[0], a[1], a[2], a[3]);
        }
    }
    /* desquantiza o ladrilho t+S-1 para o estagio de escrita */
    { int so[4]; for (int i = 0; i < 4; i++) { so[i] = cp_R(); PP("add.u32 %%r%d,%%r%d,%%r%d;\n", so[i], soffB[i], sWr); }
      cp_store_b(qt, so, 0, hi, &L); }
    /* avanca */
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sRd, sRd, STG, ploop, sRd, S * STG, ploop, sRd);
    PP("add.u32 %%r%d,%%r%d,%d;\nsetp.eq.u32 %%p%d,%%r%d,%d;\n@%%p%d mov.u32 %%r%d,0;\n", sWr, sWr, STG, ploop, sWr, S * STG, ploop, sWr);
    PP("add.u32 %%r%d,%%r%d,1;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\n@%%p%d bra LOOP;\n", t, t, ploop, t, nt, ploop);
    PP("cp.async.wait_group 0;\n");
    /* epilogo */
    const int er = cp_R(), ec = cp_R(), q = cp_R();
    PP("shr.u32 %%r%d,%%r%d,2;\nshl.b32 %%r%d,%%r%d,6;\nadd.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", er, lane, q, wm, er, er, q, er, er, m0);   /* m0 + wm*64 + lane/4 */
    PP("and.b32 %%r%d,%%r%d,3;\nshl.b32 %%r%d,%%r%d,1;\nshl.b32 %%r%d,%%r%d,5;\nadd.u32 %%r%d,%%r%d,%%r%d;\nadd.u32 %%r%d,%%r%d,%%r%d;\n", ec, lane, ec, ec, q, wn, ec, ec, q, ec, ec, n0);
    for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) for (int hh = 0; hh < 2; hh++) {
        const int *a = acc[mi * 4 + ni];
        const int tok = cp_R(), col = cp_R(), pp = cp_P(), pq = cp_P(), o = cp_D(), dd = cp_D(), idx = cp_R();
        PP("add.u32 %%r%d,%%r%d,%d;\nadd.u32 %%r%d,%%r%d,%d;\n", tok, er, mi * 16 + hh * 8, col, ec, ni * 8);
        PP("setp.lt.u32 %%p%d,%%r%d,%%r%d;\nsetp.lt.u32 %%p%d,%%r%d,%%r%d;\nand.pred %%p%d,%%p%d,%%p%d;\n", pp, tok, pn, pq, col, prow, pp, pp, pq);
        PP("mad.lo.u32 %%r%d,%%r%d,%%r%d,%%r%d;\n", idx, tok, prow, col);
        const int v0 = a[hh * 2], v1 = a[hh * 2 + 1];
        PP("setp.eq.u32 %%p%d,%%r%d,0;\n", pq, pmode);
        PP("mul.wide.u32 %%d%d,%%r%d,4;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dd, idx, o, dY, dd);
        PP("and.pred %%p%d,%%p%d,%%p%d;\n@%%p%d st.global.v2.f32 [%%d%d],{%%f%d,%%f%d};\n", pq, pq, pp, pq, o, v0, v1);
        /* modo 1: R[idx] += G[col] * v (R = Y) */
        const int g0 = cp_F(), g1 = cp_F(), r0 = cp_F(), r1 = cp_F(), dgg = cp_D(), pm1 = cp_P();
        PP("setp.eq.u32 %%p%d,%%r%d,1;\nand.pred %%p%d,%%p%d,%%p%d;\n", pm1, pmode, pm1, pm1, pp);
        PP("mul.wide.u32 %%d%d,%%r%d,4;\nadd.u64 %%d%d,%%d%d,%%d%d;\n", dgg, col, dgg, dG, dgg);
        PP("mov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\nmov.f32 %%f%d,0f00000000;\n", g0, g1, r0, r1);
        PP("@%%p%d ld.global.v2.f32 {%%f%d,%%f%d},[%%d%d];\n@%%p%d ld.global.v2.f32 {%%f%d,%%f%d},[%%d%d];\n", pm1, g0, g1, dgg, pm1, r0, r1, o);
        PP("fma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\nfma.rn.f32 %%f%d,%%f%d,%%f%d,%%f%d;\n", r0, g0, v0, r0, r1, g1, v1, r1);
        PP("@%%p%d st.global.v2.f32 [%%d%d],{%%f%d,%%f%d};\n", pm1, o, r0, r1);
        /* modo 2: F16 em Y (2 bytes por valor) */
        const int pm2 = cp_P(), hv = cp_R(), d2 = cp_D(), d3 = cp_D();
        PP("setp.eq.u32 %%p%d,%%r%d,2;\nand.pred %%p%d,%%p%d,%%p%d;\n", pm2, pmode, pm2, pm2, pp);
        PP("cvt.rn.f16x2.f32 %%r%d,%%f%d,%%f%d;\nmul.wide.u32 %%d%d,%%r%d,2;\nadd.u64 %%d%d,%%d%d,%%d%d;\n@%%p%d st.global.b32 [%%d%d],%%r%d;\n", hv, v1, v0, d2, idx, d3, dY, d2, pm2, d3, hv);
    }
    PP("ret;\n");
    /* cabecalho com as declaracoes */
    char *out = save;
    out += sprintf(out, ".visible .entry %s(.param .u64 X,.param .u64 W,.param .u64 Y,.param .u64 G,.param .u32 n,.param .u32 rows,.param .u32 K,.param .u32 mode){\n", name);
    out += sprintf(out, ".reg .b32 %%r<%d>; .reg .b64 %%d<%d>; .reg .f32 %%f<%d>; .reg .pred %%p<%d>;\n", cp_r + 1, cp_d + 1, cp_f + 1, cp_p + 1);
    out += sprintf(out, "%s}\n", body);
    free(body); cp_b = out;
}
