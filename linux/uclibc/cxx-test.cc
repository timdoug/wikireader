// SPDX-License-Identifier: GPL-2.0-only
/*
 * C++ on the C33: exceptions unwinding through saved registers and many
 * frames, RTTI, static construction, the standard library, and threads.
 * Prints "CXX PASS" or each failure and "CXX FAIL".
 */
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include <cstring>
#include <ctime>
#include <dlfcn.h>

static int failures;

#define CHECK(cond)                                                        \
	do {                                                               \
		if (!(cond)) {                                             \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				    #cond);                                \
			failures++;                                        \
		}                                                          \
	} while (0)

// Static construction: runs from .init_array before main.
static int constructed;
struct Global {
	int value;
	Global() : value(42) { constructed++; }
};
static Global global;

// RAII counter: every destructor between a throw and its catch must run.
static int live;
struct Guard {
	Guard() { live++; }
	~Guard() { live--; }
};

// Values the compiler keeps in the callee-saved %r0-%r3 across a call.  The
// middle frame holds its own values in the same registers, so the catch can
// only see its own if the unwinder restores them from the middle frame's
// pushn block.
static volatile int seed = 3;

__attribute__((noinline)) static void thrower(int depth)
{
	Guard g;
	if (depth == 0)
		throw std::runtime_error("deep");
	thrower(depth - 1);
}

__attribute__((noinline)) static int middle(int n)
{
	int a = seed * 13, b = seed * 17, c = seed * 19, d = seed * 23;
	thrower(n);
	return a + b + c + d;
}

__attribute__((noinline)) static int preserved(void)
{
	int a = seed * 2, b = seed * 5, c = seed * 7, d = seed * 11;
	int caught = 0;
	try {
		middle(40);
	} catch (const std::exception &e) {
		caught = std::string(e.what()) == "deep";
	}
	return caught && a == 6 && b == 15 && c == 21 && d == 33;
}

// A big frame between thrower and catcher, so the stack adjustment is more
// than a word or two.
__attribute__((noinline)) static int big_frame(int n)
{
	volatile char buffer[3000];
	buffer[n] = 1;
	thrower(2);
	return buffer[n];
}

struct Base {
	virtual ~Base() {}
	virtual int kind() const { return 1; }
};
struct Derived : Base {
	int kind() const override { return 2; }
};
struct Other {
	virtual ~Other() {}
};
struct Both : Derived, Other {};

struct Thrown : std::exception {
	int code;
	explicit Thrown(int c) : code(c) {}
	const char *what() const noexcept override { return "thrown"; }
};

static void exceptions(void)
{
	CHECK(preserved());
	CHECK(live == 0);

	try {
		big_frame(100);
		CHECK(false);
	} catch (const std::runtime_error &e) {
		CHECK(live == 0);
	}

	// Catch by base, rethrow, catch again.
	int stage = 0;
	try {
		try {
			throw Thrown(7);
		} catch (const std::exception &e) {
			stage = 1;
			throw;
		}
	} catch (const Thrown &t) {
		CHECK(stage == 1 && t.code == 7);
		stage = 2;
	}
	CHECK(stage == 2);

	// A scalar, and catch-all.
	try {
		throw 5;
	} catch (int i) {
		CHECK(i == 5);
	}
	try {
		throw 2.5;
	} catch (...) {
		stage = 3;
	}
	CHECK(stage == 3);

	// Thrown from inside libstdc++.
	try {
		std::stoi("not a number");
		CHECK(false);
	} catch (const std::invalid_argument &) {
	}
	try {
		(void)std::vector<int>().at(3);
		CHECK(false);
	} catch (const std::out_of_range &) {
	}

	// new fails on a no-MMU machine long before this size.  The asm uses
	// the pointer, or the compiler may drop an unused new[]/delete[] pair.
	try {
		std::unique_ptr<char[]> p(new char[size_t(1) << 30]);
		__asm__ volatile("" : : "r"(p.get()) : "memory");
		CHECK(false);
	} catch (const std::bad_alloc &) {
	}

	// exception_ptr carries one across a rethrow.
	std::exception_ptr saved;
	try {
		throw Thrown(9);
	} catch (...) {
		saved = std::current_exception();
	}
	try {
		std::rethrow_exception(saved);
	} catch (const Thrown &t) {
		CHECK(t.code == 9);
	}

	// Through a std::function and a lambda.
	std::function<void()> f = [] { throw Thrown(11); };
	try {
		f();
	} catch (const Thrown &t) {
		CHECK(t.code == 11);
	}
}

// Hidden from the optimiser, so the casts happen at run time.
__attribute__((noinline)) static Base *opaque(Base *p)
{
	__asm__("" : "+r"(p));
	return p;
}

static void rtti(void)
{
	Both both;
	Base *base = opaque(&both);
	CHECK(base->kind() == 2);
	CHECK(dynamic_cast<Derived *>(base) != nullptr);
	CHECK(dynamic_cast<Other *>(base) != nullptr);  // cross-cast
	Base plain;
	Base *other = opaque(&plain);
	CHECK(dynamic_cast<Derived *>(other) == nullptr);
	CHECK(typeid(*base) == typeid(Both));
	try {
		(void)dynamic_cast<Derived &>(*other);
		CHECK(false);
	} catch (const std::bad_cast &) {
	}
}

static void library(void)
{
	std::vector<std::string> words;
	for (int i = 0; i < 100; i++)
		words.push_back("w" + std::to_string(i));
	CHECK(words.size() == 100 && words[99] == "w99");

	std::map<std::string, int> counts;
	for (const auto &w : words)
		counts[w.substr(0, 2)]++;
	CHECK(counts["w1"] == 11);

	std::ostringstream out;
	out << "x=" << 42 << ' ' << std::hex << 255;
	CHECK(out.str() == "x=42 ff");

	auto shared = std::make_shared<int>(5);
	auto copy = shared;
	CHECK(shared.use_count() == 2 && *copy == 5);

	std::wstring wide = L"wide";
	CHECK(wide.size() == 4);
}

static std::atomic<int> tls_destroyed(0);
struct ThreadLocal {
	int value = 0;
	~ThreadLocal() { tls_destroyed++; }
};
static thread_local ThreadLocal per_thread;

static std::atomic<int> cancel_destroyed(0);
static sem_t cancel_ready, cancel_block;
struct CancelGuard {
	~CancelGuard() { cancel_destroyed++; }
};

static void *cancel_worker(void *)
{
	CancelGuard guard;
	sem_post(&cancel_ready);
	sem_wait(&cancel_block);
	return nullptr;
}

static void cancellation(void)
{
	CHECK(sem_init(&cancel_ready, 0, 0) == 0);
	CHECK(sem_init(&cancel_block, 0, 0) == 0);
	pthread_t thread;
	CHECK(pthread_create(&thread, nullptr, cancel_worker, nullptr) == 0);
	CHECK(sem_wait(&cancel_ready) == 0);
	CHECK(pthread_cancel(thread) == 0);
	void *result = nullptr;
	CHECK(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
	CHECK(cancel_destroyed == 1);
	sem_destroy(&cancel_ready);
	sem_destroy(&cancel_block);
}

static void reload_throw()
{
	throw Thrown(73);
}

static void reload_unwind()
{
	for (int i = 0; i < 8; i++) {
		void *module = dlopen("/mnt/sd/unwind.so", RTLD_NOW | RTLD_LOCAL);
		CHECK(module != nullptr);
		if (!module) return;
		auto call = reinterpret_cast<void (*)(void (*)(void))>(dlsym(module, "unwind_library"));
		CHECK(call != nullptr);
		if (call) {
			try { call(reload_throw); CHECK(false); }
			catch (const Thrown &e) { CHECK(e.code == 73); }
		}
		CHECK(dlclose(module) == 0);
	}
}

static void threads(void)
{
	std::mutex lock;
	long total = 0;
	std::atomic<int> started(0);
	std::atomic<std::uint64_t> sequence(UINT64_C(0xffffffff));
	CHECK(!sequence.is_lock_free());
	std::vector<std::thread> pool;
	for (int t = 0; t < 4; t++)
		pool.emplace_back([&, t] {
			int tls_errors = per_thread.value != 0;
			per_thread.value = t + 1;
			started++;
			// Each thread throws and catches its own exception, so the
			// per-thread exception state is exercised concurrently.
			for (int i = 0; i < 200; i++) {
				tls_errors += per_thread.value != t + 1;
				sequence.fetch_add(1, std::memory_order_relaxed);
				try {
					throw Thrown(i);
				} catch (const Thrown &e) {
					std::lock_guard<std::mutex> hold(lock);
					total += e.code;
				}
			}
			std::lock_guard<std::mutex> hold(lock);
			CHECK(tls_errors == 0);
		});
	for (auto &t : pool)
		t.join();
	CHECK(started == 4);
	CHECK(tls_destroyed == 4);
	CHECK(total == 4 * (199 * 200 / 2));
	CHECK(sequence.load() == UINT64_C(0xffffffff) + 800);
	std::uint64_t expected = sequence.load();
	CHECK(sequence.compare_exchange_strong(expected, UINT64_C(0x123456789abcdef0)));
	CHECK(sequence.exchange(0) == UINT64_C(0x123456789abcdef0));
}

static long long now()
{
	timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) std::abort();
	return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}

__attribute__((noinline)) static void bench_throw(int depth)
{
	Guard g;
	if (!depth) throw 7;
	bench_throw(depth - 1);
}

static void exception_bench()
{
	extern const char wr_throw_begin[], wr_throw_end[];
	std::printf("BENCH PC window: %p %p\n", wr_throw_begin, wr_throw_end);
	FILE *maps = std::fopen("/proc/self/maps", "r");
	if (!maps) std::abort();
	char line[256];
	while (std::fgets(line, sizeof(line), maps)) std::printf("BENCH MAP %s", line);
	std::fclose(maps);
	for (int depth : {0, 4, 16}) {
		if (depth == 0) {
			long long cold = now();
			try { bench_throw(depth); }
			catch (int value) { CHECK(value == 7 && live == 0); }
			std::printf("BENCH first throw: %lld ns\n", now() - cold);
		}
		long long start = now();
		if (depth == 4)
			__asm__ volatile(".global wr_throw_begin\nwr_throw_begin: nop" ::: "memory");
		for (int i = 0; i < 32; i++) {
			try { bench_throw(depth); }
			catch (int value) { CHECK(value == 7 && live == 0); }
		}
		if (depth == 4)
			__asm__ volatile(".global wr_throw_end\nwr_throw_end: nop" ::: "memory");
		std::printf("BENCH throw depth %d: %lld ns\n", depth, (now() - start) / 32);
	}
	long long start = now();
	for (int i = 0; i < 32; i++) {
		try { (void)std::vector<int>().at(3); CHECK(false); }
		catch (const std::out_of_range &) { }
	}
	std::printf("BENCH libstdc++ throw: %lld ns\n", (now() - start) / 32);
	std::puts(failures ? "BENCH FAIL" : "BENCH PASS");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !std::strcmp(argv[1], "--bench")) {
		exception_bench();
		return failures != 0;
	}
	CHECK(constructed == 1 && global.value == 42);
	exceptions();
	rtti();
	library();
	threads();
	reload_unwind();
	cancellation();
	std::cout << (failures ? "CXX FAIL" : "CXX PASS") << std::endl;
	return failures != 0;
}
