/*
 * Grows a single buffer with realloc from 16 bytes to 64 MB, 12.5% per step
 * (20 repetitions). This is how dynamic arrays and string builders behave.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "allocator.h"

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(void) {
    for (int mine = 0; mine < 2; mine++) {
        double t0 = now();
        for (int r = 0; r < 20; r++) {
            char *p = NULL;
            for (size_t sz = 16; sz < ((size_t)64 << 20); sz += sz / 8 + 16) {
                p = mine ? my_realloc(p, sz) : realloc(p, sz);
                p[sz - 1] = 1;
            }
            if (mine) my_free(p); else free(p);
        }
        printf("  %-13s %.3f s\n", mine ? "oguzalloc" : "glibc malloc", now() - t0);
    }
    return 0;
}
