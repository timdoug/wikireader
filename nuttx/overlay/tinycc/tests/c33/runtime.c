/* SPDX-License-Identifier: LGPL-2.0-or-later */
static void out(const char *p)
{ while(*p) *(volatile unsigned char *)0x300b00=*p++; }
int fail(int n)
{ char s[9]; int i; for(i=0;i<8;i++) s[i]="0123456789abcdef"[(n>>(28-4*i))&15]; s[8]=0; out("C33_ABI_FAIL "); out(s); out("\n"); return n; }
void okay(void) { out("C33_ABI_OK\n"); }
void *memcpy(void *dst,const void *src,unsigned n)
{ char *d=dst; const char *s=src; while(n--) *d++=*s++; return dst; }
void *memmove(void *dst,const void *src,unsigned n)
{ char *d=dst; const char *s=src; if(d<s) return memcpy(dst,src,n); while(n) { n--; d[n]=s[n]; } return dst; }
void *memset(void *dst,int c,unsigned n)
{ char *d=dst; while(n--) *d++=c; return dst; }
