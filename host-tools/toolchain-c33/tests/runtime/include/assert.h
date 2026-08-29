/*
 * <assert.h> for the C33 DejaGnu board only.
 *
 * mini-libc has no assert(), and the firmware does not want one -- it has
 * fatal_error() instead.  But a couple of dozen gcc.c-torture tests include
 * this header, and without it they fail to compile, which the harness would
 * otherwise report as a backend failure.  So it lives here, on the harness's
 * own include path, and mini-libc stays as it is.
 *
 * A failed assertion has to land where the harness is looking: abort() in
 * tests/runtime/crt0.s reports 0xdead in %r4, the same signal a torture
 * test's own abort() gives.
 */

#ifndef ASSERT_H
#define ASSERT_H

void abort(void) __attribute__((noreturn));

#undef assert

#ifdef NDEBUG
#define assert(e) ((void) 0)
#else
#define assert(e) ((e) ? (void) 0 : abort ())
#endif

#endif
