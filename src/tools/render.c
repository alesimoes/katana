/* ---------- ferramenta Render ----------
 * Chama `katana render generate` (src/render/): da imagem de uma vista do modelo 3D (SketchUp) e de um pedido, gera uma imagem realista.
 * Enxuta de proposito: imagem, pedido, motor, saida, briefing. O logo e a composicao ficam no comando (--logo) e no desktop.
 * KATANA_RENDER_BIN troca o executavel chamado (teste sem modelo: tools/tooltest/render.sh). */

static const char CT_RENDER_DESC[] =
    "Generate an image on this machine with a local image model. Any picture: illustration, character, scene, architecture; a reference image is optional. "
    "`brief` is the FINAL prompt, written by you in the engine's language and style; the engine does not rewrite, translate or add anything. "
    "stable (default, fast preview): short English tags or phrases separated by commas, subject and style first, up to ~290 tokens (~200 words). "
    "flux (high definition): English prose, subject and style first, up to ~500 tokens (~350 words). "
    "Beyond the limit, whole lines or phrases are cut by relevance (colours, numbers, rules, rare words kept); the result says `brief: N/budget tokens` and `cut: ...` so you can resend it shorter. "
    "`image` (PNG or JPEG) is optional: a view, sketch or photo the picture should follow; leave it out to draw from the text alone. `brief` may also be the path of a text file with the prompt. "
    "Flow: preview with stable first, show the result and ASK the user whether it is approved; call flux only after the user approves (more steps and a larger size for the final), "
    "never stable then flux in the same answer. If the user asks directly for the final or high definition, go straight to flux. "
    "It takes minutes (stable about 25 s to a few minutes, flux longer): tell the user before calling. Returns the path of the image written and which backend ran; tell the user the path.";
static const char CT_RENDER_PROPS[] =
    "\"request\": {\"type\": \"string\", \"description\": \"Optional short text in the user's words; with brief it is appended to the prompt as written, alone it is the whole request.\"}, "
    "\"image\": {\"type\": \"string\", \"description\": \"Optional path of a reference image (PNG or JPEG) the picture follows.\"}, "
    "\"engine\": {\"type\": \"string\", \"description\": \"stable (fast preview, default) or flux (final quality).\"}, "
    "\"out\": {\"type\": \"string\", \"description\": \"Path of the PNG to write (default: next to the image, with -render-<engine> in the name).\"}, "
    "\"brief\": {\"type\": \"string\", \"description\": \"The final prompt you wrote, in the engine's style and within its limit (see the tool description), or the path of a file with it.\"}, "
    "\"size\": {\"type\": \"string\", \"description\": \"Optional WIDTHxHEIGHT, multiples of 64, for example 1024x576. Default: the proportion of the image, 576 on the long side.\"}, "
    "\"steps\": {\"type\": \"integer\", \"description\": \"Optional number of steps (flux 1 to 8, default 4; stable with LCM default 4).\"}";
static const char CT_RENDER_REQ[] = "";

/* 1 = os modelos do motor estao na maquina (stable: Stable Diffusion; flux: FLUX.2 klein); usado pela nota do prompt do agente */
static int render_have(int flux) {
    char p[4200]; struct stat st;
    snprintf(p, sizeof p, flux ? "%s/flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf" : "%s/render/sd15-realistic-vision.gguf", models_dir());
    return stat(p, &st) == 0;
}

static void ct_render(const char *js, const char *end, char *out, size_t cap) {
    char *req = ct_str(js, end, "request"), *img = ct_str(js, end, "image"), *eng = ct_str(js, end, "engine"), *dst = ct_str(js, end, "out"), *brf = ct_str(js, end, "brief"), *siz = ct_str(js, end, "size");
    char *res = NULL, tmpb[64] = ""; const long long stp = ct_int(js, end, "steps", 0);
    out[0] = 0;
    if (!eng || !eng[0]) { free(eng); eng = strdup("stable"); }
    if (strcmp(eng, "stable") && strcmp(eng, "flux")) { snprintf(out, cap, "error: engine must be stable or flux, not \"%.30s\"", eng); goto done; }
    if ((!req || !req[0]) && (!brf || !brf[0])) { snprintf(out, cap, "error: give a brief (the briefing text you write) or a request (what the image should look like)"); goto done; }
    if (req && strlen(req) > 3000) { snprintf(out, cap, "error: request is too long (3000 characters at most); put the long text in brief"); goto done; }
    if (brf && strlen(brf) > 60000) { snprintf(out, cap, "error: brief is too long (60000 characters at most)"); goto done; }
    if (!getenv("KATANA_RENDER_BIN") && !render_have(!strcmp(eng, "flux"))) { snprintf(out, cap, "error: the %s models are not installed on this machine (%s/%s)", eng, models_dir(), !strcmp(eng, "flux") ? "flux2klein/gguf" : "render"); goto done; }
    if (siz && siz[0]) { int w = 0, h = 0; char x; if (sscanf(siz, "%d%c%d", &w, &x, &h) != 3 || (x != 'x' && x != 'X') || w < 64 || h < 64 || w > 2048 || h > 2048 || w % 64 || h % 64) { snprintf(out, cap, "error: size must be WIDTHxHEIGHT with both multiples of 64 (64 to 2048), for example 1024x576"); goto done; } }
    if (stp && (stp < 1 || stp > (!strcmp(eng, "flux") ? 8 : 200))) { snprintf(out, cap, "error: steps must be 1 to %d for %s", !strcmp(eng, "flux") ? 8 : 200, eng); goto done; }
    if (img && img[0] && access(img, R_OK) != 0) { snprintf(out, cap, "error: cannot read the image %.300s", img); goto done; }
    /* brief: texto de varias linhas (ou longo) vai para um arquivo temporario; senao e o caminho de um arquivo existente */
    if (brf && brf[0]) {
        if (strchr(brf, '\n') || strlen(brf) > 240) {
            snprintf(tmpb, sizeof tmpb, "/tmp/katana-brief-XXXXXX");
            const int fd = mkstemp(tmpb);
            if (fd < 0) { tmpb[0] = 0; snprintf(out, cap, "error: cannot write the brief to a temporary file"); goto done; }
            const size_t bl = strlen(brf); const ssize_t wn = write(fd, brf, bl); close(fd);
            if (wn < 0 || (size_t)wn != bl) { snprintf(out, cap, "error: cannot write the brief to a temporary file"); goto done; }
        } else if (access(brf, R_OK) != 0) { snprintf(out, cap, "error: cannot read the brief file %.300s (to give the briefing text, write it in several lines)", brf); goto done; }
    }
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
        if (brf && brf[0]) { if (!shq(tmpb[0] ? tmpb : brf, qf, sizeof qf)) { snprintf(out, cap, "error: path too long"); goto done; } n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --brief %s", qf); }
        if (req && req[0]) { if (!shq(req, qr, sizeof qr)) { snprintf(out, cap, "error: request too long"); goto done; } n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --nl %s", qr); }
        if (siz && siz[0]) { char sz[40]; snprintf(sz, sizeof sz, "%s", siz); for (char *c = sz; *c; c++) if (*c == 'X') *c = 'x'; n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --size %s", sz); }
        if (stp) n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --steps %lld", stp);
        if (!strcmp(eng, "stable")) {      /* previa rapida: LCM (4 passos) quando o modelo existe */
            char lp[4200]; snprintf(lp, sizeof lp, "%s/render/sd15-realistic-vision-lcm.gguf", models_dir());
            if (access(lp, R_OK) == 0) n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --lcm");
        }
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
            char used[240]; const char *tag = !strcmp(eng, "flux") ? "[flux] usado:" : "[stable] usado:";
            used[0] = 0; const char *u = strstr(res, tag);           /* o motor diz quem processou de fato (cpu, cuda ou vulkan) */
            if (u) { size_t k = 0; while (u[k] && u[k] != '\n' && k < sizeof used - 1) { used[k] = u[k]; k++; } used[k] = 0; }
            else snprintf(used, sizeof used, "%s unknown", tag);
            /* o motor diz quantos tokens o prompt usou e o que cortou ("[brief] <motor> tokens N/B" e "[brief] cut N: ..."): o modelo reenvia mais curto */
            char bl[240] = "", cl[1300] = "";
            { const char *q = strstr(res, "[brief] "); 
              while (q) {
                  const char *eol = strchr(q, '\n'); size_t k = eol ? (size_t)(eol - q) : strlen(q);
                  const char *t = strstr(q, " tokens "); const int isTok = t && t < q + k && !strncmp(q + 8, eng, strlen(eng));
                  if (isTok) { int u2 = 0, b2 = 0; if (sscanf(t + 8, "%d/%d", &u2, &b2) == 2) snprintf(bl, sizeof bl, "\nbrief: %d/%d tokens", u2, b2); }
                  else if (!strncmp(q + 8, "cut ", 4)) { const char *c = strchr(q + 12, ':'); if (c && c < q + k) snprintf(cl, sizeof cl, "\ncut: %.*s", (int)(k - (size_t)(c + 2 - q) < 1200 ? k - (size_t)(c + 2 - q) : 1200), c + 2); }
                  q = eol ? strstr(eol, "[brief] ") : NULL;
              } }
            snprintf(out, cap, "rendered with %s: %s (%lld bytes)%s%s\n%s", eng, ap, (long long)st.st_size, bl, cl, used);
        }
    }
done:
    if (tmpb[0]) unlink(tmpb);
    free(req); free(img); free(eng); free(dst); free(brf); free(siz); free(res);
}
