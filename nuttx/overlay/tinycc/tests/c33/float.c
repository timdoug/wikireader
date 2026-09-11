/* SPDX-License-Identifier: LGPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
#define C(x) do { if (!(x)) { printf("C33_FLOAT_FAIL %d\n", __LINE__); return 1; } } while (0)
static double calc(double a, double b, double c) { return a*b/c + a-b; }
static float single(float x) { return x*x - 0.5f; }
static double id(double x) { return x; }
static unsigned long long to_unsigned(double x) { return (unsigned long long)x; }
int main(void)
{
    volatile double a=1.5, b=2.0, zero=0;
    volatile float f=1.25f;
    volatile unsigned u=4000000000U;
    volatile long long ll=-0x123456789LL;
    volatile unsigned long long ull=0xf000000000000000ULL;
    double nan=zero/zero, inf=1.0/zero;
    double vals[4]={1.5,-2.25,4.0,8.5};
    double *p=vals;
    C(a+b==3.5 && a-b==-0.5 && a*b==3.0 && a/b==0.75);
    C(-a==-1.5 && -f==-1.25f);
    C(a<b && a<=b && !(a>b) && !(a>=b) && a!=b && !(a==b));
    C(a<=a && a>=a && a==a && !(a!=a));
    C(nan!=nan && !(nan==nan) && !(nan<zero) && !(nan<=zero));
    C(!(nan>zero) && !(nan>=zero) && inf>b && -inf<a);
    C(calc(a,b,4.0)==0.25 && single(f)==1.0625f);
    C((float)a==1.5f && (double)f==1.25 && (int)-a==-1);
    C((unsigned)(double)u==u && (long long)(double)ll==ll);
    C(to_unsigned((double)ull)==ull && (unsigned long long)(float)ull==ull);
    C((float)ll < -4000000000.0f && (double)(long double)a==a);
    p[1]=id(p[0]+p[2]); C(p[1]==5.5);
    C(id(a)+id(b)+id(a*b)+id(a/b)==7.25);
    { union { double d; uint64_t u; } bits; bits.d=-zero; C(bits.u==0x8000000000000000ULL); }
    puts("C33_FLOAT_OK");
    return 0;
}
