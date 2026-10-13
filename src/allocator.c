/*
 * oguzalloc
 *
 * Design:
 *  - Memory is requested from the OS in 4 MB "segments" (mmap).
 *  - A segment is split into 64 KB "pages". Each page belongs to a single size
 *    class and starts with a 64-byte page header.
 *  - Objects have no header of their own. free(p) finds the page of p by
 *    clearing the low 16 bits of the address (pages are 64 KB-aligned).
 *  - Every thread owns a "heap": a free list and an active page per size class,
 *    plus a cache of large blocks. The fast path is lock-free and O(1).
 *  - An object freed by a thread other than its owner is pushed onto the owning
 *    heap's lock-free "remote list"; the owner reclaims it later.
 *  - When a thread exits, its heap goes onto an "abandoned" list and a newly
 *    created thread adopts it, so creating and destroying threads does not leak.
 *  - Requests above 8 KB are served with mmap. Freed large blocks are kept in a
 *    size-bounded cache, and growing them uses mremap instead of copying.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "allocator.h"

#define PAGE_SIZE          ((size_t)64 * 1024)
#define PAGE_MASK          (~(uintptr_t)(PAGE_SIZE - 1))
#define SEGMENT_SIZE       ((size_t)4 * 1024 * 1024)
#define HDR_SIZE           ((size_t)64)
#define OS_PAGE            ((size_t)4096)
#define MAX_SMALL          ((size_t)8192)
#define NUM_CLASSES        76
#define CLASS_LARGE        0xFFFFFFFFu
#define PAGE_MAGIC         0x6F67757A616C6C63ULL      /* "oguzallc" */
#define LARGE_CACHE_N      64
#define LARGE_CACHE_MAX    ((size_t)1024 * 1024)      /* larger blocks are never cached */
#define LARGE_CACHE_BUDGET ((size_t)32 * 1024 * 1024) /* per-heap cache limit in bytes */

#define TLS         _Thread_local __attribute__((tls_model("initial-exec")))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NOINLINE    __attribute__((noinline))

typedef struct ThreadHeap ThreadHeap;

typedef struct PageHdr {
    uint64_t    magic;
    uint32_t    cls;        /* size class, or CLASS_LARGE */
    uint32_t    reserved;
    size_t      map_size;   /* large block: size of the mapping */
    size_t      usable;     /* large block: usable bytes */
    ThreadHeap *owner;      /* small-object page: the heap that owns it */
} PageHdr;
_Static_assert(sizeof(PageHdr) <= HDR_SIZE, "page header must fit in 64 bytes");

typedef struct {
    void     *free_list;   /* singly linked list of free objects */
    uintptr_t bump;        /* start of the not-yet-handed-out part of the active page */
    uintptr_t bump_end;
} ClassState;

struct ThreadHeap {
    ClassState      cls[NUM_CLASSES];
    _Atomic(void *) remote[NUM_CLASSES];      /* objects freed by other threads */
    uintptr_t       seg_cur, seg_end;         /* next free page in the active segment */
    PageHdr        *large_cache[LARGE_CACHE_N];
    unsigned        large_count;
    size_t          large_bytes;
    ThreadHeap     *next_abandoned;
};

static TLS ThreadHeap *tl_heap;

/* Heaps whose thread has exited. Protected by a short spinlock; only touched
   when a thread starts or exits, never on the fast path. */
static ThreadHeap     *abandoned;
static atomic_flag     abandoned_lock = ATOMIC_FLAG_INIT;
static pthread_key_t   heap_key;
static pthread_once_t  heap_key_once = PTHREAD_ONCE_INIT;

/* Marker written to the second word of a free object, used to detect double frees. */
static char free_key_anchor;
#define FREE_KEY ((void *)&free_key_anchor)

static const uint32_t class_size[NUM_CLASSES] = {
      16,   32,   48,   64,   80,   96,  112,  128,  144,  160,  176,  192,
     208,  224,  240,  256,  272,  288,  304,  320,  336,  352,  368,  384,
     400,  416,  432,  448,  464,  480,  496,  512,  528,  544,  560,  576,
     592,  608,  624,  640,  656,  672,  688,  704,  720,  736,  752,  768,
     784,  800,  816,  832,  848,  864,  880,  896,  912,  928,  944,  960,
     976,  992, 1008, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192
};

/* Maps a size in 1..MAX_SMALL to its size class index. */
static inline unsigned size_class(size_t n) {
    if (n <= 1024) {
        return (unsigned)((n + 15) >> 4) - 1;
    }
    /* Each range (2^lg, 2^(lg+1)] is split into 4 equal steps. */
    unsigned lg = 63u - (unsigned)__builtin_clzl(n - 1);
    unsigned idx = (unsigned)((n - 1 - ((size_t)1 << lg)) >> (lg - 2));
    return 64u + (lg - 10u) * 4u + idx;
}

static void die(const char *msg) {
    ssize_t r = write(2, msg, strlen(msg));
    (void)r;
    abort();
}

/* Maps a PAGE_SIZE-aligned region of `size` bytes. `size` must be a multiple of OS_PAGE. */
static void *map_aligned(size_t size) {
    size_t total = size + PAGE_SIZE;
    char *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return NULL;
    }
    uintptr_t start = ((uintptr_t)raw + PAGE_SIZE - 1) & PAGE_MASK;
    size_t head = start - (uintptr_t)raw;
    size_t tail = total - head - size;
    if (head) munmap(raw, head);
    if (tail) munmap((char *)start + size, tail);
    return (void *)start;
}

/* ---------------- Heap lifecycle ---------------- */

static void lock_abandoned(void) {
    while (atomic_flag_test_and_set_explicit(&abandoned_lock, memory_order_acquire)) { }
}
static void unlock_abandoned(void) {
    atomic_flag_clear_explicit(&abandoned_lock, memory_order_release);
}

/* Runs when a thread exits: put its heap on the abandoned list. */
static void heap_abandon(void *arg) {
    ThreadHeap *h = arg;
    if (!h) {
        return;
    }
    tl_heap = NULL;
    lock_abandoned();
    h->next_abandoned = abandoned;
    abandoned = h;
    unlock_abandoned();
}

static void make_key(void) {
    pthread_key_create(&heap_key, heap_abandon);
}

/* Runs on a thread's first allocation: adopt an abandoned heap or create a new one. */
static NOINLINE ThreadHeap *heap_init(void) {
    ThreadHeap *h = NULL;
    lock_abandoned();
    if (abandoned) {
        h = abandoned;
        abandoned = h->next_abandoned;
    }
    unlock_abandoned();
    if (!h) {
        h = mmap(NULL, sizeof(ThreadHeap), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (h == MAP_FAILED) {
            return NULL;
        }
        /* mmap memory is zeroed, so all lists and counters start empty. */
    }
    h->next_abandoned = NULL;
    tl_heap = h;   /* set first: if pthread_setspecific calls malloc, it uses this heap */
    pthread_once(&heap_key_once, make_key);
    pthread_setspecific(heap_key, h);
    return h;
}

static inline ThreadHeap *get_heap(void) {
    ThreadHeap *h = tl_heap;
    return LIKELY(h != NULL) ? h : heap_init();
}

/* ---------------- Small objects ---------------- */

static uintptr_t new_page(ThreadHeap *hp) {
    if (hp->seg_cur == hp->seg_end) {
        void *seg = map_aligned(SEGMENT_SIZE);
        if (!seg) {
            return 0;
        }
        hp->seg_cur = (uintptr_t)seg;
        hp->seg_end = hp->seg_cur + SEGMENT_SIZE;
    }
    uintptr_t page = hp->seg_cur;
    hp->seg_cur += PAGE_SIZE;
    return page;
}

/* Slow path: the class has run out of space, take a new page. */
static NOINLINE void *refill(ThreadHeap *hp, unsigned c) {
    uintptr_t page = new_page(hp);
    if (!page) {
        errno = ENOMEM;
        return NULL;
    }
    PageHdr *h = (PageHdr *)page;
    h->magic = PAGE_MAGIC;
    h->cls = c;
    h->owner = hp;
    ClassState *s = &hp->cls[c];
    s->bump = page + HDR_SIZE + class_size[c];
    s->bump_end = page + PAGE_SIZE;
    return (void *)(page + HDR_SIZE);
}

/* Pushes the object onto the owning heap's remote list without locks (Treiber stack).
   Only "push" and "take all" are performed, so the ABA problem cannot occur. */
static NOINLINE void remote_free(PageHdr *h, void **p) {
    _Atomic(void *) *head = &h->owner->remote[h->cls];
    p[1] = FREE_KEY;
    void *old = atomic_load_explicit(head, memory_order_relaxed);
    do {
        p[0] = old;
    } while (!atomic_compare_exchange_weak_explicit(head, &old, (void *)p,
                                                    memory_order_release,
                                                    memory_order_relaxed));
}

static NOINLINE void check_double_free(ClassState *s, void *ptr) {
    for (void **p = s->free_list; p; p = *p) {
        if (p == ptr) {
            die("oguzalloc: double free\n");
        }
    }
}

/* ---------------- Large blocks ---------------- */

/* If *fresh is set to 1, the memory was freshly mapped (and therefore zeroed). */
static NOINLINE void *large_alloc(size_t n, size_t align, int *fresh) {
    size_t off = align > HDR_SIZE ? align : HDR_SIZE;
    if (off >= PAGE_SIZE || n > SIZE_MAX - off - OS_PAGE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t map = (off + n + OS_PAGE - 1) & ~(OS_PAGE - 1);

    ThreadHeap *hp = get_heap();
    if (hp) {
        /* Best-fit search in the cache. Blocks more than twice the needed size
           are skipped so that big blocks are not wasted on small requests. */
        int best = -1;
        for (unsigned i = 0; i < hp->large_count; i++) {
            size_t m = hp->large_cache[i]->map_size;
            if (m >= map && m <= map * 2 &&
                (best < 0 || m < hp->large_cache[best]->map_size)) {
                best = (int)i;
            }
        }
        if (best >= 0) {
            PageHdr *h = hp->large_cache[best];
            hp->large_cache[best] = hp->large_cache[--hp->large_count];
            hp->large_bytes -= h->map_size;
            h->usable = h->map_size - off;
            if (fresh) *fresh = 0;
            return (char *)h + off;
        }
    }

    PageHdr *h = map_aligned(map);
    if (!h) {
        errno = ENOMEM;
        return NULL;
    }
    h->magic = PAGE_MAGIC;
    h->cls = CLASS_LARGE;
    h->map_size = map;
    h->usable = map - off;
    h->owner = NULL;
    if (fresh) *fresh = 1;
    return (char *)h + off;
}

static NOINLINE void large_free(PageHdr *h) {
    ThreadHeap *hp = get_heap();
    if (hp) {
        for (unsigned i = 0; i < hp->large_count; i++) {
            if (hp->large_cache[i] == h) {
                die("oguzalloc: double free (large block)\n");
            }
        }
        if (h->map_size <= LARGE_CACHE_MAX && hp->large_count < LARGE_CACHE_N &&
            hp->large_bytes + h->map_size <= LARGE_CACHE_BUDGET) {
            hp->large_cache[hp->large_count++] = h;
            hp->large_bytes += h->map_size;
            return;
        }
    }
    munmap(h, h->map_size);
}

/* Grows a large block without copying. First tries to extend it in place; if
   that fails, maps a new 64 KB-aligned address and moves the physical pages
   there with mremap. Only page table entries change; no bytes are copied. */
static NOINLINE void *large_grow(PageHdr *h, size_t n) {
    size_t off = h->map_size - h->usable;
    if (n > SIZE_MAX - off - OS_PAGE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t new_map = (off + n + OS_PAGE - 1) & ~(OS_PAGE - 1);

    void *r = mremap(h, h->map_size, new_map, 0);   /* in place */
    if (r == MAP_FAILED) {
        void *dst = map_aligned(new_map);
        if (!dst) {
            errno = ENOMEM;
            return NULL;
        }
        r = mremap(h, h->map_size, new_map, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
        if (r == MAP_FAILED) {
            munmap(dst, new_map);
            errno = ENOMEM;
            return NULL;
        }
    }
    PageHdr *nh = r;
    nh->map_size = new_map;
    nh->usable = new_map - off;
    return (char *)nh + off;
}

/* ---------------- Public API ---------------- */

void *my_malloc(size_t n) {
    if (LIKELY(n <= MAX_SMALL)) {
        ThreadHeap *hp = get_heap();
        if (UNLIKELY(hp == NULL)) {
            errno = ENOMEM;
            return NULL;
        }
        unsigned c = size_class(n ? n : 1);
        ClassState *s = &hp->cls[c];
        void **p = s->free_list;
        if (LIKELY(p != NULL)) {                       /* 1) local free list */
            s->free_list = p[0];
            p[1] = NULL;
            return p;
        }
        if (atomic_load_explicit(&hp->remote[c], memory_order_relaxed)) {
            /* 2) objects returned by other threads: take them all at once */
            void **r = atomic_exchange_explicit(&hp->remote[c], NULL, memory_order_acquire);
            s->free_list = r[0];
            r[1] = NULL;
            return r;
        }
        size_t sz = class_size[c];
        if (LIKELY(s->bump_end - s->bump >= sz)) {     /* 3) carve from the active page */
            void *q = (void *)s->bump;
            s->bump += sz;
            return q;
        }
        return refill(hp, c);                          /* 4) new page */
    }
    return large_alloc(n, 16, NULL);
}

void my_free(void *ptr) {
    if (UNLIKELY(ptr == NULL)) {
        return;
    }
    PageHdr *h = (PageHdr *)((uintptr_t)ptr & PAGE_MASK);
    if (UNLIKELY(h->magic != PAGE_MAGIC)) {
        die("oguzalloc: free of invalid pointer\n");
    }
    if (UNLIKELY(h->cls == CLASS_LARGE)) {
        large_free(h);
        return;
    }
    void **p = ptr;
    if (UNLIKELY(h->owner != tl_heap)) {
        remote_free(h, p);
        return;
    }
    ClassState *s = &h->owner->cls[h->cls];
    if (UNLIKELY(p[1] == FREE_KEY)) {
        check_double_free(s, ptr);
    }
    p[0] = s->free_list;
    p[1] = FREE_KEY;
    s->free_list = p;
}

size_t my_usable_size(void *ptr) {
    if (!ptr) {
        return 0;
    }
    PageHdr *h = (PageHdr *)((uintptr_t)ptr & PAGE_MASK);
    if (h->cls == CLASS_LARGE) {
        return h->usable;
    }
    return class_size[h->cls];
}

void *my_calloc(size_t count, size_t size) {
    size_t n;
    if (__builtin_mul_overflow(count, size, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    if (n > MAX_SMALL) {
        int fresh = 0;
        void *p = large_alloc(n, 16, &fresh);
        if (p && !fresh) {
            memset(p, 0, n);
        }
        return p;
    }
    void *p = my_malloc(n);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

void *my_realloc(void *ptr, size_t n) {
    if (!ptr) {
        return my_malloc(n);
    }
    if (n == 0) {
        my_free(ptr);
        return NULL;
    }
    size_t old = my_usable_size(ptr);
    /* Stay in place if it fits, but move a large block that shrinks a lot. */
    if (n <= old && !(old > MAX_SMALL && n <= old / 4)) {
        return ptr;
    }
    if (n > old && old > MAX_SMALL) {
        return large_grow((PageHdr *)((uintptr_t)ptr & PAGE_MASK), n);
    }
    void *q = my_malloc(n);
    if (!q) {
        return NULL;
    }
    memcpy(q, ptr, old < n ? old : n);
    my_free(ptr);
    return q;
}

void *my_memalign(size_t alignment, size_t n) {
    if (alignment == 0 || (alignment & (alignment - 1))) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment <= 16) {
        return my_malloc(n);
    }
    if (alignment <= 64) {
        /* Objects in a page sit at (page + 64 + k * size). If the size is a
           multiple of the alignment and at most 1024 (classes step by 16 there),
           every object address is aligned. */
        size_t m = (n + alignment - 1) & ~(alignment - 1);
        if (m == 0) m = alignment;
        if (m <= 1024) {
            return my_malloc(m);
        }
    }
    return large_alloc(n, alignment, NULL);
}
