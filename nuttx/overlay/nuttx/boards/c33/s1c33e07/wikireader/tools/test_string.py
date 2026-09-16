#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the C33 libc string routines against byte-loop oracles in wremu.

The routines are hand-written C33 assembly, so a host build cannot run them.
This links the real sources bare metal and exercises every size and source/
destination alignment combination that the word paths switch on, including
the overlapping memmove directions and reads that end mid-word.
"""
import argparse
from pathlib import Path
import re
import subprocess
import textwrap

DRIVER = r'''
/* SPDX-License-Identifier: Apache-2.0 */
typedef unsigned long size_t;
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);
int memcmp(const void *a, const void *b, size_t n);

static void out(const char *p)
{ while (*p) *(volatile unsigned char *)0x300b00 = *p++; }

static void hex(unsigned v)
{
  char s[9]; int i;
  for (i = 0; i < 8; i++) s[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
  s[8] = 0; out(s);
}

static unsigned failures;
static void check(int ok, const char *what, unsigned a, unsigned b, unsigned c)
{
  if (ok) return;
  failures++;
  out("C33_STRING_FAIL "); out(what); out(" ");
  hex(a); out(" "); hex(b); out(" "); hex(c); out("\n");
}

/* Guarded work areas: the pad bytes must never be touched. */
#define PAD 8
#define AREA 200
static unsigned char dst[PAD + AREA + PAD];
static unsigned char src[PAD + AREA + PAD];
static unsigned char ref[PAD + AREA + PAD];

static unsigned seed = 12345;
static unsigned rnd(void)
{ seed = seed * 1103515245u + 12345u; return seed >> 8; }

static void fill(unsigned char *p, unsigned n)
{ unsigned i; for (i = 0; i < n; i++) p[i] = (unsigned char)(rnd() | 1); }

static int same(const unsigned char *a, const unsigned char *b, unsigned n)
{ unsigned i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static void test_copy(void)
{
  unsigned n, da, sa, i;
  for (n = 0; n <= 70; n++)
    for (da = 0; da < 4; da++)
      for (sa = 0; sa < 4; sa++)
        {
          unsigned char *d = dst + PAD + da, *s = src + PAD + sa;
          void *r;
          fill(src, sizeof(src));
          for (i = 0; i < sizeof(dst); i++) dst[i] = ref[i] = (unsigned char)(i + 3);
          for (i = 0; i < n; i++) ref[PAD + da + i] = s[i];
          r = memcpy(d, s, n);
          check(r == d, "memcpy_ret", n, da, sa);
          check(same(dst, ref, sizeof(dst)), "memcpy", n, da, sa);
        }
}

static void test_set(void)
{
  unsigned n, da, i;
  for (n = 0; n <= 70; n++)
    for (da = 0; da < 4; da++)
      {
        unsigned char *d = dst + PAD + da;
        int c = (int)(rnd() & 0xff) - 128;   /* also exercises negative c */
        void *r;
        for (i = 0; i < sizeof(dst); i++) dst[i] = ref[i] = (unsigned char)(i + 7);
        for (i = 0; i < n; i++) ref[PAD + da + i] = (unsigned char)c;
        r = memset(d, c, n);
        check(r == d, "memset_ret", n, da, 0);
        check(same(dst, ref, sizeof(dst)), "memset", n, da, (unsigned)c);
      }
}

static void test_move(void)
{
  unsigned n, i;
  int delta;
  for (n = 0; n <= 70; n++)
    for (delta = -9; delta <= 9; delta++)
      {
        unsigned char *base = dst + PAD + 20;
        unsigned char *d = base + delta, *s = base;
        void *r;
        for (i = 0; i < sizeof(dst); i++) dst[i] = ref[i] = (unsigned char)(rnd());
        for (i = 0; i < n; i++) ref[(d - dst) + i] = dst[(s - dst) + i];
        r = memmove(d, s, n);
        check(r == d, "memmove_ret", n, (unsigned)delta, 0);
        check(same(dst, ref, sizeof(dst)), "memmove", n, (unsigned)delta, 0);
      }
}

static void test_len(void)
{
  unsigned n, a, i;
  for (n = 0; n <= 70; n++)
    for (a = 0; a < 4; a++)
      {
        char *p = (char *)(dst + PAD + a);
        unsigned got;
        for (i = 0; i < sizeof(dst); i++) dst[i] = (unsigned char)(rnd() | 1);
        p[n] = 0;
        got = strlen(p);
        check(got == n, "strlen", n, a, got);
      }
}

static void test_cmp(void)
{
  unsigned n, aa, ba, at, i;
  for (n = 1; n <= 70; n++)
    for (aa = 0; aa < 4; aa++)
      for (ba = 0; ba < 4; ba++)
        for (at = 0; at < n; at += (n > 8 ? 3 : 1))
          {
            unsigned char *a = dst + PAD + aa, *b = src + PAD + ba;
            int got, want;
            for (i = 0; i < n; i++) a[i] = b[i] = (unsigned char)(rnd() | 1);
            check(memcmp(a, b, n) == 0, "memcmp_eq", n, aa, ba);
            b[at] = (unsigned char)(a[at] ^ 0x80);   /* differ high or low */
            want = (int)a[at] - (int)b[at];
            got = memcmp(a, b, n);
            check((got < 0) == (want < 0) && (got > 0) == (want > 0),
                  "memcmp", n, at, (unsigned)got);
            got = memcmp(b, a, n);
            check((got < 0) == (want > 0) && (got > 0) == (want < 0),
                  "memcmp_rev", n, at, (unsigned)got);
          }
}

int main(void)
{
  test_copy();
  test_set();
  test_move();
  test_len();
  test_cmp();
  if (failures) { out("C33_STRING_FAILURES "); hex(failures); out("\n"); }
  else out("C33_STRING_OK\n");
  return 0;
}
'''

START = r'''
/* SPDX-License-Identifier: Apache-2.0 */
.section .text.start,"ax"
.global _start
_start:
    xld.w %r4,0x11000000
    ld.w %sp,%r4
    xld.w %r15,0x10000000
    xcall main
.global test_done
test_done:
    jp test_done
'''

LINKER = r'''
OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)
SECTIONS {
    . = 0x10000000;
    __dp = .;
    .text : { *(.text.start) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
'''

SOURCES = ('memcpy', 'memmove', 'memset', 'strlen', 'memcmp')


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wikireader', type=Path, default=Path.home() / 'wikireader')
    parser.add_argument('--out', type=Path, default=root / 'build/wikireader/string')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binpath = args.wikireader / 'host-tools/toolchain-c33/work/install/bin'
    gcc = str(binpath / 'c33-epson-elf-gcc')

    (out / 'driver.c').write_text(textwrap.dedent(DRIVER).lstrip())
    (out / 'start.S').write_text(textwrap.dedent(START).lstrip())
    (out / 'target.ld').write_text(textwrap.dedent(LINKER).lstrip())
    # The routines guard themselves with LIBC_BUILD_*; supply them directly
    # rather than pulling in the whole configured libc.
    (out / 'libc.h').write_text('\n'.join(
        '#define LIBC_BUILD_%s 1' % name.upper() for name in SOURCES) + '\n')

    base = [gcc, '-mc33pe', '-medda32', '-O2', '-g0', '-ffreestanding',
            '-nostdlib', '-I', str(out)]
    objects = []
    for name in SOURCES:
        source = root / 'libs/libc/machine/c33' / ('arch_%s.S' % name)
        obj = out / ('arch_%s.o' % name)
        subprocess.run(base + ['-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    for name in ('start.S', 'driver.c'):
        obj = out / (name + '.o')
        subprocess.run(base + ['-c', str(out / name), '-o', str(obj)], check=True)
        objects.append(str(obj))
    elf = out / 'string.elf'
    subprocess.run(base + ['-T', str(out / 'target.ld'), '-o', str(elf)] + objects,
                   check=True)

    symbols = subprocess.run([str(binpath / 'c33-epson-elf-nm'), str(elf)],
                             capture_output=True, text=True, check=True).stdout
    done = re.search(r'^([0-9a-f]+) T test_done$', symbols, re.M)
    if not done:
        raise SystemExit('test_done symbol is missing')
    # --bare-elf, and this is the case it exists for: what runs here is a
    # bare-metal link of the libc sources, not firmware.  There is no loader
    # to go through and nothing that depends on the state one would leave, so
    # the boot would only add three seconds to every run.
    command = [str(args.wikireader / 'emulator/wremu'), '-n', '4000000000',
               '-b', '0x' + done[1], '--bare-elf', str(elf)]
    log = subprocess.run(command, cwd=out, text=True, timeout=900,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    (out / 'emulator.log').write_text(log)
    if 'C33_STRING_OK' not in log or 'C33_STRING_FAIL' in log:
        raise SystemExit('String routines failed: %s' % (out / 'emulator.log'))
    for bad in ('unmapped read', 'unmapped write', 'misaligned', 'runaway:',
                'instruction limit reached'):
        if bad in log:
            raise SystemExit('Unexpected %r: %s' % (bad, out / 'emulator.log'))
    print('PASS: memcpy, memmove, memset, strlen and memcmp match their oracles')
    print('      at every size to 70 and every alignment pair.')
    print('Artifacts:', out)


if __name__ == '__main__':
    main()
