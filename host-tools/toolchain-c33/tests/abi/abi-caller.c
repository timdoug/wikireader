/*
 * The caller half of the ABI cross-link test.  See run-abi.sh.
 *
 * Prints one 8-hex-digit line per check, in the same format difftest uses,
 * so two builds are compared by diffing their output.
 */

#include "dt.h"

struct S8  { int a, b; };
struct S12 { int a, b, c; };
struct S16 { int a, b, c, d; };
struct S3  { char a, b, c; };
struct S4  { int a; };
struct S5  { char a[5]; };

extern u32 c_args6 (u32, u32, u32, u32, u32, u32);
extern u32 c_args10 (u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
extern u32 c_char (char, char, char);
extern u32 c_short (short, short, short);
extern u32 c_uchar (unsigned char, unsigned char);
extern unsigned long long c_ll (unsigned long long, unsigned long long);
extern u32 c_ll_mixed (u32, unsigned long long, u32);
extern u32 c_ll_straddle (u32, u32, u32, unsigned long long);
extern double c_double (double, double);
extern float c_float (float, float);
extern struct S8 c_s8 (struct S8);
extern struct S12 c_s12 (struct S12);
extern struct S16 c_s16 (struct S16);
extern u32 c_s16_by_value (struct S16, u32);
extern u32 c_d_straddle (u32, u32, u32, double, u32);
extern u32 c_d_fits (u32, u32, double, u32);
extern u32 c_s12_mid (u32, struct S12, u32);
extern u32 c_s16_first (struct S16, u32, u32);
extern u32 c_s3 (u32, struct S3, u32);
extern u32 c_s4 (u32, struct S4, u32);
extern u32 c_s5 (u32, struct S5, u32);
extern u32 c_varargs (u32, ...);
extern u32 c_varargs_d (u32, ...);
extern u32 c_clobber (u32, u32, u32, u32);
extern u32 c_callback (u32);

/* Called back from the callee half, so the convention is exercised in both
   directions across the same link.  */
u32 back (u32 x) { return x * 7 + 1; }

/* volatile so nothing is constant-folded across the call boundary -- the
   point is to make a real call, not to compute the answer at compile time. */
static volatile u32 one = 1;

int main (void)
{
	u32 k = one;

	emit (c_args6 (k, 10*k, 100*k, 1000*k, 10000*k, 100000*k));
	emit (c_args10 (k, 2*k, 3*k, 4*k, 5*k, 6*k, 7*k, 8*k, 9*k, 10*k));

	emit (c_char ((char)(100*k), (char)(-50*k), (char)(7*k)));
	emit (c_short ((short)(30000*k), (short)(-20000*k), (short)(123*k)));
	emit (c_uchar ((unsigned char)(200*k), (unsigned char)(100*k)));

	{
		unsigned long long a = 0x0123456789abcdefull * k;
		unsigned long long b = 0xfedcba9876543210ull;
		unsigned long long r = c_ll (a, b);
		emit ((u32) r);
		emit ((u32) (r >> 32));
	}
	emit (c_ll_mixed (k, 0x1122334455667788ull, 9*k));
	emit (c_ll_straddle (k, 2*k, 3*k, 0x99aabbccddeeff00ull));
	emit (c_d_straddle (k, 2*k, 3*k, 6.25, 7*k));
	emit (c_d_fits (k, 2*k, 6.25, 7*k));

	{
		double d = c_double (1.5 * k, 2.25);
		float  f = c_float (1.5f * k, 2.25f);
		emit ((u32) (d * 256.0));
		emit ((u32) (f * 256.0f));
	}

	{
		struct S8 s = { (int)k, 2*(int)k };
		struct S8 r = c_s8 (s);
		emit ((u32) r.a); emit ((u32) r.b);
	}
	{
		struct S12 s = { (int)k, 2*(int)k, 3*(int)k };
		struct S12 r = c_s12 (s);
		emit ((u32) r.a); emit ((u32) r.b); emit ((u32) r.c);
	}
	{
		struct S16 s = { (int)k, 2*(int)k, 3*(int)k, 4*(int)k };
		struct S16 r = c_s16 (s);
		emit ((u32) r.a); emit ((u32) r.b); emit ((u32) r.c); emit ((u32) r.d);
		emit (c_s16_by_value (s, 5*k));
		emit (c_s16_first (s, 6*k, 7*k));
	}
	{
		struct S12 s = { (int)k, 2*(int)k, 3*(int)k };
		emit (c_s12_mid (9*k, s, 11*k));
	}
	{
		struct S3 s3 = { (char)k, (char)(2*k), (char)(3*k) };
		struct S4 s4 = { 4*(int)k };
		struct S5 s5 = { { (char)k, 0, 0, 0, (char)(5*k) } };
		emit (c_s3 (7*k, s3, 8*k));
		emit (c_s4 (7*k, s4, 8*k));
		emit (c_s5 (7*k, s5, 8*k));
	}

	emit (c_varargs (4*k, 10*k, 20*k, 30*k, 40*k));
	emit (c_varargs_d (3*k, 1.5, 2.25, 4.125));

	/*
	 * Callee-saved set.  Hold four values across a call the compiler
	 * cannot see through; if the two toolchains disagree about which
	 * registers survive a call, these come back wrong.
	 */
	{
		u32 a = 0xa5a5a5a5u ^ k, b = 0x5a5a5a5au ^ k;
		u32 c = 0xdeadbeefu ^ k, d = 0xfeedfaceu ^ k;
		u32 r = c_clobber (k, 2*k, 3*k, 4*k);
		emit (r);
		emit (a); emit (b); emit (c); emit (d);
	}

	emit (c_callback (5*k));
	return 0;
}
