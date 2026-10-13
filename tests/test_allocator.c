#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include "allocator.h"

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static unsigned rng(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 8) & 0xffffff;
}

static void test_alignment_and_usable_size(void) {
    for (size_t n = 0; n <= 20000; n++) {
        void *p = my_malloc(n);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p % 16) == 0);
        CHECK(my_usable_size(p) >= n);
        memset(p, 0xAB, n);
        my_free(p);
    }
    puts("PASS alignment and usable size (0..20000)");
}

static void test_reuse_lifo(void) {
    void *a = my_malloc(64);
    my_free(a);
    void *b = my_malloc(64);
    CHECK(a == b);
    my_free(b);
    puts("PASS freed block is reused");
}

/* Many blocks of random sizes: fill each block with its own pattern and verify
   it is intact before freeing. Any overlap between blocks is caught here. */
static void stress(unsigned seed, int rounds, size_t max_size) {
    enum { N = 2000 };
    static __thread void *ptr[N];
    static __thread size_t len[N];
    memset(ptr, 0, sizeof ptr);
    for (int r = 0; r < rounds; r++) {
        for (int k = 0; k < N; k++) {
            int i = (int)(rng(&seed) % N);
            if (ptr[i]) {
                unsigned char *b = ptr[i];
                for (size_t j = 0; j < len[i]; j++) {
                    if (b[j] != (unsigned char)(i * 31 + j)) {
                        fprintf(stderr, "FAIL data corruption in block %d\n", i);
                        exit(1);
                    }
                }
                my_free(ptr[i]);
                ptr[i] = NULL;
            } else {
                len[i] = 1 + rng(&seed) % max_size;
                ptr[i] = my_malloc(len[i]);
                CHECK(ptr[i] != NULL);
                unsigned char *b = ptr[i];
                for (size_t j = 0; j < len[i]; j++) b[j] = (unsigned char)(i * 31 + j);
            }
        }
    }
    for (int i = 0; i < N; i++) my_free(ptr[i]);
}

static void test_stress_single(void) {
    stress(1, 20, 600);
    stress(2, 5, 9000);
    stress(3, 2, 70000);
    puts("PASS stress test (small, medium, large blocks)");
}

static void test_calloc(void) {
    /* calloc must zero memory even when a large block comes back dirty from the cache. */
    size_t n = 100000;
    unsigned char *p = my_malloc(n);
    memset(p, 0xFF, n);
    my_free(p);
    unsigned char *q = my_calloc(1, n);
    for (size_t i = 0; i < n; i++) CHECK(q[i] == 0);
    my_free(q);

    unsigned char *s = my_malloc(48);
    memset(s, 0xFF, 48);
    my_free(s);
    unsigned char *t = my_calloc(6, 8);
    for (int i = 0; i < 48; i++) CHECK(t[i] == 0);
    my_free(t);

    CHECK(my_calloc(SIZE_MAX / 2, 4) == NULL);   /* overflow */
    puts("PASS calloc (zeroing and overflow check)");
}

static void test_realloc(void) {
    unsigned char *p = my_realloc(NULL, 10);
    for (int i = 0; i < 10; i++) p[i] = (unsigned char)i;
    size_t sizes[] = {20, 100, 5000, 50000, 300000, 30, 8};
    size_t keep = 10;
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        p = my_realloc(p, sizes[k]);
        CHECK(p != NULL);
        for (size_t i = 0; i < keep && i < sizes[k]; i++) CHECK(p[i] == (unsigned char)i);
        if (sizes[k] < keep) keep = sizes[k];
    }
    CHECK(my_realloc(p, 0) == NULL);

    /* Force the mremap path: grow a large block many times; contents and alignment must survive. */
    size_t cur = 20000;
    unsigned char *q = my_malloc(cur);
    for (size_t i = 0; i < cur; i++) q[i] = (unsigned char)(i * 7);
    while (cur < (32u << 20)) {
        size_t next = cur * 3 / 2;
        q = my_realloc(q, next);
        CHECK(q != NULL && ((uintptr_t)q % 16) == 0);
        for (size_t i = 0; i < 20000; i += 97) CHECK(q[i] == (unsigned char)(i * 7));
        q[next - 1] = 1;
        cur = next;
    }
    my_free(q);
    puts("PASS realloc (grow/shrink preserves contents)");
}

static void test_memalign(void) {
    for (size_t a = 16; a <= 32768; a *= 2) {
        for (size_t n = 1; n < 5000; n += 777) {
            void *p = my_memalign(a, n);
            CHECK(p != NULL);
            CHECK(((uintptr_t)p % a) == 0);
            memset(p, 1, n);
            my_free(p);
        }
    }
    CHECK(my_memalign(48, 10) == NULL);   /* not a power of two */
    puts("PASS memalign (alignment 16..32768)");
}

static void *thread_stress(void *arg) {
    stress((unsigned)(uintptr_t)arg, 10, 2000);
    return NULL;
}

static void test_threads(void) {
    enum { T = 8 };
    pthread_t th[T];
    for (int i = 0; i < T; i++) pthread_create(&th[i], NULL, thread_stress, (void *)(uintptr_t)(i + 10));
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    puts("PASS 8 threads at once");
}

/* One thread allocates, another thread frees. */
enum { XN = 20000 };
static void *xptr[XN];
static void *x_free_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < XN; i++) my_free(xptr[i]);
    for (int i = 0; i < XN; i++) {          /* reuse the returned memory */
        char *p = my_malloc(64);
        memset(p, 7, 64);
        my_free(p);
    }
    return NULL;
}
static void test_cross_thread_free(void) {
    for (int i = 0; i < XN; i++) {
        xptr[i] = my_malloc(64);
        memset(xptr[i], 3, 64);
    }
    pthread_t t;
    pthread_create(&t, NULL, x_free_thread, NULL);
    pthread_join(t, NULL);
    puts("PASS free on another thread");
}

/* Regression test: memory must stay bounded in a producer/consumer pattern.
   (The first version grew to 612 MB over 10M objects.) */
#define PC_QN 1024
#define PC_TOTAL 3000000L
static void *pc_queue[PC_QN];
static atomic_long pc_head, pc_tail;
static void *pc_producer(void *arg) {
    (void)arg;
    for (long i = 0; i < PC_TOTAL; i++) {
        while (i - atomic_load_explicit(&pc_tail, memory_order_acquire) >= PC_QN) { }
        char *p = my_malloc(64);
        p[0] = 1;
        pc_queue[i % PC_QN] = p;
        atomic_store_explicit(&pc_head, i + 1, memory_order_release);
    }
    return NULL;
}
static void *pc_consumer(void *arg) {
    (void)arg;
    for (long i = 0; i < PC_TOTAL; i++) {
        while (atomic_load_explicit(&pc_head, memory_order_acquire) <= i) { }
        my_free(pc_queue[i % PC_QN]);
        atomic_store_explicit(&pc_tail, i + 1, memory_order_release);
    }
    return NULL;
}
static long max_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}
static void test_producer_consumer_memory(void) {
    long before = max_rss_kb();
    pthread_t a, b;
    pthread_create(&a, NULL, pc_producer, NULL);
    pthread_create(&b, NULL, pc_consumer, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    long grew_mb = (max_rss_kb() - before) / 1024;
    CHECK(grew_mb < 32);   /* with the old leak this grew by ~190 MB */
    printf("PASS producer/consumer memory is bounded (growth %ld MB)\n", grew_mb);
}

/* Regression test: creating and destroying thousands of threads must not leak.
   (Before heap adoption, every exited thread leaked ~4 KB.) */
static void *churn_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        char *p = my_malloc(100);
        memset(p, 1, 100);
        my_free(p);
    }
    char *big = my_malloc(40000);
    big[0] = 1;
    my_free(big);
    return NULL;
}
static void test_thread_churn(void) {
    long before = max_rss_kb();
    for (int i = 0; i < 3000; i++) {
        pthread_t t;
        pthread_create(&t, NULL, churn_worker, NULL);
        pthread_join(t, NULL);
    }
    long grew_mb = (max_rss_kb() - before) / 1024;
    CHECK(grew_mb < 4);
    printf("PASS 3000 threads created/destroyed without leaking (growth %ld MB)\n", grew_mb);
}

static int dies_with_abort(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", 1);
        dup2(fd, 2);
        fn();
        _exit(0);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}
static void do_double_free_small(void) { void *p = my_malloc(32); my_free(p); my_free(p); }
static void do_double_free_large(void) { void *p = my_malloc(50000); my_free(p); my_free(p); }
static void do_bad_pointer(void) { static char x[100000]; my_free(x + 70000); }

static void test_error_detection(void) {
    CHECK(dies_with_abort(do_double_free_small));
    CHECK(dies_with_abort(do_double_free_large));
    CHECK(dies_with_abort(do_bad_pointer));
    puts("PASS double free and invalid pointer are detected");
}

int main(void) {
    test_alignment_and_usable_size();
    test_reuse_lifo();
    test_stress_single();
    test_calloc();
    test_realloc();
    test_memalign();
    test_threads();
    test_cross_thread_free();
    test_producer_consumer_memory();
    test_thread_churn();
    test_error_detection();
    puts("All tests passed.");
    return 0;
}
