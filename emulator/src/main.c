/*
 * wremu -- WikiReader (Epson C33 / S1C33) full-system emulator.
 *
 * Milestone: load an ELF image, execute from its entry point, and trace.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "c33.h"
#include "mem.h"
#include "model.h"
#include "uart.h"
#include "sdcard.h"
#include "periph.h"
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "timer.h"
#include "wdt.h"
#include "itc.h"
#include "cmu.h"
#include "port.h"
#include "eeprom.h"
#include "sdramc.h"
#include "dma.h"

/* Scripted taps and button presses accepted on the command line. */
#define NSCRIPT 256
static unsigned long hold_cycles = 2000000UL;

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-t N] [-n N] [-m] <image.elf>\n"
		"  -t N   trace the first N instructions\n"
		"  -n N   stop after N instructions (default 1000000; unlimited with -g)\n"
		"  -m     trace unclaimed MMIO register accesses\n"
		"  -s     trace grifo syscalls by name\n"
		"  -A     accepted for compatibility; alignment traps are always on\n"
		"  -g     show the panel in a live SDL2 window\n"
		"  -S N   window scale factor (default 3)\n"
		"  -T x,y,c  scripted tap at pixel x,y on cycle c\n"
		"  -K c,TEXT type TEXT on the on-screen keyboard from cycle c\n"
		"  -c F   attach FAT32 card image F\n"
		"  -R     open the card image read-only\n"
		"  --uart-input F  receive console bytes from F (- for stdin)\n"
		"  --uart-start N  first console byte cycle (default 1000000)\n"
		"  --uart-gap N    cycles between console bytes (default 50000)\n"
		"  -D A   dump memory starting at address A\n"
		"  -L N   memory dump length (default 64)\n"
		"  -O F   write the memory dump as binary file F\n"
		"  -X A[,NAME] count entries to address A without stopping\n"
		"  -Z A   time the input script from the first hit of A\n"
		"  -Y A,B profile only between the first hits of A and B\n"
		"  -y M,N profile only between guest times M and N, in ms\n"
		"  -F F   write every non-empty profile bucket to F\n"
		"  -M A,N arm probes/windows only after the Nth hit of A\n", p);
}


/* grifo's numbering: 0 random, 1 search, 2 history, 3 power. */
#define BUTTON_POWER_CODE 3

/* 6 bytes x 10 bits at CTP_BPS, in 60 MHz MCLK ticks. */
#define CTP_PACKET_MCLK  ((60000000ull * 6 * 10) / CTP_BPS)

static bool cmu_slp_auto_wake_cb(void *ctx)
{
	return cmu_slp_auto_wake(ctx);
}

/*
 * Hand whatever the window collected to the emulated hardware.
 *
 * Called from both the running and the idle path. It has to be, and that is
 * easy to get wrong: the idle path pumps SDL events and then goes back to
 * sleep, so if it does not also deliver them, every click while the device
 * sits idle -- which is most of them -- is collected and discarded.
 */
static void deliver_input(struct display *disp, struct port *port,
			  struct touch *touch, struct c33 *cpu,
			  unsigned long long *last_post)
{
	if (disp->button >= 0) {
		if (disp->button == BUTTON_POWER_CODE)
			port_power_button(port, cpu, disp->button_pressed);
		else
			port_button(port, cpu, (unsigned)disp->button,
				    disp->button_pressed);
		disp->button = -1;
	}

	/*
	 * The panel cannot deliver packets faster than the wire carries
	 * them: six bytes at CTP_BPS (9600), eight data bits with start and
	 * stop, is 6.25 ms. Deferring rather than dropping keeps every event.
	 *
	 * Report on change only, never a stream while the finger sits still.
	 * An earlier version did stream, on the theory that a stationary
	 * finger must keep reporting for scroll momentum to decay. That is
	 * wrong: wikilib arms a link with set_article_link_number(), which
	 * resets its activation timer on every touch event, and
	 * check_invert_link() will not promote the link until
	 * LINK_ACTIVATION_TIME_THRESHOLD (0.1 s) passes without one. A stream
	 * re-arms it forever and no link in an article can be tapped. The
	 * hardware cannot stream either, for the 6.25 ms reason above.
	 */
	if (disp->touch_pending &&
	    cpu->clk - *last_post < CTP_PACKET_MCLK) {
		/* too soon; it goes out next time round */
	} else if (disp->touch_pending) {
		*last_post = cpu->clk;
		disp->touch_pending = false;
		if (getenv("WREMU_TOUCH_TRACE"))
			fprintf(stderr, "  [touch %s %d,%d at %llu%s]\n",
				disp->touch_pressed ? "down" : "up",
				disp->touch_x, disp->touch_y,
				(unsigned long long)cpu->cycles,
				cpu->sleeping ? ", core halted" : "");
		touch_post(touch, cpu, disp->touch_x, disp->touch_y,
			   disp->touch_pressed);
	} else {
		touch_poll(touch, cpu);
	}
}


/*
 * What the mask ROM leaves behind: mbr, linked at 0, copied from the
 * EEPROM offset the FLASH map assigns it.
 */
#define MBR_EEPROM_OFFSET     0x1
#define MASK_ROM_LOAD_BYTES   512
/*
 * The mask ROM also leaves a usable stack. Nothing in samo-lib sets one up
 * before mbr's first call: application.lds defines __dp and the load
 * address but no stack symbol, and mbr.c only loads %r15. Internal RAM is
 * the only memory available this early -- mbr deliberately does not
 * initialise SDRAM ("but will be too big") -- so the stack starts at the
 * top of the 8K a0ram and grows down toward the loaded application.
 */
#define MASK_ROM_STACK_TOP    (IVRAM_BASE + IVRAM_SIZE)

/*
 * Bring the machine up from cold: reset every device and the core, and put
 * the boot image back where the hardware would find it.
 *
 * The emulator models the device's power state rather than tying it to its
 * own lifetime, so this runs again each time the power switch is pressed.
 */
static void machine_power_on(struct c33 *cpu, struct mem *mem,
			     struct port *port, struct itc *itc,
			     struct cmu *cmu, struct periph *periph,
			     struct sdramc *sdramc, struct lcd *lcd,
			     struct touch *touch, struct timerblk *timer,
			     struct sdcard *sd, struct wdt *wdt,
			     struct dma *dma,
			     struct eeprom *eeprom,
			     const char *path, uint32_t entry, uint32_t boot_sp)
{
	/*
	 * Power was removed, so nothing volatile survives. Do this before the
	 * boot image is placed, or it would be wiped straight back out.
	 */
	mem_clear_ram(mem);

	itc_reset(itc);
	cmu_reset(cmu);
	periph_reset(periph);
	port_reset(port);
	sdramc_reset(sdramc);
	lcd_reset(lcd);
	touch_reset(touch);
	timer_reset(timer);
	wdt_reset(wdt);
	dma_reset(dma);
	sd_reset(sd);
	if (eeprom)
		eeprom_deselect(eeprom);

	if (!path) {
		for (unsigned k = 0; k < MASK_ROM_LOAD_BYTES; k++)
			mem_write(mem, k, 1,
				  eeprom->data[MBR_EEPROM_OFFSET + k]);
	} else {
		char e[256];
		elf_load(mem, path, e, sizeof e);
	}

	c33_reset(cpu, entry);
	if (boot_sp) {
		cpu->sr[SR_SP] = boot_sp;
		cpu->sp_initialized = true;
	}
}


/*
 * A direct ELF boot skips the flash boot loader, and with it init_ram():
 * the SDRAM controller is never told what it is driving, so sdramc.c
 * charges nothing for anything and the machine appears to have no memory
 * system at all. That is not a small difference. It is most of why this
 * ran CoreMark five times faster than the device it models.
 *
 * So start the controller where the loader leaves it. The values are
 * init_ram()'s, from samo-lib/include/boards/samo_a1.h: the stock timings
 * tRP 4, tRAS 8 and tRC 15, auto-refresh 0x8c, arbitration and both queues
 * on, and the geometry the board revision selects. They go in through the
 * ordinary register writes so that the model derives everything from them
 * exactly as it would have if the guest had done it.
 */
static void sdramc_boot_state(struct mem *mem)
{
	const char *rev = getenv("WREMU_BOARD_REV");
	unsigned long r = rev ? strtoul(rev, NULL, 0) : 8;
	bool small = r == 8 || r == 6;   /* 16 MB boards; the rest are 32 */
	uint32_t ctl = ((4u - 1) << 12) | ((8u - 1) << 8) | ((15u - 1) << 4) |
		       (small ? 0x2u : 0x3u);

	mem_write(mem, REG_BASE + 0x1600, 4, 0);          /* INI: off first */
	mem_write(mem, REG_BASE + 0x1610, 4, 0x8000000b); /* ARBON|CAS1|APPON|IQB */
	mem_write(mem, REG_BASE + 0x1604, 4, ctl);
	mem_write(mem, REG_BASE + 0x1608, 4, 0x01ff008c); /* SCKON|SELEN|SELCO|AURCO */
	mem_write(mem, REG_BASE + 0x1600, 4, 0x14);       /* SDON|INIMRS */
}

int main(int argc, char **argv)
{
	model_init();
	const char *path = NULL, *card = NULL;
	unsigned long trace = 0, limit = 1000000;
	bool limit_given = false;
	bool trace_mmio = false;
	bool trace_syscalls = false;
	bool gui = false; int gui_scale = 3;
	bool profile = false;
	bool card_readonly = false;
	const char *uart_input = NULL;
	int uart_fd = -1;
	uint64_t uart_due = 1000000, uart_gap = 50000;
	if (getenv("WREMU_HOLD_MS"))
		hold_cycles = strtoul(getenv("WREMU_HOLD_MS"), NULL, 0) * 60000UL;
	unsigned long drag_spacing = 300000UL;
	if (getenv("WREMU_DRAG_MS"))
		drag_spacing = strtoul(getenv("WREMU_DRAG_MS"), NULL, 0) * 60000UL;
	bool pc_profile = false;
	/* Scripted taps and button presses; each -T/-N adds one. */
	struct script_tap { int x, y; unsigned long at; bool down_done, up_done; };
	struct script_btn { int code; unsigned long at; bool down_done, up_done; };
	struct script_tap taps[NSCRIPT]; unsigned ntaps = 0;
	struct script_btn btns[NSCRIPT]; unsigned nbtns = 0;
	struct script_drag { int x, y0, y1; unsigned long at; bool announced;
	unsigned step; };
	struct script_drag drags[NSCRIPT]; unsigned ndrags = 0;
	const char *eeprom_path = NULL;
/* How long a scripted press is held before release; WREMU_HOLD_MS overrides. */
#define HOLD_CYCLES  hold_cycles
/* Total span of a scripted drag, from its first packet to its last. */
	unsigned long long last_touch_post = 0;
	unsigned long long next_gui_pump = 0;
	unsigned long long idle_skipped = 0;
	unsigned long long idle_sd_powered = 0;
	/*
	 * About 100 Hz while idle: fast enough that a click still feels
	 * immediate, slow enough that repainting a screen nothing is drawing
	 * to is not what keeps the host busy.
	 */
#define IDLE_WAIT_MS  10
#define MCLK_HZ       60000000u
	uint32_t boot_sp = 0;
	const char *type_text = NULL;
	unsigned long type_at = 0, type_gap = 6000000;
	size_t type_idx = 0; int type_phase = 0;
	uint32_t watch = 0; bool watch_on = false;
	uint32_t bp[8]; unsigned nbp = 0;
	/*
	 * Probes: like a breakpoint that does not stop. Counting how often a
	 * function is entered, and at what point in the run, is what tells a
	 * regression in generated code apart from a loop that simply spins
	 * more while waiting for the same fixed-duration I/O.
	 */
#define NPROBE 16
	uint32_t probe[NPROBE]; const char *probe_name[NPROBE];
	unsigned long long probe_hits[NPROBE];
	unsigned long long probe_first_exec[NPROBE], probe_last_exec[NPROBE];
	unsigned long long probe_first_clk[NPROBE], probe_last_clk[NPROBE];
	/*
	 * The longest gap between two hits. On the event-loop poll this is
	 * the most useful number the emulator can produce about how the
	 * device feels: the longest single stretch the application went
	 * without looking for input, which is exactly how long a tap can sit
	 * unanswered. Counting calls cannot say this -- an idle loop that
	 * spins faster racks up more calls while doing less.
	 */
	/* Top NGAP stalls per probe, longest first -- one number is a
	   worst case, the list is a distribution, and the distribution is
	   what says whether a device feels responsive. */
#define NGAP 6
	unsigned long long probe_gap_exec[NPROBE][NGAP];
	unsigned long long probe_gap_clk[NPROBE][NGAP];
	unsigned long long probe_gap_at[NPROBE][NGAP];
	unsigned nprobe = 0;
	/* Callers of each probed address: the return address on the stack at
	   entry, the eight most frequent. */
#define NCALLER 8
	uint32_t probe_caller[NPROBE][NCALLER];
	unsigned long long probe_caller_n[NPROBE][NCALLER];
	memset(probe_caller_n, 0, sizeof probe_caller_n);
	memset(probe_hits, 0, sizeof probe_hits);
	memset(probe_gap_exec, 0, sizeof probe_gap_exec);
	memset(probe_gap_clk, 0, sizeof probe_gap_clk);
	/*
	 * Instructions actually stepped. cpu.cycles counts these *and* the
	 * fabricated ones the idle skip adds, so it cannot distinguish a
	 * build that did less work from one that waited less.
	 */
	unsigned long long executed = 0;
	/*
	 * Anchor: scripted input is timed from the first time the guest
	 * reaches this address, not from an absolute instruction count.
	 * Two builds of the same firmware do not retire the same number of
	 * instructions getting to the same screen, so a fixed -T cycle
	 * delivers the tap at a different point in each one's progress and
	 * the two runs are not the same interaction.
	 */
	uint32_t anchor = 0; bool script_armed = true;
	/*
	 * init.app and wiki.app are both linked at 0x10040000, so an address
	 * taken from one application's map can be hit while the other is
	 * running.  Everything below -- probes, the script anchor, profile
	 * windows -- stays disarmed until ADDR has been reached N times, which
	 * is how you say "not until the second application is loaded".
	 */
	uint32_t arm_addr = 0; unsigned long arm_n = 0, arm_seen = 0;
	bool armed = true;
	/*
	 * Profile only between two program events. Over a whole run the hot
	 * code is whatever the idle loop happens to be, which drowns out the
	 * one phase you care about.
	 */
	const char *prof_full_path = NULL;
	double prof_ms0 = 0, prof_ms1 = 0;
	char prof_win_label[64] = "";
	uint32_t prof_start = 0, prof_end = 0;
	bool prof_window = false, prof_done = false;
	bool window_repeat = getenv("WREMU_WINDOW_REPEAT") != NULL;
	unsigned long prof_windows = 0;
	unsigned long long prof_exec0 = 0, prof_clk0 = 0, prof_idle0 = 0;
	unsigned long long prof_exec = 0, prof_clk = 0, prof_idle = 0;
	uint32_t vwatch = 0; bool vwatch_on = false;
	uint32_t dump = 0; bool dump_on = false;
	unsigned long dump_len = 64;
	const char *dump_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc)
			trace = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			limit = strtoul(argv[++i], NULL, 0);
			limit_given = true;
		}
		else if (!strcmp(argv[i], "-e") && i + 1 < argc)
			eeprom_path = argv[++i];
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			card = argv[++i];
		else if (!strcmp(argv[i], "--uart-input") && i + 1 < argc)
			uart_input = argv[++i];
		else if ((!strcmp(argv[i], "--uart-start") ||
		          !strcmp(argv[i], "--uart-gap")) && i + 1 < argc) {
			bool gap = !strcmp(argv[i], "--uart-gap");
			char *end;
			const char *number = argv[++i];
			errno = 0;
			uint64_t value = strtoull(number, &end, 0);
			if (errno || end == number || *end || *number == '-' ||
			    (gap && value == 0)) {
				fprintf(stderr, "invalid UART cycle value: %s\n", number);
				return 1;
			}
			if (gap) uart_gap = value; else uart_due = value;
		}
		else if (!strcmp(argv[i], "-R"))
			card_readonly = true;
		else if (!strcmp(argv[i], "-m"))
			trace_mmio = true;
		else if (!strcmp(argv[i], "-s"))
			trace_syscalls = true;
		else if (!strcmp(argv[i], "-A")) {
			/* Compatibility: alignment traps are now architectural. */
		}
		else if (!strcmp(argv[i], "-P"))
			profile = true;
		else if (!strcmp(argv[i], "-H"))
			pc_profile = true;
		else if (!strcmp(argv[i], "-g"))
			gui = true;
		else if (!strcmp(argv[i], "-K") && i + 1 < argc) {
			/* type a string on the on-screen keyboard: -K cycle,TEXT */
			char *a = argv[++i];
			type_at = strtoul(a, &a, 0);
			if (*a == ',') type_text = a + 1;
		}
		else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
			/* scripted tap: -T x,y,cycle (repeatable) */
			struct script_tap t = { -1, -1, 0, false, false };
			if (sscanf(argv[++i], "%d,%d,%lu", &t.x, &t.y, &t.at) != 3 ||
			    t.x < 0 || ntaps >= NSCRIPT) {
				fprintf(stderr, "invalid or too many scripted tap events (max %d)\n", NSCRIPT);
				return 2;
			}
			taps[ntaps++] = t;
		}
		else if (!strcmp(argv[i], "-N") && i + 1 < argc) {
			/* scripted button: -N code,cycle  (0 random 1 search 2 history 3 power), repeatable */
			struct script_btn b = { -1, 0, false, false };
			if (sscanf(argv[++i], "%d,%lu", &b.code, &b.at) != 2 ||
			    b.code < 0 || nbtns >= NSCRIPT) {
				fprintf(stderr, "invalid or too many scripted button events (max %d)\n", NSCRIPT);
				return 2;
			}
			btns[nbtns++] = b;
		}
		else if (!strcmp(argv[i], "-G") && i + 1 < argc) {
			/* scripted drag: -G x,y0,y1,cycle (repeatable) */
			struct script_drag g = { -1, 0, 0, 0, false, 0 };
			if (sscanf(argv[++i], "%d,%d,%d,%lu", &g.x, &g.y0, &g.y1, &g.at) != 4 ||
			    g.x < 0 || ndrags >= NSCRIPT) {
				fprintf(stderr, "invalid or too many scripted drag events (max %d)\n", NSCRIPT);
				return 2;
			}
			drags[ndrags++] = g;
		}
		else if (!strcmp(argv[i], "-S") && i + 1 < argc)
			gui_scale = (int)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-D") && i + 1 < argc) {
			dump = strtoul(argv[++i], NULL, 0); dump_on = true;
		}
		else if (!strcmp(argv[i], "-L") && i + 1 < argc)
			dump_len = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-O") && i + 1 < argc)
			dump_path = argv[++i];
		else if (!strcmp(argv[i], "-V") && i + 1 < argc) {
			vwatch = strtoul(argv[++i], NULL, 0); vwatch_on = true;
		}
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
			if (nbp < 8) bp[nbp++] = strtoul(argv[++i], NULL, 0);
			else i++;
		}
		else if (!strcmp(argv[i], "-W") && i + 1 < argc) {
			watch = strtoul(argv[++i], NULL, 0); watch_on = true;
		}
		else if (!strcmp(argv[i], "-X") && i + 1 < argc) {
			/* probe: -X ADDR[,NAME] -- count entries, do not stop */
			char *a = argv[++i];
			if (nprobe < NPROBE) {
				probe[nprobe] = (uint32_t)strtoul(a, &a, 0);
				probe_name[nprobe] = (*a == ',') ? a + 1 : NULL;
				nprobe++;
			}
		}
		else if (!strcmp(argv[i], "-F") && i + 1 < argc) {
			prof_full_path = argv[++i];
			pc_profile = true;
		}
		else if (!strcmp(argv[i], "-y") && i + 1 < argc) {
			/* profile window by guest time: -y STARTMS,ENDMS */
			char *a = argv[++i];
			prof_ms0 = strtod(a, &a);
			if (*a == ',') prof_ms1 = strtod(a + 1, NULL);
			pc_profile = profile = true;
		}
		else if (!strcmp(argv[i], "-Y") && i + 1 < argc) {
			/* profile window: -Y STARTADDR,ENDADDR */
			char *a = argv[++i];
			prof_start = (uint32_t)strtoul(a, &a, 0);
			if (*a == ',') prof_end = (uint32_t)strtoul(a + 1, NULL, 0);
			pc_profile = profile = true;
		}
		else if (!strcmp(argv[i], "-M") && i + 1 < argc) {
			/* arm everything after the Nth hit of ADDR: -M ADDR,N */
			char *a = argv[++i];
			arm_addr = (uint32_t)strtoul(a, &a, 0);
			arm_n = (*a == ',') ? strtoul(a + 1, NULL, 0) : 1;
			armed = false;
		}
		else if (!strcmp(argv[i], "-Z") && i + 1 < argc) {
			anchor = strtoul(argv[++i], NULL, 0);
			script_armed = false;
		}
		else if (argv[i][0] != '-')
			path = argv[i];
		else { usage(argv[0]); return 2; }
	}
	if (!path && !eeprom_path) { usage(argv[0]); return 2; }

	/* Interactive runs should keep going until the window is closed. */
	if (gui && !limit_given)
		limit = ~0UL;

	struct mem mem;
	if (!mem_init(&mem)) {
		fprintf(stderr, "error: cannot allocate guest memory\n");
		return 1;
	}
	mem.trace_mmio = trace_mmio;
	mem.watch = watch; mem.watch_on = watch_on;
	mem.vwatch = vwatch;   /* enabled after load, below */

	struct uart uart;
	uart_attach(&mem, &uart, stdout);

	struct periph periph;
	periph_attach(&mem, &periph);

	struct lcd lcd;
	lcd_attach(&mem, &lcd);

	struct itc itc;
	itc_attach(&mem, &itc);
	uart.itc = &itc;
	if (uart_input) {
		uart_fd = !strcmp(uart_input, "-") ? STDIN_FILENO :
			open(uart_input, O_RDONLY | O_NONBLOCK);
		if (uart_fd < 0) {
			perror(uart_input);
			return 1;
		}
	}

	struct cmu cmu;
	cmu_attach(&mem, &cmu);

	struct touch touch;
	touch_attach(&mem, &touch, &itc);

	struct display disp;
	memset(&disp, 0, sizeof disp);
	disp.button = -1;
	if (gui && !display_open(&disp, &lcd, &mem, gui_scale))
		fprintf(stderr, "warning: could not open display window\n");

	struct port port;
	port_attach(&mem, &port, &itc);

	struct sdramc sdramc, sdramc_at_open, sdramc_at_close;
	uint64_t *row_hist_at_open = calloc(SDRAMC_ROW_HIST_ROWS, sizeof *row_hist_at_open);
	struct sdramc_pair *pair_hist_at_open = calloc(SDRAMC_PAIR_HIST_SIZE, sizeof *pair_hist_at_open);
	uint64_t *row_hist_at_close = calloc(SDRAMC_ROW_HIST_ROWS, sizeof *row_hist_at_close);
	struct sdramc_pair *pair_hist_at_close = calloc(SDRAMC_PAIR_HIST_SIZE, sizeof *pair_hist_at_close);
	memset(&sdramc_at_open, 0, sizeof sdramc_at_open);
	memset(&sdramc_at_close, 0, sizeof sdramc_at_close);
	sdramc_attach(&mem, &sdramc);

	static struct eeprom eeprom;
	if (eeprom_path && !eeprom_load(&eeprom, eeprom_path, stderr)) {
		fprintf(stderr, "error: cannot open eeprom image %s\n", eeprom_path);
		return 1;
	}

	struct sdcard sd;
	if (!sd_attach(&mem, &sd, card, &port,
		       eeprom_path ? &eeprom : NULL, card_readonly)) {
		fprintf(stderr, "error: cannot open card image %s\n", card);
		return 1;
	}
	sd.trace = trace_mmio;

	struct dma dma;
	dma_attach(&mem, &dma, &itc, &cmu, &sd);
	if (card)
		fprintf(stderr, "card: %s (%llu blocks)\n", card,
			(unsigned long long)sd.blocks);

	char err[256];
	uint32_t entry;
	if (!path) {
		/*
		 * Boot the way the device does, from the serial FLASH.
		 *
		 * The first stage is Epson's mask ROM, which is not in this
		 * repository and cannot be, so its effect is emulated rather
		 * than its code: it reads the first block out of the EEPROM
		 * into RAM and jumps to it. samo-lib/mbr is linked at address
		 * 0 (-Ttext=0) and the FLASH map places it at EEPROM offset 1
		 * (SAMO_A1.mapfile-default), which is what fixes those two
		 * numbers here. From that point on everything is real
		 * firmware: mbr loads menu, menu loads file-loader, and
		 * file-loader reads kernel.elf off the card.
		 */
		for (unsigned k = 0; k < MASK_ROM_LOAD_BYTES; k++)
			mem_write(&mem, k, 1, eeprom.data[MBR_EEPROM_OFFSET + k]);
		entry = 0;
		boot_sp = MASK_ROM_STACK_TOP;
		fprintf(stderr, "mask ROM: loaded %u bytes from eeprom+0x%x to RAM 0, sp=0x%x\n",
			MASK_ROM_LOAD_BYTES, MBR_EEPROM_OFFSET, boot_sp);
		fprintf(stderr, "entry point: 0x%08x (mbr)\n\n", entry);
	} else {
		fprintf(stderr, "loading %s\n", path);
		entry = elf_load(&mem, path, err, sizeof err);
		if (!entry) {
			fprintf(stderr, "error: %s\n", err);
			mem_free(&mem);
			return 1;
		}
		fprintf(stderr, "entry point: 0x%08x\n\n", entry);
	}
	mem.vwatch_on = vwatch_on;   /* skip the loader's own stores */

	/*
	 * A device with a power switch starts off. Headless there is nobody
	 * to press it, so those runs come up powered, which is also what
	 * every scripted test expects.
	 */
	bool powered = !gui;
	unsigned power_presses_seen = 0;

	/*
	 * A reset restarts the machine, and c33_reset() puts the instruction
	 * and MCLK counters back to zero with the rest of the CPU. Carry the
	 * earlier boots' totals so that -n still bounds the whole run: without
	 * it a guest that resets in a loop gets a fresh budget every time and
	 * never stops.
	 */
	unsigned resets = 0;
	uint64_t retired_before_reset = 0, clk_before_reset = 0;

	struct c33 cpu;
	memset(&cpu, 0, sizeof cpu);
	cpu.bus = (struct c33_bus){
		.read = mem_read,
		.write = mem_write,
		.region = (uint8_t *(*)(void *, uint32_t, uint32_t *, uint32_t *))mem_region,
		.wait = mem_wait,
		.ctx = &mem,
	};
	cpu.trace_syscalls = trace_syscalls;
	c33_reset(&cpu, entry);
	cpu.trace_syscalls = trace_syscalls;
	/* SPI and DMA deadlines use the same MCLK-cycle timeline as the CPU. */
	/* The port block holds a pin the SDRAM controller needs; tell it so,
	   now that both of them exist. */
	port_watch_sdram(&port, &cpu);

	/* The loader would have done this; a direct ELF boot has no loader. */
	if (!eeprom_path)
		sdramc_boot_state(&mem);

	touch_set_clock(&touch, MCLK_HZ);
	touch_set_cmu(&touch, &cmu);

	sd_set_clock(&sd, &cpu.clk);
	dma_set_clock(&dma, &cpu.clk);
	/* A -Y window starts closed; reaching prof_start opens it. */
	cpu.profile = (prof_start || prof_ms1 > 0) ? false : profile;
	cpu.pc_profile = (prof_start || prof_ms1 > 0) ? false : pc_profile;
	if (pc_profile)
		cpu.pcbuckets = calloc(C33_PCBUCKETS, sizeof *cpu.pcbuckets),
		cpu.pcsample  = calloc(C33_PCBUCKETS, sizeof *cpu.pcsample),
		cpu.pcclk     = calloc(C33_PCBUCKETS, sizeof *cpu.pcclk),
		cpu.pcfetch   = calloc(C33_PCBUCKETS, sizeof *cpu.pcfetch),
		cpu.pcrows    = calloc(C33_PCBUCKETS, sizeof *cpu.pcrows);
	if (boot_sp)
		cpu.sr[SR_SP] = boot_sp;
	/*
	 * Report the instruction being executed, not the one after it: a
	 * diagnostic that names the wrong instruction is worse than none.
	 */
	mem.pc_src = &cpu.cur_pc;
	cpu.region_epoch = &mem.sdram_epoch;
	cpu.row_counter = &sdramc.activations;
	cpu.sp_low_dstram = ~0u;
	cpu.irq_enabled = (bool (*)(void *, unsigned))itc_enabled;
	cpu.irq_poll = (bool (*)(void *, unsigned *, unsigned *))itc_next_irq;
	cpu.irq_ctx = &itc;
	cpu.slp_auto_wake = cmu_slp_auto_wake_cb;
	cpu.slp_ctx = &cmu;

	struct wdt wdt;
	wdt_attach(&mem, &wdt, &cmu, &cpu.clk);

	struct timerblk timer;
	timer_attach(&mem, &timer, &cpu.clk, &itc, &cmu);
	lcd.clk = &cpu.clk;
	lcd.trace = getenv("WREMU_LCD_TRACE") != NULL;
	/*
	 * With a window, measure time the way the person holding the mouse
	 * does. Headless runs keep the cycle-derived tick so they stay
	 * deterministic; WREMU_WALLCLOCK=1 opts a headless run into the
	 * window's clock when a timing bug needs to be reproduced without one.
	 */
	if (gui || getenv("WREMU_WALLCLOCK"))
		timer_use_wallclock(&timer);

	char dis[128];
	/*
	 * Ring of recent PCs. When the CPU runs off into zeroed memory the
	 * interesting event is thousands of instructions in the past, so keep
	 * a trailing window to show where it left real code.
	 */
#define RING 64
	uint32_t ring[RING];
	unsigned rn = 0;
	unsigned long nop_run = 0;
	const char *stop = NULL;

	while (!cpu.halted && retired_before_reset + cpu.cycles < limit) {
		/*
		 * An off device runs nothing and consumes no input. This has
		 * to come first: the periodic pump below hands whatever the
		 * window collected to the port, so leaving it ahead of this
		 * fed the power switch straight to a machine that was not
		 * running, and the press that should have turned the device
		 * on was counted and discarded instead.
		 */
		if (!powered) {
			if (!display_update(&disp)) {
				stop = "window closed";
				break;
			}
			/*
			 * Time passes while the device is off, so a scripted
			 * press still lands and instruction limits still end
			 * the run.
			 */
			cpu.cycles += IDLE_WAIT_MS * (MCLK_HZ / 1000);
			/*
			 * A scripted press is fed through the same field a
			 * click lands in, so the two share one path from here
			 * on. They used to be separate conditions, which is
			 * how the scripted test passed while a real click did
			 * nothing at all.
			 */
			for (unsigned k = 0; k < nbtns; k++) {
				if (btns[k].code == BUTTON_POWER_CODE &&
				    !btns[k].down_done && cpu.cycles >= btns[k].at) {
					btns[k].down_done = btns[k].up_done = true;
					disp.power_presses++;
				}
			}

			if (disp.power_presses != power_presses_seen) {
				power_presses_seen = disp.power_presses;
				fprintf(stderr, "  [powered on]\n");
				machine_power_on(&cpu, &mem, &port, &itc,
						 &cmu, &periph,
						 &sdramc, &lcd, &touch, &timer,
						 &sd, &wdt,
						 &dma,
						 eeprom_path ? &eeprom : NULL,
						 path, entry, boot_sp);
				uart_reset(&uart);
				powered = true;
				disp.powered = true;
			}
			disp.button = -1;
			disp.touch_pending = false;
			display_idle_wait(&disp, IDLE_WAIT_MS);
			continue;
		}

		/* Complete an SPI character before the CPU observes its status. */
		sd_poll(&sd);

		/* Repaint and pump SDL events periodically. The interpreter's
		 * throughput varies sharply with the instruction mix and the SDRAM
		 * model, so a 200k-instruction interval could become about 30 ms of
		 * host time while drawing. The cheap calls are rejected by the
		 * display's own 120 Hz limiter before it fingerprints the panel. */
		/*
		 * Scripted typing: each key is a press then a release, spaced
		 * far enough apart for the application to consume the events.
		 */
		if (script_armed && type_text && cpu.cycles >= type_at &&
		    type_text[type_idx]) {
			unsigned long due = type_at + type_idx * type_gap +
					    (type_phase ? type_gap / 2 : 0);
			if (cpu.cycles >= due) {
				int kx, ky;
				if (touch_key_pos(type_text[type_idx], &kx, &ky)) {
					touch_post(&touch, &cpu, kx, ky,
						   type_phase == 0);
					if (type_phase == 0)
						fprintf(stderr, "  [key '%c' at %d,%d]\n",
						       type_text[type_idx], kx, ky);
				}
				if (++type_phase == 2) { type_phase = 0; type_idx++; }
			}
		}

		/*
		 * Scripted drag, for exercising the scroll path without a
		 * window: press, then a run of motion packets down the
		 * screen, then release. grifo turns the first pressed packet
		 * into EVENT_TOUCH_DOWN and every one after it into
		 * EVENT_TOUCH_MOTION, so the intermediate steps are what
		 * makes this a drag rather than a tap.
		 */
#define DRAG_STEPS   16
		/* WREMU_DRAG_MS stretches a scripted drag: milliseconds per
		 * step instead of the default 5, for slow-finger tests. */
		/*
		 * Edge triggered like taps and buttons: a step is due once the
		 * counter has passed its time.  An earlier exact-multiple test
		 * only fired when an instruction happened to end on the boundary,
		 * so steps arrived late and at random while the guest was busy.
		 */
		for (unsigned k2 = 0; k2 < ndrags; k2++) {
			struct script_drag *g = &drags[k2];
			if (script_armed && g->step <= DRAG_STEPS &&
			    cpu.cycles >= g->at + g->step * drag_spacing) {
				unsigned long k = g->step++;
				int y = g->y0 + (int)((g->y1 - g->y0) * (int)k) / DRAG_STEPS;
				bool down = k < DRAG_STEPS;
				if (!g->announced) {
					g->announced = true;
					fprintf(stderr, "  [drag %d,%d -> %d,%d]\n",
						g->x, g->y0, g->x, g->y1);
				}
				if (getenv("WREMU_TOUCH_TRACE"))
					fprintf(stderr, "  [drag step %lu y %d at %llu]\n",
						k, y, (unsigned long long)cpu.cycles);
				touch_post(&touch, &cpu, g->x, y, down);
			}
		}
		if (ndrags && (cpu.cycles % 100000) == 0)
			touch_poll(&touch, &cpu);

		/*
		 * Scripted button press, held briefly then released. Edge
		 * triggered rather than testing for an exact cycle: the
		 * counter can pause across an idle stretch, and an equality
		 * test then fires more than once.
		 */
		for (unsigned k = 0; k < nbtns; k++) {
			struct script_btn *b = &btns[k];
			if (script_armed && !b->down_done && cpu.cycles >= b->at) {
				b->down_done = true;
				fprintf(stderr, "  [button %d down]\n", b->code);
				if (b->code == BUTTON_POWER_CODE)
					port_power_button(&port, &cpu, true);
				else
					port_button(&port, &cpu, (unsigned)b->code, true);
			}
			if (b->down_done && !b->up_done &&
			    cpu.cycles >= b->at + HOLD_CYCLES) {
				b->up_done = true;
				fprintf(stderr, "  [button %d up]\n", b->code);
				if (b->code == BUTTON_POWER_CODE)
					port_power_button(&port, &cpu, false);
				else
					port_button(&port, &cpu, (unsigned)b->code, false);
			}
		}

		/* scripted taps for testing without a window */
		for (unsigned k = 0; k < ntaps; k++) {
			struct script_tap *t = &taps[k];
			if (script_armed && !t->down_done && cpu.cycles >= t->at) {
				t->down_done = true;
				fprintf(stderr, "  [tap down at %d,%d]\n", t->x, t->y);
				touch_post(&touch, &cpu, t->x, t->y, true);
			}
			if (t->down_done && !t->up_done &&
			    cpu.cycles >= t->at + HOLD_CYCLES) {
				t->up_done = true;
				fprintf(stderr, "  [tap up]\n");
				touch_post(&touch, &cpu, t->x, t->y, false);
			}
		}
		if (ntaps && (cpu.cycles % 100000) == 0)
			touch_poll(&touch, &cpu);

		if (disp.open && executed >= next_gui_pump) {
			next_gui_pump = executed + 20000;
			if (!display_update(&disp)) {
				stop = "window closed";
				break;
			}
			deliver_input(&disp, &port, &touch, &cpu, &last_touch_post);
		}

		if (cpu.cycles < trace) {
			c33_disasm(&cpu, cpu.pc, dis, sizeof dis);
			printf("%08x  %s\n", cpu.pc, dis);
		}
		ring[rn++ % RING] = cpu.pc;

		if (!armed) {
			if (cpu.pc == arm_addr && ++arm_seen >= arm_n) {
				armed = true;
				fprintf(stderr, "  [armed at 0x%08x, hit %lu]\n",
					arm_addr, arm_seen);
			}
		}

		for (unsigned k = 0; armed && k < nprobe; k++) {
			if (cpu.pc != probe[k])
				continue;
			if (!probe_hits[k]) {
				probe_first_exec[k] = executed;
				probe_first_clk[k] = cpu.clk;
			} else {
				unsigned long long g = executed - probe_last_exec[k];
				if (g > probe_gap_exec[k][NGAP - 1]) {
					unsigned j = NGAP - 1;
					while (j > 0 && probe_gap_exec[k][j - 1] < g) {
						probe_gap_exec[k][j] = probe_gap_exec[k][j - 1];
						probe_gap_clk[k][j]  = probe_gap_clk[k][j - 1];
						probe_gap_at[k][j]   = probe_gap_at[k][j - 1];
						j--;
					}
					probe_gap_exec[k][j] = g;
					probe_gap_clk[k][j] = cpu.clk - probe_last_clk[k];
					probe_gap_at[k][j] = probe_last_clk[k];
				}
			}
			probe_last_exec[k] = executed;
			probe_last_clk[k] = cpu.clk;
			probe_hits[k]++;
			{
				uint32_t ra = mem_read(&mem, cpu.sr[SR_SP], 4);
				unsigned j;
				for (j = 0; j < NCALLER; j++) {
					if (!probe_caller_n[k][j]) {
						probe_caller[k][j] = ra;
						probe_caller_n[k][j] = 1;
						break;
					}
					if (probe_caller[k][j] == ra) {
						probe_caller_n[k][j]++;
						break;
					}
				}
			}
		}

		if (armed && prof_ms1 > 0 && !prof_done) {
			double ms = cpu.clk / (MCLK_HZ / 1000.0);
			if (!prof_window && ms >= prof_ms0) {
				prof_window = true;
				prof_exec0 = executed; prof_clk0 = cpu.clk;
				prof_idle0 = idle_skipped;
				cpu.profile = profile;
				cpu.pc_profile = pc_profile;
			} else if (prof_window && ms >= prof_ms1) {
				prof_window = false; prof_done = true;
				prof_exec = executed - prof_exec0;
				prof_clk = cpu.clk - prof_clk0;
				prof_idle = idle_skipped - prof_idle0;
				cpu.profile = cpu.pc_profile = false;
			}
		}

		if (armed && prof_start && !prof_done) {
			if (!prof_window && cpu.pc == prof_start) {
				prof_window = true;
				prof_exec0 = executed; prof_clk0 = cpu.clk;
				prof_idle0 = idle_skipped;
				sdramc_trace_on = true;
				/* On a repeat the SDRAM snapshot stays the first one's. */
				if (!prof_windows) {
					sdramc_at_open = sdramc;
					if (sdramc.row_hist) {
						memcpy(row_hist_at_open, sdramc.row_hist,
						       SDRAMC_ROW_HIST_ROWS * sizeof *row_hist_at_open);
						memcpy(pair_hist_at_open, sdramc.pair_hist,
						       SDRAMC_PAIR_HIST_SIZE * sizeof *pair_hist_at_open);
					}
				}
				cpu.profile = profile;
				cpu.pc_profile = pc_profile;
			} else if (prof_window && prof_end && cpu.pc == prof_end) {
				/* WREMU_WINDOW_REPEAT: keep opening the window at
				   every later hit of the start address and add the
				   intervals up, for a phase that recurs per block. */
				prof_window = false; prof_done = !window_repeat;
				prof_exec += executed - prof_exec0;
				prof_clk += cpu.clk - prof_clk0;
				prof_idle += idle_skipped - prof_idle0;
				prof_windows++;
				sdramc_at_close = sdramc;
				sdramc_trace_on = false;
				if (sdramc.row_hist) {
					memcpy(row_hist_at_close, sdramc.row_hist,
					       SDRAMC_ROW_HIST_ROWS * sizeof *row_hist_at_close);
					memcpy(pair_hist_at_close, sdramc.pair_hist,
					       SDRAMC_PAIR_HIST_SIZE * sizeof *pair_hist_at_close);
				}
				cpu.profile = cpu.pc_profile = false;
			}
		}

		/*
		 * Rebase the whole input script onto the moment the guest got
		 * here, so "tap 400 ms after the keyboard was up" means the
		 * same thing in a build that reached that point sooner.
		 */
		if (armed && !script_armed && cpu.pc == anchor) {
			script_armed = true;
			fprintf(stderr, "  [anchor 0x%08x at %llu, script rebased]\n",
				anchor, (unsigned long long)cpu.cycles);
			type_at += cpu.cycles;
			for (unsigned k = 0; k < ntaps; k++) taps[k].at += cpu.cycles;
			for (unsigned k = 0; k < ndrags; k++) drags[k].at += cpu.cycles;
			for (unsigned k = 0; k < nbtns; k++) btns[k].at += cpu.cycles;
		}

		for (unsigned k = 0; armed && k < nbp; k++) {
			if (cpu.pc != bp[k])
				continue;
			printf("\nBREAK at 0x%08x (cycle %llu)\n", cpu.pc,
			       (unsigned long long)cpu.cycles);
			printf("  return addr on stack = 0x%08x\n",
			       mem_read(&mem, cpu.sr[SR_SP], 4));
			for (int q = 0; q < 16; q += 4)
				printf("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
				       q, cpu.r[q], q+1, cpu.r[q+1],
				       q+2, cpu.r[q+2], q+3, cpu.r[q+3]);
			printf("  preceding PCs:\n");
			for (unsigned t = RING > 12 ? RING - 12 : 0; t < RING; t++) {
				uint32_t pp = ring[(rn + t) % RING];
				c33_disasm(&cpu, pp, dis, sizeof dis);
				printf("    %08x  %s\n", pp, dis);
			}
			stop = "breakpoint";
			goto done;
		}

		/* Executing a long run of zero words means we have fallen out
		 * of real code into blank memory. */
		if (port.power_off_requested) {
			if (!disp.open) {
				stop = "powered off";
				break;
			}
			/*
			 * The device is off, but the emulator is not. Keep
			 * the window so the power switch can turn it back on,
			 * exactly as the hardware behaves.
			 */
			fprintf(stderr, "  [powered off]\n");
			powered = false;
			/*
			 * The press that asked for the power-off counted too.
			 * Without this the machine would come straight back
			 * on from its own shutdown.
			 */
			power_presses_seen = disp.power_presses;
			port.power_off_requested = false;
			disp.powered = false;
			continue;
		}


		/* Host input stays outside the UART model. Never block the CPU
		 * on a terminal read or overrun the emulated FIFO while feeding a
		 * file. A cycle deadline also participates in headless HALT waits.
		 */
		if (uart_fd >= 0 && cpu.cycles >= uart_due) {
			uart_due = cpu.cycles + uart_gap;
			struct pollfd input = { .fd = uart_fd, .events = POLLIN };
			if (uart_can_receive(&uart) && poll(&input, 1, 0) > 0) {
				uint8_t byte;
				ssize_t n = read(uart_fd, &byte, 1);
				if (n == 1)
					uart_receive(&uart, byte);
				else if (n == 0) {
					if (uart_fd != STDIN_FILENO) close(uart_fd);
					uart_fd = -1;
				} else if (errno != EAGAIN && errno != EINTR) {
					perror("UART input");
					stop = "UART input error";
					break;
				}
			}
		}
		uart_poll(&uart);
		timer_poll(&timer, &cpu);
		wdt_poll(&wdt);
		if (wdt.expired) {
			/*
			 * RESEN is the chip's reset output, and two different
			 * things arrive here through it. System_reboot() in
			 * grifo arms the watchdog for 100 us and halts, which
			 * is how the firmware restarts the device -- NSH's
			 * "reboot" on the NuttX application reaches it. A
			 * guest that simply wedges gets here too, after
			 * grifo's twenty seconds.
			 *
			 * Either way the hardware restarts from the boot
			 * vector, so restart rather than stopping: a reboot
			 * that ends the run cannot be told from a crash, and
			 * neither can be followed to see what it did next.
			 *
			 * Modelled as a cold start, RAM and all. Reset holds
			 * the SDRAM controller, so refresh stops and what was
			 * in there is not worth trusting; the boot code
			 * reprograms it from scratch regardless.
			 */
			fprintf(stderr, "  [watchdog reset]\n");
			resets++;
			retired_before_reset += cpu.cycles;
			clk_before_reset += cpu.clk;
			machine_power_on(&cpu, &mem, &port, &itc, &cmu,
					 &periph, &sdramc, &lcd, &touch,
					 &timer, &sd, &wdt, &dma,
					 eeprom_path ? &eeprom : NULL,
					 path, entry, boot_sp);
			uart_reset(&uart);
			continue;
		}
		if (wdt.nmi_pending) {
			c33_raise_nmi(&cpu);
			wdt.nmi_pending = false;
		}

		/*
		 * Idle: the core is in HALT waiting for an interrupt, so
		 * there is nothing to execute until something is due. Grind
		 * through it a cycle at a time and a host core stays pinned
		 * for no reason.
		 *
		 * With a window, hand the time back to the operating system
		 * and keep pumping events -- the tick comes from the wall
		 * clock there, so it advances by itself. Headless, jump the
		 * clock straight to whatever is due next, which costs nothing
		 * and is what the guest would have seen anyway.
		 */
		if (cpu.sleeping && !cpu.irq_pending && !cpu.nmi_pending) {
			/*
			 * A GUI uses wall-clock pacing for human-scale timer waits, but
			 * SPI characters are only a handful of MCLK cycles apart.  Do
			 * not turn each DMA byte into one 10 ms window poll: a 512-byte
			 * sector then takes seconds and firmware appears to hang while
			 * mounting the card.  Advance to the SPI deadline exactly as the
			 * headless path does; the next loop completes the byte and lets
			 * the DMA pipeline schedule the following one.
			 */
			if (disp.open && sd.busy) {
				if (sd.deadline > cpu.clk) {
					uint64_t skip = sd.deadline - cpu.clk;
					cpu.cycles += skip;
					cpu.clk += skip;
					idle_skipped += skip;
					if (port_sd_powered(&port))
						idle_sd_powered += skip;
				}
				/*
				 * DMA descriptor writeback can move MCLK beyond the
				 * next SPI deadline. Poll that already-due byte at the
				 * top of the loop instead of charging it a 10 ms GUI
				 * sleep.
				 */
				continue;
			}
			if (disp.open) {
				if (!display_update(&disp)) {
					stop = "window closed";
					break;
				}
				/*
				 * About 100 Hz: fast enough that a click still
				 * feels immediate, slow enough that repainting
				 * an idle screen is not the thing keeping the
				 * host busy.
				 */
				deliver_input(&disp, &port, &touch, &cpu,
					      &last_touch_post);
				/*
				 * If that woke the machine, get on with it
				 * rather than sitting out the rest of the
				 * slice -- otherwise every click pays up to
				 * IDLE_WAIT_MS before anything happens.
				 */
				if (cpu.irq_pending || cpu.nmi_pending)
					continue;
				display_idle_wait(&disp, IDLE_WAIT_MS);
				/*
				 * Account for the time that just passed, so
				 * instruction limits and scripted input keep
				 * their meaning across an idle stretch.
				 */
				cpu.cycles += IDLE_WAIT_MS * (MCLK_HZ / 1000);
				cpu.clk    += IDLE_WAIT_MS * (MCLK_HZ / 1000);
				idle_skipped += IDLE_WAIT_MS * (MCLK_HZ / 1000);
				if (port_sd_powered(&port))
					idle_sd_powered += IDLE_WAIT_MS * (MCLK_HZ / 1000);
				continue;
			}
			unsigned long long next = limit;
			if (uart_fd >= 0 && uart_due < next)
				next = uart_due;
			/*
			 * Peripheral deadlines are on the MCLK timeline. DMA bus
			 * phases advance that clock without retiring CPU cycles, so
			 * an absolute MCLK deadline cannot be compared directly with
			 * cpu.cycles. Convert the remaining clock delay to a cycle
			 * target; otherwise the error grows after every DMA transfer.
			 */
			if (timer.deadline_valid) {
				uint64_t delay = timer.next_deadline > cpu.clk ?
					timer.next_deadline - cpu.clk : 0;
				uint64_t due = cpu.cycles + delay;
				if (due < next)
					next = due;
			}
			if (sd.busy) {
				uint64_t delay = sd.deadline > cpu.clk ?
					sd.deadline - cpu.clk : 0;
				uint64_t due = cpu.cycles + delay;
				if (due < next)
					next = due;
			}
			{
				uint64_t delay;
				if (wdt_deadline(&wdt, &delay)) {
					uint64_t due = cpu.cycles + delay;
					if (due < next)
						next = due;
				}
			}
			/*
			 * Until the anchor fires the scripted times have not
			 * been rebased, so they are not deadlines yet -- and
			 * skipping all the way to the limit would jump clean
			 * over the anchor itself.
			 */
			if (!script_armed && next > cpu.cycles + 1000000)
				next = cpu.cycles + 1000000;
			if (script_armed && type_text && type_text[type_idx] &&
			    type_at < next)
				next = type_at;
			/*
			 * Every scripted event still to come, releases
			 * included. Missing one means jumping straight over
			 * it: a press whose release was not counted here
			 * stayed down forever, because the skip went from the
			 * press to the end of the run.
			 */
			if (script_armed) {
			for (unsigned k = 0; k < ntaps; k++) {
				const struct script_tap *t = &taps[k];
				if (!t->down_done && t->at > cpu.cycles && t->at < next)
					next = t->at;
				if (t->down_done && !t->up_done &&
				    t->at + HOLD_CYCLES > cpu.cycles &&
				    t->at + HOLD_CYCLES < next)
					next = t->at + HOLD_CYCLES;
			}
			for (unsigned k = 0; k < ndrags; k++) {
				const struct script_drag *g = &drags[k];
				if (g->step <= DRAG_STEPS) {
					unsigned long long due = g->at + g->step * drag_spacing;
					if (due < next)
						next = due > cpu.cycles ? due : cpu.cycles + 1;
				}
			}
			for (unsigned k = 0; k < nbtns; k++) {
				const struct script_btn *b = &btns[k];
				if (!b->down_done && b->at > cpu.cycles && b->at < next)
					next = b->at;
				if (b->down_done && !b->up_done &&
				    b->at + HOLD_CYCLES > cpu.cycles &&
				    b->at + HOLD_CYCLES < next)
					next = b->at + HOLD_CYCLES;
			}
			}
			if (next > cpu.cycles) {
				uint64_t skip = next - cpu.cycles;
				cpu.cycles += skip;
				cpu.clk += skip;
				idle_skipped += skip;
				if (port_sd_powered(&port))
					idle_sd_powered += skip;
				continue;
			}
		}

		c33_step(&cpu);
		executed++;

		/*
		 * Runaway detection, from the word c33_step already fetched.
		 * Re-reading it here cost a second full memory dispatch per
		 * instruction -- about 15% of total run time.
		 */
		nop_run = (cpu.last_insn == 0) ? nop_run + 1 : 0;
		if (nop_run > 8) {
			stop = "runaway: >256 consecutive zero words";
			break;
		}
	}

done:
	if (uart_fd >= 0 && uart_fd != STDIN_FILENO) close(uart_fd);
	if (stop && strcmp(stop, "breakpoint") &&
	    strcmp(stop, "window closed") && strcmp(stop, "powered off")) {
		printf("\nlast %d PCs before %s:\n", RING, stop);
		for (unsigned i = 0; i < RING; i++) {
			uint32_t p = ring[(rn + i) % RING];
			c33_disasm(&cpu, p, dis, sizeof dis);
			printf("  %08x  %s\n", p, dis);
		}
	}

	/*
	 * A reset zeroes the CPU's counters and every peripheral's, so once
	 * one has happened the figures below describe the last boot alone.
	 * Say so rather than letting them be read as the whole run.
	 */
	if (resets)
		printf("--- resets: %u; the counters below cover the last boot "
		       "only, after %llu instructions and %.1f ms guest in "
		       "earlier ones ---\n", resets,
		       (unsigned long long)retired_before_reset,
		       (double)clk_before_reset / (MCLK_HZ / 1000.0));
	printf("--- timer: %lu reads, %llu MCLK cycles (%.2f cyc/instr) ---\n",
	       timer.reads, (unsigned long long)cpu.clk,
	       cpu.cycles ? (double)cpu.clk / (double)cpu.cycles : 0.0);
	printf("\n--- touch: %lu events, %lu bytes read, %lu irqs taken, %lu masked ---\n",
	       touch.events, touch.bytes_read, cpu.irqs_taken, cpu.irqs_masked);
	printf("--- ctp link: panel %u baud, receiver %u baud, %lu packets "
	       "garbled ---\n", CTP_BPS, touch_baud(&touch), touch.garbled);
	printf("--- buttons: %lu transitions ---\n", port.button_events);
	if (disp.calls)
		printf("--- display: %lu update calls, %lu presents, %lu skipped ---\n",
		       disp.calls, disp.presents, disp.skipped);
	if (idle_skipped)
		printf("--- idle: %llu cycles skipped rather than spun ---\n",
		       (unsigned long long)idle_skipped);
	/*
	 * The instruction counter is advanced by the idle skip as well as by
	 * execution, so on its own it cannot say whether a build did less
	 * work or merely waited less. Report the two separately.
	 */
	/* A window still open at the end of the run closes here. The device
	   can suspend inside one and never wake -- the idle path continues
	   before the window check, so nothing would ever close it. */
	if (prof_window) {
		prof_exec += executed - prof_exec0;
		prof_clk += cpu.clk - prof_clk0;
		prof_idle += idle_skipped - prof_idle0;
		prof_windows++;
	}
	if (window_repeat)
		printf("--- window repeated %lu times ---\n", prof_windows);
	if (prof_ms1 > 0)
		snprintf(prof_win_label, sizeof prof_win_label,
			 "%.0f..%.0f ms", prof_ms0, prof_ms1);
	else if (prof_start)
		snprintf(prof_win_label, sizeof prof_win_label,
			 "0x%08x..0x%08x", prof_start, prof_end);
	if (prof_start || prof_ms1 > 0)
		printf("--- window %s: %llu instructions, %.2f ms, %llu idle, %.2f cyc/instr ---\n",
		       prof_win_label, prof_exec,
		       prof_clk / (MCLK_HZ / 1000.0), prof_idle,
		       prof_exec ? (double)(prof_clk - prof_idle) / (double)prof_exec : 0.0);
	if (cpu.sp_low_dstram != ~0u)
		printf("--- dstram stack: lowest sp 0x%08x, %u bytes of the 1 KB below 0x84800 used ---\n",
		       cpu.sp_low_dstram, 0x84800u - cpu.sp_low_dstram);
	printf("--- work: %llu instructions executed, %llu idle, %.1f ms guest ---\n",
	       executed, (unsigned long long)idle_skipped,
	       (double)cpu.clk / (MCLK_HZ / 1000.0));
	/* Residency in skipped HALT intervals, not an electrical current model.
	   Chip deselection and a stopped SPI clock do not remove the SD supply. */
	printf("--- idle power: SD supply on %.1f ms, off %.1f ms ---\n",
	       idle_sd_powered / (MCLK_HZ / 1000.0),
	       (idle_skipped - idle_sd_powered) / (MCLK_HZ / 1000.0));
	for (unsigned k = 0; k < nprobe; k++)
	    {
		printf("--- probe %-28s %8llu hits  first %10llu/%8.1fms  last %10llu/%8.1fms ---\n",
		       probe_name[k] ? probe_name[k] : "",
		       probe_hits[k],
		       probe_hits[k] ? probe_first_exec[k] : 0,
		       probe_hits[k] ? probe_first_clk[k] / (MCLK_HZ / 1000.0) : 0.0,
		       probe_hits[k] ? probe_last_exec[k] : 0,
		       probe_hits[k] ? probe_last_clk[k] / (MCLK_HZ / 1000.0) : 0.0);
		if (probe_caller_n[k][0]) {
			printf("      callers:");
			for (unsigned j = 0; j < NCALLER && probe_caller_n[k][j]; j++)
				printf("  %08x x%llu", probe_caller[k][j], probe_caller_n[k][j]);
			printf("\n");
		}
		if (probe_gap_exec[k][0]) {
			printf("      stalls:");
			for (unsigned g = 0; g < NGAP && probe_gap_exec[k][g]; g++)
				printf("  %.1fms(%lluk@%.0fms)",
				       probe_gap_clk[k][g] / (MCLK_HZ / 1000.0),
				       probe_gap_exec[k][g] / 1000,
				       probe_gap_at[k][g] / (MCLK_HZ / 1000.0));
			printf("\n");
		}
	    }
	printf("--- itc: %lu register writes, serial ch1 priority %u, ESIF01=0x%02x, ch1-rx %s ---\n",
	       itc.writes, itc_priority(&itc, 61),
	       itc.reg[0x276 - ITC_BASE],
	       itc_enabled(&itc, 61) ? "enabled" : "DISABLED");
	printf("\n--- lcd: %lu register writes, framebuffer=0x%08x ---\n",
	       lcd.writes, lcd.fb_addr);
	if (lcd.fb_addr) {
		lcd_dump_ascii(&lcd, &mem, stdout);
		if (lcd_write_pgm(&lcd, &mem, "screen.pgm"))
			printf("  wrote screen.pgm\n");
	}

	printf("\n--- serial output: %lu bytes ---\n", uart.tx_count);
	printf("--- wdt: %lu kicks, %lu timeouts, %lu writes blocked while"
	       " protected ---\n", wdt.kicks, wdt.timeouts, wdt.blocked);
	printf("--- sd: %lu commands, %lu blocks read, %lu written, %lu rx overflows ---\n",
	       sd.commands, sd.blocks_read, sd.blocks_written, sd.overflows);
	printf("--- spi: %lu characters, %llu shift cycles, %llu enforced wait cycles ---\n",
	       sd.xfers, sd.shift_cycles, sd.wait_cycles);
	printf("--- spi config: %lu busy control accesses, %lu disables with interrupts set ---\n",
	       sd.busy_control_accesses, sd.unsafe_disables);
	printf("--- spi clock: %lu unclamped disables with SD selected ---\n",
	       sd.unclamped_disables);
	if (sd.payloads_timed)
		printf("--- sd payload: %lu blocks, %.1f average MCLK cycles"
		       " (%llu min, %llu max) ---\n",
		       sd.payloads_timed,
		       (double)sd.payload_cycles / sd.payloads_timed,
		       sd.payload_min, sd.payload_max);
	printf("--- dma: %lu HSDMA transfers, %lu IDMA transfers,"
	       " %lu invalid descriptor tables, %llu minimum bus cycles ---\n",
	       dma.hsdma_transfers, dma.idma_transfers,
	       dma.invalid_descriptors, dma.bus_cycles);
	printf("--- dma channels: HSDMA2 TX %lu, HSDMA3 RX %lu ---\n",
	       dma.hsdma_channel_transfers[2], dma.hsdma_channel_transfers[3]);
	if (dma.hsdma_channel_transfers[0] || dma.hsdma_channel_transfers[1])
		printf("--- dma memory: HSDMA0 %lu, HSDMA1 %lu units ---\n",
		       dma.hsdma_channel_transfers[0], dma.hsdma_channel_transfers[1]);
	printf("--- sdram: %llu wait cycles, %llu refreshes, %llu self-refresh exits;"
	       " IQB %llu/%llu hit/miss, DQB %llu/%llu hit/miss,"
	       " %llu writes, %llu row activations ---\n",
	       (unsigned long long)sdramc.wait_cycles,
	       (unsigned long long)sdramc.refreshes,
	       (unsigned long long)sdramc.self_refresh_exits,
	       (unsigned long long)sdramc.iq_hits,
	       (unsigned long long)sdramc.iq_misses,
	       (unsigned long long)sdramc.dq_hits,
	       (unsigned long long)sdramc.dq_misses,
	       (unsigned long long)sdramc.writes_timed,
	       (unsigned long long)sdramc.activations);
	{
		static const char *const kind[5] = { "fetch", "read", "write", "dmar", "dmaw" };
		printf("--- sdram row activations by previous->this access kind:");
		for (unsigned a = 0; a < 5; a++)
			for (unsigned b = 0; b < 5; b++)
				if (sdramc.act_kind[a][b])
					printf(" %s>%s %llu", kind[a], kind[b],
					       (unsigned long long)sdramc.act_kind[a][b]);
		printf(" ---\n");
	}
	if (prof_done && prof_start) {
		static const char *const kind[5] = { "fetch", "read", "write", "dmar", "dmaw" };
		const struct sdramc *a = &sdramc_at_open, *b = &sdramc_at_close;
		printf("--- window sdram: %llu wait cycles, %llu refreshes, IQB %llu/%llu, DQB %llu/%llu, %llu writes, %llu row activations ---\n",
		       (unsigned long long)(b->wait_cycles - a->wait_cycles),
		       (unsigned long long)(b->refreshes - a->refreshes),
		       (unsigned long long)(b->iq_hits - a->iq_hits),
		       (unsigned long long)(b->iq_misses - a->iq_misses),
		       (unsigned long long)(b->dq_hits - a->dq_hits),
		       (unsigned long long)(b->dq_misses - a->dq_misses),
		       (unsigned long long)(b->writes_timed - a->writes_timed),
		       (unsigned long long)(b->activations - a->activations));
		printf("--- window activations by bank:");
		for (unsigned i = 0; i < 4; i++)
			printf(" bank%u %llu", i,
			       (unsigned long long)(b->act_bank[i] - a->act_bank[i]));
		printf(" ---\n--- window accesses kind/bank:");
		for (unsigned k = 0; k < 5; k++)
			for (unsigned i = 0; i < 4; i++)
				if (b->kind_bank[k][i] - a->kind_bank[k][i])
					printf(" %s/b%u %llu", kind[k], i,
					       (unsigned long long)(b->kind_bank[k][i] - a->kind_bank[k][i]));
		printf(" ---\n--- window activations prev>this:");
		for (unsigned x = 0; x < 5; x++)
			for (unsigned y = 0; y < 5; y++)
				if (b->act_kind[x][y] - a->act_kind[x][y])
					printf(" %s>%s %llu", kind[x], kind[y],
					       (unsigned long long)(b->act_kind[x][y] - a->act_kind[x][y]));
		printf(" ---\n");
		if (sdramc.row_hist) {
			/* The 24 most activated rows in the window. */
			printf("--- window activations by row (WREMU_ROWHIST):");
			for (unsigned shown = 0; shown < 24; shown++) {
				unsigned best = 0;
				uint64_t best_n = 0;
				for (unsigned r = 0; r < SDRAMC_ROW_HIST_ROWS; r++) {
					uint64_t n = row_hist_at_close[r] - row_hist_at_open[r];
					if (n > best_n) { best_n = n; best = r; }
				}
				if (!best_n)
					break;
				printf(" %08x %llu", 0x10000000u + best * 1024u,
				       (unsigned long long)best_n);
				row_hist_at_open[best] = row_hist_at_close[best];
			}
			printf(" ---\n--- window activations by row pair (from>to):");
			for (unsigned shown = 0; shown < 24; shown++) {
				unsigned best = 0;
				uint64_t best_n = 0;
				for (unsigned r = 0; r < SDRAMC_PAIR_HIST_SIZE; r++) {
					uint64_t n = pair_hist_at_close[r].n;
					if (pair_hist_at_open[r].key == pair_hist_at_close[r].key)
						n -= pair_hist_at_open[r].n;
					if (n > best_n) { best_n = n; best = r; }
				}
				if (!best_n)
					break;
				printf(" %08x>%08x %llu",
				       0x10000000u + (pair_hist_at_close[best].key >> 16) * 1024u,
				       0x10000000u + (pair_hist_at_close[best].key & 0xffffu) * 1024u,
				       (unsigned long long)best_n);
				pair_hist_at_open[best] = pair_hist_at_close[best];
			}
			printf(" ---\n");
		}
	}
	if (eeprom_path)
		printf("--- eeprom: %lu commands, %lu bytes read, %lu written ---\n",
		       eeprom.commands, eeprom.bytes_read, eeprom.bytes_written);
	printf("--- adc: %lu conversions, %lu register writes, %lu overwrite errors ---\n",
	       periph.conversions, periph.adc_writes, periph.overwrites);
	if (profile)
		c33_dump_profile(&cpu, stdout);
	{
		/* Full dump first: the top-12 summary clears buckets as it
		   picks them, so taking it the other way round loses the
		   twelve that matter most. */
		if (prof_full_path) {
			FILE *f = fopen(prof_full_path, "w");
			if (f) { c33_dump_pcprofile_full(&cpu, f); fclose(f); }
			else perror(prof_full_path);
		}
		if (pc_profile)
			c33_dump_pcprofile(&cpu, stdout);
	}
	model_describe(stdout);
	printf("--- cmu: %lu writes, %lu blocked while protected, mclk %u Hz ---\n",
	       cmu.writes, cmu.blocked, cmu_mclk_hz(&cmu));
	printf("--- stopped after %llu instructions ---\n",
	       (unsigned long long)cpu.cycles);
	if (cpu.fault) {
		printf("fault: %s at pc=0x%08x\n", cpu.fault, cpu.fault_pc);
		printf("last %d PCs:\n", RING);
		for (unsigned t = 0; t < RING; t++) {
			uint32_t pp = ring[(rn + t) % RING];
			c33_disasm(&cpu, pp, dis, sizeof dis);
			printf("  %08x  %s\n", pp, dis);
		}
	}
	else if (stop)
		printf("stop reason: %s\n", stop);
	else if (cpu.halted)
		printf("cpu halted cleanly\n");
	else
		printf("instruction limit reached\n");

	printf("pc=%08x sp=%08x psr=%08x ttbr=%08x dp=%08x\n", cpu.pc,
	       cpu.sr[SR_SP], cpu.sr[SR_PSR], cpu.sr[SR_TTBR], cpu.sr[SR_DP]);
	for (int i = 0; i < 16; i += 4)
		printf("r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
		       i, cpu.r[i], i + 1, cpu.r[i + 1],
		       i + 2, cpu.r[i + 2], i + 3, cpu.r[i + 3]);
	if (mem.unmapped_reads || mem.unmapped_writes)
		printf("unmapped: %lu reads, %lu writes\n",
		       mem.unmapped_reads, mem.unmapped_writes);

	if (dump_on) {
		printf("\nmemory at 0x%08x (%lu bytes):\n", dump, dump_len);
		for (unsigned long r = 0; r < (dump_len + 15) / 16; r++) {
			printf("  %08x  ", (uint32_t)(dump + r * 16));
			for (unsigned k = 0; k < 16 && r * 16 + k < dump_len; k++)
				printf("%02x ",
				       (unsigned)mem_read(&mem, dump + r*16 + k, 1));
			printf("\n");
		}
		if (dump_path) {
			FILE *fp = fopen(dump_path, "wb");
			if (!fp)
				fprintf(stderr, "error: cannot create dump file %s\n", dump_path);
			else {
				for (unsigned long i = 0; i < dump_len; i++)
					fputc((int)mem_read(&mem, dump + i, 1), fp);
				fclose(fp);
			}
		}
	}

	/* Did the boot sector actually land in guest RAM intact? */
	{
		const uint8_t sig[8] = {0xeb,0x58,0x90,0x42,0x53,0x44,0x20,0x20};
		unsigned found = 0;
		/*
		 * All RAM, not just SDRAM. The EEPROM boot path runs entirely
		 * in internal memory -- FatFs's sector window is a local in
		 * a0ram -- so scanning only SDRAM reported "not present" for a
		 * read that had in fact landed correctly.
		 */
		static const struct { uint32_t base, len; } ram[] = {
			{ 0,           A0RAM_SIZE },
			{ IVRAM_BASE,  IVRAM_SIZE },
			{ DSTRAM_BASE, DSTRAM_SIZE },
			{ SDRAM_BASE,  SDRAM_SIZE },
		};
		for (unsigned r = 0; r < 4 && found < 4; r++)
		for (uint32_t a = ram[r].base; a < ram[r].base + ram[r].len - 8; a++) {
			unsigned k = 0;
			while (k < 8 && (uint8_t)mem_read(&mem, a + k, 1) == sig[k])
				k++;
			if (k == 8) {
				printf("boot sector found in RAM at 0x%08x\n", a);
				if (++found >= 4)
					break;
			}
		}
		if (!found)
			printf("boot sector NOT present in guest RAM\n");
	}

	printf("\n--- forms that discarded ext prefixes ---\n");
	c33_report_dropped_ext(&cpu, stdout);

	if (disp.open) {
		display_update(&disp);   /* final frame */
		display_close(&disp);
	}

	mem_free(&mem);
	return cpu.fault ? 1 : 0;
}
