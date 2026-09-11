#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
struct Pair { int a, b; };
struct Big { int a, b, c; };
struct Odd { char a, b, c; };
static int global = 17;
static int bss[8];
static int *relocptr = &global;
static int six(int a,int b,int c,int d,int e,int f)
{ return a + 3*b + 5*c + 7*d + 11*e + 13*f; }
static struct Pair pair(struct Pair x)
{ x.a += 2; x.b *= 3; return x; }
static struct Big big(int x, struct Big b, int y)
{ b.a += x; b.b += y; return b; }
static int odd(struct Odd v, int x)
{ return v.a + v.b + v.c + x; }
static int sum(int count, ...)
{ va_list ap; int s=0; va_start(ap,count); while(count--) s+=va_arg(ap,int); va_end(ap); return s; }
static long long take(long long x) { return x + 3; }
static int factorial(int n)
{ return n < 2 ? 1 : n * factorial(n-1); }
static unsigned rng(unsigned *p)
{ *p = *p * 1664525U + 1013904223U; return *p; }
static int vla(int n)
{ int a[n]; int i; for(i=0;i<n;i++) a[i]=3*i; return a[n-1]; }
/* A frame past the C33 backend local base bias, so the slow address path
 * and the register it rebuilds are covered, including the accumulator whose
 * own slot is out there and whose 64-bit carry depends on those flags. */
static long long deep(int seed)
{
    int a[2600], b[8]; long long total = 0; int i;
    for(i=0;i<2600;i++) a[i]=seed+i*3;
    for(i=0;i<8;i++) b[i]=a[i]*2;
    for(i=0;i<2600;i++) total+=a[i];
    for(i=0;i<8;i++) total+=b[i];
    return total;
}
int main(void)
{
    volatile int a = -1234567, b = 37;
    volatile unsigned u = 0xfedcba98U, v = 97;
    volatile long long ll = 0x1ffffffffLL;
    volatile long long rr = 0x123456789LL;
    struct Pair p = {4, 5}; struct Big q = {1, 2, 3};
    struct Odd o = {1, 2, 3};
    struct Pair *pp = &p;
    volatile long long *lp = &ll;
    int (*fp)(int,int,int,int,int,int) = six;
    int i, j, ar[31], total = 0;
    struct { unsigned a:3, b:9; signed c:7; } bits;
    signed char ch=-120; unsigned short sh=60000;
    unsigned seed = 123, digest = 0;
    char *s;
    bits.a=5; bits.b=401; bits.c=-37;
    CHECK(bits.a==5 && bits.b==401 && bits.c==-37);
    CHECK(ch==-120 && sh==60000);
    i=0; CHECK((0 && ++i)==0 && i==0);
    CHECK((1 || ++i)==1 && i==0);
    CHECK((1 && ++i)==1 && i==1);
    switch(b) { case 0: return 1; case 37: i=9; break; default: return 1; }
    CHECK(i==9);
    for(i=0;i<10;i++) { if(i<3) continue; if(i==7) break; total+=i; }
    CHECK(total==18); total=0;
    CHECK(*relocptr == 17 && bss[4] == 0);
    CHECK(a / b == -33366 && a % b == -25);
    CHECK(u / v == 44081222U && u % v == 18U);
    CHECK((a >> 5) == -38581 && (u >> 29) == 7);
    CHECK((u << 17) == 0x75300000U);
    CHECK(a < b && b > a && a <= a && a >= a);
    CHECK(u > v && u >= v && !(u < v) && !(u <= v));
    CHECK((ll + rr) == 0x323456788LL);
    CHECK((ll - rr) == 0xdcba9876LL);
    CHECK((ll * rr) == 0x468acf10dcba9877LL);
    CHECK(ll / 19 == 452101820LL && ll % 19 == 11);
    CHECK((ll << 7) == 0xffffffff80LL);
    CHECK((ll >> 17) == 65535LL);
    CHECK(take(*lp) == 0x200000002LL);
    CHECK(fp(1,2,3,4,5,6) == 183);
    p=pair(*pp); CHECK(p.a==6 && p.b==15);
    q=big(7,q,11); CHECK(q.a==8 && q.b==13 && q.c==3);
    CHECK(odd(o,7)==13);
    CHECK(sum(6,1,2,3,4,5,6)==21);
    CHECK(factorial(8)==40320);
    CHECK(vla(37)==108);
    CHECK(deep(1)==10138884LL && deep(-7)==10117956LL);
    for(i=0;i<31;i++) ar[i]=(int)(rng(&seed)%1000);
    for(i=0;i<31;i++) for(j=i+1;j<31;j++)
        if(ar[j]<ar[i]) { int t=ar[j]; ar[j]=ar[i]; ar[i]=t; }
    for(i=0;i<31;i++) total+=ar[i];
    CHECK(total==19829);
    for(i=0;i<1000;i++) {
        unsigned x=rng(&seed), y=rng(&seed)|1;
        digest = digest * 33U + x/y + x%y;
        digest ^= (x & y) + (x | y) + (x ^ y);
        digest += (x > y) + (x <= y) * 3;
    }
    s=malloc(32); CHECK(s!=0);
    strcpy(s,"compiler"); memmove(s+1,s,8); s[9]=0;
    CHECK(strcmp(s,"ccompiler")==0); free(s);
    printf("C33_REGRESSION_OK %u\n",digest);
    return 0;
}
