#include <stdio.h>
#include <stdlib.h>
/* Six-channel 16-bit timer block (REG_BASE+0x780). */

#include <string.h>
#include <time.h>

#include "timer.h"

#define T16_BASE       0x0780u
#define T16_LEN        0x0080u

#define OFF_DA16_0     (0x7d0u - T16_BASE)
#define OFF_DA16_2     (0x7d4u - T16_BASE)
#define OFF_CNT_PAUSE  (0x7dcu - T16_BASE)
#define OFF_ADVMODE    (0x7deu - T16_BASE)

#define PRUNx          (1u << 0)     /* run/stop */
#define PRESETx        (1u << 1)     /* write-only counter reset command */
#define PTMx           (1u << 2)     /* timer output enable */
#define CKSLx          (1u << 3)     /* external input clock select */
#define OUTINVx        (1u << 4)     /* inverted timer output */
#define SELCRBx        (1u << 5)     /* comparison register buffer select */
#define INITOLx        (1u << 8)     /* advanced-mode initial output */
#define T16ADV         (1u << 0)
#define P16TONx        (1u << 3)     /* prescaler enable */

#define CTL_STORED     0x017du       /* D8, D[6:2], D0; D1 reads zero */

static bool is_ctl(uint32_t reg)
{
	return reg <= (0x7aeu - T16_BASE) && (reg & 7) == 6;
}

static bool is_clkctl(uint32_t reg)
{
	return reg >= (0x7e0u - T16_BASE) &&
	       reg <= (0x7eau - T16_BASE) && !(reg & 1);
}

static uint32_t ctl_offset(unsigned channel)
{
	return channel * 8u + 6u;
}

static uint32_t clkctl_offset(unsigned channel)
{
	return (0x7e0u - T16_BASE) + channel * 2u;
}

static bool is_channel_reg(uint32_t reg)
{
	return reg < 6u * 8u && !(reg & 1);
}

static unsigned channel_number(uint32_t reg)
{
	return reg / 8u;
}

static unsigned channel_reg_number(uint32_t reg)
{
	return (reg & 7u) / 2u;
}

static bool advanced(const struct timerblk *t)
{
	return (t->reg[OFF_ADVMODE / 2] & T16ADV) != 0;
}

static uint16_t *comparison_address(struct timerblk *t, unsigned channel,
				    unsigned which)
{
	uint16_t ctl = t->reg[ctl_offset(channel) / 2u];
	return (ctl & SELCRBx) ? &t->compare_buffer[channel][which]
				 : &t->compare[channel][which];
}

static void preset_channel(struct timerblk *t, unsigned channel)
{
	t->count[channel] = 0;
	t->phase[channel] = 0;
	if (t->reg[ctl_offset(channel) / 2u] & SELCRBx) {
		t->compare[channel][0] = t->compare_buffer[channel][0];
		t->compare[channel][1] = t->compare_buffer[channel][1];
	}
}

static bool is_da16(uint32_t reg)
{
	return reg >= OFF_DA16_0 && reg <= OFF_DA16_2 && !(reg & 1);
}

static void write_da16(struct timerblk *t, uint32_t reg, uint16_t value)
{
	static const unsigned timer_a[] = { 1, 3, 5 };
	static const unsigned timer_b[] = { 2, 4, 0 };
	unsigned pair = (reg - OFF_DA16_0) / 2u;

	t->reg[reg / 2] = value;
	*comparison_address(t, timer_a[pair], 0) = value >> 6;
	*comparison_address(t, timer_b[pair], 0) = value & 0x3f;
}
/* Tick_TicksPerMicroSecond in samo-lib/drivers/include/tick.h. */
#define TICKS_PER_MICROSECOND 60u
#define RAW_HZ (TICKS_PER_MICROSECOND * 1000000u)

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Wall-clock tick source, used when there is a window.
 *
 * Deriving the tick from the instruction count makes the guest's notion of
 * elapsed time proportional to how fast the host happens to be emulating,
 * which is fine headless -- it is deterministic and reproducible -- but
 * wrong under a human's hand. This build runs at about 0.75x real time, so
 * a one-second drag looks like 0.75 s to the firmware, and wikilib derives
 * finger_move_speed as pixels per tick: the scroll momentum comes out
 * inflated by the same factor. The ratio also moves around with host load
 * and with what the firmware is doing, so the fling feels inconsistent
 * rather than merely fast.
 *
 * Interactively the honest clock is the real one: the user's seconds are
 * the seconds the application should measure.
 */
void timer_use_wallclock(struct timerblk *t)
{
	t->wallclock = true;
	t->t0_ns = mono_ns();
}

static uint64_t raw_now(struct timerblk *t)
{
	if (t->wallclock) {
		uint64_t us = (mono_ns() - t->t0_ns) / 1000ull;
		return us * TICKS_PER_MICROSECOND;
	}
	if (!t->cycles)
		return 0;
	return *t->cycles;
}

struct matches {
	uint64_t a;
	uint64_t b;
};

/* Advance through the documented 0..CRB inclusive count sequence. */
static struct matches advance_channel(struct timerblk *t, unsigned channel,
				      uint64_t ticks)
{
	struct matches matches = { 0, 0 };
	uint32_t count = t->count[channel];
	uint32_t a = t->compare[channel][0];
	uint32_t b = t->compare[channel][1];
	uint64_t to_b = (uint16_t)(b - count) + 1u;
	uint64_t to_a = (uint16_t)(a - count);

	/* Equal means the current state already matched; find the next one. */
	if (to_a == 0)
		to_a = 65536u;
	if (to_a < to_b && to_a <= ticks)
		matches.a++;

	if (ticks < to_b) {
		t->count[channel] = (uint16_t)(count + ticks);
		return matches;
	}

	/* The B match resets the counter and makes buffered values active. */
	ticks -= to_b;
	matches.b++;
	t->count[channel] = 0;
	if (t->reg[ctl_offset(channel) / 2u] & SELCRBx) {
		t->compare[channel][0] = t->compare_buffer[channel][0];
		t->compare[channel][1] = t->compare_buffer[channel][1];
	}

	a = t->compare[channel][0];
	b = t->compare[channel][1];
	uint64_t period = (uint64_t)b + 1u;
	/*
	 * The reset presents count 0 to both comparators, so a comparison A of
	 * zero matches here.  The firmware's 32-bit tick cascade depends on
	 * that match: it is the one pulse per overflow that clocks timer 5.
	 */
	if (a == 0)
		matches.a++;
	if (ticks) {
		matches.b += ticks / period;
		uint64_t rem = ticks % period;
		/* A can match once in each complete 0..B period. */
		if (a <= b) {
			matches.a += ticks / period;
			if (a != 0 && rem >= a)
				matches.a++;
		}
		t->count[channel] = (uint16_t)rem;
	}
	return matches;
}

static uint32_t input_hz(const struct timerblk *t)
{
	return t->cmu ? cmu_mclk_hz(t->cmu) : RAW_HZ;
}

static uint32_t suspend_speedup(unsigned channel, const struct timerblk *t,
				uint32_t hz)
{
	/* Timer 2 also wakes short CPU-only waits at full speed. Accelerate
	 * only the deep-suspend configuration (OSC3/32, prescaler /4096). */
	if (channel != 2 || hz != OSC3_HZ / 32 ||
	    (t->reg[clkctl_offset(channel) / 2u] & 7u) != 7u)
		return 1;
	const char *s = getenv("WREMU_SUSPEND_DIV");
	int n = s ? atoi(s) : 1;
	return n > 1 ? (uint32_t)n : 1;
}

static uint32_t prescale(unsigned channel, const struct timerblk *t)
{
	static const uint16_t divisors[8] = {
		1, 2, 4, 16, 64, 256, 1024, 4096
	};
	return divisors[t->reg[clkctl_offset(channel) / 2u] & 7u];
}

static bool internally_running(const struct timerblk *t, unsigned channel)
{
	uint16_t ctl = t->reg[ctl_offset(channel) / 2u];
	uint16_t clkctl = t->reg[clkctl_offset(channel) / 2u];
	return (ctl & (PRUNx | CKSLx)) == PRUNx &&
	       (clkctl & P16TONx) &&
	       (!t->cmu || cmu_t16_enabled(t->cmu, channel)) &&
	       !(advanced(t) && (t->reg[OFF_CNT_PAUSE / 2] & (1u << channel)));
}

static bool externally_running(const struct timerblk *t, unsigned channel)
{
	uint16_t ctl = t->reg[ctl_offset(channel) / 2u];
	return (ctl & (PRUNx | CKSLx)) == (PRUNx | CKSLx) &&
	       (!t->cmu || cmu_t16_enabled(t->cmu, channel)) &&
	       !(advanced(t) && (t->reg[OFF_CNT_PAUSE / 2] & (1u << channel)));
}

static void record_matches(struct timerblk *t, unsigned channel,
			   struct matches m)
{
	if (m.a) {
		t->fires[channel][0] += m.a;
		if (t->itc)
			itc_set_flag((struct itc *)t->itc, VECTOR_T16_A(channel));
	}
	if (m.b) {
		t->fires[channel][1] += m.b;
		if (t->itc)
			itc_set_flag((struct itc *)t->itc, VECTOR_T16_B(channel));
	}
}

static uint64_t clocks_to_b(const struct timerblk *t, unsigned channel)
{
	return (uint16_t)(t->compare[channel][1] - t->count[channel]) + 1u;
}

static uint64_t clocks_to_a(const struct timerblk *t, unsigned channel)
{
	uint64_t n = (uint16_t)(t->compare[channel][0] - t->count[channel]);
	return n ? n : 65536u;
}

static void schedule_channel(struct timerblk *t, unsigned channel, uint64_t now,
			     uint64_t denominator, uint64_t rate)
{
	uint64_t ticks = clocks_to_b(t, channel);
	uint64_t a = clocks_to_a(t, channel);
	if (a < ticks)
		ticks = a;
	uint64_t need = ticks * denominator;
	if (need > t->phase[channel])
		need -= t->phase[channel];
	else
		need = 1;
	uint64_t delta = (need + rate - 1) / rate;
	uint64_t deadline = now + (delta ? delta : 1);
	if (!t->deadline_valid || deadline < t->next_deadline) {
		t->deadline_valid = true;
		t->next_deadline = deadline;
	}
}

static void schedule_deadline(struct timerblk *t, uint64_t now)
{
	uint32_t hz = input_hz(t);
	t->deadline_valid = false;
	if (!hz)
		return;

	for (unsigned channel = 0; channel < 6; channel++) {
		if (!internally_running(t, channel))
			continue;
		uint64_t denominator = (uint64_t)RAW_HZ * prescale(channel, t);
		uint64_t rate = (uint64_t)hz * suspend_speedup(channel, t, hz);
		schedule_channel(t, channel, now, denominator, rate);
	}
}

static void timer_sync(struct timerblk *t)
{
	uint64_t now = raw_now(t);
	uint64_t elapsed = now - t->last_raw;
	uint32_t hz = input_hz(t);
	t->last_raw = now;
	t->deadline_valid = false;

	struct matches channel0 = { 0, 0 };
	for (unsigned channel = 0; channel < 6; channel++) {
		if (!internally_running(t, channel) || !hz)
			continue;
		uint64_t denominator = (uint64_t)RAW_HZ * prescale(channel, t);
		uint64_t rate = (uint64_t)hz * suspend_speedup(channel, t, hz);
		uint64_t numerator = t->phase[channel] + elapsed * rate;
		if (numerator < denominator) {
			/* Usually less than one prescaled tick has elapsed. */
			t->phase[channel] = numerator;
		} else {
			uint64_t ticks = numerator / denominator;
			t->phase[channel] = numerator % denominator;
			struct matches m = advance_channel(t, channel, ticks);
			record_matches(t, channel, m);
			if (channel == 0)
				channel0 = m;
		}

		/* Reuse the clock calculation for the next match deadline. */
		schedule_channel(t, channel, now, denominator, rate);
	}

	/*
	 * On the WikiReader board TM0 is physically routed from P12 to EXCL5
	 * on P74. OUTINV selects which compare edge is rising and therefore
	 * clocks timer 5's external input.
	 */
	uint16_t ctl0 = t->reg[ctl_offset(0) / 2u];
	if ((ctl0 & PTMx) && externally_running(t, 5)) {
		uint64_t pulses = (ctl0 & OUTINVx) ? channel0.b : channel0.a;
		struct matches m = advance_channel(t, 5, pulses);
		record_matches(t, 5, m);
	}
}

void timer_poll(struct timerblk *t, struct c33 *cpu)
{
	timer_sync(t);
	if (t->itc) {
		unsigned vector, priority;
		if (itc_next_irq(t->itc, &vector, &priority))
			c33_raise_irq(cpu, vector, priority);
	}
}

static bool timer_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
			       bool is_write)
{
	struct timerblk *t = ctx;
	uint32_t reg = off - T16_BASE;
	timer_sync(t);

	if (is_write) {
		if (reg == OFF_CNT_PAUSE) {
			/* Full-sync pause writes do nothing in standard mode. */
			if (!(t->reg[OFF_ADVMODE / 2] & T16ADV))
				return true;
			t->reg[reg / 2] = (uint16_t)*val & 0x3f;
			schedule_deadline(t, raw_now(t));
			return true;
		}
		if (reg == OFF_ADVMODE) {
			t->reg[reg / 2] = (uint16_t)*val & T16ADV;
			schedule_deadline(t, raw_now(t));
			return true;
		}
		if (is_da16(reg)) {
			/* DA16 data writes are disabled in standard mode. */
			if (advanced(t))
				write_da16(t, reg, (uint16_t)*val);
			schedule_deadline(t, raw_now(t));
			return true;
		}
		if (is_channel_reg(reg)) {
			unsigned channel = channel_number(reg);
			unsigned subreg = channel_reg_number(reg);

			if (subreg < 2) {
				*comparison_address(t, channel, subreg) =
					(uint16_t)*val;
				schedule_deadline(t, raw_now(t));
				return true;
			}
			if (subreg == 2) {
				/* Counter writes are enabled only in advanced mode. */
				if (advanced(t))
					t->count[channel] = (uint16_t)*val;
				schedule_deadline(t, raw_now(t));
				return true;
			}

			uint16_t stored = (uint16_t)*val & CTL_STORED;
			/* INITOL is likewise writable only in advanced mode. */
			if (!advanced(t))
				stored = (stored & ~INITOLx) |
					 (t->reg[reg / 2] & INITOLx);
			t->reg[reg / 2] = stored;
			if (*val & PRESETx)
				preset_channel(t, channel);
			schedule_deadline(t, raw_now(t));
		}
		else if (is_clkctl(reg)) {
			t->reg[reg / 2] = (uint16_t)*val & 0x000f;
			t->phase[(reg - (0x7e0u - T16_BASE)) / 2u] = 0;
			schedule_deadline(t, raw_now(t));
		}
		else if (reg / 2 < 0x80 / 2)
			t->reg[reg / 2] = (uint16_t)*val;
		return true;             /* configuration accepted */
	}

	switch (reg) {
	case OFF_CNT_PAUSE:
		*val = t->reg[reg / 2] & 0x003f;
		return true;
	case OFF_ADVMODE:
		*val = t->reg[reg / 2] & T16ADV;
		return true;
	default:
		if (is_channel_reg(reg)) {
			unsigned channel = channel_number(reg);
			unsigned subreg = channel_reg_number(reg);

			if (subreg < 2)
				*val = *comparison_address(t, channel, subreg);
			else if (subreg == 2)
				*val = t->count[channel], t->reads++;
			else
				*val = t->reg[reg / 2] & CTL_STORED;
		}
		else if (is_ctl(reg))
			*val = t->reg[reg / 2] & CTL_STORED;
		else if (is_clkctl(reg))
			*val = t->reg[reg / 2] & 0x000f;
		else
			*val = reg / 2 < 0x80 / 2 ? t->reg[reg / 2] : 0;
		return true;
	}
}

void timer_reset(struct timerblk *t)
{
	const uint64_t *cycles = t->cycles;
	const struct itc *itc = t->itc;
	const struct cmu *cmu = t->cmu;
	bool wall = t->wallclock;
	memset(t, 0, sizeof *t);
	t->cycles = cycles;
	t->itc = itc;
	t->cmu = cmu;
	t->wallclock = wall;
	if (wall)
		t->t0_ns = mono_ns();
}

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles,
		  const struct itc *itc, const struct cmu *cmu)
{
	memset(t, 0, sizeof *t);
	t->cycles = cycles;
	t->itc = itc;
	t->cmu = cmu;
	mem_add_mmio(m, "t16", T16_BASE, T16_LEN, timer_mmio, t);
}
