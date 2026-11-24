/* ---------- ferramenta Render ----------
 * Chama `katana render generate` (src/render/): da imagem de uma vista do modelo 3D (SketchUp) e de um pedido, gera uma imagem realista.
 * Enxuta de proposito: imagem, pedido, motor, saida, briefing. O logo e a composicao ficam no comando (--logo) e no desktop.
 * KATANA_RENDER_BIN troca o executavel chamado (teste sem modelo: tools/tooltest/render.sh). */

static const char CT_RENDER_DESC[] =
    "Make a realistic image from a view of a 3D model (for example a SketchUp export) and a request, on this machine. "
    "engine stable (default): Stable Diffusion with edge guidance, keeps the geometry of the view, about 5 minutes on the CPU. "
    "engine flux: FLUX.2, final quality, the view is the reference, the request is turned into a prose prompt. "
    "brief: a briefing file (.md or .txt with MATERIAIS, PRINCIPAIS OBJETOS, ILUMINACAO sections) that gives the prompt by itself; "
    "request is then optional extra text. It takes minutes: tell the user before calling. Returns the path of the image written; "
    "SendUserFile can show it.";
static const char CT_RENDER_PROPS[] =
    "\"request\": {\"type\": \"string\", \"description\": \"What the image should look like, in the user's words.\"}, "
    "\"image\": {\"type\": \"string\", \"description\": \"Path of the view image (PNG or JPEG) to follow.\"}, "
    "\"engine\": {\"type\": \"string\", \"description\": \"stable (default) or flux.\"}, "
    "\"out\": {\"type\": \"string\", \"description\": \"Path of the PNG to write (default: next to the image, with -render-<engine> in the name).\"}, "
    "\"brief\": {\"type\": \"string\", \"description\": \"Path of a briefing file; replaces the prompt written from the request.\"}";
static const char CT_RENDER_REQ[] = "";

static void ct_render(const char *js, const char *end, char *out, size_t cap) {
    char *req = ct_str(js, end, "request"), *img = ct_str(js, end, "image"), *eng = ct_str(js, end, "engine"), *dst = ct_str(js, end, "out"), *brf = ct_str(js, end, "brief");
    char *res = NULL;
    out[0] = 0;
    if (!eng || !eng[0]) { free(eng); eng = strdup("stable"); }
    if (strcmp(eng, "stable") && strcmp(eng, "flux")) { snprintf(out, cap, "error: engine must be stable or flux, not \"%.30s\"", eng); goto done; }
    if ((!req || !req[0]) && (!brf || !brf[0])) { snprintf(out, cap, "error: give a request (what the image should look like) or a brief file"); goto done; }
    if (req && strlen(req) > 3000) { snprintf(out, cap, "error: request is too long (3000 characters at most); put the long text in a brief file"); goto done; }
    if (img && img[0] && access(img, R_OK) != 0) { snprintf(out, cap, "error: cannot read the image %.300s", img); goto done; }
    if (brf && brf[0] && access(brf, R_OK) != 0) { snprintf(out, cap, "error: cannot read the brief file %.300s", brf); goto done; }
    char target[1200];
    if (dst && dst[0]) snprintf(target, sizeof target, "%s", dst);
    else if (img && img[0]) {
        snprintf(target, sizeof target, "%s", img);
        char *dot = strrchr(target, '.'); if (dot && !strchr(dot, '/')) *dot = 0;
        snprintf(target + strlen(target), sizeof target - strlen(target), "-render-%s.png", eng);
    } else snprintf(target, sizeof target, "render-%s.png", eng);
    if (img && img[0] && !strcmp(img, target)) { snprintf(out, cap, "error: out is the same file as image; the view is never overwritten"); goto done; }
    {
        const char *bin = getenv("KATANA_RENDER_BIN"); char self[1100];
        if (!bin || !bin[0]) { const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1); if (n <= 0) { snprintf(out, cap, "error: cannot find the katana executable"); goto done; } self[n] = 0; bin = self; }
        char qb[1200], qi[1200], qo[1200], qf[1200], qr[6200], cmd[10400];
        if (!shq(bin, qb, sizeof qb) || !shq(target, qo, sizeof qo)) { snprintf(out, cap, "error: path too long"); goto done; }
        int n = snprintf(cmd, sizeof cmd, "%s render generate --engine %s", qb, eng);
        if (img && img[0]) { if (!shq(img, qi, sizeof qi)) { snprintf(out, cap, "error: path too long"); goto done; } n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --init %s", qi); }
        if (brf && brf[0]) { if (!shq(brf, qf, sizeof qf)) { snprintf(out, cap, "error: path too long"); goto done; } n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --brief %s", qf); }
        if (req && req[0]) { if (!shq(req, qr, sizeof qr)) { snprintf(out, cap, "error: request too long"); goto done; } n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --nl %s", qr); }
        snprintf(cmd + n, sizeof cmd - (size_t)n, " --out %s 2>&1 </dev/null", qo);
        int rc; size_t rn;
        res = run_capture(cmd, 200000, &rc, &rn);
        if (!res) { snprintf(out, cap, "error: could not start the render"); goto done; }
        const char *tail = tail_lines(res, rn, 6, 900);
        struct stat st;
        if (rc != 0) snprintf(out, cap, "error: render failed (exit code %d): %.900s", rc, tail);
        else if (stat(target, &st) != 0 || st.st_size <= 0) snprintf(out, cap, "error: the render finished but %.300s was not written: %.600s", target, tail);
        else {
            char ap[1300]; if (target[0] != '/' && getcwd(ap, 900)) snprintf(ap + strlen(ap), sizeof ap - strlen(ap), "/%s", target); else snprintf(ap, sizeof ap, "%s", target);
            snprintf(out, cap, "rendered with %s: %s (%lld bytes)%s", eng, ap, (long long)st.st_size, brf && brf[0] ? "; prompt from the brief file" : "");
        }
    }
done:
    free(req); free(img); free(eng); free(dst); free(brf); free(res);
}
