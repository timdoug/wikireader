/* SPDX-License-Identifier: LGPL-2.0-or-later */
#include "abi.h"
#include <stdarg.h>
struct S1 f1(struct S1 a) { a.x+=1; return a; }
struct S2 f2(struct S2 a) { a.x-=9; return a; }
struct S3 f3(int n,struct S3 a,int k)
{ a.x[0]+=n; a.x[1]+=k; return a; }
struct S4 f4(struct S4 a) { a.x*=7; return a; }
struct S5 f5(struct S5 a,int n) { a.x[4]+=n; return a; }
struct S8 f8(struct S8 a) { a.x+=7; a.y-=4; return a; }
struct S12 f12(int n,struct S12 a,int k)
{ a.x+=n; a.y+=k; return a; }
long long spill(int a,int b,int c,long long d,int e,long long f)
{ return d+f+a+b+c+e; }
long long unaligned_pair(int a,long long b,int c) { return a+b+c; }
int variadic(int scale,int count,...)
{ va_list ap; int s=0; va_start(ap,count); while(count--) s+=va_arg(ap,int); va_end(ap); return scale*s; }
long long varll(int count,...)
{ va_list ap; long long s=0; va_start(ap,count); while(count--) s+=va_arg(ap,long long); va_end(ap); return s; }
int callback(int (*fn)(int),int n) { return fn(n)+1; }
struct S12 varbig(int n,...)
{ struct S12 s; va_list ap; va_start(ap,n); s.x=n; s.y=va_arg(ap,int); s.z=va_arg(ap,int); va_end(ap); return s; }
float fsingle(float a,int n,float b) { return -(a*b) + n/2.0f; }
double fdouble(int n,double a,double b,int k) { return a/b + n*k; }
double fvar(int n,...)
{ va_list ap; double s=0; va_start(ap,n); while(n--) s+=va_arg(ap,double); va_end(ap); return s; }
unsigned long long fconvert(double x) { return (unsigned long long)x; }
int arrayvar(int n,...)
{ va_list ap; const char *s; int x; va_start(ap,n); s=va_arg(ap,const char*); x=va_arg(ap,int); va_end(ap); return n+s[0]+x; }
