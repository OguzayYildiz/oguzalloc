/*
 * Producer/consumer: thread A allocates, thread B frees.
 * At most 1024 objects are alive at once; 10 million objects pass through.
 * A correct allocator keeps memory usage small.
 * Usage: prodcons sys | prodcons my
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "allocator.h"

#define QN    1024
#define TOTAL 10000000L

static void *queue[QN];
static atomic_long head, tail;
static int mine;

static void *producer(void *arg) {
    (void)arg;
    for (long i = 0; i < TOTAL; i++) {
        while (i - atomic_load_explicit(&tail, memory_order_acquire) >= QN) { }
        char *p = mine ? my_malloc(64) : malloc(64);
        p[0] = 1;
        queue[i % QN] = p;
        atomic_store_explicit(&head, i + 1, memory_order_release);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (long i = 0; i < TOTAL; i++) {
        while (atomic_load_explicit(&head, memory_order_acquire) <= i) { }
        void *p = queue[i % QN];
        if (mine) my_free(p); else free(p);
        atomic_store_explicit(&tail, i + 1, memory_order_release);
    }
    return NULL;
}

int main(int argc, char **argv) {
    mine = argc > 1 && strcmp(argv[1], "my") == 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t a, b;
    pthread_create(&a, NULL, producer, NULL);
    pthread_create(&b, NULL, consumer, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("  %-13s time %5.2f s, max RSS %7.1f MB\n",
           mine ? "oguzalloc" : "glibc malloc", sec, (double)ru.ru_maxrss / 1024.0);
    return 0;
}
