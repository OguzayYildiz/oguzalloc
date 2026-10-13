#ifndef OGUZALLOC_H
#define OGUZALLOC_H

#include <stddef.h>

/*
 * oguzalloc: a thread-caching, size-class based memory allocator.
 * All functions are thread-safe (without locks on the fast path).
 * On failure they return NULL and set errno to ENOMEM.
 */
void  *my_malloc(size_t n);
void   my_free(void *ptr);
void  *my_calloc(size_t count, size_t size);
void  *my_realloc(void *ptr, size_t n);
void  *my_memalign(size_t alignment, size_t n);
size_t my_usable_size(void *ptr);

#endif
