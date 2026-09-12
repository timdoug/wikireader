#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test the actual touch ISR/parser with a four-byte UART FIFO on the host."""

from pathlib import Path
import subprocess
import tempfile

STUB = r"""
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define OK 0
#define CONFIG_S1C33E07_MCLK 48000000
#define CONFIG_S1C33E07_BAUDCLK 60000000
#define C33_IRQ_UART1RX 61
#define C33_IRQ_UART1ERR 60
#define TOUCH_DOWN 1
#define TOUCH_MOVE 2
#define TOUCH_UP 4
#define TOUCH_POS_VALID 8
#define TOUCH_ID_VALID 16
#define SEM_INITIALIZER(n) {n}
typedef unsigned irqstate_t;
typedef struct { int count; } sem_t;
struct touch_point_s { uint8_t id, flags; int16_t x, y; uint64_t timestamp; };
struct touch_sample_s { int npoints; struct touch_point_s point[1]; };
struct touch_lowerhalf_s { int maxpoint, xres, yres; void *priv; };
uint8_t getreg8(uintptr_t addr);
void putreg8(uint8_t value, uintptr_t addr);
static inline uint16_t getreg16(uintptr_t addr) { return 0; }
static inline uint32_t getreg32(uintptr_t addr) { return 0; }
static inline void putreg16(uint16_t value, uintptr_t addr) {}
static inline void putreg32(uint32_t value, uintptr_t addr) {}
static inline irqstate_t up_irq_save(void) { return 0; }
static inline void up_irq_restore(irqstate_t flags) {}
static inline void up_enable_irq(int irq) {}
static inline void up_udelay(unsigned usec) {}
static inline void nxsem_post(sem_t *s) { s->count++; }
static inline void nxsem_wait_uninterruptible(sem_t *s) {}
static inline uint64_t touch_get_time(void) { return 42; }
static inline void touch_event(void *p, struct touch_sample_s *s) {}
static inline int touch_register(struct touch_lowerhalf_s *p, const char *n,
                                  unsigned depth) { return 0; }
static inline void touch_unregister(struct touch_lowerhalf_s *p, const char *n) {}
static inline int irq_attach(int irq, int (*h)(int, void *, void *), void *p)
{ return 0; }
static inline void irq_detach(int irq) {}
static inline int kthread_create(const char *name, int pri, int stack,
                                  int (*entry)(int, char **), char **args)
{ return 0; }
"""

TEST = r"""
#include <assert.h>
#include <stdio.h>
#include "wikireader_touch.c"
static uint8_t fifo[4], fault;
static unsigned cursor, count;
uint8_t getreg8(uintptr_t addr)
{
  if (addr == S1C33_UART1_STATUS) return fault | (cursor < count ? 1 : 0);
  assert(addr == S1C33_UART1_RXD && cursor < count);
  return fifo[cursor++];
}
void putreg8(uint8_t value, uintptr_t addr)
{
  if (addr == S1C33_UART1_STATUS) fault &= value;
}
static void reset(void)
{
  g_head = g_tail = g_state = g_id = 0;
  g_x = g_y = 0;
  g_pressed = false;
  g_samples.count = 0;
}
static void receive(const uint8_t *bytes, unsigned n, uint8_t errors)
{
  assert(n <= 4);
  memcpy(fifo, bytes, n);
  count = n; cursor = 0; fault = errors;
  wr_touch_interrupt(errors ? 60 : 61, NULL, NULL);
  assert(cursor == count);
}
static void packet(const uint8_t bytes[6], unsigned chunk)
{
  for (unsigned i = 0; i < 6; i += chunk)
    receive(bytes + i, 6-i < chunk ? 6-i : chunk, 0);
}
static struct touch_point_s pop(void)
{
  assert(g_head != g_tail);
  struct touch_sample_s sample = g_queue[g_head];
  g_head = (g_head + 1) % WR_TOUCH_QUEUE;
  assert(sample.npoints == 1);
  return sample.point[0];
}
int main(void)
{
  const uint8_t down[] = {0xaa, 0, 120, 2, 6, 1}; /* pixel 60,131 */
  const uint8_t up[] = {0xaa, 0xff, 0xff, 0xff, 0xff, 0};
  const uint8_t outside[] = {0xaa, 127, 127, 127, 127, 1};
  for (unsigned mask = 0; mask < 32; mask++)
    {
      reset();
      unsigned start = 0;
      for (unsigned i = 0; i < 6; i++)
        if (i == 5 || (mask & (1 << i)) || i - start == 3)
          {
            receive(down + start, i-start+1, 0);
            start = i+1;
            if (i < 5) assert(g_head == g_tail);
          }
      struct touch_point_s p = pop();
      assert(p.x == 60 && p.y == 131 && (p.flags & TOUCH_DOWN));
      packet(down, 1);
      p = pop(); assert(p.flags & TOUCH_MOVE);
      packet(up, 2);
      p = pop(); assert(p.x == 60 && p.y == 131 && (p.flags & TOUCH_UP));
      packet(up, 4); assert(g_head == g_tail);
      packet(down, 4); p = pop(); assert(p.id == 1);
    }
  reset();
  receive(down, 4, 0);
  packet(down, 1); /* A new header abandons the partial packet. */
  pop();
  for (unsigned error = 4; error <= 16; error <<= 1)
    {
      receive(down, 3, error);
      struct touch_point_s errorup = pop();
      assert((errorup.flags & TOUCH_UP) && !(errorup.flags & TOUCH_POS_VALID));
      assert(g_state == 0 && fault == 0);
      packet(down, 4); assert(pop().flags & TOUCH_DOWN);
    }
  packet(outside, 4); assert(g_head == g_tail);
  packet(up, 4); /* Invalid release coordinates preserve last valid point. */
  struct touch_point_s p = pop();
  assert(p.x == 60 && p.y == 131 && (p.flags & TOUCH_UP));
  assert(!(p.flags & TOUCH_POS_VALID));
  reset();
  const uint8_t garbage[] = {0, 0x81, 0xff, 1};
  receive(garbage, 4, 0); assert(g_head == g_tail);
  packet(down, 4);
  for (unsigned i = 0; i < 100; i++) packet(down, 4);
  packet(up, 4); /* Queue pressure must not lose the final release. */
  while (g_head != g_tail) p = pop();
  assert(p.flags & TOUCH_UP);
  puts("PASS: touch FIFO fragmentation, resync, errors, bounds, IDs and overflow release");
  return 0;
}
"""


def main():
    root = Path(__file__).resolve().parents[5]
    with tempfile.TemporaryDirectory(prefix="wr-touch-") as tmp:
        out = Path(tmp)
        (out / "stub.h").write_text(STUB)
        for name in ("config.h", "arch.h", "irq.h", "kthread.h", "semaphore.h",
                     "input/touchscreen.h"):
            p = out / "nuttx" / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#include "stub.h"\n')
        # Keep real register addresses and replace only MMIO accessors.
        hardware = (root / "arch/c33/src/s1c33e07/hardware/s1c33e07.h").read_text()
        hardware = hardware.split("#define getreg8", 1)[0]
        hardware += """
static inline void modifyreg8(uintptr_t addr, uint8_t clear, uint8_t set) {}
#endif
"""
        (out / "s1c33e07.h").write_text(hardware)
        (out / "test.c").write_text(TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined",
                        "-O1", "-g", f"-I{out}",
                        f"-I{root / 'boards/c33/s1c33e07/wikireader/src'}",
                        f"-I{root / 'arch/c33/src/s1c33e07/hardware'}",
                        str(out / "test.c"), "-o", str(out / "test")], check=True)
        subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
