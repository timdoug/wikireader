/*
 * The CPU's memory and timing fast paths, included by gb.c after
 * peanut_gb.h through Peanut's PEANUT_GB_READ, _WRITE, _BATCH and _TICKED
 * hooks.
 *
 * Memory: a table of sixteen 4 KiB pages.  ROM, VRAM, work RAM and the
 * common cartridge RAM case are plain pointers; everything else -- I/O,
 * OAM, banking registers, the RTC -- goes to Peanut.
 *
 * Timing: Peanut advances the timers, serial port and LCD after every
 * instruction.  Here instructions only add up their cycles until the next
 * point where any of those changes state the CPU can see (an LCD mode
 * change, a TIMA overflow, a serial bit, an RTC second, the end of a frame
 * with the LCD off), and the whole batch is run then, at the same
 * instruction Peanut would have run it.  Only DIV and TIMA and the counters
 * behind them fall behind meanwhile; an access that meets them runs the
 * batch first, and a write that could move the next event makes the
 * current instruction's end a batch point.
 * SPDX-License-Identifier: MIT
 */

#ifdef __c33__
#define FAST_DATA __attribute__((section(".fastbss"), aligned(4)))
#else
#define FAST_DATA __attribute__((aligned(4)))
#endif

/* Shared with hot.s (gen-hot.py lays out the same fields after its
   dispatch table).  Page entries are base pointers biased by the page's
   address, page[a >> 12] + a; the region tables say where the
   host-contiguous stretch a page belongs to ends, and how long it is. */
struct gb_hot {
	uintptr_t read_page[16], write_page[16];	/* 32-bit on the C33 */
	uint32_t region_end[16], region_size[16];
	uint32_t a, bc, de, hl, sp, pc, zv, cf, hx;
	int32_t left;
	uintptr_t hram, bias;
	uint32_t size, steps, park, go;
	uintptr_t hram_biased;			/* hram_io - 0xff00 */
	int32_t stat0_left;	/* mode 3 has become 0 once left <= this */
	uintptr_t ime;		/* the byte of struct gb_s holding gb_ime */
	uint32_t ime_bit;	/* and its bit */
	uint32_t cb_get[8], cb_set[8], cb_op[32];
};

#ifdef GBW_HOT
extern struct gb_hot gb_hot_state;
int gb_hot_run(void);
void gb_hot_event(void);		/* hot.s calls it at each event */
#define hot gb_hot_state
#else
static struct gb_hot hot;
#endif
#define read_page hot.read_page
#define write_page hot.write_page
/* Cycles deferred, and how many may be before something happens. */
static int32_t pending FAST_DATA;
static int32_t budget FAST_DATA;

static void map_regions(void)
{
	static const uint8_t end[16] = {		/* in 4 KiB pages */
		4, 4, 4, 4, 8, 8, 8, 8, 10, 10, 12, 12, 14, 14, 15, 16,
	};
	static const uint8_t size[16] = {
		4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 1, 1,
	};

	for (unsigned p = 0; p < 16; ++p) {
		hot.region_end[p] = end[p] * 0x1000u;
		hot.region_size[p] = size[p] * 0x1000u;
	}
}

static void map_pages(struct gb_s *g)
{
	unsigned bank = g->mbc == 1 && g->cart_mode_select
		? g->selected_rom_bank & 0x1f : g->selected_rom_bank;
	uintptr_t low = (uintptr_t)rom;
	uintptr_t high = (uintptr_t)rom + (bank - 1) * ROM_BANK_SIZE;
	uintptr_t cart = 0;

	for (unsigned p = 0; p < 4; ++p) {
		read_page[p] = low;
		read_page[p + 4] = high;
		write_page[p] = write_page[p + 4] = 0;
	}
	read_page[8] = read_page[9] = write_page[8] = write_page[9]
		= (uintptr_t)g->vram - VRAM_ADDR;
	read_page[0xc] = read_page[0xd] = write_page[0xc] = write_page[0xd]
		= (uintptr_t)g->wram - WRAM_0_ADDR;
	read_page[0xe] = write_page[0xe] = (uintptr_t)g->wram - ECHO_ADDR;
	read_page[0xf] = write_page[0xf] = 0;

	/* The cases where Peanut reads and writes the same bytes: the
	   selected bank, or for an MBC1 in its ROM banking mode, bank 0. */
	if (!(g->mbc == 3 && g->cart_ram_bank >= 0x08) && g->cart_ram
	    && g->enable_cart_ram && g->mbc != 2) {
		size_t bank = 0;

		if (g->mbc != 1 || g->cart_mode_select)
			bank = g->cart_ram_bank < g->num_ram_banks
				? g->cart_ram_bank : (size_t)-1;
		else if (!g->num_ram_banks)
			bank = (size_t)-1;	/* reads, but no writes */
		if (bank != (size_t)-1
		    && (bank + 1) * CRAM_BANK_SIZE <= cart_ram_bytes)
			cart = (uintptr_t)cart_ram + bank * CRAM_BANK_SIZE
				- CART_RAM_ADDR;
	}
	read_page[0xa] = read_page[0xb] = write_page[0xa] = write_page[0xb] = cart;
}

/* TAC's rate as TIMA's period, 1024, 16, 64 or 256 cycles: Peanut's
   TAC_CYCLES without a table load from SDRAM. */
static inline __attribute__((always_inline)) uint32_t tac_period(unsigned tac)
{
	return 16u << (((tac + 3) & 3) * 2);
}

/* Where mode 3 ends as an event.  With the STAT mode 0 interrupt off, the
   change to mode 0 only shows in STAT's mode bits, so it waits for the line
   end, or the next event, or anything that reads or writes STAT: those run
   the deferred cycles first (slow_read, meets_deferred) or, in hot.s, work
   the mode out from the budget (gb_hot.stat0_left). */
static inline __attribute__((always_inline)) int32_t hblank_end(unsigned stat)
{
	return stat & STAT_MODE_0_INTR ? LCD_MODE3_LCD_DRAW_END : LCD_LINE_CYCLES;
}

/* When the next deferred event is due, in cycles from the last batch.
   Inlined: it runs from SDRAM after a C step and from the window buffer
   after a hot.s event. */
static inline __attribute__((always_inline)) int32_t next_event(struct gb_s *g)
{
	int32_t next = INT32_MAX;

	if (g->hram_io[IO_LCDC] & LCDC_ENABLE) {
		unsigned mode = g->hram_io[IO_STAT] & STAT_MODE;
		int32_t end = mode == IO_STAT_MODE_OAM_SCAN ? LCD_MODE2_OAM_SCAN_END
			: mode == IO_STAT_MODE_LCD_DRAW ? hblank_end(g->hram_io[IO_STAT])
			: LCD_LINE_CYCLES;

		next = end - (int32_t)g->counter.lcd_count;
	} else {
		next = LCD_FRAME_CYCLES - (int32_t)g->counter.lcd_off_count;
	}
	if (g->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK) {
		int32_t period = (int32_t)tac_period(g->hram_io[IO_TAC]);
		int32_t overflow = (256 - g->hram_io[IO_TIMA]) * period
			- (int32_t)g->counter.tima_count;

		if (overflow < next)
			next = overflow;
	}
	if (g->hram_io[IO_SC] & SERIAL_SC_TX_START) {
		/* A transfer's first cycle calls the transmit hook. */
		int32_t serial = g->counter.serial_count == 0 ? 1
			: SERIAL_CYCLES - (int32_t)g->counter.serial_count;

		if (serial < next)
			next = serial;
	}
	if (g->mbc == 3 && (g->rtc_real.reg.high & 0x40) == 0) {
		int32_t rtc = (int32_t)(RTC_CYCLES - g->counter.rtc_count);

		if (rtc < next)
			next = rtc;
	}
	return next < 1 ? 1 : next;
}

#ifdef __c33__
/* The window buffer, beside hot.s: zero-wait, where SDRAM made Peanut's
   tick wait on its own instruction fetch half the time. */
#define TICK_CODE __attribute__((section(".ivram_code"), noinline))
#else
#define TICK_CODE __attribute__((noinline))
#endif

/* Peanut's __gb_tick, for what tick() leaves to it; reached through a
   pointer because the window buffer is out of a direct call's reach of
   SDRAM. */
static uint32_t (*volatile peanut_tick)(struct gb_s *, uint32_t) = __gb_tick;

/* __gb_tick's work as ticks() does it: the same state changes in the same
   order, with DIV by arithmetic instead of a loop, and the serial port and
   the RTC's once-a-second rollover left to Peanut.  With `halted` it is
   also Peanut's HALT loop, ticking until an interrupt is pending -- about
   six passes a scanline, the way Peanut measures its waits -- so the
   counters are kept in locals for the whole wait: through the byte
   pointer to hram_io, any store could alias them, and the compiler
   reloaded every one after each. */
static TICK_CODE uint32_t ticks(struct gb_s *g, uint32_t cycles, int halted)
{
	uint8_t *io = g->hram_io;
	uint32_t div, tima, lcd, rtc, serial;
	unsigned sc, tac, lcdc, lyc, ie, stat, ly, iflag, div_reg, tima_reg;
	int rtc_on;

	/* Nothing else runs until this returns, so I/O lives in registers
	   too; LY goes back before each line is drawn, which reads it. */
#define LOAD() do {							\
		div = g->counter.div_count;				\
		tima = g->counter.tima_count;				\
		lcd = g->counter.lcd_count;				\
		rtc = g->counter.rtc_count;				\
		serial = g->counter.serial_count;			\
		rtc_on = g->mbc == 3 && (g->rtc_real.reg.high & 0x40) == 0; \
		sc = io[IO_SC];						\
		tac = io[IO_TAC];					\
		lcdc = io[IO_LCDC];					\
		lyc = io[IO_LYC];					\
		ie = io[IO_IE];						\
		stat = io[IO_STAT];					\
		ly = io[IO_LY];						\
		iflag = io[IO_IF];					\
		div_reg = io[IO_DIV];					\
		tima_reg = io[IO_TIMA];					\
	} while (0)
#define STORE() do {							\
		g->counter.div_count = div;				\
		g->counter.tima_count = tima;				\
		g->counter.lcd_count = lcd;				\
		g->counter.rtc_count = rtc;				\
		g->counter.serial_count = serial;			\
		io[IO_SC] = (uint8_t)sc;				\
		io[IO_STAT] = (uint8_t)stat;				\
		io[IO_LY] = (uint8_t)ly;				\
		io[IO_IF] = (uint8_t)iflag;				\
		io[IO_DIV] = (uint8_t)div_reg;				\
		io[IO_TIMA] = (uint8_t)tima_reg;			\
	} while (0)

	LOAD();
	halted = halted && g->gb_halt;
	for (;;) {
		if (rtc_on && rtc + cycles >= RTC_CYCLES) {
			STORE();
			cycles = peanut_tick(g, cycles);
			LOAD();
			goto next;
		}

		div += cycles;
		div_reg += div >> 8;
		div &= DIV_CYCLES - 1;
		if (rtc_on)
			rtc += cycles;

		/* A transfer with no one on the other end, as Peanut does it:
		   Tetris keeps one open, looking for a second player. */
		if (sc & SERIAL_SC_TX_START) {
			void (*tx)(struct gb_s *, const uint8_t) = g->gb_serial_tx;
			enum gb_serial_rx_ret_e (*rx_fn)(struct gb_s *, uint8_t *) =
				g->gb_serial_rx;

			if (serial == 0 && tx) {
				io[IO_SC] = (uint8_t)sc;
				tx(g, io[IO_SB]);
			}
			serial += cycles;
			if (serial >= SERIAL_CYCLES) {
				uint8_t rx;

				if (rx_fn && rx_fn(g, &rx) == GB_SERIAL_RX_SUCCESS) {
					io[IO_SB] = rx;
					sc &= 0x01;
					iflag |= SERIAL_INTR;
				} else if (sc & SERIAL_SC_CLOCK_SRC) {
					io[IO_SB] = 0xff;
					sc &= 0x01;
					iflag |= SERIAL_INTR;
				}
				serial = 0;
			}
		}

		if (tac & IO_TAC_ENABLE_MASK) {
			uint32_t period = tac_period(tac);

			for (tima += cycles; tima >= period; tima -= period)
				if ((++tima_reg & 0xff) == 0) {
					iflag |= TIMER_INTR;
					tima_reg = io[IO_TMA];
				}
		}

		if (!(lcdc & LCDC_ENABLE)) {
			g->counter.lcd_off_count += cycles;
			if (g->counter.lcd_off_count >= LCD_FRAME_CYCLES) {
				g->counter.lcd_off_count -= LCD_FRAME_CYCLES;
				g->gb_frame = true;
			}
			goto next;
		}

		lcd += cycles;
		if (lcd >= LCD_LINE_CYCLES) {
			lcd -= LCD_LINE_CYCLES;
			if (++ly == LCD_VERT_LINES)
				ly = 0;
			if (ly == lyc) {
				stat |= STAT_LYC_COINC;
				if (stat & STAT_LYC_INTR)
					iflag |= LCDC_INTR;
			} else {
				stat &= 0xfb;
			}
			if (ly == LCD_HEIGHT) {
				stat = (stat & ~STAT_MODE) | IO_STAT_MODE_VBLANK;
				g->gb_frame = true;
				iflag |= VBLANK_INTR;
				g->lcd_blank = false;
				if (stat & STAT_MODE_1_INTR)
					iflag |= LCDC_INTR;
				if (g->direct.frame_skip)
					g->display.frame_skip_count =
						!g->display.frame_skip_count;
				if (g->direct.interlace
				    && (!g->direct.frame_skip
					|| g->display.frame_skip_count))
					g->display.interlace_count =
						!g->display.interlace_count;
				if (g->gb_halt && !ie) {
					cycles = 0;
					break;
				}
			} else if (ly < LCD_HEIGHT) {
				if (ly == 0) {
					g->display.WY = io[IO_WY];
					g->display.window_clear = 0;
				}
				stat = (stat & ~STAT_MODE) | IO_STAT_MODE_OAM_SCAN;
				lcd = 0;
				if (stat & STAT_MODE_2_INTR)
					iflag |= LCDC_INTR;
				cycles = LCD_MODE2_OAM_SCAN_DURATION;
			}
		} else if ((stat & STAT_MODE) == IO_STAT_MODE_LCD_DRAW
			   && lcd >= LCD_MODE3_LCD_DRAW_END) {
			stat = (stat & ~STAT_MODE) | IO_STAT_MODE_HBLANK;
			if (stat & STAT_MODE_0_INTR)
				iflag |= LCDC_INTR;
			if (lcd < LCD_MODE0_HBLANK_MAX_DRUATION)
				cycles = LCD_MODE0_HBLANK_MAX_DRUATION - lcd;
		} else if ((stat & STAT_MODE) == IO_STAT_MODE_OAM_SCAN
			   && lcd >= LCD_MODE2_OAM_SCAN_END) {
			stat = (stat & ~STAT_MODE) | IO_STAT_MODE_LCD_DRAW;
			if (!g->lcd_blank) {
				io[IO_LY] = (uint8_t)ly;
				PEANUT_GB_DRAW_LINE(g);
			}
			if (lcd < LCD_MODE3_LCD_DRAW_MIN_DURATION)
				cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - lcd;
		}
next:
		if (!halted || !cycles || (iflag & ie))
			break;
	}
	STORE();
	return cycles;
#undef STORE
#undef LOAD
}

/* The window buffer is out of a direct call's reach of SDRAM too. */
static uint32_t (*volatile fast_ticks)(struct gb_s *, uint32_t, int) = ticks;
#define fast_tick(g, cycles) fast_ticks(g, cycles, 0)

/* The common event, when nothing but DIV, TIMA and the LCD is moving: one
   LCD mode change, the same state changes as ticks() in the same order,
   but touching only what changes, and returning the next budget, which
   next_event() would give.  Everything else goes to ticks(). */
static inline __attribute__((always_inline)) int32_t
event_tick(struct gb_s *g, uint32_t cycles)
{
	uint8_t *io = g->hram_io;
	int rtc_on = g->mbc == 3 && (g->rtc_real.reg.high & 0x40) == 0;
	uint32_t count, lcd;
	int32_t next = INT32_MAX, lcd_next;
	unsigned stat, mode;

	if ((io[IO_SC] & SERIAL_SC_TX_START) || !(io[IO_LCDC] & LCDC_ENABLE)
	    || (rtc_on && g->counter.rtc_count + cycles >= RTC_CYCLES)) {
		(void)fast_ticks(g, cycles, 0);		/* from either memory */
		return next_event(g);
	}

	count = g->counter.div_count + cycles;
	io[IO_DIV] = (uint8_t)(io[IO_DIV] + (count >> 8));
	g->counter.div_count = count & (DIV_CYCLES - 1);
	if (rtc_on) {
		g->counter.rtc_count += cycles;
		next = (int32_t)(RTC_CYCLES - g->counter.rtc_count);
	}
	if (io[IO_TAC] & IO_TAC_ENABLE_MASK) {
		uint32_t period = tac_period(io[IO_TAC]);
		int32_t overflow;

		for (count = g->counter.tima_count + cycles; count >= period;
		     count -= period)
			if (++io[IO_TIMA] == 0) {
				io[IO_IF] |= TIMER_INTR;
				io[IO_TIMA] = io[IO_TMA];
			}
		g->counter.tima_count = count;
		overflow = (int32_t)((256 - io[IO_TIMA]) * period - count);
		if (overflow < next)
			next = overflow;
	}

	stat = io[IO_STAT];
	lcd = g->counter.lcd_count + cycles;
	if (lcd >= LCD_LINE_CYCLES) {
		unsigned ly = io[IO_LY] + 1u;

		lcd -= LCD_LINE_CYCLES;
		if (ly == LCD_VERT_LINES)
			ly = 0;
		io[IO_LY] = (uint8_t)ly;
		if (ly == io[IO_LYC]) {
			stat |= STAT_LYC_COINC;
			if (stat & STAT_LYC_INTR)
				io[IO_IF] |= LCDC_INTR;
		} else {
			stat &= 0xfb;
		}
		if (ly == LCD_HEIGHT) {
			stat = (stat & ~STAT_MODE) | IO_STAT_MODE_VBLANK;
			g->gb_frame = true;
			io[IO_IF] |= VBLANK_INTR;
			g->lcd_blank = false;
			if (stat & STAT_MODE_1_INTR)
				io[IO_IF] |= LCDC_INTR;
			if (g->direct.frame_skip)
				g->display.frame_skip_count =
					!g->display.frame_skip_count;
			if (g->direct.interlace && (!g->direct.frame_skip
						    || g->display.frame_skip_count))
				g->display.interlace_count =
					!g->display.interlace_count;
		} else if (ly < LCD_HEIGHT) {
			if (ly == 0) {
				g->display.WY = io[IO_WY];
				g->display.window_clear = 0;
			}
			stat = (stat & ~STAT_MODE) | IO_STAT_MODE_OAM_SCAN;
			lcd = 0;
			if (stat & STAT_MODE_2_INTR)
				io[IO_IF] |= LCDC_INTR;
		}
	} else if ((stat & STAT_MODE) == IO_STAT_MODE_LCD_DRAW
		   && lcd >= LCD_MODE3_LCD_DRAW_END) {
		stat = (stat & ~STAT_MODE) | IO_STAT_MODE_HBLANK;
		if (stat & STAT_MODE_0_INTR)
			io[IO_IF] |= LCDC_INTR;
	} else if ((stat & STAT_MODE) == IO_STAT_MODE_OAM_SCAN
		   && lcd >= LCD_MODE2_OAM_SCAN_END) {
		stat = (stat & ~STAT_MODE) | IO_STAT_MODE_LCD_DRAW;
		io[IO_STAT] = (uint8_t)stat;
		if (!g->lcd_blank)
			PEANUT_GB_DRAW_LINE(g);
	}
	io[IO_STAT] = (uint8_t)stat;
	g->counter.lcd_count = lcd;

	mode = stat & STAT_MODE;
	lcd_next = (mode == IO_STAT_MODE_OAM_SCAN ? LCD_MODE2_OAM_SCAN_END
		    : mode == IO_STAT_MODE_LCD_DRAW ? hblank_end(stat)
		    : LCD_LINE_CYCLES) - (int32_t)lcd;
	if (lcd_next < next)
		next = lcd_next;
	return next < 1 ? 1 : next;
}

/* For Peanut's steps, in SDRAM: hot.s's events have their own copy in
   gb_hot_event, in the window buffer, which has no room for two. */
static __attribute__((noinline)) int32_t step_event(struct gb_s *g, uint32_t cycles)
{
	return event_tick(g, cycles);
}

static int32_t (*volatile fast_event)(struct gb_s *, uint32_t) = step_event;

/* A step's timing, from Peanut's step (PEANUT_GB_TICKS): the HALT loop,
   or one event. */
static int32_t step_budget = -1;

static void step_ticks(struct gb_s *g, uint32_t cycles)
{
	if (g->gb_halt) {
		(void)fast_ticks(g, cycles, 1);
		step_budget = -1;
	} else {
		step_budget = fast_event(g, cycles);
	}
}



/* Run the deferred cycles now. */
static void catch_up(struct gb_s *g)
{
	if (pending) {
		int32_t cycles = pending;

		pending = 0;
		(void)fast_tick(g, (uint32_t)cycles);
		budget = 0;
	}
}

static inline __attribute__((always_inline)) uint32_t
batch(struct gb_s *g, uint32_t cycles)
{
	int32_t total = pending + (int32_t)cycles;

	/* A HALT measures its wait from the timers, so it runs them first
	   (PEANUT_GB_HALT) and is never deferred. */
	if (total < budget && !g->gb_halt) {
		pending = total;
		return 0;
	}
	pending = 0;
	return (uint32_t)total;
}

static inline __attribute__((always_inline)) void ticked(struct gb_s *g)
{
	if (step_budget >= 0) {
		/* event_tick() found it; the host builds check it. */
#ifndef __c33__
		if (step_budget != next_event(g))
			gbw_error("event_tick's budget differs from next_event's",
				  (unsigned)step_budget);
#endif
		budget = step_budget;
		step_budget = -1;
		return;
	}
	budget = next_event(g);
}

/* Deferred cycles only show in DIV, TIMA and STAT's mode bits (hblank_end):
   LY, IF and the serial registers change at events, which are never
   deferred. */
static uint8_t slow_read(struct gb_s *g, uint_fast16_t address)
{
	if (address >= 0xff80 && address != 0xffff)
		return g->hram_io[address - IO_ADDR];
	if (address == 0xff04 || address == 0xff05 || address == 0xff41)
		catch_up(g);
	return __gb_read(g, (uint16_t)address);
}

/* Whether a write meets deferred state: the timer and serial registers and
   LCDC, whose counters it changes, and the MBC3 clock's latch and
   registers.  Everything else is only read at events. */
static int meets_deferred(struct gb_s *g, uint_fast16_t address)
{
	if (address >= IO_ADDR) {
		unsigned reg = address & 0xff;

		return reg == 0x02 || (reg >= 0x04 && reg <= 0x07) || reg == 0x40
			|| reg == 0x41;
	}
	if (address < 0x8000)
		return g->mbc == 3 && address >= 0x6000;
	if (address >= CART_RAM_ADDR && address < WRAM_0_ADDR)
		return g->mbc == 3 && g->cart_ram_bank >= 0x08;
	return 0;
}

static void slow_write(struct gb_s *g, uint_fast16_t address, uint8_t value)
{
	/* OAM, OAM DMA and LCDC's sprite size: render.h's buckets. */
	if ((address >= 0xfe00 && address < 0xfea0) || address == 0xff46
	    || address == 0xff40)
		sprites_dirty = 1;
	if (address >= 0xff80 && address != 0xffff) {
		g->hram_io[address - IO_ADDR] = value;
		return;
	}
	if (meets_deferred(g, address)) {
		/* See the state as it is now, and find the next event anew
		   at this instruction's end. */
		catch_up(g);
		__gb_write(g, address, value);
		budget = 0;
	} else {
		__gb_write(g, address, value);
	}
	if (address < 0x8000 || (address >= CART_RAM_ADDR && address < WRAM_0_ADDR))
		map_pages(g);
}

static inline __attribute__((always_inline)) uint8_t
fast_read(struct gb_s *g, uint_fast16_t address)
{
	uintptr_t page = read_page[(address >> 12) & 15];

	return page ? *(const uint8_t *)(page + (address & 0xffff))
		: slow_read(g, address);
}

static inline __attribute__((always_inline)) void
fast_write(struct gb_s *g, uint_fast16_t address, uint8_t value)
{
	uintptr_t page = write_page[(address >> 12) & 15];

	if (page)
		*(uint8_t *)(page + (address & 0xffff)) = value;
	else
		slow_write(g, address, value);
}
