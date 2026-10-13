/*
 * LD_PRELOAD shim: routes the standard malloc family to oguzalloc.
 * Usage:  LD_PRELOAD=./build/liboguzalloc.so <program>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"

#define EXPORT __attribute__((visibility("default")))

EXPORT void *malloc(size_t n)                 { return my_malloc(n); }
EXPORT void  free(void *p)                    { my_free(p); }
EXPORT void  cfree(void *p)                   { my_free(p); }
EXPORT void *calloc(size_t c, size_t s)       { return my_calloc(c, s); }
EXPORT void *realloc(void *p, size_t n)       { return my_realloc(p, n); }
EXPORT void *memalign(size_t a, size_t n)     { return my_memalign(a, n); }
EXPORT void *aligned_alloc(size_t a, size_t n){ return my_memalign(a, n); }
EXPORT void *valloc(size_t n)                 { return my_memalign(4096, n); }
EXPORT void *pvalloc(size_t n)                { return my_memalign(4096, (n + 4095) & ~(size_t)4095); }
EXPORT size_t malloc_usable_size(void *p)     { return my_usable_size(p); }

EXPORT void *reallocarray(void *p, size_t c, size_t s) {
    size_t n;
    if (__builtin_mul_overflow(c, s, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    return my_realloc(p, n);
}

EXPORT int posix_memalign(void **out, size_t a, size_t n) {
    if (a < sizeof(void *) || (a & (a - 1))) {
        return EINVAL;
    }
    void *p = my_memalign(a, n);
    if (!p) {
        return ENOMEM;
    }
    *out = p;
    return 0;
}
