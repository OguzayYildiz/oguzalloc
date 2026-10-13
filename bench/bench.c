/*
 * Compares oguzalloc and glibc malloc on the same workloads.
 * Each scenario runs 5 times after a warm-up run; the median is reported.
 * Result: average time of one malloc + one free pair, in nanoseconds.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "allocator.h"

typedef struct {
    const char *name;
    void *(*alloc)(size_t);
    void (*release)(void *);
} Allocator;

static void *sys_malloc(size_t n) { return malloc(n); }
static void  sys_free(void *p)    { free(p); }

static const Allocator ALLOCS[2] = {
    {"glibc malloc", sys_malloc, sys_free},
    {"oguzalloc",    my_malloc,  my_free},
};

typedef struct {
    const char *name;
    size_t min, max;   /* size range */
    int batch;         /* number of blocks alive at the same time */
    int rounds;
    int random_order;  /* 1: free in random order */
    int threads;
} Scenario;

static const Scenario SCENARIOS[] = {
    {"64 B, alloc then free (glibc best)",    64,   64,     1,  2000000, 0, 1},
    {"64 B, 1000 live, reverse free",         64,   64,     1000, 2000, 0, 1},
    {"16-512 B, 1000 live, random free",      16,   512,    1000, 2000, 1, 1},
    {"1-8 KB, 1000 live, random free",        1024, 8192,   1000, 500,  1, 1},
    {"16-256 KB, 64 live, random free",       16384,262144, 64,   300,  1, 1},
    {"16-512 B, 2 threads",                   16,   512,    1000, 2000, 1, 2},
};

typedef struct {
    const Allocator *a;
    const Scenario *s;
    unsigned seed;
} Job;

static unsigned rng(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 8) & 0xffffff;
}

static void *worker(void *arg) {
    Job *j = arg;
    const Scenario *s = j->s;
    void **ptr = calloc((size_t)s->batch, sizeof *ptr);
    size_t *size = malloc((size_t)s->batch * sizeof *size);
    int *order = malloc((size_t)s->batch * sizeof *order);
    unsigned seed = j->seed;

    for (int i = 0; i < s->batch; i++) {
        size[i] = s->min + (s->max > s->min ? rng(&seed) % (s->max - s->min + 1) : 0);
        order[i] = i;
    }
    if (s->random_order) {
        for (int i = s->batch - 1; i > 0; i--) {
            int k = (int)(rng(&seed) % (unsigned)(i + 1));
            int t = order[i]; order[i] = order[k]; order[k] = t;
        }
    }
    for (int r = 0; r < s->rounds; r++) {
        for (int i = 0; i < s->batch; i++) {
            char *p = j->a->alloc(size[i]);
            p[0] = (char)i;                       /* actually touch the memory */
            p[size[i] - 1] = (char)r;
            ptr[i] = p;
        }
        if (s->random_order) {
            for (int i = 0; i < s->batch; i++) j->a->release(ptr[order[i]]);
        } else {
            for (int i = s->batch - 1; i >= 0; i--) j->a->release(ptr[i]);
        }
    }
    free(ptr); free(size); free(order);
    return NULL;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static double run_once(const Allocator *a, const Scenario *s) {
    pthread_t th[16];
    Job jobs[16];
    double t0 = now();
    for (int i = 0; i < s->threads; i++) {
        jobs[i] = (Job){a, s, 1234u + (unsigned)i};
        pthread_create(&th[i], NULL, worker, &jobs[i]);
    }
    for (int i = 0; i < s->threads; i++) pthread_join(th[i], NULL);
    double t1 = now();
    double ops = (double)s->batch * s->rounds * s->threads;
    return (t1 - t0) * 1e9 / ops;
}

static int cmp(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

int main(void) {
    printf("%-38s %14s %14s %10s\n", "scenario", "glibc (ns)", "oguzalloc (ns)", "speedup");
    for (size_t k = 0; k < sizeof SCENARIOS / sizeof *SCENARIOS; k++) {
        const Scenario *s = &SCENARIOS[k];
        double med[2];
        for (int a = 0; a < 2; a++) {
            run_once(&ALLOCS[a], s);              /* warm-up run */
            double t[5];
            for (int i = 0; i < 5; i++) t[i] = run_once(&ALLOCS[a], s);
            qsort(t, 5, sizeof *t, cmp);
            med[a] = t[2];
        }
        printf("%-38s %14.1f %14.1f %9.2fx\n", s->name, med[0], med[1], med[0] / med[1]);
    }
    printf("\nspeedup > 1: oguzalloc is faster, < 1: glibc is faster\n");
    return 0;
}
