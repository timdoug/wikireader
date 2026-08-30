/* Declaration-only <math.h> for C33 compiler tests.  */

#ifndef C33_TEST_MATH_H
#define C33_TEST_MATH_H

#define HUGE_VAL  (__builtin_huge_val ())
#define HUGE_VALF (__builtin_huge_valf ())
#define HUGE_VALL (__builtin_huge_vall ())
#define INFINITY  (__builtin_inff ())
#define NAN       (__builtin_nanf (""))

#define FP_INFINITE  1
#define FP_NAN       2
#define FP_NORMAL    3
#define FP_SUBNORMAL 4
#define FP_ZERO      5

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

typedef float float_t;
typedef double double_t;

#define fpclassify(x) __builtin_fpclassify (FP_NAN, FP_INFINITE, FP_NORMAL, \
					    FP_SUBNORMAL, FP_ZERO, (x))
#define isfinite(x)   __builtin_isfinite (x)
#define isinf(x)      __builtin_isinf_sign (x)
#define isnan(x)      __builtin_isnan (x)
#define isnormal(x)   __builtin_isnormal (x)
#define signbit(x)    __builtin_signbit (x)
#define isgreater(x, y)      __builtin_isgreater ((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal ((x), (y))
#define isless(x, y)         __builtin_isless ((x), (y))
#define islessequal(x, y)    __builtin_islessequal ((x), (y))
#define islessgreater(x, y)  __builtin_islessgreater ((x), (y))
#define isunordered(x, y)    __builtin_isunordered ((x), (y))

#define C33_MATH_REAL(stem) \
  double stem (double);       \
  float stem##f (float);      \
  long double stem##l (long double)

C33_MATH_REAL (acos);
C33_MATH_REAL (acosh);
C33_MATH_REAL (asin);
C33_MATH_REAL (asinh);
C33_MATH_REAL (atan);
C33_MATH_REAL (atanh);
C33_MATH_REAL (cbrt);
C33_MATH_REAL (ceil);
C33_MATH_REAL (cos);
C33_MATH_REAL (cosh);
C33_MATH_REAL (erf);
C33_MATH_REAL (erfc);
C33_MATH_REAL (exp);
C33_MATH_REAL (exp2);
C33_MATH_REAL (expm1);
C33_MATH_REAL (fabs);
C33_MATH_REAL (floor);
C33_MATH_REAL (j0);
C33_MATH_REAL (j1);
C33_MATH_REAL (lgamma);
C33_MATH_REAL (log);
C33_MATH_REAL (log10);
C33_MATH_REAL (log1p);
C33_MATH_REAL (log2);
C33_MATH_REAL (logb);
C33_MATH_REAL (nearbyint);
C33_MATH_REAL (rint);
C33_MATH_REAL (round);
C33_MATH_REAL (sin);
C33_MATH_REAL (sinh);
C33_MATH_REAL (sqrt);
C33_MATH_REAL (tan);
C33_MATH_REAL (tanh);
C33_MATH_REAL (tgamma);
C33_MATH_REAL (trunc);
C33_MATH_REAL (y0);
C33_MATH_REAL (y1);

#define C33_MATH_BINARY(stem)     \
  double stem (double, double);   \
  float stem##f (float, float);   \
  long double stem##l (long double, long double)

C33_MATH_BINARY (atan2);
C33_MATH_BINARY (copysign);
C33_MATH_BINARY (fdim);
C33_MATH_BINARY (fmax);
C33_MATH_BINARY (fmin);
C33_MATH_BINARY (fmod);
C33_MATH_BINARY (hypot);
C33_MATH_BINARY (nextafter);
C33_MATH_BINARY (pow);
C33_MATH_BINARY (remainder);

double fma (double, double, double);
float fmaf (float, float, float);
long double fmal (long double, long double, long double);

double frexp (double, int *);
float frexpf (float, int *);
long double frexpl (long double, int *);
double ldexp (double, int);
float ldexpf (float, int);
long double ldexpl (long double, int);
double modf (double, double *);
float modff (float, float *);
long double modfl (long double, long double *);
double scalbn (double, int);
float scalbnf (float, int);
long double scalbnl (long double, int);

int ilogb (double);
int ilogbf (float);
int ilogbl (long double);
long lrint (double);
long lrintf (float);
long lrintl (long double);
long long llrint (double);
long long llrintf (float);
long long llrintl (long double);
long lround (double);
long lroundf (float);
long lroundl (long double);
long long llround (double);
long long llroundf (float);
long long llroundl (long double);

#undef C33_MATH_REAL
#undef C33_MATH_BINARY

#endif
