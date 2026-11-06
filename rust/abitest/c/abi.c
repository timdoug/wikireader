/*
 * abi.c - the C half of the Rust/C calling-convention test.
 *
 * Every value is derived from a seed by field_*() and hashed by mix(), the
 * same way src/main.rs does it, so a value that crosses the boundary in the
 * wrong place shows up as a different hash.  The struct shapes are chosen
 * around GCC's rule for this target (ABI.md): a record of 1, 2, 4 or 8
 * bytes aligned to its mode travels in registers; anything else is BLKmode,
 * returned through a hidden pointer and passed by value on the stack.
 */

#include <stdarg.h>
#include <stdint.h>

#include <grifo.h>

#define MIX(h, v) ((uint32_t)(h) * 131u + (uint32_t)(v))

static uint32_t field(uint32_t seed, uint32_t k)
{
	return seed * 2654435761u ^ k * 40503u;
}

static float field_f32(uint32_t seed, uint32_t k)
{
	return (float)(field(seed, k) & 0xffff) + 0.5f;
}

static double field_f64(uint32_t seed, uint32_t k)
{
	return (double)(field(seed, k) & 0xfffff) + 0.25;
}

static uint32_t mix_f32(uint32_t h, float f)
{
	union { float f; uint32_t u; } v = { .f = f };
	return MIX(h, v.u);
}

static uint32_t mix_f64(uint32_t h, double d)
{
	union { double d; uint32_t u[2]; } v = { .d = d };
	return MIX(MIX(h, v.u[0]), v.u[1]);
}

typedef struct { uint8_t a; } S1;
typedef struct { uint16_t a; } S2;
typedef struct { uint8_t a, b; } S2b;
typedef struct { uint8_t a, b, c; } S3;
typedef struct { uint32_t a; } S4;
typedef struct { uint16_t a, b; } S4b;
typedef struct { uint8_t a[5]; } S5;
typedef struct { uint32_t a, b; } S8;
typedef struct { uint32_t a; uint8_t b; } S8b;
typedef struct { uint16_t a, b, c, d; } S8c;
typedef struct { uint32_t a, b, c; } S12;
typedef struct { uint32_t a, b, c, d; } S16;
typedef struct { float a; } SF;
typedef struct { float a, b; } SFF;
typedef struct { double a; } SD;

#define U(n) s.n = field(seed, ++k)
static S1 make_S1(uint32_t seed) { S1 s; uint32_t k = 0; U(a); return s; }
static S2 make_S2(uint32_t seed) { S2 s; uint32_t k = 0; U(a); return s; }
static S2b make_S2b(uint32_t seed) { S2b s; uint32_t k = 0; U(a); U(b); return s; }
static S3 make_S3(uint32_t seed) { S3 s; uint32_t k = 0; U(a); U(b); U(c); return s; }
static S4 make_S4(uint32_t seed) { S4 s; uint32_t k = 0; U(a); return s; }
static S4b make_S4b(uint32_t seed) { S4b s; uint32_t k = 0; U(a); U(b); return s; }
static S5 make_S5(uint32_t seed)
{
	S5 s; uint32_t k = 0;
	for (int i = 0; i < 5; ++i)
		s.a[i] = field(seed, ++k);
	return s;
}
static S8 make_S8(uint32_t seed) { S8 s; uint32_t k = 0; U(a); U(b); return s; }
static S8b make_S8b(uint32_t seed) { S8b s; uint32_t k = 0; U(a); U(b); return s; }
static S8c make_S8c(uint32_t seed) { S8c s; uint32_t k = 0; U(a); U(b); U(c); U(d); return s; }
static S12 make_S12(uint32_t seed) { S12 s; uint32_t k = 0; U(a); U(b); U(c); return s; }
static S16 make_S16(uint32_t seed) { S16 s; uint32_t k = 0; U(a); U(b); U(c); U(d); return s; }
static SF make_SF(uint32_t seed) { SF s; s.a = field_f32(seed, 1); return s; }
static SFF make_SFF(uint32_t seed) { SFF s; s.a = field_f32(seed, 1); s.b = field_f32(seed, 2); return s; }
static SD make_SD(uint32_t seed) { SD s; s.a = field_f64(seed, 1); return s; }
#undef U

#define M(n) h = MIX(h, s->n)
static uint32_t mix_S1(const S1 *s) { uint32_t h = 0; M(a); return h; }
static uint32_t mix_S2(const S2 *s) { uint32_t h = 0; M(a); return h; }
static uint32_t mix_S2b(const S2b *s) { uint32_t h = 0; M(a); M(b); return h; }
static uint32_t mix_S3(const S3 *s) { uint32_t h = 0; M(a); M(b); M(c); return h; }
static uint32_t mix_S4(const S4 *s) { uint32_t h = 0; M(a); return h; }
static uint32_t mix_S4b(const S4b *s) { uint32_t h = 0; M(a); M(b); return h; }
static uint32_t mix_S5(const S5 *s)
{
	uint32_t h = 0;
	for (int i = 0; i < 5; ++i)
		h = MIX(h, s->a[i]);
	return h;
}
static uint32_t mix_S8(const S8 *s) { uint32_t h = 0; M(a); M(b); return h; }
static uint32_t mix_S8b(const S8b *s) { uint32_t h = 0; M(a); M(b); return h; }
static uint32_t mix_S8c(const S8c *s) { uint32_t h = 0; M(a); M(b); M(c); M(d); return h; }
static uint32_t mix_S12(const S12 *s) { uint32_t h = 0; M(a); M(b); M(c); return h; }
static uint32_t mix_S16(const S16 *s) { uint32_t h = 0; M(a); M(b); M(c); M(d); return h; }
static uint32_t mix_SF(const SF *s) { return mix_f32(0, s->a); }
static uint32_t mix_SFF(const SFF *s) { return mix_f32(mix_f32(0, s->a), s->b); }
static uint32_t mix_SD(const SD *s) { return mix_f64(0, s->a); }
#undef M

#define TYPES(X) X(S1) X(S2) X(S2b) X(S3) X(S4) X(S4b) X(S5) X(S8) X(S8b) \
	X(S8c) X(S12) X(S16) X(SF) X(SFF) X(SD)

/* What Rust calls: the struct between two words, the struct fourth after
   three words (where an 8-byte register value would run into %r10), and
   the struct as a return value. */
#define C_SIDE(T) \
	uint32_t c_take_##T(uint32_t x, T s, uint32_t y) \
	{ return MIX(MIX(MIX(0, x), mix_##T(&s)), y); } \
	uint32_t c_take4_##T(uint32_t a, uint32_t b, uint32_t c, T s, uint32_t y) \
	{ return MIX(MIX(MIX(MIX(MIX(0, a), b), c), mix_##T(&s)), y); } \
	T c_ret_##T(uint32_t seed) { return make_##T(seed); }
TYPES(C_SIDE)

/* What C calls in Rust: the same three shapes. */
#define RUST_DECL(T) \
	uint32_t rust_take_##T(uint32_t x, T s, uint32_t y); \
	uint32_t rust_take4_##T(uint32_t a, uint32_t b, uint32_t c, T s, uint32_t y); \
	T rust_ret_##T(uint32_t seed);
TYPES(RUST_DECL)

/* Scalars: promotion of narrow types, 64-bit pairs with no even-pair
   alignment, soft floats, and the DFmode-past-slot-3 rule. */
uint32_t c_scalars(int8_t a, uint8_t b, int16_t c, uint16_t d, int64_t e,
		   double f, float g, uint32_t h)
{
	uint32_t m = MIX(MIX(MIX(MIX(0, a), b), c), d);
	m = MIX(MIX(m, (uint32_t)e), (uint32_t)(e >> 32));
	return MIX(mix_f32(mix_f64(m, f), g), h);
}
uint32_t c_d4(uint32_t a, uint32_t b, uint32_t c, double d, uint32_t e)
{ return MIX(mix_f64(MIX(MIX(MIX(0, a), b), c), d), e); }
uint32_t c_l4(uint32_t a, uint32_t b, uint32_t c, int64_t d, uint32_t e)
{ return MIX(MIX(MIX(MIX(MIX(MIX(0, a), b), c), (uint32_t)d), (uint32_t)(d >> 32)), e); }
int8_t c_ret_i8(void) { return -5; }
uint8_t c_ret_u8(void) { return 250; }
int16_t c_ret_i16(void) { return -300; }
int64_t c_ret_i64(void) { return -0x123456789abcLL; }
double c_ret_f64(void) { return -1234.5; }
float c_ret_f32(void) { return 3.25f; }

/* Variadic: every anonymous argument is on the stack. */
uint32_t c_va_ints(int n, ...)
{
	va_list ap;
	uint32_t h = 0;
	va_start(ap, n);
	while (n-- > 0)
		h = MIX(h, va_arg(ap, uint32_t));
	va_end(ap);
	return h;
}
uint32_t c_va_mixed(int tag, ...)
{
	va_list ap;
	va_start(ap, tag);
	int64_t l = va_arg(ap, int64_t);
	double d = va_arg(ap, double);
	int i = va_arg(ap, int);
	va_end(ap);
	return MIX(mix_f64(MIX(MIX(MIX(0, tag), (uint32_t)l), (uint32_t)(l >> 32)), d), i);
}

int8_t rust_ret_i8(void);
int16_t rust_ret_i16(void);
int64_t rust_ret_i64(void);
double rust_ret_f64(void);
uint32_t rust_scalars(int8_t a, uint8_t b, int16_t c, uint16_t d, int64_t e,
		      double f, float g, uint32_t h);
uint32_t rust_d4(uint32_t a, uint32_t b, uint32_t c, double d, uint32_t e);

/* Call every Rust function and check what comes back against C's own
   answer; returns the number of failures, each one printed. */
static int checks, failures;

static void check(const char *what, uint32_t got, uint32_t want)
{
	++checks;
	if (got != want) {
		++failures;
		debug_printf("abi: C->Rust %s: got %08lx want %08lx\n", what,
			     (unsigned long)got, (unsigned long)want);
	}
}

int c_call_rust(int *count)
{
	const uint32_t x = 0x11223344, y = 0x55667788, seed = 77;
#define CALL(T) { \
	T s = make_##T(seed); \
	check("take " #T, rust_take_##T(x, s, y), c_take_##T(x, s, y)); \
	check("take4 " #T, rust_take4_##T(1, 2, 3, s, y), c_take4_##T(1, 2, 3, s, y)); \
	T r = rust_ret_##T(seed + 1); \
	T w = make_##T(seed + 1); \
	check("ret " #T, mix_##T(&r), mix_##T(&w)); }
	TYPES(CALL)
#undef CALL
	check("ret i8", (uint32_t)(int32_t)rust_ret_i8(), (uint32_t)-5);
	check("ret i16", (uint32_t)(int32_t)rust_ret_i16(), (uint32_t)-300);
	int64_t l = rust_ret_i64();
	check("ret i64", (uint32_t)(l >> 32) ^ (uint32_t)l,
	      (uint32_t)(-0x123456789abcLL >> 32) ^ (uint32_t)-0x123456789abcLL);
	check("ret f64", mix_f64(0, rust_ret_f64()), mix_f64(0, -1234.5));
	check("scalars", rust_scalars(-7, 200, -1000, 60000, -0x1122334455LL, 2.5, -0.75f, 9),
	      c_scalars(-7, 200, -1000, 60000, -0x1122334455LL, 2.5, -0.75f, 9));
	check("d4", rust_d4(1, 2, 3, 6.125, 4), c_d4(1, 2, 3, 6.125, 4));
	*count = checks;
	return failures;
}
