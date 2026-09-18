/* The operands: every value the divide has a special path or a boundary
   for, and a spread of ordinary ones.  Shared with tests/divref.c. */
static const uint32_t table[] = {
	0, 1, 2, 3, 7, 10, 15, 16, 17, 100, 255, 256, 257, 1000, 4095, 4096,
	0x7fff, 0x8000, 0x8001, 0xffff, 0x10000, 0x10001, 0xfffff, 0x100000,
	0x12345678, 0x7ffffffe, 0x7fffffff, 0x80000000, 0x80000001,
	0xbfffffff, 0xc0000000, 0xfffffffe, 0xffffffff, 0xdeadbeef, 0x9e3779b9,
	0x40000000, 0x3fffffff, 1000000007, 0xfffffff0, 0x0000fff0,
};
#define NTABLE (sizeof table / sizeof table[0])
