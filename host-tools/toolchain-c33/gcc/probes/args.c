extern void sink(void);
int  f_args(int a, int b, int c, int d, int e, int f) { return a+b+c+d+e+f; }
long long f_ll(long long a, long long b) { return a+b; }
double f_d(double a, double b) { return a+b; }
float  f_f(float a, float b) { return a+b; }
struct S8  { int a, b; };
struct S16 { int a,b,c,d; };
struct S8  f_s8(struct S8 s)  { s.a++; return s; }
struct S16 f_s16(struct S16 s){ s.a++; return s; }
char f_char(char a, char b) { return a+b; }
