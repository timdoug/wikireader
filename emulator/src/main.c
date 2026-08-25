/*
 * wremu -- WikiReader (Epson C33 / S1C33) full-system emulator.
 *
 * Milestone: load an ELF image, execute from its entry point, and trace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c33.h"
#include "mem.h"
#include "uart.h"
#include "sdcard.h"
#include "periph.h"
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "timer.h"
#include "itc.h"
#include "cmu.h"
#include "port.h"
#include "eeprom.h"
#include "sdramc.h"

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-t N] [-n N] [-m] <image.elf>\n"
		"  -t N   trace the first N instructions\n"
		"  -n N   stop after N instructions (default 1000000; unlimited with -g)\n"
		"  -m     trace unclaimed MMIO register accesses\n"
		"  -s     trace grifo syscalls by name\n"
		"  -A     trap misaligned halfword/word accesses (vector 6)\n"
		"  -g     show the panel in a live SDL2 window\n"
		"  -S N   window scale factor (default 3)\n"
		"  -T x,y,c  scripted tap at pixel x,y on cycle c\n"
		"  -K c,TEXT type TEXT on the on-screen keyboard from cycle c\n"
		"  -c F   attach FAT32 card image F\n"
		"  -D A   dump memory starting at address A\n"
		"  -L N   memory dump length (default 64)\n"
		"  -O F   write the memory dump as binary file F\n", p);
}


/* grifo's numbering: 0 random, 1 search, 2 history, 3 power. */
#define BUTTON_POWER_CODE 3

/* 6 bytes x 10 bits at CTP_BPS 9600, in 60 MHz cycles. */
#define CTP_PACKET_CYCLES  ((60000000ull * 6 * 10) / 9600)

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
	    cpu->cycles - *last_post < CTP_PACKET_CYCLES) {
		/* too soon; it goes out next time round */
	} else if (disp->touch_pending) {
		*last_post = cpu->cycles;
		disp->touch_pending = false;
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
			     struct sdramc *sdramc, struct lcd *lcd,
			     struct touch *touch, struct timerblk *timer,
			     struct sdcard *sd, struct eeprom *eeprom,
			     const char *path, uint32_t entry, uint32_t boot_sp)
{
	itc_reset(itc);
	port_reset(port);
	sdramc_reset(sdramc);
	lcd_reset(lcd);
	touch_reset(touch);
	timer_reset(timer);
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
	if (boot_sp)
		cpu->sr[SR_SP] = boot_sp;
}

int main(int argc, char **argv)
{
	const char *path = NULL, *card = NULL;
	unsigned long trace = 0, limit = 1000000;
	bool limit_given = false;
	bool trace_mmio = false;
	bool trace_syscalls = false;
	bool gui = false; int gui_scale = 3;
	bool check_align = false;
	bool profile = false;
	bool pc_profile = false;
	int tap_x = -1, tap_y = -1; unsigned long tap_at = 0;
	bool tap_down_done = false, tap_up_done = false;
	int drag_x = -1, drag_y0 = 0, drag_y1 = 0; unsigned long drag_at = 0;
	const char *eeprom_path = NULL;
	int btn_code = -1; unsigned long btn_at = 0;
	bool btn_down_done = false, btn_up_done = false;
/* How long a scripted press is held before release. */
#define HOLD_CYCLES  2000000UL
/* Total span of a scripted drag, from its first packet to its last. */
#define DRAG_SPAN    (16UL * 300000UL)
	unsigned long long last_touch_post = 0;
	unsigned long long idle_skipped = 0;
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
		else if (!strcmp(argv[i], "-m"))
			trace_mmio = true;
		else if (!strcmp(argv[i], "-s"))
			trace_syscalls = true;
		else if (!strcmp(argv[i], "-A"))
			check_align = true;
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
			/* scripted tap: -T x,y,cycle */
			sscanf(argv[++i], "%d,%d,%lu", &tap_x, &tap_y, &tap_at);
		}
		else if (!strcmp(argv[i], "-N") && i + 1 < argc) {
			/* scripted button: -N code,cycle  (0 random 1 search 2 history) */
			sscanf(argv[++i], "%d,%lu", &btn_code, &btn_at);
		}
		else if (!strcmp(argv[i], "-G") && i + 1 < argc) {
			/* scripted drag: -G x,y0,y1,cycle */
			sscanf(argv[++i], "%d,%d,%d,%lu",
			       &drag_x, &drag_y0, &drag_y1, &drag_at);
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

	struct sdramc sdramc;
	sdramc_attach(&mem, &sdramc);

	static struct eeprom eeprom;
	if (eeprom_path && !eeprom_load(&eeprom, eeprom_path, stderr)) {
		fprintf(stderr, "error: cannot open eeprom image %s\n", eeprom_path);
		return 1;
	}

	struct sdcard sd;
	if (!sd_attach(&mem, &sd, card, &port,
		       eeprom_path ? &eeprom : NULL)) {
		fprintf(stderr, "error: cannot open card image %s\n", card);
		return 1;
	}
	sd.trace = trace_mmio;
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

	struct c33 cpu;
	memset(&cpu, 0, sizeof cpu);
	cpu.bus = (struct c33_bus){ mem_read, mem_write,
				   (uint8_t *(*)(void *, uint32_t, uint32_t *, uint32_t *))mem_region,
				   &mem };
	cpu.trace_syscalls = trace_syscalls;
	c33_reset(&cpu, entry);
	cpu.trace_syscalls = trace_syscalls;
	cpu.check_alignment = check_align;
	cpu.profile = profile;
	cpu.pc_profile = pc_profile;
	if (pc_profile)
		cpu.pcbuckets = calloc(C33_PCBUCKETS, sizeof *cpu.pcbuckets),
		cpu.pcsample  = calloc(C33_PCBUCKETS, sizeof *cpu.pcsample);
	if (boot_sp)
		cpu.sr[SR_SP] = boot_sp;
	mem.pc_src = &cpu.pc;
	cpu.irq_enabled = (bool (*)(void *, unsigned))itc_enabled;
	cpu.irq_ctx = &itc;

	struct timerblk timer;
	timer_attach(&mem, &timer, &cpu.clk, &itc);
	/*
	 * With a window, measure time the way the person holding the mouse
	 * does. Headless runs keep the cycle-derived tick so they stay
	 * deterministic.
	 */
	if (gui)
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

	while (!cpu.halted && cpu.cycles < limit) {
		/*
		 * Repaint and pump SDL events periodically. 200k instructions
		 * is frequent enough to feel live without the event pump
		 * dominating run time.
		 */
		/*
		 * Scripted typing: each key is a press then a release, spaced
		 * far enough apart for the application to consume the events.
		 */
		if (type_text && cpu.cycles >= type_at && type_text[type_idx]) {
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
#define DRAG_SPACING 300000UL
		if (drag_x >= 0 && cpu.cycles >= drag_at &&
		    cpu.cycles <= drag_at + DRAG_STEPS * DRAG_SPACING &&
		    ((cpu.cycles - drag_at) % DRAG_SPACING) == 0) {
			unsigned long k = (cpu.cycles - drag_at) / DRAG_SPACING;
			int y = drag_y0 + (int)((drag_y1 - drag_y0) * (int)k) / DRAG_STEPS;
			bool down = k < DRAG_STEPS;
			if (k == 0)
				fprintf(stderr, "  [drag %d,%d -> %d,%d]\n",
					drag_x, drag_y0, drag_x, drag_y1);
			touch_post(&touch, &cpu, drag_x, y, down);
		}
		if (drag_x >= 0 && (cpu.cycles % 100000) == 0)
			touch_poll(&touch, &cpu);

		/*
		 * Scripted button press, held briefly then released. Edge
		 * triggered rather than testing for an exact cycle: the
		 * counter can pause across an idle stretch, and an equality
		 * test then fires more than once.
		 */
		if (btn_code >= 0 && !btn_down_done && cpu.cycles >= btn_at) {
			btn_down_done = true;
			fprintf(stderr, "  [button %d down]\n", btn_code);
			if (btn_code == BUTTON_POWER_CODE)
				port_power_button(&port, &cpu, true);
			else
				port_button(&port, &cpu, (unsigned)btn_code, true);
		}
		if (btn_code >= 0 && btn_down_done && !btn_up_done &&
		    cpu.cycles >= btn_at + HOLD_CYCLES) {
			btn_up_done = true;
			fprintf(stderr, "  [button %d up]\n", btn_code);
			if (btn_code == BUTTON_POWER_CODE)
				port_power_button(&port, &cpu, false);
			else
				port_button(&port, &cpu, (unsigned)btn_code, false);
		}

		/* scripted tap for testing without a window */
		if (tap_x >= 0 && !tap_down_done && cpu.cycles >= tap_at) {
			tap_down_done = true;
			fprintf(stderr, "  [tap down at %d,%d]\n", tap_x, tap_y);
			touch_post(&touch, &cpu, tap_x, tap_y, true);
		}
		if (tap_x >= 0 && tap_down_done && !tap_up_done &&
		    cpu.cycles >= tap_at + HOLD_CYCLES) {
			tap_up_done = true;
			fprintf(stderr, "  [tap up]\n");
			touch_post(&touch, &cpu, tap_x, tap_y, false);
		}
		if (tap_x >= 0 && (cpu.cycles % 100000) == 0)
			touch_poll(&touch, &cpu);

		if (disp.open && (cpu.cycles % 200000) == 0) {
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

		for (unsigned k = 0; k < nbp; k++) {
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
			port.power_off_requested = false;
			disp.powered = false;
			continue;
		}

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
			bool scripted = btn_code == BUTTON_POWER_CODE &&
					!btn_down_done && cpu.cycles >= btn_at;
			if (scripted)
				btn_down_done = btn_up_done = true;

			if (scripted ||
			    (disp.button == BUTTON_POWER_CODE &&
			     disp.button_pressed)) {
				fprintf(stderr, "  [powered on]\n");
				machine_power_on(&cpu, &mem, &port, &itc,
						 &sdramc, &lcd, &touch, &timer,
						 &sd, eeprom_path ? &eeprom : NULL,
						 path, entry, boot_sp);
				powered = true;
				disp.powered = true;
			}
			disp.button = -1;
			disp.touch_pending = false;
			display_idle_wait(&disp, IDLE_WAIT_MS);
			continue;
		}

		timer_poll(&timer, &cpu);

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
		if (cpu.sleeping && !cpu.irq_pending) {
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
				if (cpu.irq_pending)
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
				continue;
			}
			unsigned long long next = limit;
			if (timer.t2_running && timer.t2_deadline < next)
				next = timer.t2_deadline;
			if (type_text && type_text[type_idx] && type_at < next)
				next = type_at;
			/*
			 * Every scripted event still to come, releases
			 * included. Missing one means jumping straight over
			 * it: a press whose release was not counted here
			 * stayed down forever, because the skip went from the
			 * press to the end of the run.
			 */
			if (tap_x >= 0 && !tap_down_done && tap_at > cpu.cycles &&
			    tap_at < next)
				next = tap_at;
			if (tap_x >= 0 && tap_down_done && !tap_up_done &&
			    tap_at + HOLD_CYCLES > cpu.cycles &&
			    tap_at + HOLD_CYCLES < next)
				next = tap_at + HOLD_CYCLES;
			if (drag_x >= 0 && drag_at + DRAG_SPAN > cpu.cycles &&
			    drag_at < next)
				next = drag_at > cpu.cycles ? drag_at : cpu.cycles + 1;
			if (btn_code >= 0 && !btn_down_done && btn_at > cpu.cycles &&
			    btn_at < next)
				next = btn_at;
			if (btn_code >= 0 && btn_down_done && !btn_up_done &&
			    btn_at + HOLD_CYCLES > cpu.cycles &&
			    btn_at + HOLD_CYCLES < next)
				next = btn_at + HOLD_CYCLES;
			if (next > cpu.cycles) {
				uint64_t skip = next - cpu.cycles;
				cpu.cycles += skip;
				cpu.clk += skip;
				idle_skipped += skip;
				continue;
			}
		}

		c33_step(&cpu);

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
	if (stop && strcmp(stop, "breakpoint")) {
		printf("\n%s\n", stop);
		printf("last %d PCs before running away:\n", RING);
		for (unsigned i = 0; i < RING; i++) {
			uint32_t p = ring[(rn + i) % RING];
			c33_disasm(&cpu, p, dis, sizeof dis);
			printf("  %08x  %s\n", p, dis);
		}
	}

	printf("--- timer: %lu reads, %llu MCLK cycles (%.2f cyc/instr) ---\n",
	       timer.reads, (unsigned long long)cpu.clk,
	       cpu.cycles ? (double)cpu.clk / (double)cpu.cycles : 0.0);
	printf("\n--- touch: %lu events, %lu bytes read, %lu irqs taken, %lu masked ---\n",
	       touch.events, touch.bytes_read, cpu.irqs_taken, cpu.irqs_masked);
	printf("--- buttons: %lu transitions ---\n", port.button_events);
	if (disp.calls)
		printf("--- display: %lu update calls, %lu presents, %lu skipped ---\n",
		       disp.calls, disp.presents, disp.skipped);
	if (idle_skipped)
		printf("--- idle: %llu cycles skipped rather than spun ---\n",
		       (unsigned long long)idle_skipped);
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
	printf("--- sd: %lu commands, %lu blocks read, %lu rx overflows ---\n",
	       sd.commands, sd.blocks_read, sd.overflows);
	if (eeprom_path)
		printf("--- eeprom: %lu commands, %lu bytes read, %lu written ---\n",
		       eeprom.commands, eeprom.bytes_read, eeprom.bytes_written);
	printf("--- adc: %lu conversions, %lu register writes, %lu overwrite errors ---\n",
	       periph.conversions, periph.adc_writes, periph.overwrites);
	if (profile)
		c33_dump_profile(&cpu, stdout);
	if (pc_profile)
		c33_dump_pcprofile(&cpu, stdout);
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
