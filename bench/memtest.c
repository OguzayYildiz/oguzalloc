/*
 * Memory efficiency: allocates 1,000,000 objects (16-256 bytes), keeps them all
 * alive, then prints the peak physical memory usage of the process (max RSS).
 * Usage: memtest sys | memtest my
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "allocator.h"

int main(int argc, char **argv) {
    int mine = argc > 1 && strcmp(argv[1], "my") == 0;
    enum { N = 1000000 };
    static void *ptr[N];
    unsigned seed = 42;
    size_t requested = 0;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 16 + ((seed >> 8) % 241);
        requested += n;
        ptr[i] = mine ? my_malloc(n) : malloc(n);
        memset(ptr[i], 1, n);
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double rss_mb = (double)ru.ru_maxrss / 1024.0;
    double req_mb = (double)requested / (1024.0 * 1024.0);
    printf("  %-13s requested %6.1f MB, used (max RSS) %6.1f MB, overhead %.0f%%\n",
           mine ? "oguzalloc" : "glibc malloc", req_mb, rss_mb, (rss_mb / req_mb - 1) * 100);
    return 0;
}
