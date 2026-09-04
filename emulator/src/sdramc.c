/*
 * SDRAM controller (REG_BASE+0x1600).
 *
 * The RAM bytes themselves live in mem.c. This module supplies the control
 * registers and the waits imposed by the SDRAM interface. The EEPROM boot
 * chain brings up SDRAM before it can load anything large, and spins on
 *
 *     while ((REG_SDRAMC_INI & SDEN) == 0)
 *
 * which never completes if the register reads as zero.
 *
 * SDEN (D3) is read-only: "This bit indicates that the SDRAM has finished
 * initialization (Mode Register Set). ... SDEN is reset to 0 after power-on,
 * and is set to 1 upon completion of the initialization sequence."
 * (S1C33E07 Technical Manual, 0x301600). The sequence is PALL, REF then MRS
 * with the controller enabled, so the MRS command is what sets it.
 */

#include <string.h>

#include "sdramc.h"

#define OFF_INI  ((0x1600u - SDRAMC_BASE) / 4)
#define OFF_CTL  ((0x1604u - SDRAMC_BASE) / 4)
#define OFF_REF  ((0x1608u - SDRAMC_BASE) / 4)
#define OFF_APP  ((0x1610u - SDRAMC_BASE) / 4)

#define SDON    (1u << 4)   /* controller enable        R/W */
#define SDEN    (1u << 3)   /* initialized flag         R   */
#define INIMRS  (1u << 2)   /* mode register set        R/W */

#define DBF     (1u << 5)   /* SDCLK is twice MCLK          */
#define APPON   (1u << 1)   /* application unit enable     */
#define IQBEN   (1u << 0)   /* instruction queue enable    */

/*
 * Refresh register (0x301608). SELDO is read-only and reports whether the
 * SDRAM is actually in self-refresh:
 *
 *   D25 SELDO  SDRAM self-refresh status   1 Refresh mode  0 Done   R
 *   D23 SELEN  SDRAM self-refresh enable                            R/W
 *
 * grifo's suspend code, relocated into internal RAM, enables self-refresh
 * and then spins until SELDO reads back 1 before it powers things down.
 * With the bit unmodelled that loop never ends, and because Suspend()
 * disables interrupts first, nothing could wake it -- touch events queued
 * up and no interrupt was ever taken.
 */
#define SELDO   (1u << 25)  /* self-refresh status      R   */
#define SELEN   (1u << 23)  /* self-refresh enable      R/W */

struct geometry {
	unsigned banks, row_bits, col_bits;
};

/* Technical Manual table II.4.1.3.2, indexed by ADDRC[2:0]. */
static const struct geometry geometry[8] = {
	{ 2, 11,  8 }, { 4, 12,  8 }, { 4, 12,  9 }, { 4, 13,  9 },
	{ 2, 11,  9 }, { 4, 12,  9 }, { 4, 12, 10 }, { 4, 13, 10 },
};

static uint64_t sd_tick(const struct sdramc *s)
{
	return s->reg[OFF_APP] & DBF ? 1 : 2;
}

static unsigned trp(const struct sdramc *s)
{
	return ((s->reg[OFF_CTL] >> 12) & 3) + 1;
}

static unsigned tras(const struct sdramc *s)
{
	return ((s->reg[OFF_CTL] >> 8) & 7) + 1;
}

static unsigned trc(const struct sdramc *s)
{
	return ((s->reg[OFF_CTL] >> 4) & 15) + 1;
}

static unsigned cas(const struct sdramc *s)
{
	unsigned v = (s->reg[OFF_APP] >> 2) & 3;
	return v ? v : 2; /* 00 is reserved; retain the reset-safe latency. */
}

/*
 * Tell the memory map how much SDRAM the controller decodes.  Only once the
 * device is on line: direct ELF boots never program the controller and must
 * keep the flat window.
 */
static void publish_size(struct sdramc *s)
{
	uint32_t bytes = 0;

	if (s->initialised && (s->reg[OFF_INI] & SDON) &&
	    (s->reg[OFF_APP] & APPON)) {
		const struct geometry *g = &geometry[s->reg[OFF_CTL] & 7];
		bytes = (uint32_t)g->banks << (g->row_bits + g->col_bits + 1);
	}
	if (s->mem)
		mem_set_sdram_size(s->mem, bytes);
}

static void empty_queues(struct sdramc *s)
{
	for (unsigned i = 0; i < 4; i++)
		s->bank[i].valid = false;
	for (unsigned i = 0; i < 2; i++)
		s->iq[i].valid = false;
	s->dq.valid = false;
	s->iq_next = 0;
}

static void address_parts(const struct sdramc *s, uint32_t addr,
			  unsigned *bank, uint32_t *row)
{
	const struct geometry *g = &geometry[s->reg[OFF_CTL] & 7];
	uint32_t word = (addr - SDRAM_BASE) >> 1;

	*row = (word >> g->col_bits) & ((1u << g->row_bits) - 1);
	*bank = (word >> (g->col_bits + g->row_bits)) & (g->banks - 1);
}

/*
 * Insert an auto-refresh which became due before this request. Refresh
 * precharges all banks and occupies tRP + tRFC SDCLKs. Long idle intervals
 * are collapsed to the most recent refresh; completed older cycles cannot
 * delay the new request, but are retained in the diagnostic count.
 */
static void service_refresh(struct sdramc *s, uint64_t now)
{
	uint64_t tick = sd_tick(s);
	uint64_t period = ((s->reg[OFF_REF] & 0xfff) + 1) * tick;
	uint64_t n, latest, start;

	if (!s->next_refresh) {
		s->next_refresh = now + period;
		return;
	}
	if (now < s->next_refresh)
		return;

	n = (now - s->next_refresh) / period + 1;
	latest = s->next_refresh + (n - 1) * period;
	start = s->bus_free > latest ? s->bus_free : latest;
	for (unsigned i = 0; i < 4; i++)
		if (s->bank[i].valid) {
			uint64_t earliest = s->bank[i].activated + tras(s) * tick;
			if (start < earliest)
				start = earliest;
		}
	s->bus_free = start + (trp(s) + trc(s)) * tick;
	s->next_refresh += n * period;
	s->refreshes += n;
	s->last_sdram_access = s->bus_free;
	for (unsigned i = 0; i < 4; i++)
		s->bank[i].valid = false;
}

/* Advance the autonomous self-refresh counter without waking the SDRAM. */
static void observe_self_refresh(struct sdramc *s, uint64_t now)
{
	uint64_t idle, due;

	if (s->self_refresh || !(s->reg[OFF_REF] & SELEN) ||
	    !s->last_sdram_access)
		return;
	idle = ((s->reg[OFF_REF] >> 16) & 0x7f) * sd_tick(s);
	if (!idle)
		return;
	due = s->last_sdram_access + idle;
	/* A valid setup makes SELCO smaller than AURCO (manual II.4.1.5). */
	if (now >= due && (!s->next_refresh || due < s->next_refresh)) {
		s->self_refresh = true;
		s->next_refresh = 0;
		for (unsigned i = 0; i < 4; i++)
			s->bank[i].valid = false;
	}
}

static void prepare_external_access(struct sdramc *s, uint64_t now)
{
	observe_self_refresh(s, now);
	if (s->self_refresh) {
		uint64_t exit = now + (trc(s) + 1) * sd_tick(s);
		if (s->bus_free < exit)
			s->bus_free = exit;
		s->self_refresh = false;
		s->next_refresh = 0; /* self-refresh reset the auto-refresh counter */
		s->self_refresh_exits++;
	}
	service_refresh(s, now);
}

/* Return the READ/WRIT command time, activating or changing rows first. */
static uint64_t select_row(struct sdramc *s, uint32_t addr, uint64_t now)
{
	uint64_t tick = sd_tick(s);
	uint64_t at = s->bus_free > now ? s->bus_free : now;
	uint32_t row;
	unsigned b;

	address_parts(s, addr, &b, &row);
	if (s->bank[b].valid && s->bank[b].row == row)
		return at;

	if (s->bank[b].valid) {
		uint64_t earliest = s->bank[b].activated + tras(s) * tick;
		if (at < earliest)
			at = earliest;
		at += trp(s) * tick;
		earliest = s->bank[b].activated + trc(s) * tick;
		if (at < earliest)
			at = earliest;
	}

	s->bank[b].valid = true;
	s->bank[b].row = row;
	s->bank[b].activated = at;
	return at + trp(s) * tick; /* T24NS programs both tRP and tRCD. */
}

static uint64_t schedule_read(struct sdramc *s, uint32_t addr,
			      unsigned halfwords, uint64_t now,
			      uint64_t *ready)
{
	uint64_t tick = sd_tick(s);
	uint64_t command = select_row(s, addr, now);

	for (unsigned i = 0; i < halfwords; i++)
		ready[i] = command + (cas(s) + i + 1) * tick;
	s->bus_free = ready[halfwords - 1];
	s->last_sdram_access = s->bus_free;
	return command;
}

static void flush_written(struct sdramc *s, uint32_t addr, unsigned size)
{
	uint32_t last = addr + size - 1;

	for (unsigned i = 0; i < 2; i++)
		if (s->iq[i].valid && addr <= s->iq[i].tag + 15 &&
		    last >= s->iq[i].tag)
			s->iq[i].valid = false;
	if (s->dq.valid && addr <= s->dq.tag + 3 && last >= s->dq.tag)
		s->dq.valid = false;
}

static uint64_t schedule_write(struct sdramc *s, uint32_t addr,
			       unsigned size, uint64_t now)
{
	uint64_t tick = sd_tick(s);
	uint64_t command = select_row(s, addr, now);
	unsigned transfers = (size + (addr & 1) + 1) / 2;

	/* Writes are individual operations, one per external 16-bit transfer. */
	s->bus_free = command + transfers * tick;
	s->last_sdram_access = s->bus_free;
	flush_written(s, addr, size);
	s->writes_timed++;
	return s->bus_free;
}

static uint64_t sdramc_wait(void *ctx, enum mem_access access, uint32_t addr,
			    unsigned size, uint64_t mclk_now)
{
	struct sdramc *s = ctx;
	uint64_t now, ready, wait;

	if (addr < SDRAM_BASE || addr - SDRAM_BASE >= SDRAM_SIZE ||
	    !(s->reg[OFF_INI] & SDON) || !(s->reg[OFF_APP] & APPON) ||
	    !s->initialised)
		return 0;

	now = mclk_now * 2;
	s->accesses[access]++;
	observe_self_refresh(s, now);
	if (!s->self_refresh)
		service_refresh(s, now);

	if (access == MEM_CPU_FETCH && (s->reg[OFF_APP] & IQBEN)) {
		uint32_t tag = addr & ~15u;
		unsigned word = (addr >> 1) & 7;
		unsigned slot;

		for (slot = 0; slot < 2; slot++)
			if (s->iq[slot].valid && s->iq[slot].tag == tag)
				break;
		if (slot < 2) {
			s->iq_hits++;
			ready = s->iq[slot].ready[word];
		} else {
			prepare_external_access(s, now);
			slot = s->iq_next++ & 1;
			s->iq[slot].valid = true;
			s->iq[slot].tag = tag;
			schedule_read(s, tag, 8, now, s->iq[slot].ready);
			s->iq_misses++;
			ready = s->iq[slot].ready[word];
		}
	} else if (access == MEM_CPU_READ || access == MEM_DMA_READ ||
		   access == MEM_CPU_FETCH) {
		uint32_t tag = addr & ~3u;
		unsigned first = (addr >> 1) & 1;
		unsigned last = ((addr + size - 1) >> 1) & 1;

		if (s->dq.valid && s->dq.tag == tag) {
			s->dq_hits++;
		} else {
			prepare_external_access(s, now);
			s->dq.valid = true;
			s->dq.tag = tag;
			schedule_read(s, tag, 2, now, s->dq.ready);
			s->dq_misses++;
		}
		ready = s->dq.ready[first] > s->dq.ready[last]
		      ? s->dq.ready[first] : s->dq.ready[last];
	} else {
		prepare_external_access(s, now);
		ready = schedule_write(s, addr, size, now);
	}

	wait = ready > now ? (ready - now + 1) / 2 : 0;
	s->wait_cycles += wait;
	return wait;
}

static bool sdramc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
			bool is_write)
{
	struct sdramc *s = ctx;
	uint32_t i = (off - SDRAMC_BASE) / 4;

	if (i >= SDRAMC_LEN / 4)
		return false;

	if (is_write) {
		if (i == OFF_REF) {
			s->reg[i] = *val & 0x01ff0fffu; /* SELDO/reserved read zero */
		} else if (i == OFF_INI) {
			/* SDEN is not writable; the MRS command raises it. */
			s->reg[i] = *val & 0x17u;
			if (!(*val & SDON))
				s->initialised = false;
			if ((*val & SDON) && (*val & INIMRS))
				s->initialised = true;
		} else if (i == OFF_CTL) {
			s->reg[i] = *val & 0x000037f7u;
		} else if (i == OFF_APP) {
			s->reg[i] = *val & 0x8000003fu;
		} else {
			return true;             /* 0x160c is not a register */
		}
		if (i == OFF_CTL || i == OFF_APP ||
		    (i == OFF_INI && !(*val & SDON))) {
			empty_queues(s);
			s->bus_free = 0;
			s->next_refresh = 0;
			s->last_sdram_access = 0;
			s->self_refresh = false;
		}
		publish_size(s);
		if (i == OFF_REF) {
			s->next_refresh = 0;
			if (!(*val & SELEN))
				s->self_refresh = false;
		}
		s->writes++;
		return true;
	}

	*val = s->reg[i];
	if (i == OFF_INI && s->initialised)
		*val |= SDEN;
	/* Self-refresh is entered and left as soon as it is asked for. */
	if (i == OFF_REF && (s->reg[i] & SELEN))
		*val |= SELDO;
	return true;
}

/* Reset state without re-registering the device. */
void sdramc_reset(struct sdramc *s)
{
	struct mem *m = s->mem;

	memset(s, 0, sizeof *s);
	s->mem = m;
	/* S1C33E07 Technical Manual register tables, init. column. */
	s->reg[OFF_CTL] = 0x000000e0u;      /* tRC/tRFC/tXSR = 15 cycles */
	s->reg[OFF_REF] = 0x007f008cu;      /* self/auto-refresh counters */
	s->reg[OFF_APP] = 0x00000008u;      /* CAS latency 2 */
	publish_size(s);                    /* back to the flat window */
}

void sdramc_attach(struct mem *m, struct sdramc *s)
{
	s->mem = m;
	sdramc_reset(s);
	mem_add_mmio(m, "sdramc", SDRAMC_BASE, SDRAMC_LEN, sdramc_mmio, s);
	mem_set_timing(m, sdramc_wait, s);
}
