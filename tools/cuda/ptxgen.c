/* Ferramenta de desenvolvimento (nao faz parte do katana): compila kernels escritos em CUDA C (tools/cuda/*.cu) para PTX com o NVRTC,
 * carregado por dlopen (so no computador de quem desenvolve; o katana embute o PTX gerado em src/engine/cu_kern.inc e nao precisa do NVRTC).
 *   cc -o ptxgen tools/cuda/ptxgen.c -ldl && ./ptxgen /caminho/libnvrtc.so.13 sm_80 tools/cuda/kern.cu > saida.ptx */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: ptxgen libnvrtc arch fonte.cu [opcoes...] > saida.ptx\n"); return 1; }
    void *h = dlopen(argv[1], RTLD_NOW); if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*create)(void **, const char *, const char *, int, const char *const *, const char *const *) = dlsym(h, "nvrtcCreateProgram");
    int (*compile)(void *, int, const char *const *) = dlsym(h, "nvrtcCompileProgram");
    int (*logsz)(void *, size_t *) = dlsym(h, "nvrtcGetProgramLogSize"), (*log)(void *, char *) = dlsym(h, "nvrtcGetProgramLog");
    int (*ptxsz)(void *, size_t *) = dlsym(h, "nvrtcGetPTXSize"), (*ptx)(void *, char *) = dlsym(h, "nvrtcGetPTX");
    FILE *f = fopen(argv[3], "rb"); if (!f) { perror(argv[3]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(n + 1); if (fread(src, 1, n, f) != (size_t)n) return 1; src[n] = 0; fclose(f);
    void *p; if (create(&p, src, "k.cu", 0, NULL, NULL)) { fprintf(stderr, "create\n"); return 1; }
    char arch[64]; snprintf(arch, sizeof arch, "--gpu-architecture=compute_%s", argv[2] + 3);
    const char *opt[16]; int no = 0; opt[no++] = arch; opt[no++] = "--std=c++14"; opt[no++] = "-default-device";
    for (int i = 4; i < argc && no < 16; i++) opt[no++] = argv[i];
    int r = compile(p, no, opt);
    size_t ls; logsz(p, &ls); if (ls > 1) { char *l = malloc(ls); log(p, l); fprintf(stderr, "%s\n", l); }
    if (r) return 2;
    size_t sz; ptxsz(p, &sz); char *o = malloc(sz); ptx(p, o); fputs(o, stdout);
    return 0;
}
