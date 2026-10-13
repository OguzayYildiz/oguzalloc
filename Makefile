CC      = gcc
CFLAGS  = -O2 -std=gnu11 -Wall -Wextra -Iinclude
LDLIBS  = -lpthread

SRC = src/allocator.c

.PHONY: all test sanitize tsan bench preload clean

all: test bench preload

build/test_allocator: tests/test_allocator.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_allocator.c $(SRC) $(LDLIBS)

test: build/test_allocator
	./build/test_allocator

# UndefinedBehaviorSanitizer: finds undefined behavior (overflow, misaligned access, ...).
sanitize:
	@mkdir -p build
	$(CC) -O1 -g -std=gnu11 -Iinclude -fsanitize=undefined -fno-sanitize-recover \
	    -o build/test_ubsan tests/test_allocator.c $(SRC) $(LDLIBS)
	./build/test_ubsan

# ThreadSanitizer: finds data races between threads.
tsan:
	@mkdir -p build
	$(CC) -O1 -g -std=gnu11 -Iinclude -fsanitize=thread \
	    -o build/test_tsan tests/test_allocator.c $(SRC) $(LDLIBS)
	./build/test_tsan

build/bench: bench/bench.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(SRC) $(LDLIBS)

build/prodcons: bench/prodcons.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ bench/prodcons.c $(SRC) $(LDLIBS)

build/realloc_growth: bench/realloc_growth.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ bench/realloc_growth.c $(SRC) $(LDLIBS)

build/thread_churn: bench/thread_churn.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ bench/thread_churn.c $(SRC) $(LDLIBS)

build/memtest: bench/memtest.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ bench/memtest.c $(SRC) $(LDLIBS)

bench: build/bench build/memtest build/prodcons build/realloc_growth build/thread_churn
	./build/bench
	@echo
	@echo "Memory usage (1,000,000 live objects, 16-256 bytes):"
	@./build/memtest sys
	@./build/memtest my
	@echo
	@echo "Producer/consumer (A allocates, B frees, 10M objects):"
	@./build/prodcons sys
	@./build/prodcons my
	@echo
	@echo "Buffer grown with realloc (16 B -> 64 MB, 20 repetitions):"
	@./build/realloc_growth
	@echo
	@echo "Thread churn (2000 threads created and destroyed):"
	@./build/thread_churn sys
	@./build/thread_churn my

build/liboguzalloc.so: src/preload.c $(SRC) include/allocator.h
	@mkdir -p build
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Iinclude -fPIC -shared -fno-builtin \
	    -o $@ src/preload.c $(SRC)

preload: build/liboguzalloc.so

clean:
	rm -rf build
