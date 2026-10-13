# oguzalloc

> **Status:** Built with AI assistance. I am currently studying every component
> in depth and adding my own changes on top; see the commit history.

A thread-caching memory allocator for Linux, written from scratch in C.
It is a drop-in replacement for `malloc`/`free` (via `LD_PRELOAD`) and runs real
programs such as `gcc`, `git` and multi-threaded Python.

```
LD_PRELOAD=./build/liboguzalloc.so <any program>
```

## Results

Measured against glibc 2.39 `malloc` on a 2-vCPU cloud VM (Intel Xeon 2.1 GHz),
gcc 13.3, `-O2`. Microbenchmarks report the median of 5 runs. Numbers vary by roughly
±20% between runs on this machine; the ratios are stable.

**Allocator microbenchmarks** (time per `malloc` + `free` pair):

| Workload                                   | glibc    | oguzalloc | Speedup |
|--------------------------------------------|---------:|----------:|--------:|
| 64 B, allocate then free immediately *     | 12.3 ns  | 6.8 ns    | 1.8×    |
| 64 B, 1000 live objects                    | 37.5 ns  | 6.0 ns    | 6.3×    |
| 16–512 B, 1000 live, random free order     | 63.9 ns  | 9.7 ns    | 6.6×    |
| 1–8 KB, 1000 live, random free order       | 222.8 ns | 23.9 ns   | 9.3×    |
| 16–256 KB, 64 live, random free order      | 1272 ns  | 75.9 ns   | 16.8×   |
| 16–512 B, 2 threads                        | 38.7 ns  | 5.9 ns    | 6.5×    |

\* glibc's best case: its per-thread cache (tcache) holds up to 7 objects per size.

**Memory and correctness under stress:**

| Scenario                                          | glibc             | oguzalloc         |
|---------------------------------------------------|-------------------|-------------------|
| 1M live objects (16–256 B, 129.6 MB requested)    | 145.6 MB (+12%)   | 138.2 MB (+7%)    |
| Producer/consumer, 10M objects across 2 threads   | 1.95 s, 2.3 MB    | 0.80 s, 2.3 MB    |
| Buffer grown with `realloc` from 16 B to 64 MB    | 0.006 s           | 0.009 s           |
| 2000 threads created and destroyed                | 2.3 MB            | 2.3 MB            |

**Real programs** (`scripts/real_world.sh`, median of 5 runs, alternating order):

| Program                                 | glibc  | oguzalloc | Difference |
|-----------------------------------------|-------:|----------:|-----------:|
| `gcc -O2` on a 3000-function C file     | 9.16 s | 8.67 s    | −5.4%      |
| Python: build, JSON round-trip, sort    | 2.40 s | 2.33 s    | −3.0%      |

The honest reading: the allocator itself is several times faster, but in real
programs allocation is a small share of total work. `gcc` was faster in every
run (−5% to −10% across separate runs); the Python result is within run-to-run
noise (separate runs ranged from −3% to +7%), because CPython serves small objects from its own allocator (pymalloc) and
only sends larger requests to `malloc`.

## Design

```
 4 MB segment (mmap)
 ├── 64 KB page ── [64 B header | obj | obj | obj | ... ]   size class 3 (64 B)
 ├── 64 KB page ── [64 B header | obj    | obj    | ... ]   size class 70 (3072 B)
 └── ...
```

- **Size classes.** 76 classes: 16-byte steps up to 1 KB, then 4 steps per power
  of two up to 8 KB. Worst-case internal waste is 15 bytes below 1 KB and 25% above.
- **No per-object header.** Pages are 64 KB-aligned, so `free(p)` finds the page
  header by clearing the low 16 bits of `p`. The header stores the size class and
  owning heap. This is why memory overhead is lower than glibc's.
- **Thread-local heaps.** Each thread owns a heap with a free list per size class.
  The fast path of `malloc` and `free` is a few instructions with no locks and no
  atomic read-modify-write operations.
- **Cross-thread frees.** If thread B frees an object owned by thread A, it is
  pushed onto A's per-class lock-free stack (one CAS). A drains the whole stack
  with a single atomic exchange when its local list runs empty. Push-only plus
  take-all avoids the ABA problem.
- **Heap reuse.** When a thread exits, its heap goes onto an "abandoned" list; the
  next new thread adopts it instead of mapping fresh memory.
- **Large blocks (> 8 KB).** Mapped directly with `mmap`, 64 KB-aligned so the
  same header lookup works. Freed blocks go into a per-heap best-fit cache
  (capped at 64 blocks / 32 MB). `realloc` grows them with `mremap`, moving page
  table entries instead of copying bytes.
- **Error detection.** Double frees and frees of pointers not owned by the
  allocator abort with a message (see limitations for the exact coverage).

## Build and test

Requires Linux, gcc, make. Tested on x86-64; arm64 should work but is untested.

```
make test        # 11 test groups: alignment, stress, calloc, realloc, memalign,
                 # 8 threads, cross-thread free, producer/consumer memory,
                 # thread churn, double-free / bad-pointer detection
make sanitize    # same tests under UndefinedBehaviorSanitizer
make tsan        # same tests under ThreadSanitizer (data races)
make bench       # all benchmarks above
make preload     # build/liboguzalloc.so
./scripts/real_world.sh
```

Continuous integration runs all of the above on every push
(`.github/workflows/ci.yml`).

## Bugs found by measuring

The first working version passed its unit tests but was not usable. Each of these
was found by a benchmark, fixed, and now has a regression test:

1. **Memory grew without bound in producer/consumer code** (612 MB instead of 2 MB).
   Objects freed on another thread went to that thread's list and were never
   returned. Fixed with per-heap remote free stacks.
2. **`realloc` growth was ~800× slower than glibc.** Large blocks were copied on
   every resize. Fixed with `mremap`.
3. **Every exited thread leaked its heap** (~4 KB RSS and a 4 MB address-space
   reservation). Fixed with heap adoption.
4. **Large blocks were 7× slower than glibc** because a 16-entry cache missed
   constantly. Fixed with a best-fit cache bounded by bytes instead of count.

## Limitations

- **Memory is not returned to the OS for small objects.** Pages stay with their
  heap after use, so RSS does not shrink after a peak. glibc can trim its heap.
- **A page holds only one size class.** A program that keeps one live object in
  many different classes pays up to 64 KB per class per thread.
- **Double-free detection is partial.** It catches a double free when the object
  is still in the current thread's local free list, and all double frees of large
  blocks still in the cache. A double free across threads is not detected.
- **Invalid-pointer detection reads the would-be page header.** If that address
  is not mapped, the program crashes with SIGSEGV instead of a clean message.
- **No hardening.** Free-list pointers are stored in plain form; this allocator
  is not designed to resist heap exploitation (glibc and hardened allocators
  encode or check these pointers).
- **Alignment of 64 KB or more** in `memalign`/`posix_memalign` is not supported
  and returns `NULL`/`ENOMEM`.
- **Linux only.** Uses `mmap`, `mremap` and initial-exec TLS.
- **Not compared with mimalloc, jemalloc or tcmalloc.** Only glibc was available
  in the test environment; those allocators use similar ideas and are expected to
  be competitive with or faster than this one.

## Layout

```
include/allocator.h     public API (my_malloc, my_free, my_calloc, ...)
src/allocator.c         the allocator
src/preload.c           malloc/free/... symbols for LD_PRELOAD
tests/test_allocator.c  correctness and regression tests
bench/                  benchmarks
scripts/real_world.sh   real-program correctness and timing
```

## License

MIT
