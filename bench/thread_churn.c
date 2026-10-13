/*
 * 2000 threads are created and joined one after another; each allocates and
 * frees a few small blocks. Prints the peak memory usage (max RSS) at the end.
 * Usage: thread_churn sys | thread_churn my
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "allocator.h"

static int mine;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        char *p = mine ? my_malloc(100) : malloc(100);
        memset(p, 1, 100);
        if (mine) my_free(p); else free(p);
    }
    return NULL;
}

int main(int argc, char **argv) {
    mine = argc > 1 && strcmp(argv[1], "my") == 0;
    for (int i = 0; i < 2000; i++) {
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        pthread_join(t, NULL);
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("  %-13s max RSS %.1f MB\n", mine ? "oguzalloc" : "glibc malloc",
           (double)ru.ru_maxrss / 1024.0);
    return 0;
}
