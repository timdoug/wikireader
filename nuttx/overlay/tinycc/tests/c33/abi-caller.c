/* SPDX-License-Identifier: LGPL-2.0-or-later */
#include "abi.h"
#define C(x) do { if (!(x)) return fail(__LINE__); } while(0)
static int twice(int n) { return 2*n; }
int main(void)
{
    struct S1 a={-7}; struct S2 b={-1234}; struct S3 c={{1,2,3}};
    struct S4 d={-13}; struct S5 e={{1,2,3,4,5}};
    struct S8 f={-10,20}; struct S12 g={1,2,3};
    a=f1(a); C(a.x==-6);
    b=f2(b); C(b.x==-1243);
    c=f3(7,c,9); C(c.x[0]==8 && c.x[1]==11 && c.x[2]==3);
    d=f4(d); C(d.x==-91);
    e=f5(e,10); C(e.x[0]==1 && e.x[4]==15);
    f=f8(f); C(f.x==-3 && f.y==16);
    g=f12(11,g,31); C(g.x==12 && g.y==33 && g.z==3);
    C(spill(1,2,3,0x123456789LL,4,0x987654321LL)==0xaaaaaaaaaLL+10);
    C(unaligned_pair(3,0x123456789LL,7)==0x123456793LL);
    C(variadic(3,6,1,2,3,4,5,6)==63);
    C(varll(3,0x100000001LL,0x200000002LL,-7LL)==0x2fffffffcLL);
    C(callback(twice,17)==35);
    g=varbig(2,17,31); C(g.x==2 && g.y==17 && g.z==31);
    C(fsingle(1.5f,7,2.0f)==0.5f);
    C(fdouble(3,21.0,4.0,2)==11.25);
    C(fvar(4,1.0,2.25,-7.0,12.5)==8.75);
    C(fconvert(0x1.00000001p33)==0x200000002ULL);
    { struct { char text[1024]; } a={{17}}; C(arrayvar(3,a.text,31)==51); }
    okay();
    return 0;
}
