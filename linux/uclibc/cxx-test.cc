// SPDX-License-Identifier: GPL-2.0-only
/*
 * C++ on the C33: exceptions unwinding through saved registers and many
 * frames, RTTI, static construction, the standard library, and threads.
 * Prints "CXX PASS" or each failure and "CXX FAIL".
 */
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

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

static void threads(void)
{
	std::mutex lock;
	long total = 0;
	std::atomic<int> started(0);
	std::vector<std::thread> pool;
	for (int t = 0; t < 4; t++)
		pool.emplace_back([&] {
			started++;
			// Each thread throws and catches its own exception, so the
			// per-thread exception state is exercised concurrently.
			for (int i = 0; i < 200; i++) {
				try {
					throw Thrown(i);
				} catch (const Thrown &e) {
					std::lock_guard<std::mutex> hold(lock);
					total += e.code;
				}
			}
		});
	for (auto &t : pool)
		t.join();
	CHECK(started == 4);
	CHECK(total == 4 * (199 * 200 / 2));
}

int main()
{
	CHECK(constructed == 1 && global.value == 42);
	exceptions();
	rtti();
	library();
	threads();
	std::cout << (failures ? "CXX FAIL" : "CXX PASS") << std::endl;
	return failures != 0;
}
