/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_MATH_H
#define _TCC_MATH_H
#define HUGE_VAL (__builtin_huge_val())
double ldexp(double, int);
long double ldexpl(long double, int);
#endif
