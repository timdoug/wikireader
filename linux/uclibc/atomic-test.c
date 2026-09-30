// SPDX-License-Identifier: GPL-2.0-only
/* Exercise C11, sized GCC and generic GCC atomics on the target. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define THREADS 4
#define ROUNDS 1000
#define BASE UINT64_C(0x12345678fffff800)

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("ATOMIC FAIL line %d: %s\n", __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static _Atomic uint64_t counter = BASE;
static _Atomic uint64_t cas_counter;
static uint64_t sums[THREADS];

/* An aggregate larger than the runtime's largest integer exercises the
 * generic locking/memcpy path, not just the sized 8-byte entry points.
 * Place it across the lock-table's page boundary to exercise wraparound. */
struct record { uint64_t count, inverse, check; };
static struct {
	unsigned char pad[4080];
	struct record value;
} storage __attribute__((aligned(4096)));
#define RECORD(n) ((struct record){ (n), ~(n), (n) ^ UINT64_C(0xdeadbeef12345678) })

static void *adder(void *arg)
{
	uintptr_t id = (uintptr_t)arg;
	uint64_t sum = 0;
	for (int i = 0; i < ROUNDS; i++) {
		sum += atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed) - BASE;
		uint64_t expected = atomic_load_explicit(&cas_counter, memory_order_relaxed);
		if ((i & 31) == 0)
			sched_yield();
		while (!atomic_compare_exchange_weak_explicit(&cas_counter, &expected,
				expected + 1, memory_order_acq_rel, memory_order_relaxed))
			;
		struct record seen, next;
		__atomic_load(&storage.value, &seen, __ATOMIC_ACQUIRE);
		do {
			if (seen.inverse != ~seen.count ||
			    seen.check != (seen.count ^ UINT64_C(0xdeadbeef12345678)))
				return (void *)1;
			next = RECORD(seen.count + 1);
		} while (!__atomic_compare_exchange(&storage.value, &seen, &next, 0,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	}
	/* pthread_join synchronizes this ordinary per-thread result. */
	sums[id] = sum;
	return NULL;
}

static void sized(void)
{
	uint64_t x = 0, expected;
	const uint64_t a = UINT64_C(0x12345678abcdef01);
	__atomic_store_n(&x, a, __ATOMIC_RELEASE);
	CHECK(__atomic_load_n(&x, __ATOMIC_ACQUIRE) == a);
	CHECK(__atomic_exchange_n(&x, 5, __ATOMIC_SEQ_CST) == a);
	expected = 6;
	CHECK(!__atomic_compare_exchange_n(&x, &expected, a, 0,
					 __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	CHECK(expected == 5 && x == 5);
	CHECK(__atomic_compare_exchange_n(&x, &expected, a, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	CHECK(x == a);
	CHECK(__atomic_fetch_add(&x, 3, __ATOMIC_RELAXED) == a);
	CHECK(__atomic_sub_fetch(&x, 3, __ATOMIC_SEQ_CST) == a);
	CHECK(__atomic_fetch_sub(&x, 3, __ATOMIC_ACQ_REL) == a);
	CHECK(__atomic_add_fetch(&x, 3, __ATOMIC_SEQ_CST) == a);
	CHECK(__atomic_fetch_and(&x, UINT64_C(0xffffffff), __ATOMIC_SEQ_CST) == a);
	CHECK(__atomic_or_fetch(&x, UINT64_C(0x100000000), __ATOMIC_SEQ_CST) ==
	      UINT64_C(0x1abcdef01));
	CHECK(__atomic_fetch_or(&x, UINT64_C(0x200000000), __ATOMIC_SEQ_CST) ==
	      UINT64_C(0x1abcdef01));
	CHECK(__atomic_and_fetch(&x, UINT64_C(0xffffffff), __ATOMIC_SEQ_CST) ==
	      UINT64_C(0xabcdef01));
	CHECK(__atomic_fetch_xor(&x, UINT64_MAX, __ATOMIC_SEQ_CST) == UINT64_C(0xabcdef01));
	CHECK(__atomic_xor_fetch(&x, UINT64_MAX, __ATOMIC_SEQ_CST) == UINT64_C(0xabcdef01));
	CHECK(__atomic_fetch_nand(&x, UINT64_MAX, __ATOMIC_SEQ_CST) == UINT64_C(0xabcdef01));
	CHECK(__atomic_nand_fetch(&x, UINT64_MAX, __ATOMIC_SEQ_CST) == UINT64_C(0xabcdef01));
	CHECK(!__atomic_is_lock_free(sizeof(x), &x));
	_Atomic uint32_t word = 0;
	CHECK(atomic_is_lock_free(&word));
	CHECK(atomic_fetch_add(&word, 1) == 0 && atomic_load(&word) == 1);
}

static void generic(void)
{
	struct record first = RECORD(7), second = RECORD(8), seen;
	__atomic_store(&storage.value, &first, __ATOMIC_RELEASE);
	__atomic_load(&storage.value, &seen, __ATOMIC_ACQUIRE);
	CHECK(memcmp(&seen, &first, sizeof(seen)) == 0);
	__atomic_exchange(&storage.value, &second, &seen, __ATOMIC_SEQ_CST);
	CHECK(memcmp(&seen, &first, sizeof(seen)) == 0);
	CHECK(!__atomic_compare_exchange(&storage.value, &seen, &first, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	CHECK(memcmp(&seen, &second, sizeof(seen)) == 0);
	CHECK(__atomic_compare_exchange(&storage.value, &seen, &first, 0,
				       __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
	CHECK(!__atomic_is_lock_free(sizeof(first), &storage.value));
	first = RECORD(0);
	__atomic_store(&storage.value, &first, __ATOMIC_SEQ_CST);
}

int main(void)
{
	sized();
	generic();
	pthread_t threads[THREADS];
	int created = 0;
	for (; created < THREADS; created++) {
		int ret = pthread_create(&threads[created], NULL, adder, (void *)(uintptr_t)created);
		CHECK(ret == 0);
		if (ret)
			break;
	}
	uint64_t sum = 0;
	for (int i = 0; i < created; i++) {
		void *result = (void *)1;
		CHECK(pthread_join(threads[i], &result) == 0);
		CHECK(result == NULL);
		sum += sums[i];
	}
	uint64_t total = THREADS * ROUNDS;
	CHECK(atomic_load(&counter) == BASE + total);
	CHECK(sum == total * (total - 1) / 2);
	CHECK(atomic_load(&cas_counter) == total);
	struct record seen, wanted = RECORD(total);
	__atomic_load(&storage.value, &seen, __ATOMIC_SEQ_CST);
	CHECK(memcmp(&seen, &wanted, sizeof(seen)) == 0);
	printf("ATOMIC %s: 64-bit and aggregate operations, %d threads, %d failed\n",
	       failures ? "FAIL" : "PASS", created, failures);
	return failures != 0;
}
