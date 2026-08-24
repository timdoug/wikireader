int g_normal;
extern int g_ext;
static int s_static;
int  rd(void)      { return g_normal + g_ext + s_static; }
void wr(int v)     { g_normal = v; g_ext = v; s_static = v; }
int *addr(void)    { return &g_normal; }
