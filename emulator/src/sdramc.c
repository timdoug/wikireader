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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
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

/* Technical Manual table II.4.1.3.2, indexed by ADDRC[2:0], except for the
   32 MB settings, which are set from measurement instead.
 
   Both 32 MB WikiReaders (ADDRC 3) behave as 4 MB banks: alternating reads
   4, 8 and 16 MB apart all run at the speed of reads within one row, while
   everything from 1 KB to 2 MB apart pays a row change in hardware probes.
   The table's geometry would put the
   bank stride at 8 MB and make the 16 MB pair a row conflict.  The boards
   carry a single memory device, so the reason for the difference is not
   established; the entries below describe what the hardware does, which is
   what the reader's buffer placement needs.  The row size, 1 KB, is the
   table's and the measurement agrees. */
static const struct geometry geometry[8] = {
	{ 2, 11,  8 }, { 4, 12,  8 }, { 4, 12,  9 }, { 8, 12,  9 },
	{ 2, 11,  9 }, { 4, 12,  9 }, { 4, 12, 10 }, { 8, 12, 10 },
};

/* One SDCLK, in the half-MCLK units this module counts in.
 *
 * DBF selects the faster of the controller's two clocks. What the slower one
 * is the device had to say: a read that changes rows costs it 8.5 MCLK more
 * than one that does not, against a programmed tRP + tRCD of four SDCLK, so
 * an SDCLK is two MCLK and not one. The model ran the memory at twice the
 * speed of the machine, and every fitted overhead above it has been carrying
 * some of the difference.
 */
static uint64_t sd_tick(const struct sdramc *s)
{
	return s->reg[OFF_APP] & DBF ? model.sdclk_half / 2 : model.sdclk_half;
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
	for (unsigned i = 0; i < SDRAMC_MAX_DQ; i++)
		s->dq[i].valid = false;
	s->iq_next = 0;
	s->dq_next = 0;
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
	s->array_free = s->bus_free;
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
		if (s->array_free < exit)
			s->array_free = exit;
		s->self_refresh = false;
		s->next_refresh = 0; /* self-refresh reset the auto-refresh counter */
		s->self_refresh_exits++;
	}
	service_refresh(s, now);
}

/* Return the READ/WRIT command time, activating or changing rows first. */
/*
 * Benchmarking overrides.  Every write of the timing or refresh register,
 * whether by the boot loader or by the kernel's retime, is replaced with the
 * values from the environment, so one firmware image can be timed under
 * several controller settings:
 *
 *   WREMU_SDRAM_TIMING=tRP,tRAS,tRC   clocks, 1-4, 1-8, 1-16
 *   WREMU_SDRAM_AURCO=N               auto-refresh interval, SDCLK cycles - 1
 */
static void override_timing(struct sdramc *s)
{
	const char *e = getenv("WREMU_SDRAM_TIMING");
	unsigned trp_clk, tras_clk, trc_clk;

	if (e && sscanf(e, "%u,%u,%u", &trp_clk, &tras_clk, &trc_clk) == 3)
		s->reg[OFF_CTL] = (s->reg[OFF_CTL] & 7) |
			(((trp_clk - 1) & 3) << 12) |
			(((tras_clk - 1) & 7) << 8) |
			(((trc_clk - 1) & 15) << 4);
}

static void override_refresh(struct sdramc *s)
{
	const char *e = getenv("WREMU_SDRAM_AURCO");

	if (e)
		s->reg[OFF_REF] = (s->reg[OFF_REF] & ~0xfffu) |
			(strtoul(e, NULL, 0) & 0xfff);
}

/* Which kind of access is being timed, for the row-activation statistics. */
static unsigned current_kind;
bool sdramc_trace_on;
static bool trace_row_set;
static uint32_t trace_row;
static unsigned trace_left = 48;
static unsigned long trace_skip;   /* WREMU_ROWTRACE_SKIP: activations to pass first */
static bool last_was_write;

/* Which row of the whole device an address is in, bank bits and all: with
   the banks collapsed onto one row register the bank bits are part of what
   makes two rows different, and masking them off made two addresses four
   megabytes apart look like the same row. */
static uint32_t row_identity(const struct sdramc *s, uint32_t addr)
{
	const struct geometry *g = &geometry[s->reg[OFF_CTL] & 7];

	return (addr - SDRAM_BASE) >> (g->col_bits + 1);
}

/* Which row register an access uses: the bank its address decodes to, or,
   under the model's row_ports, the kind of access it is. */
static unsigned row_port(unsigned kind)
{
	switch (kind) {
	case MEM_CPU_FETCH:            return 0;
	case MEM_DMA_READ:
	case MEM_DMA_WRITE:            return 2;
	default:                       return 1;
	}
}

static uint64_t select_row(struct sdramc *s, uint32_t addr, uint64_t now)
{
	uint64_t tick = sd_tick(s);
	/* Without act_overlap the whole access, precharge and activation
	   included, queues behind the data bus. With it the activation starts
	   as soon as the request does and only the transfer serialises --
	   which is what the parts do: ACTIVATE is a command, tRCD elapses
	   inside the bank, and another bank may be moving data throughout. */
	uint64_t at = s->bus_free > now ? s->bus_free : now;

	if (model.act_overlap)
		at = s->array_free > now ? s->array_free : now;
	uint32_t row;
	unsigned b, pb;

	address_parts(s, addr, &b, &row);
	pb = b;
	if (model.row_ports) {
		row = row_identity(s, addr);
		b = row_port(current_kind);
	}
	s->kind_bank[current_kind][b]++;
	if (s->bank[b].valid && s->bank[b].row == row) {
		s->bank_last_kind[b] = current_kind;
		return at;
	}
	s->act_kind[s->bank_last_kind[b]][current_kind]++;
	s->act_bank[b]++;
	s->bank_last_kind[b] = current_kind;

	/*
	 * Closing the open row costs tRP.  tRAS and tRC are the array
	 * recovering and belong to the physical bank, which is not the same
	 * thing as the row register when row_ports collapses every data
	 * address onto one: charging them against the previous access on the
	 * port serialises accesses the eight banks overlap.  With the ubench
	 * rate sweep at two timings the device says a row change costs tRP
	 * and a constant -- 4 + 5.87 MCLK programmed one way, 2 + 5.90 the
	 * other -- while this charged tRC as well, and tRC alone was 12 of
	 * the 13.52 it put on a change the device measures at 9.87.
	 */
	{
		unsigned fb = model.bank_floors ? pb : b;
		bool open = model.bank_floors ? s->phys[fb].valid
					      : s->bank[fb].valid;
		uint64_t since = model.bank_floors ? s->phys[fb].activated
						   : s->bank[fb].activated;

		if (open) {
			uint64_t earliest = since + tras(s) * tick;
			if (at < earliest)
				at = earliest;
			at += trp(s) * tick;
			earliest = since + trc(s) * tick;
			if (at < earliest)
				at = earliest;
		}
	}

	s->bank[b].valid = true;
	s->bank[b].row = row;
	s->bank[b].activated = at;
	s->phys[pb].valid = true;
	s->phys[pb].activated = at;
	s->activations++;
	if (sdramc_trace_on && trace_row_set &&
	    ((addr - SDRAM_BASE) >> 10) == trace_row && trace_left &&
	    (trace_skip ? trace_skip-- == 0 : 1)) {
		trace_left--;
		fprintf(stderr, "rowtrace: pc %08x kind %u addr %08x from row %08x\n",
			wremu_cur_pc, current_kind, addr,
			0x10000000u + s->pair_last[b] * 1024u);
	}
	if (s->row_hist && ((addr - SDRAM_BASE) >> 10) < SDRAMC_ROW_HIST_ROWS) {
		uint32_t r = (addr - SDRAM_BASE) >> 10;
		uint32_t key = (s->pair_last[b] << 16) | r;
		uint32_t i = (key * 2654435761u) >> 16;

		s->row_hist[r]++;
		while (s->pair_hist[i].n && s->pair_hist[i].key != key)
			i = (i + 1) & (SDRAMC_PAIR_HIST_SIZE - 1);
		s->pair_hist[i].key = key;
		s->pair_hist[i].n++;
		s->pair_last[b] = r;
	}
	/* T24NS programs tRCD as well as tRP, and the device pays for both.
	   Sweeping the field alone moves a row change 2.56 MCLK a cycle
	   against tRC's 1.17 -- twice, because an activation waits tRCD
	   before the read whether or not a row had to be closed first
	   (boards/.../tools/ubench-sdclk-device.txt). */
	at += trp(s) * tick;
	return at + model.row_change_extra;
}

/*
 * A read of `halfwords` from the row.  The fitted model (model.h) adds the
 * controller's overheads: a line fill for the instruction queue pays
 * iqb_first before its first halfword and iqb_word_gap before every word
 * after the first, since the controller fetches a line as separate 32-bit
 * reads rather than one burst; a data-queue fill pays dq_extra.
 */
static uint64_t schedule_read(struct sdramc *s, uint32_t addr,
			      unsigned halfwords, uint64_t now,
			      uint64_t *ready)
{
	uint64_t tick = sd_tick(s);
	uint64_t command;
	uint64_t extra = halfwords > 2 ? model.iqb_first : model.dq_extra;

	/* A data read issued by code that is not itself coming over this bus
	   costs more than the bus timings account for; see model.h.
	   This charge existed, was fitted, and was then dropped by the row-model
	   refit while its parameter, its documentation and its override entry
	   all stayed -- so the model has been silently missing it since, and
	   dq_iram_extra could be set to anything without changing an answer.
	   The rv32 interpreter is what noticed: it is the only workload here
	   that runs almost entirely from internal RAM, and the device charges
	   it a fifth more than the model does. */
	if (halfwords <= 2 && current_kind != MEM_DMA_READ &&
	    wremu_cur_pc < 0x10000000u)
		extra += model.dq_iram_extra * tick;

	/* A read after a write waits for write recovery and the bus turn. */
	if (last_was_write && s->bus_free + model.wr_rd_turn * tick > now)
		now = s->bus_free + model.wr_rd_turn * tick;
	last_was_write = false;
	command = select_row(s, addr, now);
	/* The transfer still waits for the bus even where the activation did
	   not. */
	if (model.act_overlap && command < s->bus_free)
		command = s->bus_free;

	for (unsigned i = 0; i < halfwords; i++) {
		if (halfwords > 2)
			extra += (i && !(i & 1)) ? model.iqb_word_gap : 0;
		ready[i] = command + (cas(s) + i + model.cas_first) * tick + extra;
	}

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
	for (unsigned i = 0; i < SDRAMC_MAX_DQ; i++)
		if (s->dq[i].valid && addr <= s->dq[i].tag + 3 &&
		    last >= s->dq[i].tag)
			s->dq[i].valid = false;
}

static uint64_t schedule_write(struct sdramc *s, uint32_t addr,
			       unsigned size, uint64_t now)
{
	uint64_t tick = sd_tick(s);
	uint64_t command = select_row(s, addr, now);
	unsigned transfers = (size + (addr & 1) + 1) / 2;

	/* The transfer waits for the bus even where the activation did not --
	   the same clamp reads get, and it was missing here. Under
	   act_overlap every store in a run started at `now`, so the bus never
	   filled up and ramspeed's memset ran at twice the device's rate. */
	if (model.act_overlap && command < s->bus_free)
		command = s->bus_free;

	/* Writes are individual operations, one per external 16-bit transfer,
	 * unless the fitted model gives every write a flat cost. */
	s->bus_free = command + (model.wr_ticks ? model.wr_ticks
					       : transfers * tick);
	last_was_write = true;
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
	current_kind = access;
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
			unsigned fill_bank;
			uint32_t fill_row;

			prepare_external_access(s, now);
			slot = s->iq_next++ & 1;

			/*
			 * A queue line is only good while its row is open. A
			 * burst cannot cross a page boundary, so filling from
			 * another row of the same bank precharges the one the
			 * other line came from and leaves it stale.
			 *
			 * It shows up as a cliff. A loop that fits the queue
			 * costs the device 15 cycles a pass wherever it sits,
			 * until it straddles a 1 KB page -- its two lines then
			 * land in adjacent rows of one bank, each fill throws
			 * the other away, and it costs 47. Without this the
			 * model held both lines for ever and charged 14.55 at
			 * every offset, which is the right answer for seven of
			 * the eight places the loop can be and wrong for the
			 * one that matters.
			 */

			address_parts(s, tag, &fill_bank, &fill_row);
			for (unsigned i = 0; model.iq_row_evict && i < 2; i++) {
				unsigned bank;
				uint32_t row;

				if (!s->iq[i].valid || i == slot)
					continue;
				address_parts(s, s->iq[i].tag, &bank, &row);
				if (bank == fill_bank && row != fill_row) {
					s->iq[i].valid = false;
					s->iq_row_evictions++;
				}
			}

			s->iq[slot].valid = true;
			s->iq[slot].tag = tag;
			schedule_read(s, tag, 8, now, s->iq[slot].ready);
			s->iq_misses++;
			ready = s->iq[slot].ready[word];
		}

		/*
		 * The fetch unit runs ahead of the instruction being executed,
		 * so a loop whose body ends near the end of its second line
		 * pulls in a third before the branch takes it back, and the
		 * third evicts the first.  That, and not how many lines a body
		 * spans, is what decides whether a loop is resident here.  The
		 * device runs one fast while its offset inside the line plus
		 * its size is at most about 27 bytes and slowly from 30: a
		 * 26-byte body is fast at offset 0 and 2.5x slower at offset 4,
		 * an 18-byte one is fast until offset 12, and a 10-byte one is
		 * fast anywhere.  Spans alone predict none of that.
		 */

		/*
		 * Only while the stream is flowing.  The fetcher runs ahead of
		 * the instruction being executed, but a redirect restarts it,
		 * and a fetcher that has just restarted is not yet ahead of
		 * anything: it cannot have pulled the line past its target.
		 * callret is the case that says so -- eight call/ret pairs a
		 * pass, every fetch in it a redirect, and the device holds the
		 * 28-byte loop in the queue at 21.4 cycles a pair while this
		 * prefetched the line past the callee on every one of them and
		 * charged 68.8.
		 */
		bool flowing = addr > s->last_fetch && addr - s->last_fetch <= 4;

		s->last_fetch = addr;
		if (model.iq_lookahead && (flowing || !model.iq_lookahead_seq)) {
			uint32_t ahead = (addr + model.iq_lookahead) & ~15u;
			unsigned i;

			if (ahead != tag) {
				for (i = 0; i < 2; i++)
					if (s->iq[i].valid &&
					    s->iq[i].tag == ahead)
						break;
				if (i == 2) {
					unsigned pslot = s->iq_next++ & 1;

					prepare_external_access(s, now);
					s->iq[pslot].valid = true;
					s->iq[pslot].tag = ahead;
					schedule_read(s, ahead, 8, now,
						      s->iq[pslot].ready);
					s->iq_prefetches++;
				}
			}
		}
	} else if (access == MEM_CPU_READ || access == MEM_DMA_READ ||
		   access == MEM_CPU_FETCH) {
		uint32_t tag = addr & ~3u;
		unsigned first = (addr >> 1) & 1;
		unsigned last = ((addr + size - 1) >> 1) & 1;

		unsigned entries = model.dq_entries ? model.dq_entries : 1;
		unsigned slot;

		if (entries > SDRAMC_MAX_DQ)
			entries = SDRAMC_MAX_DQ;
		for (slot = 0; slot < entries; slot++)
			if (s->dq[slot].valid && s->dq[slot].tag == tag)
				break;
		if (slot < entries) {
			s->dq_hits++;
			if (model.dq_hit) {
				/* dq_hit counts in half-MCLK, not SDCLK: the
				   queue answers out of the controller rather
				   than over the bus, and the device puts the
				   cost at one MCLK -- half an SDCLK, which
				   the coarser unit cannot say. */
				uint64_t at = now + model.dq_hit;
				if (s->dq[slot].ready[0] < at) s->dq[slot].ready[0] = at;
				if (s->dq[slot].ready[1] < at) s->dq[slot].ready[1] = at;
			}
		} else {
			prepare_external_access(s, now);
			slot = s->dq_next++ % entries;
			s->dq[slot].valid = true;
			s->dq[slot].tag = tag;
			schedule_read(s, tag, 2, now, s->dq[slot].ready);
			s->dq_misses++;
		}
		ready = s->dq[slot].ready[first] > s->dq[slot].ready[last]
		      ? s->dq[slot].ready[first] : s->dq[slot].ready[last];
	} else {
		prepare_external_access(s, now);
		ready = schedule_write(s, addr, size, now);

		/* A posted write does not hold the CPU up: it goes into the
		 * controller's buffer and drains behind whatever the program
		 * does next, which is why the device copies a word at a time
		 * faster than four at a time -- the compare and the branch
		 * between two accesses are free time for the bus. The buffer
		 * is shallow, so a run of stores with nothing between them
		 * fills it and waits; write_post is how many it holds.
		 */
		if (model.write_post) {
			unsigned i, oldest = 0;

			for (i = 0; i < model.write_post &&
				    i < SDRAMC_WRITE_POST_MAX; i++)
				if (s->posted[i] < s->posted[oldest])
					oldest = i;
			if (s->posted[oldest] <= now)
				ready = now;          /* room: post it */
			else
				ready = s->posted[oldest];
			s->posted[oldest] = s->bus_free;
		}
	}

	/* The fetch after a jump waits for the fetch path to restart whatever
	   the queue happens to hold, so this is a floor and not an addition:
	   the line the target sits in is very often already there -- a jump to
	   the next instruction never leaves it -- and charging only the queue
	   makes such a jump free, which the device says it is not.  ubench's
	   br32 is long with half its adds replaced by jumps to the following
	   instruction, same bytes and same instruction count, and the device
	   charges 312 cycles against 183. */
	if (access == MEM_CPU_FETCH && wremu_fetch_restart && model.branch_bubble) {
		uint64_t floor = now + 2 * (uint64_t)model.branch_bubble;

		if (ready < floor)
			ready = floor;
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
			override_refresh(s);
		} else if (i == OFF_INI) {
			/* SDEN is not writable; the MRS command raises it. */
			s->reg[i] = *val & 0x17u;
			if (!(*val & SDON))
				s->initialised = false;
			if ((*val & SDON) && (*val & INIMRS))
				s->initialised = true;
		} else if (i == OFF_CTL) {
			s->reg[i] = *val & 0x000037f7u;
			override_timing(s);
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

	uint64_t *row_hist = s->row_hist;
	struct sdramc_pair *pair_hist = s->pair_hist;

	memset(s, 0, sizeof *s);
	s->mem = m;
	s->row_hist = row_hist;
	s->pair_hist = pair_hist;
	/* S1C33E07 Technical Manual register tables, init. column. */
	s->reg[OFF_CTL] = 0x000000e0u;      /* tRC/tRFC/tXSR = 15 cycles */
	s->reg[OFF_REF] = 0x007f008cu;      /* self/auto-refresh counters */
	s->reg[OFF_APP] = 0x00000008u;      /* CAS latency 2 */
	publish_size(s);                    /* back to the flat window */
}

void sdramc_attach(struct mem *m, struct sdramc *s)
{
	s->mem = m;
	s->row_hist = NULL;
	s->pair_hist = NULL;
	sdramc_reset(s);
	if (getenv("WREMU_ROWTRACE")) {
		trace_row = (uint32_t)((strtoul(getenv("WREMU_ROWTRACE"), NULL, 0) - SDRAM_BASE) >> 10);
		trace_row_set = true;
		if (getenv("WREMU_ROWTRACE_SKIP"))
			trace_skip = strtoul(getenv("WREMU_ROWTRACE_SKIP"), NULL, 0);
	}
	if (getenv("WREMU_ROWHIST")) {
		s->row_hist = calloc(SDRAMC_ROW_HIST_ROWS, sizeof *s->row_hist);
		s->pair_hist = calloc(SDRAMC_PAIR_HIST_SIZE, sizeof *s->pair_hist);
	}
	mem_add_mmio(m, "sdramc", SDRAMC_BASE, SDRAMC_LEN, sdramc_mmio, s);
	mem_set_timing(m, sdramc_wait, s);
}
