#include <errno.h>
/* ---------- desfazer as alteracoes de arquivo do agente ----------
 * Antes de write_file e edit_file gravarem, o motor guarda a versao anterior do arquivo em ~/.katana/undo. /undo
 * devolve os arquivos ao estado de antes do ultimo pedido do usuario que alterou algo (de novo: o pedido anterior).
 * Nao cobre o que um comando do shell altera, nem arquivos lidos so como root ou maiores que 8 MB. */
#define UNDO_N 400
typedef struct { char *path, *bak; int req; } UndoE;      /* bak NULL: o arquivo nao existia (desfazer apaga) */
static UndoE g_undo[UNDO_N];
static int g_undo_n, g_undo_req, g_undo_seq, g_undo_exit;

static void undo_drop(int i) {
    if (g_undo[i].bak) { remove(g_undo[i].bak); free(g_undo[i].bak); }
    free(g_undo[i].path);
    memmove(&g_undo[i], &g_undo[i + 1], (size_t)(g_undo_n - i - 1) * sizeof g_undo[0]);
    g_undo_n--;
}
static void undo_cleanup(void) { while (g_undo_n) undo_drop(g_undo_n - 1); }

/* chamada antes de alterar path (ja resolvido) */
static void undo_note(const char *path) {
    for (int i = g_undo_n - 1; i >= 0 && g_undo[i].req == g_undo_req; i--) if (strcmp(g_undo[i].path, path) == 0) return;   /* ja guardado neste pedido */
    if (g_undo_n == UNDO_N) undo_drop(0);
    char *bak = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        char h[1024], bp[1400];
        agent_home(h, sizeof h);
        snprintf(bp, sizeof bp, "%s/.katana/undo/%ld-%d.bak", h, (long)getpid(), ++g_undo_seq);
        mkdir_parents(bp);
        FILE *o = fopen(bp, "wb");
        if (!o) { fclose(f); return; }
        char buf[65536]; size_t n, tot = 0; int bad = 0;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) { tot += n; if (tot > 8000000 || fwrite(buf, 1, n, o) != n) { bad = 1; break; } }
        fclose(f); fclose(o);
        if (bad) { remove(bp); return; }
        bak = strdup(bp);
    } else if (errno != ENOENT) return;                     /* existe mas nao da para ler (root): nao registra */
    g_undo[g_undo_n].path = strdup(path); g_undo[g_undo_n].bak = bak; g_undo[g_undo_n].req = g_undo_req; g_undo_n++;
    if (!g_undo_exit) { atexit(undo_cleanup); g_undo_exit = 1; }
}

/* desfaz o ultimo pedido com alteracoes; escreve a lista em out. Devolve quantos arquivos voltaram */
static int undo_last(char *out, size_t cap) {
    out[0] = 0;
    if (!g_undo_n) return 0;
    const int req = g_undo[g_undo_n - 1].req;
    int n = 0; size_t o = 0;
    while (g_undo_n && g_undo[g_undo_n - 1].req == req) {
        UndoE *e = &g_undo[g_undo_n - 1];
        int ok = 0;
        if (!e->bak) ok = remove(e->path) == 0 || errno == ENOENT;
        else {
            FILE *f = fopen(e->bak, "rb"), *w = f ? fopen(e->path, "wb") : NULL;
            if (f && w) { char buf[65536]; size_t k; ok = 1; while ((k = fread(buf, 1, sizeof buf, f)) > 0) if (fwrite(buf, 1, k, w) != k) ok = 0; }
            if (f) fclose(f);
            if (w) fclose(w);
        }
        if (o + strlen(e->path) + 40 < cap) o += (size_t)snprintf(out + o, cap - o, "%s%s%s", o ? "; " : "", e->path, !ok ? " (FAILED)" : e->bak ? " (restored)" : " (removed, it was new)");
        n += ok;
        undo_drop(g_undo_n - 1);
    }
    return n;
}
