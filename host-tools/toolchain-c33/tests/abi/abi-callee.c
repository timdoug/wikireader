/*
 * The callee half of the ABI cross-link test.  See run-abi.sh.
 *
 * Every function here is deliberately *not* inlinable across the link, so
 * each call has to go through the real calling convention: arguments in
 * %r6-%r9 then the stack, %r4/%r5 for return values, %r0-%r3 preserved,
 * and a hidden pointer for aggregates over 8 bytes.
 *
 * Only fixed-width 32-bit-and-below scalars plus long long and double,
 * because those are what the convention actually distinguishes between.
 */

typedef unsigned int u32;

struct S8  { int a, b; };
struct S12 { int a, b, c; };
struct S16 { int a, b, c, d; };

/* --- integer arguments, spilling past %r9 onto the stack --- */
u32 c_args6 (u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
{
	return a + 2*b + 3*c + 4*d + 5*e + 6*f;
}

u32 c_args10 (u32 a, u32 b, u32 c, u32 d, u32 e,
	      u32 f, u32 g, u32 h, u32 i, u32 j)
{
	return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j;
}

/* --- narrow types: who promotes, and does the callee re-narrow? --- */
u32 c_char (char a, char b, char c)     { return (u32)(a + b + c); }
u32 c_short (short a, short b, short c) { return (u32)(a + b + c); }
u32 c_uchar (unsigned char a, unsigned char b) { return (u32)(a + b); }

/* --- 64-bit: register pairing and alignment --- */
unsigned long long c_ll (unsigned long long a, unsigned long long b)
{
	return a * 3 + b;
}

u32 c_ll_mixed (u32 a, unsigned long long b, u32 c)
{
	return a + (u32)b + (u32)(b >> 32) + c;
}

/*
 * The 64-bit value lands in the last argument slot and straddles the end of
 * the argument registers.  The original compiler does not split it: it uses
 * %r9 and %r10, one register past the documented set, and the callee reads
 * it from there.  A fifth *scalar* argument still goes on the stack, so
 * %r10 is not simply a fifth argument register -- this shape only.
 */
u32 c_ll_straddle (u32 a, u32 b, u32 c, unsigned long long d)
{
	return a + 2*b + 3*c + (u32)d + (u32)(d >> 32);
}

/* --- floating point, which is soft on this target --- */
double c_double (double a, double b) { return a * 2.0 + b; }
float  c_float  (float a, float b)   { return a * 2.0f + b; }

/* --- aggregates: <=8 bytes in registers, larger via a hidden pointer --- */
struct S8  c_s8  (struct S8 s)  { s.a += 1; s.b += 2; return s; }
struct S12 c_s12 (struct S12 s) { s.a += 1; s.b += 2; s.c += 3; return s; }
struct S16 c_s16 (struct S16 s) { s.a += 1; s.b += 2; s.c += 3; s.d += 4; return s; }

u32 c_s16_by_value (struct S16 s, u32 x)
{
	return s.a + 2*s.b + 3*s.c + 4*s.d + 5*x;
}

/* --- varargs --- */
u32 c_varargs (u32 n, ...)
{
	__builtin_va_list ap;
	u32 sum = 0, i;

	__builtin_va_start (ap, n);
	for (i = 0; i < n; i++)
		sum += (i + 1) * __builtin_va_arg (ap, u32);
	__builtin_va_end (ap);
	return sum;
}

u32 c_varargs_d (u32 n, ...)
{
	__builtin_va_list ap;
	double sum = 0;
	u32 i;

	__builtin_va_start (ap, n);
	for (i = 0; i < n; i++)
		sum += __builtin_va_arg (ap, double);
	__builtin_va_end (ap);
	return (u32) sum;
}

/*
 * Callee-saved set.  The caller puts known values in %r0-%r3 before the
 * call and checks them after; this function must clobber enough registers
 * to force real saves, or it proves nothing.
 */
u32 c_clobber (u32 a, u32 b, u32 c, u32 d)
{
	u32 t0 = a ^ 0x11111111u, t1 = b ^ 0x22222222u;
	u32 t2 = c ^ 0x33333333u, t3 = d ^ 0x44444444u;
	u32 t4 = t0 + t1, t5 = t2 + t3, t6 = t4 ^ t5, t7 = t4 + t5;
	u32 t8 = t6 * 3, t9 = t7 * 5, ta = t8 ^ t9, tb = t8 + t9;
	return ta + tb + t0 + t1 + t2 + t3;
}

/* A function the *caller* half calls back into, so the convention is
   exercised in both directions across the same link.  */
extern u32 back (u32 x);

u32 c_callback (u32 x)
{
	return back (x + 1) * 2;
}
