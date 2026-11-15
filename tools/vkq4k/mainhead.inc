void main() {
    const uint ir = gl_WorkGroupID.x * BM, ic = gl_WorkGroupID.y * BN;
    const uint id = gl_LocalInvocationID.x;
    const uint warp = gl_SubgroupID, wr = warp % 2, wc = warp / 2;
    coopmat<ACCT, gl_ScopeSubgroup, T, T, gl_MatrixUseAccumulator> acc[CR * CC];
    for (uint i = 0; i < CR * CC; i++) acc[i] = coopmat<ACCT, gl_ScopeSubgroup, T, T, gl_MatrixUseAccumulator>(0.0);
    const uint r0 = id & 31, grp = id >> 5;
