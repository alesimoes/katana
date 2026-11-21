#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void) {
    size_t cap = 1024, n = 0;
    double *v = malloc(cap * sizeof *v);
    char *line = NULL;
    size_t lcap = 0;
    long lineno = 0;
    if (!v) return 3;
    while (getline(&line, &lcap, stdin) != -1) {
        char *p = line, *end;
        double x;
        lineno++;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        x = strtod(p, &end);
        while (isspace((unsigned char)*end)) end++;
        if (end == p || *end) {
            fprintf(stderr, "linha %ld invalida\n", lineno);
            return 2;
        }
        if (n == cap) {
            cap *= 2;
            v = realloc(v, cap * sizeof *v);
            if (!v) return 3;
        }
        v[n++] = x;
    }
    if (n == 0) {
        puts("sem dados");
        return 1;
    }
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    qsort(v, n, sizeof *v, cmp);
    double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    printf("n=%zu\nsoma=%.2f\nmedia=%.2f\nmin=%.2f\nmax=%.2f\nmediana=%.2f\n", n, sum, sum / n, v[0], v[n - 1], med);
    free(v);
    free(line);
    return 0;
}
