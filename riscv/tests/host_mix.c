/* host_mix.c - what a guest actually executes, measured on the build machine.
 *
 * The benchmark in guest/bench.c is a synthetic mix chosen to exercise the
 * interpreter, and the device numbers in riscv/README.md are all against it.
 * Nobody has ever looked at what a Linux boot executes, and every design
 * decision left -- what a translator would have to emit, how often it would
 * have to give up, how much code it would have to hold -- turns on that and
 * not on the benchmark.
 *
 * This runs a guest image under the same interpreter, one instruction at a
 * time, on the host.  Stepping rather than instrumenting rv32.c is what keeps
 * the device build untouched: the machine is inspected between instructions,
 * from the outside.  A Linux boot is about thirty million guest instructions,
 * which is nine minutes in the full-system emulator and a few seconds here.
 *
 * It reports four things:
 *
 *   - the opcode mix, which says which bodies are worth hand-writing;
 *   - what the assembly hot path would decline, by reason.  Each one costs a
 *     return to C, an instruction interpreted in SDRAM and a re-entry, and
 *     the benchmark never does it at all;
 *   - the shape of the control flow: how many instructions a basic block
 *     runs for, which is what a translated block amortises its exit over;
 *   - the code footprint: how many blocks cover most of the execution, which
 *     is the size of the code cache a translator would need, and how often
 *     the guest writes to a page it has already executed, which is what
 *     invalidating that cache would cost.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rv32.h"

/* The device gives the guest what the board has spare, and rewrites the
 * device tree's memory node to match; riscv.c uses board RAM less 4 MB. */
#define DEFAULT_RAM  (12u * 1024 * 1024)
#define DTB_RESERVE  (16u * 1024)

static rv32_t machine;
static uint8_t *ram;
static uint32_t ram_size;

/* ---- counters, declared first: the console timestamps its lines with the
   instruction count, so it has to be able to see it ------------------------ */

enum {
	K_LUI, K_AUIPC, K_JAL, K_JALR, K_BRANCH, K_LOAD, K_STORE,
	K_OPIMM, K_OP, K_MUL, K_DIV, K_AMO, K_SYSTEM, K_FENCE, K_OTHER,
	K_KINDS
};

/* Why the hand-written path in rv32_hot.s would hand this one back to C.
 * The list is that file's, read off its dispatch table and its guards. */
enum {
	D_NONE, D_SYSTEM, D_AMO, D_JALR_F3, D_HOLE,
	D_DEVICE, D_MISALIGNED, D_BAD_TARGET, D_REASONS
};

struct counts {
	uint64_t total;
	uint64_t kind[K_KINDS];
	uint64_t decline[D_REASONS];
	uint64_t taken, not_taken;
	uint64_t blocks;             /* control transfers, i.e. block exits */
	uint64_t divop;              /* the C helper the asm path calls */
	uint64_t traps;
};

static struct counts all, phase;

/* ---- console ------------------------------------------------------------ */

static char tail[256];          /* the last bytes the guest printed */
static unsigned tail_len;
static bool quiet;
static unsigned long long fence_i;

static const char *feed;        /* keystrokes to deliver once prompted */
static bool prompted;

/* Instructions between one console line and the next.  The kernel's own
   timestamps say the same thing, but only to the resolution of a timebase
   that advances one tick per instruction and only where it printed at all;
   this is exact, needs no marker chosen in advance, and catches the gaps
   nobody thought to bracket. */
#define LINES 512
static struct {
	char text[72];
	uint64_t at;
} line[LINES];
static unsigned lines;
static char pending[72];
static unsigned pending_len;

static void host_putchar(void *arg, int c)
{
	(void)arg;
	if (!quiet)
		fputc(c, stdout);
	if (tail_len == sizeof tail) {
		memmove(tail, tail + 1, --tail_len);
	}
	tail[tail_len++] = (char)c;

	if (c == '\n') {
		if (lines < LINES) {
			memcpy(line[lines].text, pending, pending_len);
			line[lines].text[pending_len] = 0;
			line[lines].at = all.total;
			lines++;
		}
		pending_len = 0;
	} else if (pending_len < sizeof pending - 1 && c >= ' ') {
		pending[pending_len++] = (char)c;
	}
}

static int host_getchar(void *arg)
{
	(void)arg;
	if (!prompted || !feed || !*feed)
		return -1;
	return *feed++;
}

/* ---- counters ----------------------------------------------------------- */

static const char *kind_name[K_KINDS] = {
	"lui", "auipc", "jal", "jalr", "branch", "load", "store",
	"op-imm", "op", "mul/mulh", "div/rem", "amo", "system", "fence", "other"
};

static const char *decline_name[D_REASONS] = {
	"-", "system/csr", "atomic", "jalr funct3", "unimplemented encoding",
	"device address", "misaligned access", "target outside RAM",
};

/* Per-instruction-slot execution, for the static footprint. */
static uint8_t *seen;            /* one byte per guest word */
static uint64_t seen_count;

/* Block heads, open-addressed. */
#define BLOCK_BITS 21
#define BLOCK_SIZE (1u << BLOCK_BITS)
static uint32_t *block_pc;
static uint64_t *block_insns;    /* guest instructions run from this head */
static uint32_t *block_len;      /* the longest run seen from it */
static unsigned block_used;

/* The guest registers a block touches, and the ones it reads before it
   writes them.  A translator has to hold the first set somewhere and load
   the second at the top of the block, and the C33 has about ten registers
   to spare once the register-file base, the address adjustment, the two
   ends of guest RAM and a scratch pair are live -- so whether a block fits
   in them is what decides how much of jit_probe.s's 5.5x is reachable. */
static uint32_t *block_touched, *block_live_in;

static unsigned block_index(uint32_t pc)
{
	uint32_t h = (pc * 2654435761u) >> (32 - BLOCK_BITS);
	for (;;) {
		if (!block_pc[h]) {
			block_pc[h] = pc;
			block_used++;
			return h;
		}
		if (block_pc[h] == pc)
			return h;
		h = (h + 1) & (BLOCK_SIZE - 1);
	}
}

/* Within the block being executed: what it has written so far, and what it
   read before writing it. */
static unsigned cur_block;
static uint32_t run_written, run_live_in;

static void reads(unsigned r)
{
	if (r && !(run_written & (1u << r)))
		run_live_in |= 1u << r;
}

static void writes(unsigned r)
{
	if (r)
		run_written |= 1u << r;
}

/* Pages the guest has executed from, and writes into them: what a code cache
 * would have to notice and throw away. */
static uint8_t *code_page, *invalidated;
static uint64_t stores_into_code, invalidations, invalidated_pages;

static uint64_t executed_pages(void)
{
	uint64_t n = 0;
	for (uint32_t p = 0; p < ram_size / 4096; ++p)
		if (code_page[p])
			n++;
	return n;
}

/* ---- classification ----------------------------------------------------- */

static uint32_t at(uint32_t addr)
{
	return *(const uint32_t *)(ram + (addr - RV_RAM_BASE));
}

static bool in_ram(uint32_t addr)
{
	return addr - RV_RAM_BASE < ram_size;
}

static void bump(int kind, int decline)
{
	all.total++; phase.total++;
	all.kind[kind]++; phase.kind[kind]++;
	all.decline[decline]++; phase.decline[decline]++;
}

/* Returns true if this instruction ends a basic block. */
static bool classify(uint32_t pc, uint32_t ir)
{
	unsigned op = ir & 0x7f, f3 = (ir >> 12) & 7;
	uint32_t rs1 = machine.x[(ir >> 15) & 0x1f];
	bool ends = false;

	/* rs1 is in the same place in every format that has one, and so is
	   rs2; which of them exists follows from the opcode. */
	switch (op) {
	case 0x37: case 0x17:                                 /* lui, auipc */
		writes((ir >> 7) & 0x1f);
		break;
	case 0x6f:                                            /* jal */
		writes((ir >> 7) & 0x1f);
		break;
	case 0x67: case 0x03: case 0x13:              /* jalr, load, op-imm */
		reads((ir >> 15) & 0x1f);
		writes((ir >> 7) & 0x1f);
		break;
	case 0x63: case 0x23:                            /* branch, store */
		reads((ir >> 15) & 0x1f);
		reads((ir >> 20) & 0x1f);
		break;
	case 0x33:                                            /* op */
		reads((ir >> 15) & 0x1f);
		reads((ir >> 20) & 0x1f);
		writes((ir >> 7) & 0x1f);
		break;
	case 0x2f:                                            /* amo */
		reads((ir >> 15) & 0x1f);
		reads((ir >> 20) & 0x1f);
		writes((ir >> 7) & 0x1f);
		break;
	default:
		break;
	}

	if ((ir & 3) != 3) {           /* a compressed instruction; not built */
		bump(K_OTHER, D_HOLE);
		return true;
	}

	switch (op) {
	case 0x37: bump(K_LUI, D_NONE); break;
	case 0x17: bump(K_AUIPC, D_NONE); break;

	case 0x6f: {                   /* jal */
		int32_t imm = (int32_t)((ir & 0x80000000u) ? 0xfff00000u : 0)
			| ((ir >> 20) & 0x7fe) | ((ir >> 9) & 0x800)
			| (ir & 0xff000);
		uint32_t target = pc + (uint32_t)imm;
		bump(K_JAL, in_ram(target) && !(target & 3) ? D_NONE : D_BAD_TARGET);
		ends = true;
		break;
	}
	case 0x67: {                   /* jalr */
		uint32_t target = (rs1 + (uint32_t)((int32_t)ir >> 20)) & ~1u;
		bump(K_JALR, f3 ? D_JALR_F3
		     : in_ram(target) && !(target & 3) ? D_NONE : D_BAD_TARGET);
		ends = true;
		break;
	}
	case 0x63: {                   /* branch */
		if (f3 == 2 || f3 == 3) { bump(K_BRANCH, D_HOLE); ends = true; break; }
		int32_t imm = (int32_t)((ir & 0x80000000u) ? 0xfffff000u : 0)
			| ((ir << 4) & 0x800) | ((ir >> 20) & 0x7e0) | ((ir >> 7) & 0x1e);
		uint32_t target = pc + (uint32_t)imm;
		uint32_t rs2 = machine.x[(ir >> 20) & 0x1f];
		bool take;
		switch (f3) {
		case 0: take = rs1 == rs2; break;
		case 1: take = rs1 != rs2; break;
		case 4: take = (int32_t)rs1 < (int32_t)rs2; break;
		case 5: take = (int32_t)rs1 >= (int32_t)rs2; break;
		case 6: take = rs1 < rs2; break;
		default: take = rs1 >= rs2; break;
		}
		bump(K_BRANCH, take && (!in_ram(target) || (target & 3))
		     ? D_BAD_TARGET : D_NONE);
		if (take) { all.taken++; phase.taken++; ends = true; }
		else { all.not_taken++; phase.not_taken++; }
		break;
	}
	case 0x03: {                   /* load */
		uint32_t addr = rs1 + (uint32_t)((int32_t)ir >> 20);
		unsigned width = f3 == 0 || f3 == 4 ? 1 : f3 == 1 || f3 == 5 ? 2 : 4;
		bump(K_LOAD, f3 == 3 || f3 == 6 || f3 == 7 ? D_HOLE
		     : !in_ram(addr) ? D_DEVICE
		     : (addr & (width - 1)) ? D_MISALIGNED : D_NONE);
		break;
	}
	case 0x23: {                   /* store */
		uint32_t imm = ((ir >> 7) & 0x1f) | ((uint32_t)((int32_t)ir >> 25) << 5);
		uint32_t addr = rs1 + imm;
		unsigned width = f3 == 0 ? 1 : f3 == 1 ? 2 : 4;
		bump(K_STORE, f3 > 2 ? D_HOLE
		     : !in_ram(addr) ? D_DEVICE
		     : (addr & (width - 1)) ? D_MISALIGNED : D_NONE);
		if (in_ram(addr)) {
			uint32_t page = (addr - RV_RAM_BASE) >> 12;
			if (code_page[page]) {
				stores_into_code++;
				if (code_page[page] == 1) {
					invalidations++;
					if (!invalidated[page]) {
						invalidated[page] = 1;
						invalidated_pages++;
					}
					code_page[page] = 2;   /* until executed again */
				}
			}
		}
		break;
	}
	case 0x13: bump(K_OPIMM, D_NONE); break;
	case 0x33: {                   /* op, including the M extension */
		bool m = (ir >> 25) & 1;
		if (!m) { bump(K_OP, D_NONE); break; }
		if (f3 < 4) {
			bump(K_MUL, D_NONE);
			if (f3 == 2) { all.divop++; phase.divop++; }  /* mulhsu: the C helper */
		} else {
			bump(K_DIV, D_NONE);
			all.divop++; phase.divop++;
		}
		break;
	}
	case 0x2f: bump(K_AMO, D_AMO); break;
	case 0x73: bump(K_SYSTEM, D_SYSTEM); ends = true; break;
	case 0x0f:
		bump(K_FENCE, f3 > 1 ? D_HOLE : D_NONE);
		/* fence.i is where a translator learns that code has changed,
		   and what one costs depends on how often a boot executes one. */
		if (f3 == 1)
			fence_i++;
		break;
	default:   bump(K_OTHER, D_HOLE); break;
	}
	return ends;
}

/* ---- reporting ---------------------------------------------------------- */

static void report_counts(const char *title, const struct counts *c)
{
	printf("\n--- %s: %llu instructions ---\n", title,
	       (unsigned long long)c->total);
	if (!c->total)
		return;
	printf("fence.i executed %llu times\n", fence_i);
	printf("%-10s %14s %7s\n", "kind", "count", "share");
	for (int i = 0; i < K_KINDS; ++i)
		if (c->kind[i])
			printf("%-10s %14llu %6.2f%%\n", kind_name[i],
			       (unsigned long long)c->kind[i],
			       100.0 * (double)c->kind[i] / (double)c->total);

	uint64_t declined = c->total - c->decline[D_NONE];
	printf("\ndeclined to the C interpreter: %llu, %.2f%% of instructions, "
	       "one every %.0f\n", (unsigned long long)declined,
	       100.0 * (double)declined / (double)c->total,
	       declined ? (double)c->total / (double)declined : 0.0);
	for (int i = 1; i < D_REASONS; ++i)
		if (c->decline[i])
			printf("  %-24s %12llu %6.3f%%\n", decline_name[i],
			       (unsigned long long)c->decline[i],
			       100.0 * (double)c->decline[i] / (double)c->total);
	printf("  %-24s %12llu %6.3f%%   (a call, not a decline)\n",
	       "divide/mulhsu helper", (unsigned long long)c->divop,
	       100.0 * (double)c->divop / (double)c->total);

	uint64_t exits = c->taken + c->kind[K_JAL] + c->kind[K_JALR] + c->kind[K_SYSTEM];
	printf("\nbranches taken %llu, not taken %llu (%.0f%% taken)\n",
	       (unsigned long long)c->taken, (unsigned long long)c->not_taken,
	       c->taken + c->not_taken
	       ? 100.0 * (double)c->taken / (double)(c->taken + c->not_taken) : 0.0);
	printf("basic block: %.2f instructions between control transfers\n",
	       exits ? (double)c->total / (double)exits : 0.0);
}

/* Eight bytes of C33 code per guest instruction: a load-operate-store
   template with the guest register file in internal RAM is four two-byte
   instructions, which is what the common shapes cost. */
#define BYTES_PER_GUEST_INSN 8

static uint32_t *order;

static int by_count(const void *a, const void *b)
{
	uint64_t x = block_insns[*(const uint32_t *)a];
	uint64_t y = block_insns[*(const uint32_t *)b];
	return x < y ? 1 : x > y ? -1 : 0;
}

/* The boot, as the console saw it: which gap between two printed lines cost
   the most instructions.  A phase nobody printed the start of shows up here
   as one enormous gap, which is the point -- README.md said the boot was
   mostly unpacking the initramfs, and this image never prints that at all. */
static void report_phases(unsigned top)
{
	printf("\n--- the boot, by what it cost between console lines ---\n");
	if (!lines)
		return;
	unsigned *order = malloc(lines * sizeof *order);
	for (unsigned i = 0; i < lines; ++i)
		order[i] = i;
	for (unsigned i = 1; i < lines; ++i)       /* insertion sort; 512 lines */
		for (unsigned j = i; j; --j) {
			uint64_t a = line[order[j]].at
				- (order[j] ? line[order[j] - 1].at : 0);
			uint64_t b = line[order[j - 1]].at
				- (order[j - 1] ? line[order[j - 1] - 1].at : 0);
			if (a <= b)
				break;
			unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
		}
	printf("  %12s %7s  %s\n", "instructions", "share", "printed at the end of it");
	for (unsigned i = 0; i < top && i < lines; ++i) {
		unsigned k = order[i];
		uint64_t cost = line[k].at - (k ? line[k - 1].at : 0);
		printf("  %12llu %6.1f%%  %s\n", (unsigned long long)cost,
		       100.0 * (double)cost / (double)all.total, line[k].text);
	}
	free(order);
}

/* How many guest registers a block needs at once, weighted by the
   instructions executed in blocks of that size.  A translator that can hold
   this many in C33 registers gets jit_probe.s's register-allocated column;
   one that cannot gets the other one. */
static void report_pressure(void)
{
	uint64_t touched[33] = { 0 }, live[33] = { 0 }, total = 0;
	for (unsigned i = 0; i < BLOCK_SIZE; ++i) {
		if (!block_pc[i])
			continue;
		unsigned t = 0, l = 0;
		for (unsigned r = 1; r < 32; ++r) {
			t += (block_touched[i] >> r) & 1;
			l += (block_live_in[i] >> r) & 1;
		}
		touched[t] += block_insns[i];
		live[l] += block_insns[i];
		total += block_insns[i];
	}
	printf("\n--- guest registers a block needs at once ---\n");
	printf("  %-8s %10s %10s\n", "at most", "touched", "live in");
	uint64_t ct = 0, cl = 0;
	for (unsigned n = 0, k = 0; n <= 32; ++n) {
		ct += touched[n];
		cl += live[n];
		static const unsigned show[] = { 4, 6, 8, 10, 12, 16, 32 };
		if (k < sizeof show / sizeof show[0] && n == show[k]) {
			printf("  %-8u %9.1f%% %9.1f%%\n", n,
			       100.0 * (double)ct / (double)total,
			       100.0 * (double)cl / (double)total);
			k++;
		}
	}
	printf("  (share of executed instructions in blocks needing that many)\n");
}

/* Where the code is, in 4 KiB spans.  The console can only bracket a phase
   it printed the end of, and two thirds of this boot is one unprinted
   stretch; naming the addresses inside it is the only way to say what the
   stretch was doing. */
static void report_ranges(unsigned top)
{
	enum { SPAN = 4096 };
	unsigned spans = ram_size / SPAN;
	uint64_t *cost = calloc(spans, sizeof *cost);
	uint64_t total = 0;
	for (unsigned i = 0; i < BLOCK_SIZE; ++i)
		if (block_pc[i]) {
			cost[(block_pc[i] - RV_RAM_BASE) / SPAN] += block_insns[i];
			total += block_insns[i];
		}
	printf("\n--- where the executed code is, by 4 KiB span ---\n");
	for (unsigned shown = 0; shown < top; ++shown) {
		unsigned best = 0;
		for (unsigned i = 0; i < spans; ++i)
			if (cost[i] > cost[best])
				best = i;
		if (!cost[best])
			break;
		printf("  %08x %12llu %6.1f%%\n", RV_RAM_BASE + best * SPAN,
		       (unsigned long long)cost[best],
		       100.0 * (double)cost[best] / (double)total);
		cost[best] = 0;
	}
	free(cost);
}

static void report_footprint(void)
{
	printf("\n--- code footprint ---\n");
	printf("distinct instruction addresses executed: %llu (%llu KiB of guest code)\n",
	       (unsigned long long)seen_count, (unsigned long long)(seen_count * 4 / 1024));
	printf("distinct basic blocks entered: %u\n", block_used);

	order = malloc(block_used * sizeof *order);
	unsigned n = 0;
	uint64_t total = 0;
	for (unsigned i = 0; i < BLOCK_SIZE; ++i)
		if (block_pc[i]) {
			order[n++] = i;
			total += block_insns[i];
		}
	qsort(order, n, sizeof *order, by_count);

	/* What a translator would have to hold to cover a share of everything
	   the guest executes: the blocks, the guest instructions in them, and
	   the C33 code that would take. */
	printf("what a code cache would have to hold to cover:\n");
	printf("  %-6s %8s %10s %9s\n", "share", "blocks", "guest ins", "C33 code");
	const double want[] = { 0.5, 0.8, 0.9, 0.95, 0.99, 1.0 };
	unsigned k = 0;
	uint64_t run = 0, code = 0;
	for (unsigned w = 0; w < sizeof want / sizeof want[0]; ++w) {
		while (k < n && (double)run < want[w] * (double)total) {
			run += block_insns[order[k]];
			code += block_len[order[k]];
			k++;
		}
		printf("  %4.0f%%  %8u %10llu %7llu B\n", 100 * want[w], k,
		       (unsigned long long)code,
		       (unsigned long long)(code * BYTES_PER_GUEST_INSN));
	}

	printf("\nthe hottest blocks:\n");
	printf("  %-10s %12s %8s %10s\n", "head", "executions", "length", "share");
	for (unsigned i = 0; i < 12 && i < n; ++i) {
		unsigned b = order[i];
		uint64_t runs = block_len[b] ? block_insns[b] / block_len[b] : 0;
		printf("  %08x %12llu %8u %9.2f%%\n", block_pc[b],
		       (unsigned long long)runs, block_len[b],
		       100.0 * (double)block_insns[b] / (double)total);
	}
	free(order);

	printf("\nstores into a page the guest has executed: %llu\n",
	       (unsigned long long)stores_into_code);
	printf("pages invalidated (first write after an execution): %llu, "
	       "one every %.0f instructions\n", (unsigned long long)invalidations,
	       invalidations ? (double)all.total / (double)invalidations : 0.0);
	printf("distinct pages ever invalidated: %llu of %llu executed\n",
	       (unsigned long long)invalidated_pages,
	       (unsigned long long)executed_pages());
}

/* ---- main --------------------------------------------------------------- */

static long load(const char *path, uint8_t *where, size_t room)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return -1; }
	long n = (long)fread(where, 1, room, f);
	fclose(f);
	return n;
}

/* The device tree says how much RAM there is and it was written for whatever
   the author had; riscv.c rewrites the memory node in place and so does this,
   so that the guest sees the same machine in both. */
static void dtb_set_ram(uint8_t *dtb, size_t length, uint32_t size)
{
	static const uint8_t pattern[12] = { 0,0,0,0, 0x80,0,0,0, 0,0,0,0 };
	for (size_t i = 0; i + 16 <= length; i += 4) {
		if (memcmp(dtb + i, pattern, sizeof pattern) != 0)
			continue;
		uint32_t was = ((uint32_t)dtb[i + 12] << 24) | ((uint32_t)dtb[i + 13] << 16)
			| ((uint32_t)dtb[i + 14] << 8) | dtb[i + 15];
		if (!was || was > 0x20000000)
			continue;
		dtb[i + 12] = (uint8_t)(size >> 24); dtb[i + 13] = (uint8_t)(size >> 16);
		dtb[i + 14] = (uint8_t)(size >> 8);  dtb[i + 15] = (uint8_t)size;
		return;
	}
	fprintf(stderr, "no memory node in the device tree\n");
}

int main(int argc, char **argv)
{
	const char *image = NULL, *dtb = NULL, *until = NULL;
	uint64_t limit = 200000000, phase_every = 0;
	ram_size = DEFAULT_RAM;

	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		if (!strcmp(a, "--image") && i + 1 < argc) image = argv[++i];
		else if (!strcmp(a, "--dtb") && i + 1 < argc) dtb = argv[++i];
		else if (!strcmp(a, "--limit") && i + 1 < argc) limit = strtoull(argv[++i], 0, 0);
		else if (!strcmp(a, "--ram") && i + 1 < argc) ram_size = (uint32_t)(strtoul(argv[++i], 0, 0) * 1024 * 1024);
		else if (!strcmp(a, "--until") && i + 1 < argc) until = argv[++i];
		else if (!strcmp(a, "--type") && i + 1 < argc) feed = argv[++i];
		else if (!strcmp(a, "--phase") && i + 1 < argc) phase_every = strtoull(argv[++i], 0, 0);
		else if (!strcmp(a, "--quiet")) quiet = true;
		else if (!image) image = a;
		else { fprintf(stderr, "unknown argument %s\n", a); return 2; }
	}
	if (!image) {
		fprintf(stderr, "usage: %s --image FILE [--dtb FILE] [--ram MB] "
			"[--limit N] [--until TEXT] [--type TEXT] [--phase N] [--quiet]\n",
			argv[0]);
		return 2;
	}

	ram = calloc(1, ram_size);
	seen = calloc(1, ram_size / 4);
	code_page = calloc(1, ram_size / 4096 + 1);
	invalidated = calloc(1, ram_size / 4096 + 1);
	block_pc = calloc(BLOCK_SIZE, sizeof *block_pc);
	block_insns = calloc(BLOCK_SIZE, sizeof *block_insns);
	block_len = calloc(BLOCK_SIZE, sizeof *block_len);
	block_touched = calloc(BLOCK_SIZE, sizeof *block_touched);
	block_live_in = calloc(BLOCK_SIZE, sizeof *block_live_in);
	if (!ram || !seen || !code_page || !invalidated || !block_touched
	    || !block_live_in || !block_pc || !block_insns || !block_len) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	long size = load(image, ram, ram_size - DTB_RESERVE);
	if (size <= 0)
		return 1;
	fprintf(stderr, "%s: %ld bytes into %u KiB of guest RAM\n",
		image, size, ram_size / 1024);

	uint32_t dtb_at = 0;
	if (dtb) {
		uint8_t *where = ram + ram_size - DTB_RESERVE;
		long n = load(dtb, where, DTB_RESERVE);
		if (n <= 0)
			return 1;
		dtb_set_ram(where, (size_t)n, ram_size - DTB_RESERVE);
		dtb_at = RV_RAM_BASE + ram_size - DTB_RESERVE;
	}

	machine.ram = ram;
	machine.ram_size = ram_size;
	machine.putchar = host_putchar;
	machine.getchar = host_getchar;
	rv32_reset(&machine, RV_RAM_BASE, dtb_at);

	uint32_t block_head = machine.pc;
	uint32_t block_run = 0;
	cur_block = block_index(block_head);
	uint64_t phase_no = 0;
	rv32_stop_t stop = RV_RAN_OUT;

	while (all.total < limit) {
		uint32_t pc = machine.pc;
		uint64_t before = machine.retired;
		bool fetchable = in_ram(pc) && !(pc & 3);
		uint32_t ir = fetchable ? at(pc) : 0;

		stop = rv32_run(&machine, 1, 1);
		if (stop != RV_RAN_OUT) {
			if (fetchable)
				classify(pc, ir);   /* the store that stopped it */
			break;
		}
		if (machine.retired == before)
			continue;            /* a trap, or waiting for an interrupt */

		if (!fetchable) {
			all.traps++;
			block_head = machine.pc;
			block_run = 0;
			cur_block = block_index(block_head);
			run_written = run_live_in = 0;
			continue;
		}

		uint32_t slot = (pc - RV_RAM_BASE) >> 2;
		if (!seen[slot]) { seen[slot] = 1; seen_count++; }
		code_page[(pc - RV_RAM_BASE) >> 12] = 1;
		block_run++;

		bool ends = classify(pc, ir);
		/* A trap taken by the instruction itself also ends the block. */
		if (!ends && machine.pc != pc + 4)
			ends = true;
		if (ends) {
			block_insns[cur_block] += block_run;
			if (block_run > block_len[cur_block])
				block_len[cur_block] = block_run;
			block_touched[cur_block] |= run_written | run_live_in;
			block_live_in[cur_block] |= run_live_in;
			all.blocks++; phase.blocks++;
			block_head = machine.pc;
			block_run = 0;
			cur_block = block_index(block_head);
			run_written = run_live_in = 0;
		}

		if (until && !prompted && tail_len >= strlen(until)
		    && !memcmp(tail + tail_len - strlen(until), until, strlen(until))) {
			prompted = true;
			fprintf(stderr, "\n[reached \"%s\" after %llu instructions]\n",
				until, (unsigned long long)all.total);
			if (!feed)
				break;
		}
		if (phase_every && all.total % phase_every == 0) {
			char title[64];
			snprintf(title, sizeof title, "phase %llu",
				 (unsigned long long)++phase_no);
			report_counts(title, &phase);
			memset(&phase, 0, sizeof phase);
		}
	}

	fflush(stdout);
	fprintf(stderr, "\nstopped: %s, pc %08x, %llu instructions\n",
		stop == RV_POWEROFF ? "power off" : stop == RV_FAULT ? "fault"
		: stop == RV_REBOOT ? "reboot" : "limit or prompt",
		machine.pc, (unsigned long long)all.total);

	report_counts("whole run", &all);
	report_footprint();
	report_pressure();
	report_ranges(12);
	report_phases(12);
	return 0;
}
