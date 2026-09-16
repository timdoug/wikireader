/* host_run.c - run a guest image under the interpreter on the build machine.
 *
 * The cheap half of the measurement: it proves the ISA implementation is
 * right and reports how many guest instructions each kernel costs, which is
 * the denominator the emulator run divides host cycles by.  No C33 involved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rv32.h"

#define GUEST_RAM (4u * 1024 * 1024)

static void host_putchar(void *arg, int c)
{
	(void)arg;
	fputc(c, stdout);
}

static rv32_t machine;

static uint64_t mark_retired[64];
static uint32_t mark_id[64];
static unsigned marks;

static void host_mark(void *arg, uint32_t id)
{
	(void)arg;
	if (marks < sizeof mark_id / sizeof mark_id[0]) {
		mark_id[marks] = id;
		mark_retired[marks] = machine.retired;
		++marks;
	}
}

static const char *const kernel_name[] = {
	"end", "alu", "branch", "mul", "div", "load",
	"store", "copy", "bytes", "crc", "sieve",
};

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <image.bin>\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	uint8_t *ram = calloc(1, GUEST_RAM);
	if (!ram) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	size_t size = fread(ram, 1, GUEST_RAM, f);
	fclose(f);

	machine.ram = ram;
	machine.ram_size = GUEST_RAM;
	machine.putchar = host_putchar;
	machine.getchar = NULL;
	machine.mark = host_mark;
	rv32_reset(&machine, RV_RAM_BASE, 0);

	fprintf(stderr, "image %zu bytes\n", size);

	rv32_stop_t stop = RV_RAN_OUT;
	for (uint64_t batch = 0; batch < 200000; ++batch) {
		stop = rv32_run(&machine, 65536, 65536);
		if (stop != RV_RAN_OUT)
			break;
	}
	fflush(stdout);

	if (stop == RV_FAULT) {
		fprintf(stderr, "fault: mcause %u at pc %08x\n",
			machine.mcause, machine.pc);
		return 1;
	}
	if (stop != RV_POWEROFF) {
		fprintf(stderr, "guest did not power off (stop %d, pc %08x)\n",
			(int)stop, machine.pc);
		return 1;
	}

	printf("\n%-8s %14s\n", "kernel", "instructions");
	for (unsigned i = 0; i + 1 < marks; ++i) {
		uint32_t id = mark_id[i];
		const char *name = id < sizeof kernel_name / sizeof kernel_name[0]
			? kernel_name[id] : "?";
		printf("%-8s %14llu\n", name,
		       (unsigned long long)(mark_retired[i + 1] - mark_retired[i]));
	}
	printf("%-8s %14llu\n", "total", (unsigned long long)machine.retired);
	free(ram);
	return 0;
}
