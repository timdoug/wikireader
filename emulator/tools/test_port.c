/* GPIO register and input-interrupt semantics from the S1C33E07 manual. */
#include <stdio.h>
#include <string.h>

#include "../src/c33.h"
#include "../src/itc.h"
#include "../src/mem.h"
#include "../src/port.h"

#define P0D   (REG_BASE + 0x380)
#define P3D   (REG_BASE + 0x386)
#define P8IOC (REG_BASE + 0x391)
#define PPOL  (REG_BASE + 0x3c2)
#define PEL   (REG_BASE + 0x3c3)
#define PPOL2 (REG_BASE + 0x3c6)
#define PEL2  (REG_BASE + 0x3c7)
#define PSEL  (REG_BASE + 0x3c0)
#define KSEL  (REG_BASE + 0x3d0)
#define SCPK0 (REG_BASE + 0x3d2)
#define SCPK1 (REG_BASE + 0x3d3)
#define SMPK0 (REG_BASE + 0x3d4)
#define SMPK1 (REG_BASE + 0x3d5)
#define FLAGS (REG_BASE + 0x280)

#define FP3 (1u << 3)
#define FK0 (1u << 4)

static int fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-64s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got 0x%x, wanted 0x%x\n", got, want);
		fails++;
	}
}

static void clear_flag(struct mem *mem, unsigned flag)
{
	mem_write(mem, FLAGS, 1, flag);
}

int main(void)
{
	struct c33 cpu;
	struct itc itc;
	struct mem mem;
	struct port port;

	memset(&cpu, 0, sizeof cpu);
	if (!mem_init(&mem))
		return 1;
	itc_attach(&mem, &itc);
	port_attach(&mem, &port, &itc);

	check("power input idles high", mem_read(&mem, P0D, 1), 0x08);
	check("port interrupt polarity resets high/rising",
	      mem_read(&mem, PPOL, 1), 0xff);
	check("port interrupt trigger resets to edge",
	      mem_read(&mem, PEL, 1), 0xff);
	check("upper port interrupt polarity resets high/rising",
	      mem_read(&mem, PPOL2, 1), 0xff);
	check("upper port interrupt trigger resets to edge",
	      mem_read(&mem, PEL2, 1), 0xff);

	mem_write(&mem, P3D, 1, 0xff);
	check("reserved P3 data bit reads zero", mem_read(&mem, P3D, 1), 0x7f);
	mem_write(&mem, P8IOC, 1, 0xff);
	check("reserved P8 direction bits read zero",
	      mem_read(&mem, P8IOC, 1), 0x3f);
	mem_write(&mem, KSEL, 1, 0xff);
	mem_write(&mem, SCPK0, 1, 0xff);
	mem_write(&mem, SCPK1, 1, 0xff);
	mem_write(&mem, SMPK0, 1, 0xff);
	mem_write(&mem, SMPK1, 1, 0xff);
	check("reserved key-select bits read zero", mem_read(&mem, KSEL, 1), 0x77);
	check("FPK0 comparison is five bits", mem_read(&mem, SCPK0, 1), 0x1f);
	check("FPK1 comparison is four bits", mem_read(&mem, SCPK1, 1), 0x0f);
	check("FPK0 mask is five bits", mem_read(&mem, SMPK0, 1), 0x1f);
	check("FPK1 mask is four bits", mem_read(&mem, SMPK1, 1), 0x0f);
	mem_write(&mem, REG_BASE + 0x3d1, 1, 0xff);
	check("reserved key-register hole stays zero",
	      mem_read(&mem, REG_BASE + 0x3d1, 1), 0);

	/* Reset selects a rising edge: the active-low press is not that edge. */
	port_power_button(&port, &cpu, true);
	check("default rising-edge mode ignores the falling press",
	      mem_read(&mem, FLAGS, 1) & FP3, 0);
	port_power_button(&port, &cpu, false);
	check("default rising-edge mode accepts the release",
	      mem_read(&mem, FLAGS, 1) & FP3, FP3);
	clear_flag(&mem, FP3);

	mem_write(&mem, PPOL, 1, 0xf7);       /* FPT3 falling edge */
	port_power_button(&port, &cpu, true);
	check("programmed falling edge accepts the press",
	      mem_read(&mem, FLAGS, 1) & FP3, FP3);
	clear_flag(&mem, FP3);
	port_power_button(&port, &cpu, false);
	check("falling-edge mode ignores the release",
	      mem_read(&mem, FLAGS, 1) & FP3, 0);
	mem_write(&mem, PSEL, 1, 0x80);       /* FPT3 selects P13, not P03 */
	port_power_button(&port, &cpu, true);
	check("P03 cannot trigger FPT3 when P13 is selected",
	      mem_read(&mem, FLAGS, 1) & FP3, 0);

	port_reset(&port);
	mem_write(&mem, KSEL, 1, 0x04);       /* FPK0 selects P6[4:0] */
	mem_write(&mem, SMPK0, 1, 0x07);
	mem_write(&mem, SCPK0, 1, 0x00);
	port_button(&port, &cpu, 1, true);
	check("matched-to-unmatched button edge raises FPK0",
	      mem_read(&mem, FLAGS, 1) & FK0, FK0);
	clear_flag(&mem, FK0);
	port_button(&port, &cpu, 1, true);
	check("remaining unmatched does not repeat FPK0",
	      mem_read(&mem, FLAGS, 1) & FK0, 0);
	mem_write(&mem, SCPK0, 1, 0x02);      /* handler re-arms current state */
	port_button(&port, &cpu, 1, false);
	check("re-armed release raises the opposite edge",
	      mem_read(&mem, FLAGS, 1) & FK0, FK0);
	clear_flag(&mem, FK0);
	mem_write(&mem, KSEL, 1, 0x05);       /* FPK0 selects P5[4:0] */
	port_button(&port, &cpu, 0, true);
	check("P6 buttons cannot trigger FPK0 when P5 is selected",
	      mem_read(&mem, FLAGS, 1) & FK0, 0);

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all port tests passed");
	return fails != 0;
}
