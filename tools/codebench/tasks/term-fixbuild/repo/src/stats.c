#include <math.h>
#include "stats.h"

double mean(const double *v, int n) {
    double s = 0;
    int i, unused;
    for (i = 0; i < n; i++) s += v[i];
    return n ? s / n : 0;
}

double stddev(const double *v, int n) {
    double m = mean(v, n), s = 0;
    for (int i = 0; i < n; i++) s += (v[i] - m) * (v[i] - m);
    return n ? sqrt(s / n) : 0;
}
