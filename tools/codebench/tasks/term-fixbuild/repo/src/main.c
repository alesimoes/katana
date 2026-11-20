#include <stdio.h>
#include "stats.h"
#include "format.h"

int main(int argc, char **argv) {
    double v[1000];
    int n = 0;
    if (argc < 2) { fprintf(stderr, "usage: stats FILE\n"); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    while (n < 1000 && fscanf(f, "%lf", &v[n]) == 1) n++;
    fclose(f);
    print_stats(n, mean(v, n), stddev(v, n));
    return 0;
}
