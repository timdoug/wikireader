/* riscv.c - run an rv32ima image on the WikiReader and report what it cost.
 *
 * Step one of putting Linux on this thing: no display console and no
 * keyboard yet, just the interpreter, a bare-metal guest, and an honest
 * cycles-per-guest-instruction number for each kind of work.  The C33's
 * timer counts at 60 per microsecond and MCLK is 60 MHz, so a timer_get()
 * delta is a cycle count directly.
 */

#include <grifo.h>
#include <regs.h>
#include <string.h>

#include "console.h"
#include "jit_probe.h"
#include "rv32.h"
#ifdef RV32_JIT
#include "rv32_jit.h"
#endif

/* Guest RAM is whatever the board has spare.  ram_size() reads the SDRAM
   controller, so this gives the guest 28 MB on a 32 MB board and 12 on a
   16 MB one, rather than a constant chosen for whichever machine happened
   to be in front of the author.  The reserve covers Grifo, this
   application and the allocator's own headroom. */
/* What the SDRAM controller was programmed for.  This is ram_size() from
   samo-lib/include/boards/samo_a1.h, reimplemented on the register rather
   than included: that header pulls in the drivers' delay.h, whose
   delay_us() disagrees with Grifo's. */
static unsigned long board_ram(void)
{
	switch (REG_SDRAMC_CTL & ADDRC_MASK) {
	case ADDRC_32M_x_16_bits_x_1: return 64u * 1024 * 1024;
	case ADDRC_16M_x__8_bits_x_2: return 32u * 1024 * 1024;
	case ADDRC_16M_x_16_bits_x_1: return 32u * 1024 * 1024;
	case ADDRC__8M_x__8_bits_x_2: return 16u * 1024 * 1024;
	case ADDRC__8M_x_16_bits_x_1: return 16u * 1024 * 1024;
	default:                      return  4u * 1024 * 1024;
	}
}

#define RAM_RESERVE (4u * 1024 * 1024)
#define RAM_MINIMUM (6u * 1024 * 1024)

/* The device tree goes in the last page of guest RAM, which is where the
   memory node stops, and its address is what the guest finds in a1. */
#define DTB_RESERVE (16u * 1024)

static uint8_t *guest_ram;
static uint32_t guest_ram_size;
#ifdef RV32_JIT
static void *jit_arena;
static uint32_t jit_bytes;
#endif

static rv32_t machine RV32_STATE;

/* The guest's console: the panel, and the emulator's stdout so a run can
   be read back from a log. */
static void console_out(void *arg, int c)
{
	(void)arg;
	console_put(c);
	debug_print_char(c);
}

/* Say something on the panel and the serial log at once.  The panel is the
   only sign of life a user has: loading the kernel off the card takes a
   while and the guest runs for a while more before its first printk, and
   without this the machine looks wedged for all of it. */
static void say(const char *s)
{
	for (; *s; ++s)
		console_out(0, *s);
}

static int console_in(void *arg)
{
	(void)arg;
	return console_get();
}

/* ---- measurement ------------------------------------------------------- */

enum { MAX_MARKS = 32 };

static struct {
	uint32_t id;
	uint32_t retired;
	uint64_t cycles;
} marks[MAX_MARKS];
static unsigned mark_count;

/* timer_get() is 32 bits and wraps every 71 seconds at 60 MHz, so elapsed
   time is accumulated from deltas taken far more often than that. */
static uint32_t last_tick;
static uint64_t elapsed_cycles;

static void tick(void)
{
	uint32_t now = timer_get();
	elapsed_cycles += (uint32_t)(now - last_tick);
	last_tick = now;
}

static void on_mark(void *arg, uint32_t id)
{
	(void)arg;
	tick();
	if (mark_count < MAX_MARKS) {
		marks[mark_count].id = id;
		marks[mark_count].retired = (uint32_t)machine.retired;
		marks[mark_count].cycles = elapsed_cycles;
		++mark_count;
	}
}

static const char *const kernel_name[] = {
	"end", "alu", "branch", "mul", "div", "load",
	"store", "copy", "bytes", "crc", "sieve",
};

/* Reporting arithmetic, done on 32-bit limbs on purpose.  A 64-bit divide
   pulls libgcc's __udivmoddi4 into the link, and that calls the 32-bit
   division helpers the interpreter has moved into internal RAM (memory.lds)
   -- which a short call from SDRAM cannot reach.  These run a handful of
   times per benchmark, so a bit-at-a-time loop costs nothing. */
static uint64_t divmod_u64(uint64_t value, uint64_t divisor, uint64_t *rem)
{
	uint64_t quotient = 0, r = 0;
	for (int bit = 63; bit >= 0; --bit) {
		r = (r << 1) | ((value >> bit) & 1);
		quotient <<= 1;
		if (r >= divisor) {
			r -= divisor;
			quotient |= 1;
		}
	}
	*rem = r;
	return quotient;
}

/* The report goes out three ways at once.  A serial cable is the developer's
   view and the only one the emulator reads, but a device on a desk has
   neither that nor a screen once the run cuts the power rail -- so the
   numbers are also buffered and written to the card, which is the copy that
   survives.  The panel is 40 columns and gets the summary only; the table
   below is half as wide again and would wrap into nonsense. */
/* Eighteen template rows and a header outgrew the kilobyte this was, and a
   truncated report is exactly the kind of quiet loss the card copy exists to
   prevent.  It is ordinary .bss in SDRAM and costs nothing. */
static char report_buf[2048];
static unsigned report_used;
static bool report_truncated;
static bool report_to_panel;
static bool hold_at_end;
static bool probe_only;

static void report_char(char c)
{
	debug_print_char(c);
	if (report_to_panel)
		console_put(c);
	if (report_used < sizeof report_buf)
		report_buf[report_used++] = c;
	else
		report_truncated = true;
}

static void report(const char *s)
{
	for (; *s; ++s)
		report_char(*s);
}

/* mini-libc's printf has no long long, and these counts outgrow 32 bits on
   a long run. */
static void report_u64(uint64_t v)
{
	char buf[21];
	int n = 0;
	if (!v) {
		report_char('0');
		return;
	}
	while (v) {
		uint64_t digit;
		v = divmod_u64(v, 10, &digit);
		buf[n++] = (char)('0' + (uint32_t)digit);
	}
	while (n)
		report_char(buf[--n]);
}

/* The table is read down its columns, so every field is padded here rather
   than by a printf this libc does not have. */
static void report_pad(unsigned width, unsigned printed)
{
	while (printed++ < width)
		report_char(' ');
}

static unsigned digits(uint64_t v)
{
	unsigned n = 1;
	uint64_t ignored;
	while ((v = divmod_u64(v, 10, &ignored)) != 0)
		++n;
	return n;
}

static void report_right(uint64_t v, unsigned width)
{
	report_pad(width, digits(v));
	report_u64(v);
}

static void report_left(const char *s, unsigned width)
{
	report(s);
	report_pad(width, (unsigned)strlen(s));
}

/* Cycles per instruction to two decimals, without floating point. */
static void report_cpi(uint64_t cycles, uint32_t insns)
{
	uint64_t ignored;
	uint32_t hundredths = insns
		? (uint32_t)divmod_u64(cycles * 100 + insns / 2, insns, &ignored) : 0;
	report_u64(hundredths / 100);
	report_char('.');
	report_char((char)('0' + (hundredths / 10) % 10));
	report_char((char)('0' + hundredths % 10));
}

/* Thousands of guest instructions per second of wall clock. */
static void report_kips(uint64_t cycles, uint32_t insns)
{
	/* insns / (cycles / 60e6) in thousands = insns * 60000 / cycles */
	uint64_t ignored;
	report_u64(cycles ? divmod_u64((uint64_t)insns * 60000u, cycles, &ignored) : 0);
}

/* Write the report beside the image it measured: rvbench.bin -> rvbench.txt,
   unless a name was given on the command line.  That matters when several
   builds of this application measure the same image -- the placement
   variants do -- because otherwise each run would overwrite the last.
   file_create is FA_CREATE_ALWAYS, so a previous run's numbers are replaced
   rather than appended to: the file always describes the run that just
   finished.  Returns the name it used, or NULL if the card would not take
   it, which is what decides whether the machine may power off afterwards. */
static const char *report_save(const char *image, const char *named)
{
	static char path[64];
	size_t len = strlen(named ? named : image);
	if (len < 4 || len >= sizeof path)
		return NULL;
	memcpy(path, named ? named : image, len + 1);
	memcpy(path + len - 3, "txt", 4);
	/* file_create returns a handle, not a status, despite its type. */
	int handle = file_create(path, FILE_OPEN_WRITE);
	if (handle < 0)
		return NULL;
	ssize_t wrote = file_write(handle, report_buf, report_used);
	bool ok = file_close(handle) == FILE_ERROR_OK && wrote == (ssize_t)report_used;
	return ok ? path : NULL;
}

/* ---- the translator probe ------------------------------------------------
 *
 * What translated code would cost, measured by running some.  jit_probe.s
 * holds the sequences a translator would emit for blocks a Linux boot
 * actually spends its time in; this copies each one into SDRAM and then into
 * A0 RAM and times it in both, because a code cache in internal RAM is about
 * twice as cheap to fetch from and there is only room for a few kilobytes.
 *
 * It exists because the emulator cannot answer this.  Its model is known to
 * charge SDRAM-resident code about ten percent too much, and code walking two
 * streams twenty to thirty percent too much (emulator/README.md), and
 * translated code is both of those at once.  Every other number in this file
 * the model gets within a few percent; this one has to come off the device.
 */

/* Enough for the longest template that is measured in A0 RAM -- alu_reg, at
   180 bytes -- and not a byte more, because this competes with the hot path
   for the same 5 KB.  A template too big for it is reported as such rather
   than silently skipped; the exit chains are 474 bytes and SDRAM-only for
   this reason. */
#define JIT_CODE_BYTES 192
/* ".fastcode.probe" rather than ".fastcode": the linker script places the
   plain section first and the dotted ones after it, so the interpreter keeps
   the addresses it was measured at and the buffer takes what is left.
   Under the placement switch like everything else, because compare.py checks
   that the all-in-SDRAM build really has nothing in internal RAM and a
   probe buffer sitting there unconditionally is exactly the kind of thing
   that check exists to catch. */
#ifdef RV32_FASTCODE
static uint8_t jit_a0_code[JIT_CODE_BYTES]
	__attribute__((section(".fastcode.probe"))) __attribute__((aligned(4)));
#else
static uint8_t jit_a0_code[JIT_CODE_BYTES] __attribute__((aligned(4)));
#endif

/* The stream the copy and load templates walk.  Two of them, a megabyte
   apart, so the pair behaves like a real copy rather than like one open
   row. */
#define JIT_STREAM (256u * 1024)

static uint32_t jit_regs[32] RV32_STATE;
static uint32_t jit_table[2 * JP_BLOCKS];

static void jit_probe(void)
{
	static const struct {
		const char *name;
		const void *start;
		const uint8_t *end;
		uint32_t guest;     /* guest instructions a pass stands for */
		uint32_t passes;    /* 0: the template runs the stream instead */
		uint32_t repeats;
		unsigned check;
	} templates[] = {
#define JP_ROW(name, guest, passes, repeats, check) \
		{ #name, (const void *)jp_##name, jp_##name##_end, \
		  guest, passes, repeats, check },
		JP_TEMPLATES(JP_ROW)
#undef JP_ROW
	};

	uint8_t *stream = memory_allocate(2 * JIT_STREAM + 4096, "jit stream");
	uint8_t *code = memory_allocate(4096 + 1024, "jit code");
	if (!stream || !code) {
		say("no memory for the probe\n");
		return;
	}
	/* Both buffers on a row boundary: ubench measured a malloc'd stream
	   swinging a two-stream loop by 19% on its alignment alone, so where
	   these land is recorded rather than left to chance. */
	uint8_t *src = (uint8_t *)(((uint32_t)stream + 1023) & ~1023u);
	uint8_t *dst = src + JIT_STREAM;
	uint8_t *sdram_code = (uint8_t *)(((uint32_t)code + 1023) & ~1023u);

	struct jit_ctx ctx;
	ctx.regs = jit_regs;
	ctx.ram = src;
	ctx.ram_end = src + 2 * JIT_STREAM;
	/* The templates hold guest addresses in the register file and convert
	   them the way rv32_hot.s does, so the probe needs a guest address
	   space: RV_RAM_BASE maps onto the stream. */
	ctx.adj = (uint32_t)src - RV_RAM_BASE;
	ctx.table = jit_table;

	report("\nrv32: jit template     where  bytes  guest    cyc/pass  cyc/guest\n");
	debug_printf("rv32: code sdram %08lx a0 %08lx, stream %08lx\n",
		     (unsigned long)sdram_code, (unsigned long)jit_a0_code,
		     (unsigned long)src);

	for (unsigned t = 0; t < sizeof templates / sizeof templates[0]; ++t) {
		uint32_t bytes = (uint32_t)(templates[t].end -
					    (const uint8_t *)templates[t].start);
		for (unsigned place = 0; place < 2; ++place) {
			uint8_t *where = place ? jit_a0_code : sdram_code;
			if (place && bytes > sizeof jit_a0_code) {
				report("rv32: ");
				report_left(templates[t].name, 12);
				report(" a0ram  does not fit\n");
				continue;
			}
			memcpy(where, templates[t].start, bytes);

			/* The lookup's targets are addresses inside the copy,
			   so they exist only once the copy does -- which is
			   the one thing in here a translator would also have
			   to do.  The tags are what jit_probe.s hashes: block
			   i answers to 0x1000 + i*4, so (tag >> 2) & 7 is i. */
			if (templates[t].start == (const void *)jp_exit_hash) {
				uint32_t head = (uint32_t)(jp_exit_hash_blocks -
							   (const uint8_t *)jp_exit_hash);
				if (bytes != head + JP_BLOCKS * JP_SLOT) {
					report("rv32: exit_hash layout mismatch\n");
					continue;
				}
				for (unsigned b = 0; b < JP_BLOCKS; ++b) {
					jit_table[2 * b] = 0x1000 + b * 4;
					jit_table[2 * b + 1] =
						(uint32_t)where + head + b * JP_SLOT;
				}
			}

			/* The copy templates run until the source reaches the
			   end, so their pass count is the stream and not an
			   argument; every other one is told how many. */
			bool walks_stream = templates[t].passes == 0;
			uint32_t passes = walks_stream ? JIT_STREAM / 4
						       : templates[t].passes;
			uint64_t cycles = 0;
			bool ran = true;

			for (unsigned r = 0; r < templates[t].repeats; ++r) {
				for (unsigned i = 0; i < 32; ++i)
					jit_regs[i] = 0;
				jit_regs[11] = RV_RAM_BASE;                 /* a1 */
				jit_regs[13] = RV_RAM_BASE + JIT_STREAM;    /* a3 */
				jit_regs[31] = RV_RAM_BASE + JIT_STREAM;    /* t6 */
				ctx.src = src;
				ctx.dst = dst;
				ctx.end = src + JIT_STREAM;

				watchdog(WATCHDOG_KEY);
				uint32_t t0 = timer_get();
				uint32_t left = ((jp_fn)where)(passes, &ctx);
				cycles += (uint32_t)(timer_get() - t0);
				watchdog(WATCHDOG_KEY);

				/* A template that fails a bound check leaves
				   through its epilogue having done part of the
				   work, and the number that comes out of that
				   is a fast lie -- ld_check first measured
				   half the cost of ld_free, which does
				   strictly less.  So every run says where it
				   left the register it walks with. */
				(void)left;
				switch (templates[t].check) {
				case JP_WALK:
					ran = jit_regs[11] ==
						RV_RAM_BASE + 16 * passes;
					break;
				case JP_STREAM:
					ran = jit_regs[11] ==
						RV_RAM_BASE + JIT_STREAM;
					break;
				case JP_DRAINED:
					ran = left == 0;
					break;
				case JP_COPIED:
					ran = *(uint32_t *)(dst + JIT_STREAM - 4)
						== *(uint32_t *)(src + JIT_STREAM - 4);
					break;
				}
				if (!ran)
					break;
			}
			if (!ran) {
				report("rv32: ");
				report_left(templates[t].name, 12);
				report(place ? " a0ram" : " sdram");
				report("  stopped early\n");
				continue;
			}

			uint64_t guest = (uint64_t)passes * templates[t].guest
				* templates[t].repeats;
			passes *= templates[t].repeats;
			report("rv32: ");
			report_left(templates[t].name, 12);
			report_left(place ? " a0ram" : " sdram", 7);
			report_right(bytes, 6);
			report_right(templates[t].guest, 7);
			report("  ");
			report_cpi((uint64_t)cycles, passes);
			report("  ");
			report_cpi((uint64_t)cycles, (uint32_t)guest);
			report_char('\n');
		}
	}
}

/* ---- image loading ------------------------------------------------------ */

/* The device tree states how much RAM the guest has, and it was built for
   whatever size the author of the .dts picked.  The board decides instead,
   so the memory node is rewritten in place: its reg is
   <0 RV_RAM_BASE 0 size>, four big-endian words, and only the last needs
   changing.  Matching on the address as well as the zero cells keeps this
   from finding some other property that happens to start with zeros. */
static bool dtb_set_ram(uint8_t *dtb, size_t length, uint32_t size)
{
	static const uint8_t pattern[12] = {
		0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0
	};
	for (size_t i = 0; i + 16 <= length; i += 4) {
		if (memcmp(dtb + i, pattern, sizeof pattern) != 0)
			continue;
		uint32_t was = ((uint32_t)dtb[i + 12] << 24) | ((uint32_t)dtb[i + 13] << 16) |
			       ((uint32_t)dtb[i + 14] << 8) | dtb[i + 15];
		if (was == 0 || was > 0x20000000)
			continue;      /* not a memory size */
		dtb[i + 12] = (uint8_t)(size >> 24);
		dtb[i + 13] = (uint8_t)(size >> 16);
		dtb[i + 14] = (uint8_t)(size >> 8);
		dtb[i + 15] = (uint8_t)size;
		debug_printf("rv32: device tree RAM %lu KiB -> %lu KiB\n",
			     (unsigned long)(was / 1024), (unsigned long)(size / 1024));
		return true;
	}
	debug_print("rv32: no memory node found in the device tree\n");
	return false;
}

static long load_file(const char *path, uint8_t *where, size_t room)
{
	int handle = file_open(path, FILE_OPEN_READ);
	if (handle < 0)
		return handle;
	long total = 0;
	for (;;) {
		ssize_t n = file_read(handle, where + total, room - (size_t)total);
		if (n <= 0)
			break;
		total += n;
		watchdog(WATCHDOG_KEY);
	}
	file_close(handle);
	return total;
}

int grifo_main(int argc, char **argv)
{
	/* Grifo's own argv describes the boot ("grifo-kernel", "auto-boot"),
	   so an image is recognised by its extension rather than position. */
	const char *path = NULL, *report_name = NULL;
	for (int i = 1; i < argc; ++i) {
		/* "hold" keeps the panel up at the end instead of powering off.
		   The benchmark takes under two seconds on the device, so
		   without it the summary is on screen for less time than it
		   takes to look up.  Off by default: in the emulator the wait
		   would be minutes of modelled time for a number the log
		   already has. */
		if (strcmp(argv[i], "hold") == 0)
			hold_at_end = true;
		/* "jit" measures translated code instead of running a guest:
		   no image, no interpreter, just the templates. */
		if (strcmp(argv[i], "jit") == 0)
			probe_only = true;
		size_t len = strlen(argv[i]);
		if (!path && len > 4 && strcmp(argv[i] + len - 4, ".bin") == 0)
			path = argv[i];
		/* Where to write the report, when the default name would
		   collide with another build measuring the same image. */
		if (!report_name && len > 4 && strcmp(argv[i] + len - 4, ".txt") == 0)
			report_name = argv[i];
	}
	if (!path)
		path = "rvbench.bin";

	/* The dispatch table is linked into the window buffer (memory.lds),
	   so the window must not be drawing from it. */
	lcd_window_disable();
	console_init();
	say("rv32ima\n");

	if (probe_only) {
		say("Translator probe...\n");
		last_tick = timer_get();
		jit_probe();
		if (report_truncated)
			report("rv32: report truncated\n");
		report_to_panel = true;
		report("\njit probe done\n");
		const char *saved = report_save("rvjit.bin", report_name);
		if (saved) {
			say("saved ");
			say(saved);
			say("\n");
		} else {
			say("not saved to the card.\n");
		}
		if (saved && !hold_at_end)
			power_off();
		say("press a key when you have read this.\n");
		for (;;) {
			console_poll();
			watchdog(WATCHDOG_KEY);
			if (console_get() >= 0)
				break;
		}
		power_off();
	}
	say("Loading ");
	say(path);
	say("...\n");

#ifdef RV32_JIT
	/* The code cache is claimed before the guest, which then takes whatever
	   is left: a translator with nowhere to put its output is no use, and a
	   megabyte holds far more than the 385 KiB of guest code a whole Linux
	   boot executes even at this translation's density. */
	{
		/* An eighth of the board, which is four megabytes on this one
		   and two on a 16 MB one.  A Linux boot executes 385 KiB of
		   guest code and this translation is about thirty bytes a
		   guest instruction, so the whole boot fits and never has to
		   be thrown away -- and throwing it away is expensive out of
		   all proportion: at one megabyte the cache filled ten times
		   over a boot, every block was translated five times, and the
		   translator was 80% of the run. */
		uint32_t want = (uint32_t)board_ram() / 8;

		if (want > 4u * 1024 * 1024)
			want = 4u * 1024 * 1024;
		jit_arena = memory_allocate(want, "rv32 jit");
		jit_bytes = jit_arena ? want : 0;
	}
#endif

	/* Take as much as the allocator will give, largest first. */
	for (guest_ram_size = (uint32_t)board_ram() - RAM_RESERVE;
	     guest_ram_size >= RAM_MINIMUM; guest_ram_size -= 1024 * 1024) {
		guest_ram = memory_allocate(guest_ram_size, "rv32 guest");
		if (guest_ram)
			break;
	}
	if (!guest_ram) {
		say("no memory for the guest\n");
		power_off();
	}
	debug_printf("rv32: board has %lu KiB, guest gets %lu KiB\n",
		     board_ram() / 1024, (unsigned long)(guest_ram_size / 1024));

	long size = load_file(path, guest_ram, guest_ram_size - DTB_RESERVE);
	if (size <= 0) {
		debug_printf("rv32: cannot load %s (%ld)\n", path, size);
		power_off();
	}
	debug_printf("rv32: %s, %ld bytes into %lu KiB of guest RAM\n",
		     path, size, (unsigned long)(guest_ram_size / 1024));

	/* A device tree beside the image turns this into a Linux boot: the
	   kernel takes the memory map, the console and the timer frequency
	   from it, and finds it through a1. */
	uint32_t dtb_at = 0;
	char dtb_path[64];
	size_t len = strlen(path);
	if (len > 4 && len < sizeof dtb_path - 1) {
		strcpy(dtb_path, path);
		strcpy(dtb_path + len - 4, ".dtb");
		uint8_t *where = guest_ram + guest_ram_size - DTB_RESERVE;
		long dtb = load_file(dtb_path, where, DTB_RESERVE);
		if (dtb > 0) {
			dtb_set_ram(where, (size_t)dtb, guest_ram_size - DTB_RESERVE);
			dtb_at = RV_RAM_BASE + guest_ram_size - DTB_RESERVE;
			debug_printf("rv32: %s, %ld bytes at %08lx\n",
				     dtb_path, dtb, (unsigned long)dtb_at);
		}
	}

	say(dtb_at ? "Starting Linux...\n" : "Starting...\n");

	machine.ram = guest_ram;
	machine.ram_size = guest_ram_size;
#ifdef RV32_JIT
	/* After the guest, because the page map a store checks is sized to the
	   guest's RAM. */
	if (!jit_bytes || !rv32_jit_init(jit_arena, jit_bytes, guest_ram_size))
		say("no code cache; interpreting\n");
#endif
	machine.putchar = console_out;
	machine.getchar = console_in;
	machine.mark = on_mark;
	rv32_reset(&machine, RV_RAM_BASE, dtb_at);

	/* A batch is short enough to keep the watchdog happy and long enough
	   that the per-call setup does not show up in the measurement. */
	enum { BATCH = 16384 };
	rv32_stop_t stop = RV_RAN_OUT;
	last_tick = timer_get();
	for (unsigned long batch = 0; batch < 2000000ul; ++batch) {
		stop = rv32_run(&machine, BATCH, BATCH);
		console_poll();
		/* Every batch, not just at the markers: timer_get() is 32 bits
		   and a slow kernel can run for more than the 71 seconds that
		   covers, which silently loses 2^32 cycles from the total. */
		tick();
		watchdog(WATCHDOG_KEY);
		if (stop != RV_RAN_OUT)
			break;
	}
	tick();

	if (stop == RV_FAULT) {
		debug_printf("rv32: fault, mcause %lu at pc %08lx\n",
			     (unsigned long)machine.mcause,
			     (unsigned long)machine.pc);
		power_off();
	}

	report("\nrv32: kernel        insns      cycles    cyc/insn    kIPS\n");
	for (unsigned i = 0; i + 1 < mark_count; ++i) {
		uint32_t id = marks[i].id;
		const char *name = id < sizeof kernel_name / sizeof kernel_name[0]
			? kernel_name[id] : "?";
		uint32_t insns = marks[i + 1].retired - marks[i].retired;
		uint64_t cycles = marks[i + 1].cycles - marks[i].cycles;
		report("rv32: ");
		report_left(name, 8);
		report_right(insns, 13);
		report_char(' ');
		report_u64(cycles);
		report("  ");
		report_cpi(cycles, insns);
		report("  ");
		report_kips(cycles, insns);
		report_char('\n');
	}
	report("rv32: total    ");
	report_u64(machine.retired);
	report(" insns, ");
	report_u64(elapsed_cycles);
	report(" cycles, ");
	report_cpi(elapsed_cycles, (uint32_t)machine.retired);
	report(" cyc/insn, ");
	report_kips(elapsed_cycles, (uint32_t)machine.retired);
	report(" kIPS\n");
	report("rv32: stopped, reason ");
	report_u64((uint32_t)stop);
	report_char('\n');
#ifdef RV32_JIT
	/* What the cache cost to fill and how often it was not enough: a
	   translator that spends its time translating is the failure mode the
	   cycle count alone would not name. */
	report("rv32: jit ");
	report_u64(rv32_jit.blocks);
	report(" blocks, ");
	report_u64(rv32_jit.bytes);
	report(" bytes, ");
	report_u64(rv32_jit.flushes);
	report(" flushes, ");
	report_u64(rv32_jit.entries);
	report(" entries, ");
	report_u64(rv32_jit.declines);
	report(" declines, ");
	report_u64(rv32_jit.stores);
	report(" watched stores\n");
#endif
	if (report_truncated)
		report("rv32: report truncated\n");

	/* The panel only now, and only the two numbers the run exists to
	   produce: it is 40 columns, and the table above is wider. */
	report_to_panel = true;
	report("\n");
	report_cpi(elapsed_cycles, (uint32_t)machine.retired);
	report(" cyc/insn, ");
	report_kips(elapsed_cycles, (uint32_t)machine.retired);
	report(" kIPS\n");

	/* Cut the power rail rather than spin: the emulator stops with the
	   machine, so a run costs the benchmark and nothing else.  But only
	   once the numbers exist somewhere other than this screen -- if the
	   card would not take them, powering off is what destroys them, so
	   the machine stays up until someone has read them off the panel. */
	const char *saved = report_save(path, report_name);
	if (saved) {
		say("saved ");
		say(saved);
		say("\n");
	} else {
		say("not saved to the card.\n");
	}
	if (saved && !hold_at_end)
		power_off();
	say("press a key when you have read this.\n");
	for (;;) {
		console_poll();
		watchdog(WATCHDOG_KEY);
		if (console_get() >= 0)
			break;
	}
	power_off();
}
