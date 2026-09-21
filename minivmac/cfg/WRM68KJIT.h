/*
 * WikiReader 68000 -> C33 trace translator.
 *
 * Included near the end of upstream MINEM68K.c so the generated code can
 * call the core's private instruction handlers. Hot paths become real C33
 * functions in an SDRAM code cache. A translation initially calls the
 * original handlers for uncommon operations, while common control-flow
 * instructions are emitted natively. This keeps the fallback exact and lets
 * native coverage grow one instruction family at a time. Frequently used
 * blocks are re-emitted into IVRAM after proving hot.
 */
#ifndef MINIVMAC_WR_M68K_JIT_H
#define MINIVMAC_WR_M68K_JIT_H

#ifndef MINIVMAC_JIT_NATIVE_MASK
#define MINIVMAC_JIT_NATIVE_MASK 0x1fff
#endif
#ifndef MINIVMAC_JIT_INLINE_MEMORY_MASK
#define MINIVMAC_JIT_INLINE_MEMORY_MASK (1 | 2 | 32)
#endif
#ifndef MINIVMAC_JIT_CHAIN
#define MINIVMAC_JIT_CHAIN 1
#endif
#ifndef MINIVMAC_PROFILE
#define MINIVMAC_PROFILE 0
#endif
#ifndef MINIVMAC_JIT_SLOTS
#define MINIVMAC_JIT_SLOTS 16384
#endif
#ifndef MINIVMAC_JIT_CODE_BYTES
#define MINIVMAC_JIT_CODE_BYTES (16 * 1024 * 1024)
#endif
#ifndef MINIVMAC_JIT_WAYS
#define MINIVMAC_JIT_WAYS 2
#endif
#ifndef MINIVMAC_JIT_DIRECT_RAM
#define MINIVMAC_JIT_DIRECT_RAM 1
#endif
#ifndef MINIVMAC_JIT_IMMEDIATE_ARITH
#define MINIVMAC_JIT_IMMEDIATE_ARITH 0
#endif
#ifndef MINIVMAC_JIT_CMP_TST
#define MINIVMAC_JIT_CMP_TST 0
#endif
#ifndef MINIVMAC_JIT_BYTE_ARITH
#define MINIVMAC_JIT_BYTE_ARITH 1
#endif
#ifndef MINIVMAC_JIT_AINDEX
#define MINIVMAC_JIT_AINDEX 1
#endif
#ifndef MINIVMAC_JIT_REG_CACHE
#define MINIVMAC_JIT_REG_CACHE 1
#endif
#define WR_JIT_NATIVE_BRA   0x01
#define WR_JIT_NATIVE_DBF   0x02
#define WR_JIT_NATIVE_MOVEQ 0x04
#define WR_JIT_NATIVE_NOP   0x08
#define WR_JIT_NATIVE_REGL  0x10
#define WR_JIT_NATIVE_CC    0x20
#define WR_JIT_NATIVE_MOVEL 0x40
#define WR_JIT_NATIVE_BIT   0x80
#define WR_JIT_NATIVE_LOGIC 0x100
#define WR_JIT_NATIVE_MOVEM 0x200
#define WR_JIT_NATIVE_MOVEA 0x400
#define WR_JIT_NATIVE_CLR   0x800
#define WR_JIT_NATIVE_CALL  0x1000

enum {
	WR_JIT_SLOTS = MINIVMAC_JIT_SLOTS,
	WR_JIT_WAYS = MINIVMAC_JIT_WAYS,
	WR_JIT_SETS = WR_JIT_SLOTS / WR_JIT_WAYS,
	WR_JIT_OPS = 16,
	WR_JIT_CODE_BYTES = MINIVMAC_JIT_CODE_BYTES,
	WR_JIT_MAX_BLOCK_BYTES = 8192,
	WR_JIT_FAST_BYTES = 0x1600,
	WR_JIT_PROMOTE_HITS = 64,
	WR_JIT_REBUILD_HITS = 8192,
	/* pushn, materialize V_regs, then load the live PC and cycle count. */
	WR_JIT_CHAIN_ENTRY_BYTES = 14,
	WR_JIT_CHAIN_MAX_BYTES = 64,
};

/* Mini vMac uses IVRAM 0x80000..0x819ff for its LCD framebuffer.  The
 * remaining 5.5 KiB window is executable and has dramatically cheaper
 * instruction fetches than SDRAM.  Unlike a memcpy promotion, re-emitting at
 * the final address keeps the C33's PC-relative calls correct. */
#define WR_JIT_FAST_BASE ((ui3p)0x00081a00)

enum wr_jit_kind {
	wr_jit_interpret,
	wr_jit_bra,
	wr_jit_dbf,
	wr_jit_moveq,
	wr_jit_nop,
	wr_jit_move_rr_b,
	wr_jit_move_rr_w,
	wr_jit_move_rr_l,
	wr_jit_movea_w,
	wr_jit_movea_l,
	wr_jit_clr_b,
	wr_jit_clr_w,
	wr_jit_clr_l,
	wr_jit_tst_r_b,
	wr_jit_tst_r_w,
	wr_jit_tst_r_l,
	wr_jit_cmp_rr_b,
	wr_jit_cmp_rr_w,
	wr_jit_cmp_rr_l,
	wr_jit_add_rr_l,
	wr_jit_sub_rr_l,
	wr_jit_addq_a,
	wr_jit_subq_a,
	wr_jit_addq_w,
	wr_jit_subq_w,
	wr_jit_add_rr_w,
	wr_jit_sub_rr_w,
	wr_jit_addq_b,
	wr_jit_subq_b,
	wr_jit_add_rr_b,
	wr_jit_sub_rr_b,
	wr_jit_logic_rr,
	wr_jit_not_r,
	wr_jit_asr_w,
	wr_jit_btst_b,
	wr_jit_bcc_pending,
	wr_jit_bcc,
	wr_jit_dbcc,
	wr_jit_movem_pre_l,
	wr_jit_movem_post_l,
	wr_jit_movem_store_l,
	wr_jit_adda,
	wr_jit_suba,
	wr_jit_jsr,
	wr_jit_jmp,
	wr_jit_rts,
	wr_jit_link,
	wr_jit_unlk,
};

LOCALINLINEFUNC blnr wr_jit_simple_move_mode(ui3r amd, ui3r size)
{
	if (size == 1)
		return amd == kAMdRegB || amd == kAMdIndirectB
			|| amd == kAMdAPosIncB || amd == kAMdAPosInc7B
			|| amd == kAMdAPreDecB || amd == kAMdAPreDec7B
			|| amd == kAMdADispB;
	if (size == 2)
		return amd == kAMdRegW || amd == kAMdIndirectW
			|| amd == kAMdAPosIncW || amd == kAMdAPreDecW
			|| amd == kAMdADispW;
	return amd == kAMdRegL || amd == kAMdIndirectL
		|| amd == kAMdAPosIncL || amd == kAMdAPreDecL
		|| amd == kAMdADispL;
}

LOCALINLINEFUNC blnr wr_jit_is_adisp(ui3r amd)
{
	return amd == kAMdADispB || amd == kAMdADispW || amd == kAMdADispL;
}

LOCALINLINEFUNC blnr wr_jit_is_aindex(ui3r amd)
{
	return amd == kAMdAIndexB || amd == kAMdAIndexW
		|| amd == kAMdAIndexL;
}

struct wr_jit_op {
	ui3p at;
	ui3p next;
	func_pointer_t handler;
	DecOpYR y;
	ui4rr cycles;
	ui4b opcode;
	ui4b extension;
	ui4b extension2;
	ui3b kind;
	ui3b reg;
	ui3b src_reg;
	ui3b check_extension;
};

struct wr_jit_block {
	ui3p start;
	ui3p code;
	ui3p fast_code;
	ui5r code_bytes;
	ui5r hits;
	ui5r cycles;
	ui3b count;
	ui3b validate;
	struct wr_jit_op op[WR_JIT_OPS];
	ui5r direct_reg_value;
	ui5r direct_guest_addr;
	ui3p direct_host_addr;
	ui5r guest_start;
	ui5r profile_runs;
	ui3b direct_guest_reg;
	ui3b fallback_count;
	ui3b memory_overlay;
	/* Keep direct-map lookup to a single C33 shift. */
	ui3b lookup_padding[13];
};

typedef char wr_jit_block_must_be_512_bytes[
	(sizeof(struct wr_jit_block) == 512) ? 1 : -1];

/* Generated code shares a compact coalescing allocator.  Every chunk has a
 * 16-byte header, keeping the returned C33 entry point 16-byte aligned.  The
 * previous-size field makes both-neighbour coalescing constant-time; free
 * chunks use the remaining header words as intrusive list links. */
struct wr_jit_code_chunk {
	ui5r size_flags;
	ui5r prev_size;
	struct wr_jit_code_chunk *next_free;
	struct wr_jit_code_chunk *prev_free;
};

typedef char wr_jit_code_chunk_must_be_16_bytes[
	(sizeof(struct wr_jit_code_chunk) == 16) ? 1 : -1];

enum {
	WR_JIT_CODE_CHUNK_USED = 1,
	WR_JIT_CODE_CHUNK_HEADER = 16,
	WR_JIT_CODE_CHUNK_MIN = 32,
	WR_JIT_CODE_BINS = 25,
};

LOCALVAR struct wr_jit_block wr_jit_blocks[WR_JIT_SLOTS];
LOCALVAR ui3b wr_jit_replace[WR_JIT_SETS];
LOCALVAR ui3b wr_jit_code[WR_JIT_CODE_BYTES]
	__attribute__((aligned(16)));
LOCALVAR ui5r wr_jit_code_used;
LOCALVAR ui5r wr_jit_code_peak;
LOCALVAR ui5r wr_jit_code_reclaimed;
LOCALVAR struct wr_jit_code_chunk *wr_jit_code_bins[WR_JIT_CODE_BINS];
LOCALVAR blnr wr_jit_code_initialized;
LOCALVAR ui5r wr_jit_fast_used;
LOCALVAR blnr wr_jit_fast_rebuilt;
LOCALVAR ui5r wr_jit_active_slots;

#if MINIVMAC_PROFILE
enum {
	WR_JIT_PROFILE_BUILDS,
	WR_JIT_PROFILE_PROMOTIONS,
	WR_JIT_PROFILE_REBUILDS,
	WR_JIT_PROFILE_FLUSHES,
	WR_JIT_PROFILE_DIRECT_BUILDS,
	WR_JIT_PROFILE_DIRECT_REG_INVALID,
	WR_JIT_PROFILE_DIRECT_MAP_INVALID,
	WR_JIT_PROFILE_CODE_INVALID,
	WR_JIT_PROFILE_GUEST_OPS_BUILT,
	WR_JIT_PROFILE_FALLBACK_OPS_BUILT,
	WR_JIT_PROFILE_EVICTIONS,
	WR_JIT_PROFILE_COUNT
};
LOCALVAR ui5r wr_jit_profile[WR_JIT_PROFILE_COUNT];
LOCALVAR ui5r wr_jit_fallback_hist[8192];
#define WR_JIT_PROFILE_INC(which) (++wr_jit_profile[(which)])

GLOBALPROC MiniVMac_JIT_GetProfile(ui5r *out)
{
	ui3r i;
	for (i = 0; i < WR_JIT_PROFILE_COUNT; ++i)
		out[i] = wr_jit_profile[i];
}

GLOBALPROC MiniVMac_JIT_GetCacheProfile(ui5r *out)
{
	ui5r largest = 0;
	ui5r chunks = 0;
	ui5r free_bytes = 0;
	ui5r linear_free = 0;
	ui5r linear_used = 0;
	ui5r errors = 0;
	ui3r i;

	for (i = 0; i < WR_JIT_CODE_BINS; ++i) {
		struct wr_jit_code_chunk *chunk;
		for (chunk = wr_jit_code_bins[i]; chunk != 0;
			chunk = chunk->next_free) {
			ui5r size = chunk->size_flags;
			if (size > largest)
				largest = size;
			free_bytes += size;
			++chunks;
		}
	}
	if (wr_jit_code_initialized) {
		ui3p at = wr_jit_code;
		ui5r previous = 0;
		while (at < wr_jit_code + WR_JIT_CODE_BYTES) {
			struct wr_jit_code_chunk *chunk =
				(struct wr_jit_code_chunk *)at;
			ui5r size = chunk->size_flags & ~(ui5r)15;
			if (size < WR_JIT_CODE_CHUNK_MIN || (size & 15) != 0
				|| size > (ui5r)(wr_jit_code + WR_JIT_CODE_BYTES - at)) {
				++errors;
				break;
			}
			if (chunk->prev_size != previous)
				++errors;
			if ((chunk->size_flags & WR_JIT_CODE_CHUNK_USED) != 0)
				linear_used += size;
			else
				linear_free += size;
			previous = size;
			at += size;
		}
		if (at != wr_jit_code + WR_JIT_CODE_BYTES
			|| linear_used != wr_jit_code_used
			|| linear_free != free_bytes
			|| linear_used + linear_free != WR_JIT_CODE_BYTES)
			++errors;
	}
	out[0] = wr_jit_active_slots;
	out[1] = WR_JIT_SLOTS;
	out[2] = WR_JIT_WAYS;
	out[3] = wr_jit_code_used;
	out[4] = WR_JIT_CODE_BYTES;
	out[5] = wr_jit_fast_used;
	out[6] = largest;
	out[7] = chunks;
	out[8] = wr_jit_code_peak;
	out[9] = wr_jit_code_reclaimed;
	out[10] = free_bytes;
	out[11] = errors;
}

/* Return the live guest PC followed by the four hottest trace start PCs,
 * dispatch counts, and first opcodes since the previous report.  A periodic
 * PC sample alone tends to land in the 60 Hz interrupt epilogue; counting
 * trace dispatches identifies the loop consuming the interval instead. */
GLOBALPROC MiniVMac_JIT_GetProfileHot(ui5r *out)
{
	ui5r best_runs[4] = {0, 0, 0, 0};
	ui5r best_pc[4] = {0, 0, 0, 0};
	ui5r best_opcode[4] = {0, 0, 0, 0};
	ui5r i;
	ui3r j;
	ui5r executed_ops = 0;
	ui5r fallback_ops = 0;
	ui3r top;

	for (i = 0; i < 8192; ++i)
		wr_jit_fallback_hist[i] = 0;

	out[0] = m68k_getpc();
	for (i = 0; i < WR_JIT_SLOTS; ++i) {
		struct wr_jit_block *block = &wr_jit_blocks[i];
		ui5r runs = block->profile_runs;
		ui5r address;
		if (runs == 0 || block->start == nullpr)
			continue;
		executed_ops += runs * block->count;
		fallback_ops += runs * block->fallback_count;
		for (j = 0; j < block->count; ++j)
			if (block->op[j].kind == wr_jit_interpret)
				wr_jit_fallback_hist[block->op[j].opcode >> 3] += runs;
		if ((ui5r)block->start >= (ui5r)RAM
			&& (ui5r)block->start < (ui5r)RAM + kRAM_Size)
			address = (ui5r)block->start - (ui5r)RAM;
		else if ((ui5r)block->start >= (ui5r)ROM
			&& (ui5r)block->start < (ui5r)ROM + kROM_Size)
			address = kROM_Base + (ui5r)block->start - (ui5r)ROM;
		else
			address = 0xffffffffUL;
		for (j = 0; j < 4; ++j) {
			if (runs > best_runs[j]) {
				ui3r k;
				for (k = 3; k > j; --k) {
					best_runs[k] = best_runs[k - 1];
					best_pc[k] = best_pc[k - 1];
					best_opcode[k] = best_opcode[k - 1];
				}
				best_runs[j] = runs;
				best_pc[j] = address;
				best_opcode[j] = block->count != 0
					? block->op[0].opcode : 0;
				break;
			}
		}
	}
	for (i = 0; i < WR_JIT_SLOTS; ++i)
		wr_jit_blocks[i].profile_runs = 0;
	for (j = 0; j < 4; ++j) {
		out[1 + j * 3] = best_pc[j];
		out[2 + j * 3] = best_runs[j];
		out[3 + j * 3] = best_opcode[j];
	}
	out[13] = executed_ops;
	out[14] = fallback_ops;
	for (top = 0; top < 8; ++top) {
		ui5r best_count = 0;
		ui5r best_group = 0;
		for (i = 0; i < 8192; ++i) {
			if (wr_jit_fallback_hist[i] > best_count) {
				best_count = wr_jit_fallback_hist[i];
				best_group = i;
			}
		}
		out[15 + top * 2] = best_group << 3;
		out[16 + top * 2] = best_count;
		wr_jit_fallback_hist[best_group] = 0;
	}
}
#else
#define WR_JIT_PROFILE_INC(which) ((void)0)
#endif

LOCALINLINEFUNC ui5r wr_jit_code_chunk_size(
	const struct wr_jit_code_chunk *chunk)
{
	return chunk->size_flags & ~(ui5r)15;
}

LOCALINLINEFUNC ui3r wr_jit_code_bin(ui5r size)
{
	ui3r bin = 0;
	while (size > 1 && bin + 1 < WR_JIT_CODE_BINS) {
		size >>= 1;
		++bin;
	}
	return bin;
}

LOCALPROC wr_jit_code_remove_free(struct wr_jit_code_chunk *chunk)
{
	ui3r bin = wr_jit_code_bin(wr_jit_code_chunk_size(chunk));
	if (chunk->prev_free != 0)
		chunk->prev_free->next_free = chunk->next_free;
	else
		wr_jit_code_bins[bin] = chunk->next_free;
	if (chunk->next_free != 0)
		chunk->next_free->prev_free = chunk->prev_free;
}

LOCALPROC wr_jit_code_insert_free(struct wr_jit_code_chunk *chunk)
{
	ui3r bin = wr_jit_code_bin(wr_jit_code_chunk_size(chunk));
	chunk->prev_free = 0;
	chunk->next_free = wr_jit_code_bins[bin];
	if (chunk->next_free != 0)
		chunk->next_free->prev_free = chunk;
	wr_jit_code_bins[bin] = chunk;
}

LOCALPROC wr_jit_code_allocator_init(void)
{
	struct wr_jit_code_chunk *chunk =
		(struct wr_jit_code_chunk *)wr_jit_code;
	ui3r i;

	for (i = 0; i < WR_JIT_CODE_BINS; ++i)
		wr_jit_code_bins[i] = 0;
	chunk->size_flags = WR_JIT_CODE_BYTES;
	chunk->prev_size = 0;
	chunk->next_free = 0;
	chunk->prev_free = 0;
	wr_jit_code_insert_free(chunk);
	wr_jit_code_used = 0;
	wr_jit_code_initialized = trueblnr;
}

LOCALFUNC ui3p wr_jit_code_alloc(ui5r payload_bytes)
{
	struct wr_jit_code_chunk *chunk = 0;
	ui5r needed = (payload_bytes + WR_JIT_CODE_CHUNK_HEADER + 15)
		& ~(ui5r)15;
	ui5r chunk_size;
	ui3r bin;

	if (!wr_jit_code_initialized)
		wr_jit_code_allocator_init();
	bin = wr_jit_code_bin(needed);
	for (; bin < WR_JIT_CODE_BINS && chunk == 0; ++bin) {
		struct wr_jit_code_chunk *candidate;
		for (candidate = wr_jit_code_bins[bin]; candidate != 0;
			candidate = candidate->next_free) {
			if (wr_jit_code_chunk_size(candidate) >= needed) {
				chunk = candidate;
				break;
			}
		}
	}
	if (chunk == 0)
		return nullpr;

	wr_jit_code_remove_free(chunk);
	chunk_size = wr_jit_code_chunk_size(chunk);
	if (chunk_size - needed >= WR_JIT_CODE_CHUNK_MIN) {
		struct wr_jit_code_chunk *remainder =
			(struct wr_jit_code_chunk *)((ui3p)chunk + needed);
		struct wr_jit_code_chunk *after;
		ui5r remainder_size = chunk_size - needed;

		chunk_size = needed;
		remainder->size_flags = remainder_size;
		remainder->prev_size = chunk_size;
		after = (struct wr_jit_code_chunk *)
			((ui3p)remainder + remainder_size);
		if ((ui3p)after < wr_jit_code + WR_JIT_CODE_BYTES)
			after->prev_size = remainder_size;
		wr_jit_code_insert_free(remainder);
	} else {
		struct wr_jit_code_chunk *after =
			(struct wr_jit_code_chunk *)((ui3p)chunk + chunk_size);
		if ((ui3p)after < wr_jit_code + WR_JIT_CODE_BYTES)
			after->prev_size = chunk_size;
	}
	chunk->size_flags = chunk_size | WR_JIT_CODE_CHUNK_USED;
	chunk->next_free = 0;
	chunk->prev_free = 0;
	wr_jit_code_used += chunk_size;
	if (wr_jit_code_used > wr_jit_code_peak)
		wr_jit_code_peak = wr_jit_code_used;
	return (ui3p)chunk + WR_JIT_CODE_CHUNK_HEADER;
}

LOCALPROC wr_jit_code_free(ui3p code)
{
	struct wr_jit_code_chunk *chunk;
	struct wr_jit_code_chunk *next;
	ui5r size;

	if (code == nullpr)
		return;
	chunk = (struct wr_jit_code_chunk *)
		(code - WR_JIT_CODE_CHUNK_HEADER);
	size = wr_jit_code_chunk_size(chunk);
	wr_jit_code_used -= size;
	wr_jit_code_reclaimed += size;
	chunk->size_flags = size;

	next = (struct wr_jit_code_chunk *)((ui3p)chunk + size);
	if ((ui3p)next < wr_jit_code + WR_JIT_CODE_BYTES
		&& (next->size_flags & WR_JIT_CODE_CHUNK_USED) == 0) {
		ui5r next_size = wr_jit_code_chunk_size(next);
		wr_jit_code_remove_free(next);
		size += next_size;
		chunk->size_flags = size;
	}
	if (chunk->prev_size != 0) {
		struct wr_jit_code_chunk *previous =
			(struct wr_jit_code_chunk *)
			((ui3p)chunk - chunk->prev_size);
		if ((previous->size_flags & WR_JIT_CODE_CHUNK_USED) == 0) {
			wr_jit_code_remove_free(previous);
			size += wr_jit_code_chunk_size(previous);
			previous->size_flags = size;
			chunk = previous;
		}
	}
	next = (struct wr_jit_code_chunk *)((ui3p)chunk + size);
	if ((ui3p)next < wr_jit_code + WR_JIT_CODE_BYTES)
		next->prev_size = size;
	wr_jit_code_insert_free(chunk);
}

LOCALPROC wr_jit_code_trim(ui3p code, ui5r payload_bytes)
{
	struct wr_jit_code_chunk *chunk =
		(struct wr_jit_code_chunk *)(code - WR_JIT_CODE_CHUNK_HEADER);
	ui5r old_size = wr_jit_code_chunk_size(chunk);
	ui5r needed = (payload_bytes + WR_JIT_CODE_CHUNK_HEADER + 15)
		& ~(ui5r)15;
	struct wr_jit_code_chunk *remainder;
	struct wr_jit_code_chunk *after;
	ui5r remainder_size;

	if (old_size - needed < WR_JIT_CODE_CHUNK_MIN)
		return;
	remainder = (struct wr_jit_code_chunk *)((ui3p)chunk + needed);
	remainder_size = old_size - needed;
	chunk->size_flags = needed | WR_JIT_CODE_CHUNK_USED;
	remainder->size_flags = remainder_size;
	remainder->prev_size = needed;
	after = (struct wr_jit_code_chunk *)
		((ui3p)remainder + remainder_size);
	if ((ui3p)after < wr_jit_code + WR_JIT_CODE_BYTES
		&& (after->size_flags & WR_JIT_CODE_CHUNK_USED) == 0) {
		ui5r after_size = wr_jit_code_chunk_size(after);
		wr_jit_code_remove_free(after);
		remainder_size += after_size;
		remainder->size_flags = remainder_size;
		after = (struct wr_jit_code_chunk *)
			((ui3p)remainder + remainder_size);
	}
	if ((ui3p)after < wr_jit_code + WR_JIT_CODE_BYTES)
		after->prev_size = remainder_size;
	wr_jit_code_used -= old_size - needed;
	wr_jit_code_insert_free(remainder);
}

LOCALINLINEFUNC struct wr_jit_block *wr_jit_slot(ui3p pc)
{
	ui5r address = (ui5r)pc;
	ui5r hash = (address >> 1) ^ (address >> 12) ^ (address >> 20);
	ui5r set = hash & (WR_JIT_SETS - 1);
	struct wr_jit_block *way = &wr_jit_blocks[set * WR_JIT_WAYS];
	ui3r i;

	for (i = 0; i < WR_JIT_WAYS; ++i)
		if (way[i].start == pc)
			return &way[i];
	for (i = 0; i < WR_JIT_WAYS; ++i)
		if (way[i].start == nullpr)
			return &way[i];
	return &way[wr_jit_replace[set]];
}

LOCALPROC wr_jit_flush(void)
{
	ui5r i;
	WR_JIT_PROFILE_INC(WR_JIT_PROFILE_FLUSHES);
	for (i = 0; i < WR_JIT_SLOTS; ++i) {
		wr_jit_blocks[i].start = nullpr;
		wr_jit_blocks[i].code = nullpr;
		wr_jit_blocks[i].fast_code = nullpr;
		wr_jit_blocks[i].code_bytes = 0;
		wr_jit_blocks[i].hits = 0;
		wr_jit_blocks[i].profile_runs = 0;
		wr_jit_blocks[i].count = 0;
	}
	for (i = 0; i < WR_JIT_SETS; ++i)
		wr_jit_replace[i] = 0;
	wr_jit_code_allocator_init();
	wr_jit_fast_used = 0;
	wr_jit_fast_rebuilt = falseblnr;
	wr_jit_active_slots = 0;
}

LOCALINLINEFUNC blnr wr_jit_direct_mapping(ui3p at, ui3p target,
	ui3p map_lo, ui3p map_hi)
{
	return at + 2 < map_hi && target >= map_lo && target < map_hi;
}

/* Execute one instruction through the original core and retain everything a
 * replay needs. This is also the correctness oracle for each native case. */
LOCALPROC wr_jit_record_one(struct wr_jit_op *op)
{
	ui5r opcode;
	DecOpR *decoded;
	ui4rr main_class;
	ui3p old_lo = V_regs.pc_pLo;
	ui3p old_hi = V_pc_pHi;

	op->at = V_pc_p;
	opcode = nextiword();
	decoded = &V_regs.disp_table[opcode];
	main_class = decoded->x.MainClas;
	op->handler = OpDispatch[main_class];
	op->cycles = decoded->x.Cycles;
	op->y = decoded->y;
	op->opcode = (ui4b)opcode;
	op->extension = 0;
	op->extension2 = 0;
	op->kind = wr_jit_interpret;
	op->reg = 0;
	op->src_reg = 0;
	op->check_extension = 0;

	V_MaxCyclesToGo -= op->cycles;
	V_regs.CurDecOpY = op->y;
	op->handler();
	op->next = V_pc_p;

	/* Keep direct cases inside one translated host-memory span. Crossing one
	 * requires Recalc_PC_Block and remains with the original interpreter. */
	if (MINIVMAC_JIT_NATIVE_MASK != 0 && old_lo == V_regs.pc_pLo) {
		switch (main_class) {
		case kIKindBraB:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_BRA) != 0
				&& wr_jit_direct_mapping(op->at, op->next,
					old_lo, old_hi))
				op->kind = wr_jit_bra;
			break;
		case kIKindBraW:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_BRA) != 0
				&& wr_jit_direct_mapping(op->at, op->next,
					old_lo, old_hi))
				op->kind = wr_jit_bra;
			break;
		case kIKindBccB:
			op->src_reg = op->y.v[0].ArgDat;
			if (wr_jit_direct_mapping(op->at, op->at + 2,
					old_lo, old_hi)
				&& wr_jit_direct_mapping(op->at,
					op->at + 2
						+ (si5r)(si3b)(ui3b)op->y.v[1].ArgDat,
					old_lo, old_hi)) {
				op->kind = wr_jit_bcc_pending;
			}
			break;
		case kIKindBccW:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			op->src_reg = op->y.v[0].ArgDat;
			if (wr_jit_direct_mapping(op->at, op->at + 4,
					old_lo, old_hi)
				&& wr_jit_direct_mapping(op->at,
					op->at + 2 + (si5r)(si4b)op->extension,
					old_lo, old_hi)) {
				op->kind = wr_jit_bcc_pending;
			}
			break;
		case kIKindDBcc:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			op->src_reg = op->y.v[0].ArgDat;
			op->reg = op->y.v[1].ArgDat;
			if (((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CC) != 0
					|| (op->src_reg & 15) == 6
					|| (op->src_reg & 15) == 7)
				&& wr_jit_direct_mapping(op->at, op->at + 4,
					old_lo, old_hi)
				&& wr_jit_direct_mapping(op->at,
					op->at + 2 + (si5r)(si4b)op->extension,
					old_lo, old_hi))
				op->kind = wr_jit_dbcc;
			break;
		case kIKindDBF:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			op->reg = op->y.v[1].ArgDat;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_DBF) != 0
				&& wr_jit_direct_mapping(op->at, op->at + 4,
					old_lo, old_hi)
				&& wr_jit_direct_mapping(op->at,
					op->at + 2 + (si5r)(si4b)op->extension,
					old_lo, old_hi))
				op->kind = wr_jit_dbf;
			break;
		case kIKindMoveQ:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_MOVEQ) != 0) {
				op->kind = wr_jit_moveq;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindNop:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_NOP) != 0)
				op->kind = wr_jit_nop;
			break;
		case kIKindJsr:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CALL) != 0) {
				ui3r amd = op->y.v[1].AMd;
				ui3r words = 0;
				if (amd == kAMdADispL || amd == kAMdAIndexL
					|| amd == kAMdAbsWL || amd == kAMdPCDispL
					|| amd == kAMdPCIndexL)
					words = 1;
				else if (amd == kAMdAbsLL)
					words = 2;
				else if (amd != kAMdIndirectL)
					break;
				op->reg = op->y.v[1].ArgDat;
				if (words != 0) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
				if (words == 2) {
					op->extension2 = do_get_mem_word(op->at + 4);
					op->check_extension = 2;
				}
				if (wr_jit_direct_mapping(op->at, op->next,
						old_lo, old_hi))
					op->kind = wr_jit_jsr;
			}
			break;
		case kIKindJmp:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CALL) != 0) {
				ui3r amd = op->y.v[1].AMd;
				ui3r words = 0;
				if (amd == kAMdADispL || amd == kAMdAIndexL
					|| amd == kAMdAbsWL || amd == kAMdPCDispL
					|| amd == kAMdPCIndexL)
					words = 1;
				else if (amd == kAMdAbsLL)
					words = 2;
				else if (amd != kAMdIndirectL)
					break;
				op->reg = op->y.v[1].ArgDat;
				if (words != 0) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
				if (words == 2) {
					op->extension2 = do_get_mem_word(op->at + 4);
					op->check_extension = 2;
				}
				if (wr_jit_direct_mapping(op->at, op->next,
						old_lo, old_hi))
					op->kind = wr_jit_jmp;
			}
			break;
		case kIKindRts:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CALL) != 0
				&& wr_jit_direct_mapping(op->at, op->next,
					old_lo, old_hi)) {
				op->kind = wr_jit_rts;
			}
			break;
		case kIKindLinkA6:
		case kIKindLink:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CALL) != 0) {
				op->reg = op->y.v[1].ArgDat;
				op->kind = wr_jit_link;
			}
			break;
		case kIKindUnlkA6:
		case kIKindUnlk:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CALL) != 0) {
				op->reg = op->y.v[1].ArgDat;
				op->kind = wr_jit_unlk;
			}
			break;
		case kIKindMOVEMRmML:
		case kIKindMOVEMApRL:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_MOVEM) != 0) {
				op->kind = main_class == kIKindMOVEMRmML
					? wr_jit_movem_pre_l : wr_jit_movem_post_l;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindMOVEMrmL:
			op->extension = do_get_mem_word(op->at + 2);
			op->check_extension = 1;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_MOVEM) != 0
				&& (op->y.v[1].AMd == kAMdIndirectL
					|| op->y.v[1].AMd == kAMdADispL)) {
				op->kind = wr_jit_movem_store_l;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[1].AMd == kAMdADispL) {
					op->extension2 = do_get_mem_word(op->at + 4);
					op->check_extension = 2;
				}
			}
			break;
		case kIKindMoveB:
		case kIKindMoveW:
		case kIKindMoveL:
			{
				ui3r size = main_class == kIKindMoveB ? 1
					: (main_class == kIKindMoveW ? 2 : 4);
			if ((((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
					&& op->y.v[0].AMd == (size == 1 ? kAMdRegB
						: (size == 2 ? kAMdRegW : kAMdRegL))
					&& op->y.v[1].AMd == (size == 1 ? kAMdRegB
						: (size == 2 ? kAMdRegW : kAMdRegL)))
				|| ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_MOVEL) != 0
					&& (wr_jit_simple_move_mode(op->y.v[0].AMd, size)
						|| (MINIVMAC_JIT_AINDEX != 0
							&& wr_jit_is_aindex(op->y.v[0].AMd)))
					&& (wr_jit_simple_move_mode(op->y.v[1].AMd, size)
						|| (MINIVMAC_JIT_AINDEX != 0
							&& wr_jit_is_aindex(op->y.v[1].AMd)))))) {
				op->kind = size == 1 ? wr_jit_move_rr_b
					: (size == 2 ? wr_jit_move_rr_w : wr_jit_move_rr_l);
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (wr_jit_is_adisp(op->y.v[0].AMd)
					|| wr_jit_is_aindex(op->y.v[0].AMd)) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
				if (wr_jit_is_adisp(op->y.v[1].AMd)
					|| wr_jit_is_aindex(op->y.v[1].AMd)) {
					ui3p ext = op->at + 2
						+ (op->check_extension != 0 ? 2 : 0);
					if (op->check_extension == 0)
						op->extension = do_get_mem_word(ext);
					else
						op->extension2 = do_get_mem_word(ext);
					++op->check_extension;
				}
			}
			}
			break;
		case kIKindMoveAW:
		case kIKindMoveAL:
			{
			ui3r size = main_class == kIKindMoveAW ? 2 : 4;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_MOVEA) != 0
				&& wr_jit_simple_move_mode(op->y.v[0].AMd, size)
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = size == 2 ? wr_jit_movea_w : wr_jit_movea_l;
				op->src_reg = op->y.v[0].ArgDat;
				/* MOVEA's decoder keeps the architectural A-register number
				 * (0..7), unlike ordinary register operands whose ArgDat is
				 * already an index into regs[]. */
				op->reg = op->y.v[1].ArgDat + 8;
				if (wr_jit_is_adisp(op->y.v[0].AMd)) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
			}
			}
			break;
		case kIKindClr:
			{
			ui3r mode = op->y.v[1].AMd;
			ui3r size = mode == kAMdRegB || mode == kAMdIndirectB
				|| mode == kAMdAPosIncB || mode == kAMdAPosInc7B
				|| mode == kAMdAPreDecB || mode == kAMdAPreDec7B
				|| mode == kAMdADispB ? 1
				: (mode == kAMdRegW || mode == kAMdIndirectW
					|| mode == kAMdAPosIncW || mode == kAMdAPreDecW
					|| mode == kAMdADispW ? 2 : 4);
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CLR) != 0
				&& wr_jit_simple_move_mode(mode, size)) {
				op->kind = size == 1 ? wr_jit_clr_b
					: (size == 2 ? wr_jit_clr_w : wr_jit_clr_l);
				op->reg = op->y.v[1].ArgDat;
				if (wr_jit_is_adisp(mode)) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
			}
			}
			break;
		case kIKindBTstB:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_BIT) != 0
				&& (op->y.v[0].AMd == kAMdRegB
					|| op->y.v[0].AMd == kAMdImmedB)
				&& (wr_jit_simple_move_mode(op->y.v[1].AMd, 1)
					|| op->y.v[1].AMd == kAMdADispB)) {
				ui3p ext = op->at + 2;
				op->kind = wr_jit_btst_b;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				op->check_extension = 0;
				if (op->y.v[0].AMd == kAMdImmedB) {
					op->extension = do_get_mem_word(ext);
					op->check_extension = 1;
					ext += 2;
				}
				if (op->y.v[1].AMd == kAMdADispB) {
					if (op->check_extension == 0)
						op->extension = do_get_mem_word(ext);
					else
						op->extension2 = do_get_mem_word(ext);
					++op->check_extension;
				}
			}
			break;
		case kIKindTst:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[1].AMd == kAMdRegB
					|| op->y.v[1].AMd == kAMdRegW
					|| op->y.v[1].AMd == kAMdRegL)
				&& (MINIVMAC_JIT_CMP_TST != 0
					|| op->y.v[1].AMd == kAMdRegL)) {
				op->kind = op->y.v[1].AMd == kAMdRegB ? wr_jit_tst_r_b
					: (op->y.v[1].AMd == kAMdRegW
						? wr_jit_tst_r_w : wr_jit_tst_r_l);
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindCmpB:
		case kIKindCmpW:
		case kIKindCmpL:
			{
			ui3r size = main_class == kIKindCmpB ? 1
				: (main_class == kIKindCmpW ? 2 : 4);
			ui3r reg_mode = size == 1 ? kAMdRegB
				: (size == 2 ? kAMdRegW : kAMdRegL);
			ui3r immed_mode = size == 1 ? kAMdImmedB
				: (size == 2 ? kAMdImmedW : kAMdImmedL);
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == reg_mode
					|| (MINIVMAC_JIT_CMP_TST != 0
						&& op->y.v[0].AMd == immed_mode))
				&& op->y.v[1].AMd == reg_mode
				&& (MINIVMAC_JIT_CMP_TST != 0
					|| (size == 4 && op->y.v[0].AMd == kAMdRegL))) {
				op->kind = size == 1 ? wr_jit_cmp_rr_b
					: (size == 2 ? wr_jit_cmp_rr_w : wr_jit_cmp_rr_l);
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == immed_mode) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
					if (size == 4) {
						op->extension2 = do_get_mem_word(op->at + 4);
						op->check_extension = 2;
					}
				}
			}
			}
			break;
		case kIKindAddL:
		case kIKindSubL:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdRegL
					|| op->y.v[0].AMd == kAMdDat4
					|| (MINIVMAC_JIT_IMMEDIATE_ARITH != 0
						&& op->y.v[0].AMd == kAMdImmedL))
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = main_class == kIKindAddL
					? wr_jit_add_rr_l : wr_jit_sub_rr_l;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == kAMdImmedL) {
					op->extension = do_get_mem_word(op->at + 2);
					op->extension2 = do_get_mem_word(op->at + 4);
					op->check_extension = 2;
				}
			}
			break;
		case kIKindAddQA:
		case kIKindSubQA:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& op->y.v[0].AMd == kAMdDat4
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = main_class == kIKindAddQA
					? wr_jit_addq_a : wr_jit_subq_a;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindAddA:
		case kIKindSubA:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdRegW
					|| op->y.v[0].AMd == kAMdRegL
					|| op->y.v[0].AMd == kAMdImmedW
					|| op->y.v[0].AMd == kAMdImmedL)
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = main_class == kIKindAddA
					? wr_jit_adda : wr_jit_suba;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == kAMdImmedW) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				} else if (op->y.v[0].AMd == kAMdImmedL) {
					op->extension = do_get_mem_word(op->at + 2);
					op->extension2 = do_get_mem_word(op->at + 4);
					op->check_extension = 2;
				}
			}
			break;
		case kIKindAddW:
		case kIKindSubW:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdDat4
					|| op->y.v[0].AMd == kAMdRegW
					|| (MINIVMAC_JIT_IMMEDIATE_ARITH != 0
						&& op->y.v[0].AMd == kAMdImmedW))
				&& op->y.v[1].AMd == kAMdRegW) {
				if (op->y.v[0].AMd == kAMdDat4
					|| op->y.v[0].AMd == kAMdImmedW)
					op->kind = main_class == kIKindAddW
						? wr_jit_addq_w : wr_jit_subq_w;
				else
					op->kind = main_class == kIKindAddW
						? wr_jit_add_rr_w : wr_jit_sub_rr_w;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == kAMdImmedW) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
			}
			break;
		case kIKindAddB:
		case kIKindSubB:
			if (MINIVMAC_JIT_BYTE_ARITH != 0
				&& (MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdDat4
					|| op->y.v[0].AMd == kAMdRegB
					|| (MINIVMAC_JIT_IMMEDIATE_ARITH != 0
						&& op->y.v[0].AMd == kAMdImmedB))
				&& op->y.v[1].AMd == kAMdRegB) {
				if (op->y.v[0].AMd == kAMdDat4
					|| op->y.v[0].AMd == kAMdImmedB)
					op->kind = main_class == kIKindAddB
						? wr_jit_addq_b : wr_jit_subq_b;
				else
					op->kind = main_class == kIKindAddB
						? wr_jit_add_rr_b : wr_jit_sub_rr_b;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == kAMdImmedB) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
			}
			break;
		case kIKindAndI:
		case kIKindAndEaD:
		case kIKindAndDEa:
		case kIKindOrI:
		case kIKindOrDEa:
		case kIKindOrEaD:
		case kIKindEor:
		case kIKindEorI:
			{
			ui3r size = op->y.v[1].AMd == kAMdRegB ? 1
				: (op->y.v[1].AMd == kAMdRegW ? 2 : 4);
			ui3r reg_mode = size == 1 ? kAMdRegB
				: (size == 2 ? kAMdRegW : kAMdRegL);
			ui3r immed_mode = size == 1 ? kAMdImmedB
				: (size == 2 ? kAMdImmedW : kAMdImmedL);
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_LOGIC) != 0
				&& op->y.v[1].AMd == reg_mode
				&& (op->y.v[0].AMd == reg_mode
					|| op->y.v[0].AMd == immed_mode)) {
				op->kind = wr_jit_logic_rr;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (op->y.v[0].AMd == immed_mode) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
					if (size == 4) {
						op->extension2 = do_get_mem_word(op->at + 4);
						op->check_extension = 2;
					}
				} else {
					op->extension = main_class == kIKindAndEaD
						|| main_class == kIKindAndDEa ? 0
						: (main_class == kIKindOrDEa
							|| main_class == kIKindOrEaD ? 1 : 2);
				}
			}
			}
			break;
		case kIKindNot:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_LOGIC) != 0
				&& (op->y.v[1].AMd == kAMdRegB
					|| op->y.v[1].AMd == kAMdRegW
					|| op->y.v[1].AMd == kAMdRegL)) {
				op->kind = wr_jit_not_r;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindAsrW:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdDat4
					|| op->y.v[0].AMd == kAMdRegL)
				&& op->y.v[1].AMd == kAMdRegW) {
				op->kind = wr_jit_asr_w;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		default:
			break;
		}
	}
}

LOCALFUNC blnr wr_jit_valid(const struct wr_jit_block *block)
{
	ui3r i;
	/* Direct-RAM translations bake in the Plus memory map.  Overlay changes
	 * are rare (boot and reset), so validate once at dispatch rather than on
	 * every guest load or store. */
	if (block->memory_overlay != MemOverlay) {
		WR_JIT_PROFILE_INC(WR_JIT_PROFILE_DIRECT_MAP_INVALID);
		return falseblnr;
	}
	if (block->direct_guest_reg != 0xff) {
		ui5r addr = block->direct_guest_addr;
		if (V_regs.regs[block->direct_guest_reg]
				!= block->direct_reg_value) {
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_DIRECT_REG_INVALID);
			return falseblnr;
		}
		if ((addr & V_regs.MATCrdW.cmpmask)
				!= V_regs.MATCrdW.cmpvalu
			|| (addr & V_regs.MATCrdW.usemask)
					+ V_regs.MATCrdW.usebase
				!= block->direct_host_addr) {
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_DIRECT_MAP_INVALID);
			return falseblnr;
		}
	}
	if (!block->validate)
		return trueblnr;
	for (i = 0; i < block->count; ++i) {
		const struct wr_jit_op *op = &block->op[i];
		if (do_get_mem_word(op->at) != op->opcode) {
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_CODE_INVALID);
			return falseblnr;
		}
		if (op->check_extension
			&& do_get_mem_word(op->at + 2) != op->extension) {
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_CODE_INVALID);
			return falseblnr;
		}
		if (op->check_extension > 1
			&& do_get_mem_word(op->at + 4) != op->extension2) {
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_CODE_INVALID);
			return falseblnr;
		}
	}
	return trueblnr;
}

/* Resolve a 68000 condition without the core's condition/action double
 * dispatch. Evaluating lazy flags early is architecturally invisible. */
LOCALFUNC ui5r wr_jit_cc_true(ui5r condition)
{
	ui5r c;
	ui5r n;
	ui5r z;
	ui5r v;
	ui5r src;
	ui5r dst;
	ui5r result;

	switch (V_regs.LazyFlagKind) {
	case kLazyFlagsTstL:
		result = V_regs.LazyFlagArgDst;
		c = v = 0;
		z = result == 0;
		n = ui5r_MSBisSet(result);
		break;
	case kLazyFlagsCmpB:
	case kLazyFlagsSubB:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		result = ui5r_FromSByte(dst - src);
		z = result == 0;
		n = ui5r_MSBisSet(result);
		v = ((((dst - src) >> 1) ^ (dst - src)) >> 7) & 1;
		c = ((ui5r_FromUByte(dst) - ui5r_FromUByte(src)) >> 8) & 1;
		break;
	case kLazyFlagsCmpW:
	case kLazyFlagsSubW:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		result = ui5r_FromSWord(dst - src);
		z = result == 0;
		n = ui5r_MSBisSet(result);
		v = ((((dst - src) >> 1) ^ (dst - src)) >> 15) & 1;
		c = ((ui5r_FromUWord(dst) - ui5r_FromUWord(src)) >> 16) & 1;
		break;
	case kLazyFlagsCmpL:
	case kLazyFlagsSubL:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		result = dst - src;
		z = result == 0;
		n = ui5r_MSBisSet(result);
		{
			ui5r srcn = ui5r_MSBisSet(src);
			ui5r dst_not_n = ui5r_MSBisSet(dst) ^ 1;
			ui5r both = srcn & dst_not_n;
			ui5r either = srcn | dst_not_n;
			v = ((n | either) ^ 1) | (n & both);
			c = both | (n & either);
		}
		break;
	default:
		NeedDefaultLazyAllFlags();
		c = CFLG;
		n = NFLG;
		z = ZFLG;
		v = VFLG;
		break;
	}
	switch (condition & 15) {
	case 0: return 1;
	case 1: return 0;
	case 2: return (c | z) == 0;
	case 3: return (c | z) != 0;
	case 4: return c == 0;
	case 5: return c != 0;
	case 6: return z == 0;
	case 7: return z != 0;
	case 8: return v == 0;
	case 9: return v != 0;
	case 10: return n == 0;
	case 11: return n != 0;
	case 12: return n == v;
	case 13: return n != v;
	case 14: return z == 0 && n == v;
	default: return z != 0 || n != v;
	}
}

/* NE/EQ dominate the boot workload.  Resolve their zero flag directly from
 * the common lazy forms instead of entering the core's condition/action
 * double dispatch.  Rare flag forms retain the exact materialization path. */
LOCALFUNC MINIVMAC_FAST_M68K ui5r my_reg_call wr_jit_eq_true(
	ui5r condition)
{
	ui5r src;
	ui5r dst;
	ui5r z;

	switch (V_regs.LazyFlagKind) {
	case kLazyFlagsDefault:
#if UseLazyZ
	case kLazyFlagsZSet:
#endif
		z = ZFLG;
		break;
	case kLazyFlagsTstB:
		z = (ui3b)V_regs.LazyFlagArgDst == 0;
		break;
	case kLazyFlagsTstW:
		z = (ui4b)V_regs.LazyFlagArgDst == 0;
		break;
	case kLazyFlagsTstL:
		z = V_regs.LazyFlagArgDst == 0;
		break;
	case kLazyFlagsCmpB:
	case kLazyFlagsSubB:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = (ui3b)dst == (ui3b)src;
		break;
	case kLazyFlagsCmpW:
	case kLazyFlagsSubW:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = (ui4b)dst == (ui4b)src;
		break;
	case kLazyFlagsCmpL:
	case kLazyFlagsSubL:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = dst == src;
		break;
	case kLazyFlagsAddB:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = (ui3b)(dst + src) == 0;
		break;
	case kLazyFlagsAddW:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = (ui4b)(dst + src) == 0;
		break;
	case kLazyFlagsAddL:
		src = V_regs.LazyFlagArgSrc;
		dst = V_regs.LazyFlagArgDst;
		z = dst + src == 0;
		break;
	case kLazyFlagsNegB:
		z = (ui3b)V_regs.LazyFlagArgDst == 0;
		break;
	case kLazyFlagsNegW:
		z = (ui4b)V_regs.LazyFlagArgDst == 0;
		break;
	case kLazyFlagsNegL:
		z = V_regs.LazyFlagArgDst == 0;
		break;
	default:
		NeedDefaultLazyAllFlags();
		z = ZFLG;
		break;
	}
	return (condition & 15) == 6 ? z == 0 : z != 0;
}

LOCALPROC MINIVMAC_FAST_M68K my_reg_call wr_jit_set_z_from_bit(
	ui5r bit, ui5r value)
{
	WillSetZFLG();
	ZFLG = ((value >> (bit & 7)) ^ 1) & 1;
}

LOCALPROC MINIVMAC_FAST_M68K my_reg_call wr_jit_asr_word(
	ui5r count, ui5r reg)
{
	ui5r dst = ui5r_FromSWord(V_regs.regs[reg]);
	ui5r result;
	count &= 63;
	if (count == 0) {
		V_regs.LazyFlagKind = kLazyFlagsTstL;
		V_regs.LazyFlagArgDst = dst;
		result = dst;
	} else if (count >= 16) {
		if (ui5r_MSBisSet(dst)) {
			result = (ui5r)-1;
			XFLG = CFLG = NFLG = 1;
			ZFLG = VFLG = 0;
		} else {
			result = 0;
			XFLG = CFLG = NFLG = VFLG = 0;
			ZFLG = 1;
		}
		V_regs.LazyXFlagKind = kLazyFlagsDefault;
		V_regs.LazyFlagKind = kLazyFlagsDefault;
	} else {
		result = Ui5rASR(dst, count);
		V_regs.LazyFlagKind = kLazyFlagsAsrW;
		V_regs.LazyFlagArgSrc = count;
		V_regs.LazyFlagArgDst = dst;
		V_regs.LazyXFlagKind = kLazyFlagsAsrW;
		V_regs.LazyXFlagArgSrc = count;
		V_regs.LazyXFlagArgDst = dst;
	}
	V_regs.regs[reg] = (V_regs.regs[reg] & 0xffff0000)
		| (result & 0xffff);
}

/* ---- C33 emitter ------------------------------------------------------ */

enum {
	WR_R0, WR_R1, WR_R2, WR_R3, WR_R4, WR_R5, WR_R6, WR_R7,
	WR_R8, WR_R9, WR_R10, WR_R11, WR_R12
};
enum {
	WR_O_LDB = 0x20, WR_O_LDUB = 0x24,
	WR_O_LDH = 0x28, WR_O_LDUH = 0x2c,
	WR_O_LDW = 0x30, WR_O_STB = 0x34,
	WR_O_STH = 0x38, WR_O_STW = 0x3c, WR_O_ADD = 0x22,
	WR_O_SUB = 0x26, WR_O_CMP = 0x2a, WR_O_MOV = 0x2e,
	WR_O_AND = 0x32, WR_O_OR = 0x36, WR_O_XOR = 0x3a,
	WR_O_SWAPH = 0x9a, WR_O_SEXTH = 0xa9,
};
enum {
	WR_I_ADD = 0x18, WR_I_SUB = 0x19, WR_I_CMP = 0x1a,
	WR_I_MOV = 0x1b,
};
enum { WR_S_SRL = 0x2200, WR_S_SLL = 0x2600 };
enum {
	WR_B_GT = 0x08, WR_B_GE = 0x0a, WR_B_LT = 0x0c,
	WR_B_LE = 0x0e, WR_B_EQ = 0x18, WR_B_NE = 0x1a,
	WR_B_ULT = 0x14, WR_B_CALL = 0x1c, WR_B_JP = 0x1e,
};

struct wr_emit {
	ui3p base;
	ui5r off;
	ui5r exits[WR_JIT_OPS * 3];
	ui3r nexits;
	ui3b cached_guest[2];
	blnr cache_rdw;
	blnr direct_ram;
	blnr write;
	const struct wr_jit_block *block;
};

LOCALINLINEPROC wr_w(struct wr_emit *e, ui4r word)
{
	if (e->write)
		*(ui4b *)(e->base + e->off) = (ui4b)word;
	e->off += 2;
}

LOCALINLINEPROC wr_ext(struct wr_emit *e, ui5r value)
{
	wr_w(e, 0xc000u | (value & 0x1fffu));
}

LOCALINLINEPROC wr_rr(struct wr_emit *e, ui4r op, ui4r a, ui4r b)
{
	wr_w(e, (op << 8) | (b << 4) | a);
}

LOCALINLINEPROC wr_ri(struct wr_emit *e, ui4r op, ui4r reg, ui5r value)
{
	si5r signed_value = (si5r)value;
	if (signed_value < -32 || signed_value > 31) {
		if (signed_value < -(1 << 18) || signed_value > (1 << 18) - 1)
			wr_ext(e, value >> 19);
		wr_ext(e, value >> 6);
	}
	wr_w(e, (op << 10) | ((value & 0x3f) << 4) | reg);
}

LOCALINLINEPROC wr_addi(struct wr_emit *e, ui4r reg, ui5r value, blnr sub)
{
	if ((si5r)value < 0) {
		sub = !sub;
		value = 0 - value;
	}
	if (value > 63) {
		if (value > 0x7ffff)
			wr_ext(e, value >> 19);
		wr_ext(e, value >> 6);
	}
	wr_w(e, ((sub ? WR_I_SUB : WR_I_ADD) << 10)
		| ((value & 0x3f) << 4) | reg);
}

LOCALINLINEPROC wr_shifti(struct wr_emit *e, ui4r op, ui4r reg, ui4r count)
{
	wr_w(e, op | ((count & 31) << 4) | reg);
}

LOCALINLINEPROC wr_flush_guest_cache(struct wr_emit *e);
LOCALINLINEPROC wr_reload_guest_cache(struct wr_emit *e);

LOCALINLINEPROC wr_xcall(struct wr_emit *e, ui5r target)
{
	wr_flush_guest_cache(e);
	ui5r self = (ui5r)e->base + e->off + 4;
	ui5r distance = (ui5r)((si5r)(target - self) / 2);
	wr_ext(e, ((distance >> 21) & 0x3ff) << 3);
	wr_ext(e, distance >> 8);
	wr_w(e, (WR_B_CALL << 8) | (distance & 0xff));
	/* C33's C ABI permits callees to use the high registers.  A direct-RAM
	 * region keeps its mapping constants there, so restore them after every
	 * helper call before another guest access can consume stale values. */
	if (e->direct_ram) {
		wr_ri(e, WR_I_MOV, WR_R10, 0x00c00000);
		wr_ri(e, WR_I_MOV, WR_R11, 0x003fffff);
		wr_ri(e, WR_I_MOV, WR_R12, (ui5r)RAM);
	}
	wr_reload_guest_cache(e);
}

LOCALINLINEPROC wr_xjump(struct wr_emit *e, ui5r target)
{
	ui5r self = (ui5r)e->base + e->off + 4;
	ui5r distance = (ui5r)((si5r)(target - self) / 2);
	wr_ext(e, ((distance >> 21) & 0x3ff) << 3);
	wr_ext(e, distance >> 8);
	wr_w(e, (WR_B_JP << 8) | (distance & 0xff));
}

LOCALINLINEPROC wr_jump(struct wr_emit *e, ui5r target)
{
	si5r distance = (si5r)(target - ((ui5r)e->base + e->off)) / 2;
	if (distance >= -128 && distance <= 127)
		wr_w(e, (WR_B_JP << 8) | ((ui5r)distance & 0xff));
	else
		wr_xjump(e, target);
}

LOCALINLINEPROC wr_jp_reg(struct wr_emit *e, ui4r reg)
{
	wr_w(e, 0x0680 | reg);
}

LOCALINLINEFUNC ui5r wr_forward(struct wr_emit *e, ui4r condition)
{
	ui5r at = e->off;
	wr_w(e, condition << 8);
	return at;
}

LOCALINLINEPROC wr_land(struct wr_emit *e, ui5r at)
{
	if (e->write)
		e->base[at] = (ui3b)((e->off - at) / 2);
}

LOCALINLINEFUNC ui5r wr_exit_if(struct wr_emit *e, ui4r condition)
{
	ui5r at = e->off;
	wr_ext(e, 0);
	wr_w(e, condition << 8);
	return at;
}

LOCALINLINEPROC wr_land_far(struct wr_emit *e, ui5r at, ui5r target)
{
	ui5r distance = (ui5r)((si5r)(target - (at + 2)) / 2);
	if (e->write) {
		e->base[at] = (ui3b)(distance >> 8);
		e->base[at + 1] = (ui3b)(0xc0 | ((distance >> 16) & 0x1f));
		e->base[at + 2] = (ui3b)distance;
	}
}

LOCALINLINEPROC wr_mem(struct wr_emit *e, ui4r op, ui4r value,
	ui4r base, ui5r offset)
{
	if (offset != 0)
		wr_ext(e, offset);
	wr_rr(e, op, value, base);
}

LOCALINLINEPROC wr_ind(struct wr_emit *e, ui4r op, ui4r value,
	ui4r base, ui5r offset)
{
	if (offset != 0)
		wr_ext(e, offset);
	wr_rr(e, op, value, base);
}

LOCALINLINEFUNC ui3r wr_cached_native_reg(const struct wr_emit *e,
	ui3r guest)
{
	if (e->cached_guest[0] == guest)
		return WR_R8;
	if (e->cached_guest[1] == guest)
		return WR_R9;
	return 0xff;
}

LOCALINLINEPROC wr_load_guest_l(struct wr_emit *e, ui3r native,
	ui3r guest)
{
	ui3r cached = wr_cached_native_reg(e, guest);
	if (cached != 0xff) {
		if (native != cached)
			wr_rr(e, WR_O_MOV, native, cached);
	} else {
		wr_mem(e, WR_O_LDW, native, WR_R0,
			(ui5r)((ui3p)&V_regs.regs[guest] - (ui3p)&V_regs));
	}
}

LOCALINLINEPROC wr_store_guest_l(struct wr_emit *e, ui3r native,
	ui3r guest)
{
	ui3r cached = wr_cached_native_reg(e, guest);
	if (cached != 0xff) {
		if (native != cached)
			wr_rr(e, WR_O_MOV, cached, native);
	} else {
		wr_mem(e, WR_O_STW, native, WR_R0,
			(ui5r)((ui3p)&V_regs.regs[guest] - (ui3p)&V_regs));
	}
}

LOCALINLINEPROC wr_flush_guest_cache(struct wr_emit *e)
{
	ui3r i;
	for (i = 0; i < 2; ++i)
		if (e->cached_guest[i] != 0xff)
			wr_mem(e, WR_O_STW, WR_R8 + i, WR_R0,
				(ui5r)((ui3p)&V_regs.regs[e->cached_guest[i]]
					- (ui3p)&V_regs));
}

LOCALINLINEPROC wr_reload_guest_cache(struct wr_emit *e)
{
	ui3r i;
	for (i = 0; i < 2; ++i)
		if (e->cached_guest[i] != 0xff)
			wr_mem(e, WR_O_LDW, WR_R8 + i, WR_R0,
				(ui5r)((ui3p)&V_regs.regs[e->cached_guest[i]]
					- (ui3p)&V_regs));
}

LOCALINLINEPROC wr_note_exit(struct wr_emit *e, ui4r condition)
{
	e->exits[e->nexits++] = wr_exit_if(e, condition);
}

LOCALINLINEPROC wr_store_machine(struct wr_emit *e)
{
	wr_mem(e, WR_O_STW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_STW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
}

/* A region commonly reads several words through the same one-entry MATC.
 * Keep that translation in callee-saved native registers: promoted loops can
 * then reuse it across every backedge instead of fetching four cache words
 * from SDRAM for every guest access. */
LOCALINLINEPROC wr_load_cached_rdw(struct wr_emit *e)
{
	wr_mem(e, WR_O_LDW, WR_R8, WR_R0,
		(ui5r)((ui3p)&V_regs.MATCrdW.cmpmask - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R9, WR_R0,
		(ui5r)((ui3p)&V_regs.MATCrdW.cmpvalu - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R10, WR_R0,
		(ui5r)((ui3p)&V_regs.MATCrdW.usemask - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R11, WR_R0,
		(ui5r)((ui3p)&V_regs.MATCrdW.usebase - (ui3p)&V_regs));
}

LOCALINLINEPROC wr_guard_next(struct wr_emit *e, ui3p next)
{
	wr_ri(e, WR_I_MOV, WR_R4, (ui5r)next);
	wr_rr(e, WR_O_CMP, WR_R1, WR_R4);
	wr_note_exit(e, WR_B_NE);
}

/* The Macintosh Plus exposes 4 MiB of RAM whenever address bits 23:22 are
 * zero; the 68000's high byte is ignored.  R10/R11/R12 retain the compare
 * mask, offset mask and host RAM base for an entire translated region.  On
 * success R5 is the host pointer.  Other regions fall through to Mini
 * vMac's exact MATC path, preserving ROM, MMIO and boundary behaviour. */
LOCALFUNC ui5r wr_emit_direct_ram_address(struct wr_emit *e, ui3r size,
	ui5r *not_ram_end)
{
	ui5r not_ram;
	*not_ram_end = 0;
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R10);
	wr_ri(e, WR_I_CMP, WR_R5, 0);
	not_ram = wr_forward(e, WR_B_NE);
	/* A 68000 longword may be only word-aligned.  Keep the access on the
	 * direct path only when its second halfword is in RAM too. */
	if (size == 4) {
		wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
		wr_addi(e, WR_R5, 2, falseblnr);
		wr_rr(e, WR_O_AND, WR_R5, WR_R10);
		wr_ri(e, WR_I_CMP, WR_R5, 0);
		*not_ram_end = wr_forward(e, WR_B_NE);
	}
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R11);
	wr_rr(e, WR_O_ADD, WR_R5, WR_R12);
	return not_ram;
}

/* Compact big-endian longword access once wr_emit_direct_ram_address has
 * placed the translated host address in R5. */
LOCALINLINEPROC wr_emit_host_long_read(struct wr_emit *e)
{
	wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
	wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
	wr_shifti(e, WR_S_SLL, WR_R3, 16);
	wr_ind(e, WR_O_LDUH, WR_R4, WR_R5, 2);
	wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
	wr_rr(e, WR_O_OR, WR_R3, WR_R4);
}

LOCALINLINEPROC wr_emit_host_long_write(struct wr_emit *e)
{
	wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
	wr_shifti(e, WR_S_SRL, WR_R4, 16);
	wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
	wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
	wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
	wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
	wr_ind(e, WR_O_STH, WR_R4, WR_R5, 2);
}

/* Inline the same one-entry address-translation cache used by get/put_*.
 * A miss takes the exact core helper, which also refreshes the cache; normal
 * Macintosh RAM traffic then stays entirely inside the translated block. */
LOCALPROC wr_emit_guest_read(struct wr_emit *e, ui3r size, ui3p next)
{
	MATCp matc = size == 1 ? &V_regs.MATCrdB : &V_regs.MATCrdW;
	ui5r direct_done = 0;
	ui5r miss1;
	ui5r miss2 = 0;
	ui5r done;

	if (e->direct_ram) {
		ui5r not_ram_end;
		ui5r not_ram = wr_emit_direct_ram_address(e, size,
			&not_ram_end);
		if (size == 1) {
			wr_ind(e, WR_O_LDB, WR_R3, WR_R5, 0);
		} else if (size == 2) {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_rr(e, WR_O_SEXTH, WR_R3, WR_R3);
		} else {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_shifti(e, WR_S_SLL, WR_R3, 16);
			wr_ind(e, WR_O_LDUH, WR_R4, WR_R5, 2);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_rr(e, WR_O_OR, WR_R3, WR_R4);
		}
		wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
		direct_done = wr_exit_if(e, WR_B_EQ);
		wr_land(e, not_ram);
		if (not_ram_end != 0)
			wr_land(e, not_ram_end);
	}

	if (size == 1 || !e->cache_rdw)
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
			(ui5r)((ui3p)&matc->cmpmask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5,
		size != 1 && e->cache_rdw ? WR_R8 : WR_R4);
	if (size == 1 || !e->cache_rdw)
		wr_mem(e, WR_O_LDW, WR_R7, WR_R0,
			(ui5r)((ui3p)&matc->cmpvalu - (ui3p)&V_regs));
	wr_rr(e, WR_O_CMP, WR_R5,
		size != 1 && e->cache_rdw ? WR_R9 : WR_R7);
	miss1 = wr_forward(e, WR_B_NE);
	if (size == 4) {
		wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
		wr_addi(e, WR_R5, 2, falseblnr);
		wr_rr(e, WR_O_AND, WR_R5,
			e->cache_rdw ? WR_R8 : WR_R4);
		wr_rr(e, WR_O_CMP, WR_R5,
			e->cache_rdw ? WR_R9 : WR_R7);
		miss2 = wr_forward(e, WR_B_NE);
	}
	if (size == 1 || !e->cache_rdw)
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
			(ui5r)((ui3p)&matc->usemask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5,
		size != 1 && e->cache_rdw ? WR_R10 : WR_R4);
	if (size == 1 || !e->cache_rdw)
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
			(ui5r)((ui3p)&matc->usebase - (ui3p)&V_regs));
	wr_rr(e, WR_O_ADD, WR_R5,
		size != 1 && e->cache_rdw ? WR_R11 : WR_R4);
	if (size == 1) {
		wr_ind(e, WR_O_LDB, WR_R3, WR_R5, 0);
	} else if (size == 2) {
		/* Word-cache hits are necessarily even-addressed.  Load the
		 * little-endian host halfword, swap its two bytes into 68000
		 * order, then reproduce get_word's signed result. */
		wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
		wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
		wr_rr(e, WR_O_SEXTH, WR_R3, WR_R3);
	} else {
		wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
		wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
		wr_shifti(e, WR_S_SLL, WR_R3, 16);
		wr_ind(e, WR_O_LDUH, WR_R4, WR_R5, 2);
		wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
		wr_rr(e, WR_O_OR, WR_R3, WR_R4);
	}
	wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
	done = wr_forward(e, WR_B_EQ);
	wr_land(e, miss1);
	if (size == 4)
		wr_land(e, miss2);
	wr_store_machine(e);
	wr_xcall(e, (ui5r)(size == 1 ? get_byte
		: (size == 2 ? get_word : get_long)));
	wr_rr(e, WR_O_MOV, WR_R3, WR_R4);
	wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
	/* A miss may replace MATCrdW. Refresh the region-live copy before
	 * continuing at the next guest instruction. */
	if (size != 1 && e->cache_rdw)
		wr_load_cached_rdw(e);
	wr_guard_next(e, next);
	wr_land(e, done);
	if (direct_done != 0)
		wr_land_far(e, direct_done, e->off);
}

LOCALPROC wr_emit_guest_write(struct wr_emit *e, ui3r size, ui3p next)
{
	MATCp matc = size == 1 ? &V_regs.MATCwrB : &V_regs.MATCwrW;
	ui5r direct_done = 0;
	ui5r miss1;
	ui5r miss2 = 0;
	ui5r done;

	if (e->direct_ram) {
		ui5r not_ram_end;
		ui5r not_ram = wr_emit_direct_ram_address(e, size,
			&not_ram_end);
		if (size == 1) {
			wr_ind(e, WR_O_STB, WR_R3, WR_R5, 0);
		} else if (size == 2) {
			wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
		} else {
			wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
			wr_shifti(e, WR_S_SRL, WR_R4, 16);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
			wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_ind(e, WR_O_STH, WR_R4, WR_R5, 2);
		}
		wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
		direct_done = wr_exit_if(e, WR_B_EQ);
		wr_land(e, not_ram);
		if (not_ram_end != 0)
			wr_land(e, not_ram_end);
	}

	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->cmpmask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R4);
	wr_mem(e, WR_O_LDW, WR_R7, WR_R0,
		(ui5r)((ui3p)&matc->cmpvalu - (ui3p)&V_regs));
	wr_rr(e, WR_O_CMP, WR_R5, WR_R7);
	miss1 = wr_forward(e, WR_B_NE);
	if (size == 4) {
		wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
		wr_addi(e, WR_R5, 2, falseblnr);
		wr_rr(e, WR_O_AND, WR_R5, WR_R4);
		wr_rr(e, WR_O_CMP, WR_R5, WR_R7);
		miss2 = wr_forward(e, WR_B_NE);
	}
	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->usemask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R4);
	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->usebase - (ui3p)&V_regs));
	wr_rr(e, WR_O_ADD, WR_R5, WR_R4);
	if (size == 1) {
		wr_ind(e, WR_O_STB, WR_R3, WR_R5, 0);
	} else if (size == 2) {
		wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
		wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
		wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
	} else {
		wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
		wr_shifti(e, WR_S_SRL, WR_R4, 16);
		wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
		wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
		wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
		wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
		wr_ind(e, WR_O_STH, WR_R4, WR_R5, 2);
	}
	wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
	done = wr_forward(e, WR_B_EQ);
	wr_land(e, miss1);
	if (size == 4)
		wr_land(e, miss2);
	wr_rr(e, WR_O_MOV, WR_R7, WR_R3);
	wr_mem(e, WR_O_STW, WR_R3, WR_R0,
		(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
	wr_store_machine(e);
	wr_xcall(e, (ui5r)(size == 1 ? put_byte
		: (size == 2 ? put_word : put_long)));
	/* put_* receives the guest value in R7, but a C helper may clobber the
	 * caller-saved R3.  MOVE still has to set N/Z from that original value
	 * after this slow-path write returns. */
	wr_mem(e, WR_O_LDW, WR_R3, WR_R0,
		(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
	wr_guard_next(e, next);
	wr_land(e, done);
	if (direct_done != 0)
		wr_land_far(e, direct_done, e->off);
}

/* MOVEM's register mask describes one contiguous memory span.  Check that
 * span against the word translation cache once, then transfer every selected
 * register without returning to C or repeating the mapping lookup. */
LOCALPROC wr_emit_movem_l(struct wr_emit *e, const struct wr_jit_op *op)
{
	blnr load = op->kind == wr_jit_movem_post_l;
	blnr predec = op->kind == wr_jit_movem_pre_l;
	MATCp matc = load ? &V_regs.MATCrdW : &V_regs.MATCwrW;
	ui5r mask = op->extension;
	ui5r count = 0;
	ui5r bytes;
	ui5r miss1;
	ui5r miss2;
	ui5r done;
	ui5r fallback;
	int z;

	for (z = 0; z < 16; ++z)
		if ((mask & ((ui5r)1 << z)) != 0)
			++count;
	bytes = count * 4;
	wr_load_guest_l(e, WR_R6, op->reg);
	if (predec && bytes != 0)
		wr_addi(e, WR_R6, bytes, trueblnr);
	else if (op->kind == wr_jit_movem_store_l
		&& op->y.v[1].AMd == kAMdADispL)
		wr_addi(e, WR_R6,
			(ui5r)(si5r)(si4b)op->extension2, falseblnr);
	if (bytes == 0) {
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		return;
	}

	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->cmpmask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R4);
	wr_mem(e, WR_O_LDW, WR_R7, WR_R0,
		(ui5r)((ui3p)&matc->cmpvalu - (ui3p)&V_regs));
	wr_rr(e, WR_O_CMP, WR_R5, WR_R7);
	miss1 = wr_exit_if(e, WR_B_NE);
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_addi(e, WR_R5, bytes - 2, falseblnr);
	wr_rr(e, WR_O_AND, WR_R5, WR_R4);
	wr_rr(e, WR_O_CMP, WR_R5, WR_R7);
	miss2 = wr_exit_if(e, WR_B_NE);

	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->usemask - (ui3p)&V_regs));
	wr_rr(e, WR_O_MOV, WR_R5, WR_R6);
	wr_rr(e, WR_O_AND, WR_R5, WR_R4);
	wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
		(ui5r)((ui3p)&matc->usebase - (ui3p)&V_regs));
	wr_rr(e, WR_O_ADD, WR_R5, WR_R4);

	for (z = 0; z < 16; ++z) {
		ui5r bit = predec ? ((ui5r)1 << (15 - z)) : ((ui5r)1 << z);
		if ((mask & bit) == 0)
			continue;
		if (load) {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_shifti(e, WR_S_SLL, WR_R3, 16);
			wr_ind(e, WR_O_LDUH, WR_R4, WR_R5, 2);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_rr(e, WR_O_OR, WR_R3, WR_R4);
			wr_store_guest_l(e, WR_R3, z);
		} else {
			wr_load_guest_l(e, WR_R3, z);
			wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
			wr_shifti(e, WR_S_SRL, WR_R4, 16);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_ind(e, WR_O_STH, WR_R4, WR_R5, 0);
			wr_rr(e, WR_O_MOV, WR_R4, WR_R3);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_ind(e, WR_O_STH, WR_R4, WR_R5, 2);
		}
		wr_addi(e, WR_R5, 4, falseblnr);
	}
	if (load)
		wr_addi(e, WR_R6, bytes, falseblnr);
	if (load || predec)
		wr_store_guest_l(e, WR_R6, op->reg);
	wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
	wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
	done = wr_exit_if(e, WR_B_EQ);

	fallback = e->off;
	wr_land_far(e, miss1, fallback);
	wr_land_far(e, miss2, fallback);
	wr_addi(e, WR_R1, 2, falseblnr);
	wr_store_machine(e);
	{
		ui5r y;
		MyMoveBytes((anyp)&op->y, (anyp)&y, sizeof y);
		wr_ri(e, WR_I_MOV, WR_R4, y);
	}
	wr_mem(e, WR_O_STW, WR_R4, WR_R0,
		(ui5r)((ui3p)&V_regs.CurDecOpY - (ui3p)&V_regs));
	wr_xcall(e, (ui5r)op->handler);
	wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
	wr_guard_next(e, op->next);
	wr_land_far(e, done, e->off);
}

/* Native register operations cannot ask the outer scheduler to stop.  Keep
 * the expensive cycle-state exit only around fallback handlers and guest
 * memory operations, where MMIO can call NeedToGetOut().  The dispatcher
 * admits a trace only when its complete fixed cycle cost fits in the current
 * slice, so the other per-instruction exits are redundant. */
LOCALINLINEFUNC blnr wr_jit_op_may_exit(const struct wr_jit_op *op)
{
	switch (op->kind) {
	case wr_jit_move_rr_b:
		return op->y.v[0].AMd != kAMdRegB
			|| op->y.v[1].AMd != kAMdRegB;
	case wr_jit_move_rr_w:
		return op->y.v[0].AMd != kAMdRegW
			|| op->y.v[1].AMd != kAMdRegW;
	case wr_jit_move_rr_l:
		return op->y.v[0].AMd != kAMdRegL
			|| op->y.v[1].AMd != kAMdRegL;
	case wr_jit_movea_w:
		return op->y.v[0].AMd != kAMdRegW;
	case wr_jit_movea_l:
		return op->y.v[0].AMd != kAMdRegL;
	case wr_jit_clr_b:
		return op->y.v[1].AMd != kAMdRegB;
	case wr_jit_clr_w:
		return op->y.v[1].AMd != kAMdRegW;
	case wr_jit_clr_l:
		return op->y.v[1].AMd != kAMdRegL;
	case wr_jit_btst_b:
		return op->y.v[1].AMd != kAMdRegB;
	case wr_jit_movem_pre_l:
	case wr_jit_movem_post_l:
	case wr_jit_movem_store_l:
	case wr_jit_jsr:
	case wr_jit_jmp:
	case wr_jit_rts:
	case wr_jit_link:
	case wr_jit_unlk:
		return trueblnr;
	case wr_jit_interpret:
		return trueblnr;
	default:
		return falseblnr;
	}
}

LOCALINLINEFUNC blnr wr_jit_op_needs_cycle_commit(
	const struct wr_jit_op *op)
{
	return wr_jit_op_may_exit(op)
		|| op->kind == wr_jit_dbf
		|| op->kind == wr_jit_bcc
		|| op->kind == wr_jit_dbcc;
}

LOCALINLINEFUNC blnr wr_jit_op_direct_rdw(const struct wr_emit *e,
	const struct wr_jit_op *op)
{
	ui5r displacement;
	if (e->block->direct_guest_reg == 0xff
		|| (op->kind != wr_jit_move_rr_w
			&& op->kind != wr_jit_movea_w)
		|| op->src_reg != e->block->direct_guest_reg
		|| (op->y.v[0].AMd != kAMdIndirectW
			&& op->y.v[0].AMd != kAMdADispW))
		return falseblnr;
	displacement = op->y.v[0].AMd == kAMdADispW
		? (ui5r)(si5r)(si4b)op->extension : 0;
	return e->block->direct_reg_value + displacement
		== e->block->direct_guest_addr;
}

LOCALINLINEFUNC blnr wr_jit_emitted_op_may_exit(const struct wr_emit *e,
	const struct wr_jit_op *op)
{
	return wr_jit_op_may_exit(op) && !wr_jit_op_direct_rdw(e, op);
}

/* R8 and R9 hold the two most-used 68000 address registers in a region.
 * Address registers are especially profitable: effective-address formation
 * otherwise crosses SDRAM before nearly every guest memory access, while all
 * architectural writes to An are full-width and need no partial merge. */
LOCALPROC wr_jit_choose_cached_aregs(const struct wr_jit_block *block,
	ui3b cached[2])
{
	ui3b uses[16] = {0};
	ui3r i;

	cached[0] = 0xff;
	cached[1] = 0xff;
	/* Fallback handlers can update registers not described by the decoded
	 * operation, while MOVEM has special base-register-in-mask semantics.
	 * Keep those regions memory-backed instead of trying to synchronize a
	 * partial architectural view around them. */
	for (i = 0; i < block->count; ++i) {
		switch (block->op[i].kind) {
		case wr_jit_interpret:
		case wr_jit_movem_pre_l:
		case wr_jit_movem_post_l:
		case wr_jit_movem_store_l:
			return;
		default:
			break;
		}
	}

	for (i = 0; i < block->count; ++i) {
		const struct wr_jit_op *op = &block->op[i];
		switch (op->kind) {
		case wr_jit_move_rr_b:
		case wr_jit_move_rr_w:
		case wr_jit_move_rr_l:
		case wr_jit_movea_w:
		case wr_jit_movea_l:
			if (op->src_reg >= 8 && op->src_reg < 16)
				++uses[op->src_reg];
			if (op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		case wr_jit_clr_b:
		case wr_jit_clr_w:
		case wr_jit_clr_l:
		case wr_jit_addq_a:
		case wr_jit_subq_a:
		case wr_jit_movem_pre_l:
		case wr_jit_movem_post_l:
		case wr_jit_movem_store_l:
			if (op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		case wr_jit_adda:
		case wr_jit_suba:
			if (op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			if ((op->y.v[0].AMd == kAMdRegW
					|| op->y.v[0].AMd == kAMdRegL)
				&& op->src_reg >= 8 && op->src_reg < 16)
				++uses[op->src_reg];
			break;
		case wr_jit_btst_b:
			if (op->y.v[1].AMd != kAMdRegB
				&& op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		case wr_jit_jsr:
			++uses[15];
			if ((op->y.v[1].AMd == kAMdIndirectL
					|| op->y.v[1].AMd == kAMdADispL
					|| op->y.v[1].AMd == kAMdAIndexL)
				&& op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		case wr_jit_jmp:
			if ((op->y.v[1].AMd == kAMdIndirectL
					|| op->y.v[1].AMd == kAMdADispL
					|| op->y.v[1].AMd == kAMdAIndexL)
				&& op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		case wr_jit_rts:
			++uses[15];
			break;
		case wr_jit_link:
		case wr_jit_unlk:
			++uses[15];
			if (op->reg >= 8 && op->reg < 16)
				++uses[op->reg];
			break;
		default:
			break;
		}
	}
	for (i = 0; i < 2; ++i) {
		ui3r reg;
		ui3r best = 0xff;
		ui3r best_uses = 2;
		for (reg = 8; reg < 16; ++reg) {
			if (reg != cached[0] && uses[reg] > best_uses) {
				best = reg;
				best_uses = uses[reg];
			}
		}
		cached[i] = best;
	}
}

LOCALINLINEPROC wr_emit_aindex(struct wr_emit *e, ui3r base,
	ui4r extension)
{
	ui3r index = (extension >> 12) & 15;
	if ((extension & 0x0800) != 0) {
		wr_load_guest_l(e, WR_R4, index);
	} else {
		/* A cached address-register index may be newer than its V_regs
		 * shadow. Commit it before taking the architecturally low word. */
		wr_flush_guest_cache(e);
		wr_mem(e, WR_O_LDH, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.regs[index] - (ui3p)&V_regs));
	}
	wr_rr(e, WR_O_ADD, base, WR_R4);
	wr_addi(e, base, (ui5r)(si5r)(si3b)(ui3b)extension, falseblnr);
}

/* Run a recorded instruction through the reference core after a speculative
 * control-flow guard fails. R1 still names the opcode, so reproduce the
 * dispatcher's fetch before entering the handler. The caller then exits the
 * region with the handler's exact PC/register state. */
LOCALPROC wr_emit_control_fallback(struct wr_emit *e,
	const struct wr_jit_op *op)
{
	ui5r y;
	wr_addi(e, WR_R1, 2, falseblnr);
	wr_store_machine(e);
	MyMoveBytes((anyp)&op->y, (anyp)&y, sizeof y);
	wr_ri(e, WR_I_MOV, WR_R4, y);
	wr_mem(e, WR_O_STW, WR_R4, WR_R0,
		(ui5r)((ui3p)&V_regs.CurDecOpY - (ui3p)&V_regs));
	wr_xcall(e, (ui5r)op->handler);
	wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
}

LOCALPROC wr_emit_op(struct wr_emit *e, const struct wr_jit_op *op)
{
	ui5r reg_offset = (ui5r)((ui3p)&V_regs.regs[op->reg] - (ui3p)&V_regs);
	ui5r src_reg_offset =
		(ui5r)((ui3p)&V_regs.regs[op->src_reg] - (ui3p)&V_regs);

	switch (op->kind) {
	case wr_jit_bra:
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_dbf:
		wr_mem(e, WR_O_LDUH, WR_R4, WR_R0, reg_offset);
		wr_addi(e, WR_R4, 1, trueblnr);
		wr_mem(e, WR_O_STH, WR_R4, WR_R0, reg_offset);
		wr_ri(e, WR_I_MOV, WR_R1,
			(ui5r)(op->at + 2 + (si5r)(si4b)op->extension));
		/* The interpreter sign-extends the word before decrementing it.
		 * Zero therefore becomes 0xffffffff, not 0x0000ffff. */
		wr_ri(e, WR_I_CMP, WR_R4, (ui5r)-1);
		{
			ui5r taken = wr_forward(e, WR_B_NE);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)(op->at + 4));
			wr_land(e, taken);
		}
		wr_guard_next(e, op->next);
		break;
	case wr_jit_moveq:
		wr_ri(e, WR_I_MOV, WR_R4,
			ui5r_FromSByte(op->y.v[0].ArgDat));
		wr_store_guest_l(e, WR_R4, op->reg);
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4,
			ui5r_FromSByte(op->y.v[0].ArgDat));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_nop:
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_jsr:
		{
			ui5r mismatch;
			ui5r not_ram;
			ui5r not_ram_end;
			ui5r done;
			ui3r amd = op->y.v[1].AMd;
			ui3r words = amd == kAMdIndirectL ? 0
				: (amd == kAMdAbsLL ? 2 : 1);
			ui5r target_guest = e->block->guest_start
				+ (op->next - e->block->start);
			ui5r return_guest = e->block->guest_start
				+ (op->at + 2 + words * 2 - e->block->start);
			if (!e->direct_ram) {
				wr_emit_control_fallback(e, op);
				wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
				wr_note_exit(e, WR_B_EQ);
				break;
			}
			if (amd == kAMdIndirectL || amd == kAMdADispL
				|| amd == kAMdAIndexL) {
				wr_load_guest_l(e, WR_R4, op->reg);
				if (amd == kAMdADispL)
					wr_addi(e, WR_R4,
						(ui5r)(si5r)(si4b)op->extension, falseblnr);
				else if (amd == kAMdAIndexL)
					wr_emit_aindex(e, WR_R4, op->extension);
			} else if (amd == kAMdPCIndexL) {
				wr_ri(e, WR_I_MOV, WR_R4, return_guest - 2);
				wr_emit_aindex(e, WR_R4, op->extension);
			} else {
				wr_ri(e, WR_I_MOV, WR_R4, target_guest);
			}
			wr_ri(e, WR_I_CMP, WR_R4, target_guest);
			mismatch = wr_forward(e, WR_B_NE);
			wr_load_guest_l(e, WR_R6, 15);
			wr_addi(e, WR_R6, 4, trueblnr);
			not_ram = wr_emit_direct_ram_address(e, 4, &not_ram_end);
			wr_store_guest_l(e, WR_R6, 15);
			wr_ri(e, WR_I_MOV, WR_R3, return_guest);
			wr_emit_host_long_write(e);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)op->next);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			done = wr_forward(e, WR_B_EQ);
			wr_land(e, mismatch);
			wr_land(e, not_ram);
			wr_land(e, not_ram_end);
			wr_emit_control_fallback(e, op);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			wr_note_exit(e, WR_B_EQ);
			wr_land(e, done);
		}
		break;
	case wr_jit_jmp:
		{
			ui5r mismatch;
			ui5r done;
			ui3r amd = op->y.v[1].AMd;
			ui5r target_guest = e->block->guest_start
				+ (op->next - e->block->start);
			ui5r pc_base = e->block->guest_start
				+ (op->at + 2 - e->block->start);
			if (amd == kAMdIndirectL || amd == kAMdADispL
				|| amd == kAMdAIndexL) {
				wr_load_guest_l(e, WR_R4, op->reg);
				if (amd == kAMdADispL)
					wr_addi(e, WR_R4,
						(ui5r)(si5r)(si4b)op->extension, falseblnr);
				else if (amd == kAMdAIndexL)
					wr_emit_aindex(e, WR_R4, op->extension);
			} else if (amd == kAMdPCIndexL) {
				wr_ri(e, WR_I_MOV, WR_R4, pc_base);
				wr_emit_aindex(e, WR_R4, op->extension);
			} else {
				wr_ri(e, WR_I_MOV, WR_R4, target_guest);
			}
			wr_ri(e, WR_I_CMP, WR_R4, target_guest);
			mismatch = wr_forward(e, WR_B_NE);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)op->next);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			done = wr_forward(e, WR_B_EQ);
			wr_land(e, mismatch);
			wr_emit_control_fallback(e, op);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			wr_note_exit(e, WR_B_EQ);
			wr_land(e, done);
		}
		break;
	case wr_jit_rts:
		{
			ui5r mismatch;
			ui5r not_ram;
			ui5r not_ram_end;
			ui5r done;
			ui5r target_guest = e->block->guest_start
				+ (op->next - e->block->start);
			if (!e->direct_ram) {
				wr_emit_control_fallback(e, op);
				wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
				wr_note_exit(e, WR_B_EQ);
				break;
			}
			wr_load_guest_l(e, WR_R6, 15);
			not_ram = wr_emit_direct_ram_address(e, 4, &not_ram_end);
			wr_emit_host_long_read(e);
			wr_ri(e, WR_I_CMP, WR_R3, target_guest);
			mismatch = wr_forward(e, WR_B_NE);
			wr_load_guest_l(e, WR_R4, 15);
			wr_addi(e, WR_R4, 4, falseblnr);
			wr_store_guest_l(e, WR_R4, 15);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)op->next);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			done = wr_forward(e, WR_B_EQ);
			wr_land(e, mismatch);
			wr_land(e, not_ram);
			wr_land(e, not_ram_end);
			wr_emit_control_fallback(e, op);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			wr_note_exit(e, WR_B_EQ);
			wr_land(e, done);
		}
		break;
	case wr_jit_link:
		{
			ui5r not_ram;
			ui5r not_ram_end;
			ui5r done;
			if (!e->direct_ram) {
				wr_emit_control_fallback(e, op);
				wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
				wr_note_exit(e, WR_B_EQ);
				break;
			}
			wr_load_guest_l(e, WR_R6, 15);
			wr_addi(e, WR_R6, 4, trueblnr);
			not_ram = wr_emit_direct_ram_address(e, 4, &not_ram_end);
			wr_store_guest_l(e, WR_R6, 15);
			wr_load_guest_l(e, WR_R3, op->reg);
			wr_emit_host_long_write(e);
			wr_store_guest_l(e, WR_R6, op->reg);
			wr_addi(e, WR_R6,
				(ui5r)(si5r)(si4b)op->extension, falseblnr);
			wr_store_guest_l(e, WR_R6, 15);
			wr_addi(e, WR_R1, 4, falseblnr);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			done = wr_forward(e, WR_B_EQ);
			wr_land(e, not_ram);
			wr_land(e, not_ram_end);
			wr_emit_control_fallback(e, op);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			wr_note_exit(e, WR_B_EQ);
			wr_land(e, done);
		}
		break;
	case wr_jit_unlk:
		{
			ui5r not_ram;
			ui5r not_ram_end;
			ui5r done;
			if (!e->direct_ram) {
				wr_emit_control_fallback(e, op);
				wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
				wr_note_exit(e, WR_B_EQ);
				break;
			}
			wr_load_guest_l(e, WR_R6, op->reg);
			not_ram = wr_emit_direct_ram_address(e, 4, &not_ram_end);
			wr_emit_host_long_read(e);
			if (op->reg == 15) {
				wr_addi(e, WR_R3, 4, falseblnr);
				wr_store_guest_l(e, WR_R3, 15);
			} else {
				wr_store_guest_l(e, WR_R3, op->reg);
				wr_addi(e, WR_R6, 4, falseblnr);
				wr_store_guest_l(e, WR_R6, 15);
			}
			wr_addi(e, WR_R1, 2, falseblnr);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			done = wr_forward(e, WR_B_EQ);
			wr_land(e, not_ram);
			wr_land(e, not_ram_end);
			wr_emit_control_fallback(e, op);
			wr_rr(e, WR_O_CMP, WR_R3, WR_R3);
			wr_note_exit(e, WR_B_EQ);
			wr_land(e, done);
		}
		break;
	case wr_jit_move_rr_b:
	case wr_jit_move_rr_w:
	case wr_jit_move_rr_l:
		{
		ui5r size = op->kind == wr_jit_move_rr_b ? 1
			: (op->kind == wr_jit_move_rr_w ? 2 : 4);
		ui4r reg_mode = size == 1 ? kAMdRegB
			: (size == 2 ? kAMdRegW : kAMdRegL);
		ui4r post_mode = size == 1 ? kAMdAPosIncB
			: (size == 2 ? kAMdAPosIncW : kAMdAPosIncL);
		ui4r pre_mode = size == 1 ? kAMdAPreDecB
			: (size == 2 ? kAMdAPreDecW : kAMdAPreDecL);
		ui5r src_step = size;
		ui5r dst_step = size;
		ui5r src_disp = op->extension;
		ui5r dst_disp = (wr_jit_is_adisp(op->y.v[0].AMd)
			|| wr_jit_is_aindex(op->y.v[0].AMd))
			? op->extension2 : op->extension;
		if (op->y.v[0].AMd == kAMdAPosInc7B
			|| op->y.v[0].AMd == kAMdAPreDec7B)
			src_step = 2;
		if (op->y.v[1].AMd == kAMdAPosInc7B
			|| op->y.v[1].AMd == kAMdAPreDec7B)
			dst_step = 2;
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		if (op->y.v[0].AMd == reg_mode) {
			if (size == 4)
				wr_load_guest_l(e, WR_R3, op->src_reg);
			else
				wr_mem(e, size == 1 ? WR_O_LDB : WR_O_LDH,
					WR_R3, WR_R0, src_reg_offset);
		} else if (wr_jit_op_direct_rdw(e, op)) {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R12, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_rr(e, WR_O_SEXTH, WR_R3, WR_R3);
		} else {
			wr_load_guest_l(e, WR_R6, op->src_reg);
			if (op->y.v[0].AMd == post_mode
				|| op->y.v[0].AMd == kAMdAPosInc7B) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, src_step, falseblnr);
				wr_store_guest_l(e, WR_R4, op->src_reg);
			} else if (op->y.v[0].AMd == pre_mode
				|| op->y.v[0].AMd == kAMdAPreDec7B) {
				wr_addi(e, WR_R6, src_step, trueblnr);
				wr_store_guest_l(e, WR_R6, op->src_reg);
			} else if (wr_jit_is_aindex(op->y.v[0].AMd)) {
				wr_emit_aindex(e, WR_R6, src_disp);
			} else if (wr_jit_is_adisp(op->y.v[0].AMd)) {
				wr_addi(e, WR_R6,
					(ui5r)(si5r)(si4b)src_disp, falseblnr);
			}
			if ((MINIVMAC_JIT_INLINE_MEMORY_MASK
				& (size == 1 ? 1 : (size == 2 ? 2 : 4))) != 0) {
				wr_emit_guest_read(e, size, op->next);
			} else {
				wr_store_machine(e);
				wr_xcall(e, (ui5r)(size == 1 ? get_byte
					: (size == 2 ? get_word : get_long)));
				wr_rr(e, WR_O_MOV, WR_R3, WR_R4);
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
		}
		if (op->y.v[1].AMd == reg_mode) {
			if (size == 4)
				wr_store_guest_l(e, WR_R3, op->reg);
			else
				wr_mem(e, size == 1 ? WR_O_STB : WR_O_STH,
					WR_R3, WR_R0, reg_offset);
		} else {
			wr_load_guest_l(e, WR_R6, op->reg);
			if (op->y.v[1].AMd == post_mode
				|| op->y.v[1].AMd == kAMdAPosInc7B) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, dst_step, falseblnr);
				wr_store_guest_l(e, WR_R4, op->reg);
			} else if (op->y.v[1].AMd == pre_mode
				|| op->y.v[1].AMd == kAMdAPreDec7B) {
				wr_addi(e, WR_R6, dst_step, trueblnr);
				wr_store_guest_l(e, WR_R6, op->reg);
			} else if (wr_jit_is_aindex(op->y.v[1].AMd)) {
				wr_emit_aindex(e, WR_R6, dst_disp);
			} else if (wr_jit_is_adisp(op->y.v[1].AMd)) {
				wr_addi(e, WR_R6,
					(ui5r)(si5r)(si4b)dst_disp, falseblnr);
			}
			if ((MINIVMAC_JIT_INLINE_MEMORY_MASK
				& (size == 1 ? 8 : (size == 2 ? 16 : 32))) != 0) {
				wr_emit_guest_write(e, size, op->next);
			} else {
				wr_rr(e, WR_O_MOV, WR_R7, WR_R3);
				wr_mem(e, WR_O_STW, WR_R3, WR_R0,
					(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
				wr_store_machine(e);
				wr_xcall(e, (ui5r)(size == 1 ? put_byte
					: (size == 2 ? put_word : put_long)));
				/* Preserve MOVE's source for its condition-code update. */
				wr_mem(e, WR_O_LDW, WR_R3, WR_R0,
					(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
		}
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		}
		break;
	case wr_jit_movea_w:
	case wr_jit_movea_l:
		{
		ui5r size = op->kind == wr_jit_movea_w ? 2 : 4;
		ui4r reg_mode = size == 2 ? kAMdRegW : kAMdRegL;
		ui4r post_mode = size == 2 ? kAMdAPosIncW : kAMdAPosIncL;
		ui4r pre_mode = size == 2 ? kAMdAPreDecW : kAMdAPreDecL;

		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		if (op->y.v[0].AMd == reg_mode) {
			if (size == 4)
				wr_load_guest_l(e, WR_R3, op->src_reg);
			else
				wr_mem(e, WR_O_LDH, WR_R3, WR_R0, src_reg_offset);
		} else if (wr_jit_op_direct_rdw(e, op)) {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R12, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_rr(e, WR_O_SEXTH, WR_R3, WR_R3);
		} else {
			wr_load_guest_l(e, WR_R6, op->src_reg);
			if (op->y.v[0].AMd == post_mode) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, size, falseblnr);
				wr_store_guest_l(e, WR_R4, op->src_reg);
			} else if (op->y.v[0].AMd == pre_mode) {
				wr_addi(e, WR_R6, size, trueblnr);
				wr_store_guest_l(e, WR_R6, op->src_reg);
			} else if (wr_jit_is_adisp(op->y.v[0].AMd)) {
				wr_addi(e, WR_R6,
					(ui5r)(si5r)(si4b)op->extension, falseblnr);
			}
			if ((MINIVMAC_JIT_INLINE_MEMORY_MASK
				& (size == 2 ? 2 : 4)) != 0) {
				wr_emit_guest_read(e, size, op->next);
			} else {
				wr_store_machine(e);
				wr_xcall(e, (ui5r)(size == 2 ? get_word : get_long));
				wr_rr(e, WR_O_MOV, WR_R3, WR_R4);
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
		}
		wr_store_guest_l(e, WR_R3, op->reg);
		}
		break;
	case wr_jit_clr_b:
	case wr_jit_clr_w:
	case wr_jit_clr_l:
		{
		ui5r size = op->kind == wr_jit_clr_b ? 1
			: (op->kind == wr_jit_clr_w ? 2 : 4);
		ui4r reg_mode = size == 1 ? kAMdRegB
			: (size == 2 ? kAMdRegW : kAMdRegL);
		ui4r post_mode = size == 1 ? kAMdAPosIncB
			: (size == 2 ? kAMdAPosIncW : kAMdAPosIncL);
		ui4r pre_mode = size == 1 ? kAMdAPreDecB
			: (size == 2 ? kAMdAPreDecW : kAMdAPreDecL);
		ui5r step = size;

		if (op->y.v[1].AMd == kAMdAPosInc7B
			|| op->y.v[1].AMd == kAMdAPreDec7B)
			step = 2;
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		wr_ri(e, WR_I_MOV, WR_R3, 0);
		if (op->y.v[1].AMd == reg_mode) {
			wr_mem(e, size == 1 ? WR_O_STB
				: (size == 2 ? WR_O_STH : WR_O_STW),
				WR_R3, WR_R0, reg_offset);
		} else {
			wr_load_guest_l(e, WR_R6, op->reg);
			if (op->y.v[1].AMd == post_mode
				|| op->y.v[1].AMd == kAMdAPosInc7B) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, step, falseblnr);
				wr_store_guest_l(e, WR_R4, op->reg);
			} else if (op->y.v[1].AMd == pre_mode
				|| op->y.v[1].AMd == kAMdAPreDec7B) {
				wr_addi(e, WR_R6, step, trueblnr);
				wr_store_guest_l(e, WR_R6, op->reg);
			} else if (wr_jit_is_adisp(op->y.v[1].AMd)) {
				wr_addi(e, WR_R6,
					(ui5r)(si5r)(si4b)op->extension, falseblnr);
			}
			if ((MINIVMAC_JIT_INLINE_MEMORY_MASK
				& (size == 1 ? 8 : (size == 2 ? 16 : 32))) != 0) {
				wr_emit_guest_write(e, size, op->next);
			} else {
				wr_rr(e, WR_O_MOV, WR_R7, WR_R3);
				wr_mem(e, WR_O_STW, WR_R3, WR_R0,
					(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
				wr_store_machine(e);
				wr_xcall(e, (ui5r)(size == 1 ? put_byte
					: (size == 2 ? put_word : put_long)));
				wr_mem(e, WR_O_LDW, WR_R3, WR_R0,
					(ui5r)((ui3p)&V_regs.SrcVal - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
		}
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		}
		break;
	case wr_jit_tst_r_b:
	case wr_jit_tst_r_w:
	case wr_jit_tst_r_l:
		if (op->kind == wr_jit_tst_r_l)
			wr_load_guest_l(e, WR_R4, op->reg);
		else
			wr_mem(e, op->kind == wr_jit_tst_r_b ? WR_O_LDB : WR_O_LDH,
				WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, op->kind == wr_jit_tst_r_b
			? kLazyFlagsTstB : (op->kind == wr_jit_tst_r_w
				? kLazyFlagsTstW : kLazyFlagsTstL));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_cmp_rr_b:
	case wr_jit_cmp_rr_w:
	case wr_jit_cmp_rr_l:
		{
		ui3r size = op->kind == wr_jit_cmp_rr_b ? 1
			: (op->kind == wr_jit_cmp_rr_w ? 2 : 4);
		ui3r immed_mode = size == 1 ? kAMdImmedB
			: (size == 2 ? kAMdImmedW : kAMdImmedL);
		if (op->y.v[0].AMd == immed_mode)
			wr_ri(e, WR_I_MOV, WR_R3, size == 1
				? (ui5r)(si5r)(si3b)(ui3b)op->extension
				: (size == 2 ? (ui5r)(si5r)(si4b)op->extension
					: ((ui5r)op->extension << 16) | op->extension2));
		else if (size == 4)
			wr_load_guest_l(e, WR_R3, op->src_reg);
		else
			wr_mem(e, size == 1 ? WR_O_LDB : WR_O_LDH,
				WR_R3, WR_R0, src_reg_offset);
		if (size == 4)
			wr_load_guest_l(e, WR_R4, op->reg);
		else
			wr_mem(e, size == 1 ? WR_O_LDB : WR_O_LDH,
				WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, size == 1 ? kLazyFlagsCmpB
			: (size == 2 ? kLazyFlagsCmpW : kLazyFlagsCmpL));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		}
		break;
	case wr_jit_add_rr_l:
	case wr_jit_sub_rr_l:
		if (op->y.v[0].AMd == kAMdDat4)
			wr_ri(e, WR_I_MOV, WR_R3, op->src_reg);
		else if (op->y.v[0].AMd == kAMdImmedL)
			wr_ri(e, WR_I_MOV, WR_R3,
				((ui5r)op->extension << 16) | op->extension2);
		else
			wr_load_guest_l(e, WR_R3, op->src_reg);
		wr_load_guest_l(e, WR_R4, op->reg);
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgDst - (ui3p)&V_regs));
		wr_rr(e, op->kind == wr_jit_add_rr_l ? WR_O_ADD : WR_O_SUB,
			WR_R4, WR_R3);
		wr_store_guest_l(e, WR_R4, op->reg);
		wr_ri(e, WR_I_MOV, WR_R4,
			op->kind == wr_jit_add_rr_l
				? kLazyFlagsAddL : kLazyFlagsSubL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_addq_a:
	case wr_jit_subq_a:
		wr_load_guest_l(e, WR_R4, op->reg);
		wr_addi(e, WR_R4, op->src_reg,
			op->kind == wr_jit_subq_a);
		wr_store_guest_l(e, WR_R4, op->reg);
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_adda:
	case wr_jit_suba:
		if (op->y.v[0].AMd == kAMdRegW)
			wr_mem(e, WR_O_LDH, WR_R3, WR_R0, src_reg_offset);
		else if (op->y.v[0].AMd == kAMdRegL)
			wr_load_guest_l(e, WR_R3, op->src_reg);
		else if (op->y.v[0].AMd == kAMdImmedW)
			wr_ri(e, WR_I_MOV, WR_R3,
				(ui5r)(si5r)(si4b)op->extension);
		else
			wr_ri(e, WR_I_MOV, WR_R3,
				((ui5r)op->extension << 16) | op->extension2);
		wr_load_guest_l(e, WR_R4, op->reg);
		wr_rr(e, op->kind == wr_jit_adda ? WR_O_ADD : WR_O_SUB,
			WR_R4, WR_R3);
		wr_store_guest_l(e, WR_R4, op->reg);
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_logic_rr:
		{
		ui3r size = op->y.v[1].AMd == kAMdRegB ? 1
			: (op->y.v[1].AMd == kAMdRegW ? 2 : 4);
		ui3r immed_mode = size == 1 ? kAMdImmedB
			: (size == 2 ? kAMdImmedW : kAMdImmedL);
		ui3r operation = op->extension;
		if (op->y.v[0].AMd == immed_mode) {
			ui5r immediate = size == 1
				? (ui5r)(si5r)(si3b)(ui3b)op->extension
				: (size == 2
					? (ui5r)(si5r)(si4b)op->extension
					: ((ui5r)op->extension << 16) | op->extension2);
			wr_ri(e, WR_I_MOV, WR_R3, immediate);
			operation = (op->opcode & 0x0e00) == 0x0200 ? 0
				: ((op->opcode & 0x0e00) == 0 ? 1 : 2);
		} else {
			wr_mem(e, size == 1 ? WR_O_LDB
				: (size == 2 ? WR_O_LDH : WR_O_LDW),
				WR_R3, WR_R0, src_reg_offset);
		}
		wr_mem(e, size == 1 ? WR_O_LDB
			: (size == 2 ? WR_O_LDH : WR_O_LDW),
			WR_R4, WR_R0, reg_offset);
		wr_rr(e, operation == 0 ? WR_O_AND
			: (operation == 1 ? WR_O_OR : WR_O_XOR),
			WR_R4, WR_R3);
		wr_mem(e, size == 1 ? WR_O_STB
			: (size == 2 ? WR_O_STH : WR_O_STW),
			WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		}
		break;
	case wr_jit_not_r:
		{
		ui3r size = op->y.v[1].AMd == kAMdRegB ? 1
			: (op->y.v[1].AMd == kAMdRegW ? 2 : 4);
		wr_mem(e, size == 1 ? WR_O_LDB
			: (size == 2 ? WR_O_LDH : WR_O_LDW),
			WR_R4, WR_R0, reg_offset);
		wr_ri(e, WR_I_MOV, WR_R3, (ui5r)-1);
		wr_rr(e, WR_O_XOR, WR_R4, WR_R3);
		wr_mem(e, size == 1 ? WR_O_STB
			: (size == 2 ? WR_O_STH : WR_O_STW),
			WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R3, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		}
		break;
	case wr_jit_addq_w:
	case wr_jit_subq_w:
	case wr_jit_add_rr_w:
	case wr_jit_sub_rr_w:
		wr_mem(e, WR_O_LDH, WR_R4, WR_R0, reg_offset);
		if (op->y.v[0].AMd == kAMdImmedW)
			wr_ri(e, WR_I_MOV, WR_R3,
				(ui5r)(si5r)(si4b)op->extension);
		else if (op->kind == wr_jit_add_rr_w || op->kind == wr_jit_sub_rr_w)
			wr_mem(e, WR_O_LDH, WR_R3, WR_R0, src_reg_offset);
		else
			wr_ri(e, WR_I_MOV, WR_R3, op->src_reg);
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgDst - (ui3p)&V_regs));
		if (op->kind == wr_jit_add_rr_w || op->kind == wr_jit_sub_rr_w)
			wr_rr(e, op->kind == wr_jit_add_rr_w
				? WR_O_ADD : WR_O_SUB, WR_R4, WR_R3);
		else
			wr_addi(e, WR_R4, op->src_reg,
				op->kind == wr_jit_subq_w);
		wr_mem(e, WR_O_STH, WR_R4, WR_R0, reg_offset);
		wr_ri(e, WR_I_MOV, WR_R4,
			op->kind == wr_jit_addq_w || op->kind == wr_jit_add_rr_w
				? kLazyFlagsAddW : kLazyFlagsSubW);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_addq_b:
	case wr_jit_subq_b:
	case wr_jit_add_rr_b:
	case wr_jit_sub_rr_b:
		wr_mem(e, WR_O_LDB, WR_R4, WR_R0, reg_offset);
		if (op->y.v[0].AMd == kAMdImmedB)
			wr_ri(e, WR_I_MOV, WR_R3,
				(ui5r)(si5r)(si3b)(ui3b)op->extension);
		else if (op->kind == wr_jit_add_rr_b || op->kind == wr_jit_sub_rr_b)
			wr_mem(e, WR_O_LDB, WR_R3, WR_R0, src_reg_offset);
		else
			wr_ri(e, WR_I_MOV, WR_R3, op->src_reg);
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagArgDst - (ui3p)&V_regs));
		if (op->kind == wr_jit_add_rr_b || op->kind == wr_jit_sub_rr_b)
			wr_rr(e, op->kind == wr_jit_add_rr_b
				? WR_O_ADD : WR_O_SUB, WR_R4, WR_R3);
		else
			wr_addi(e, WR_R4, op->src_reg,
				op->kind == wr_jit_subq_b);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0, reg_offset);
		wr_ri(e, WR_I_MOV, WR_R4,
			op->kind == wr_jit_addq_b || op->kind == wr_jit_add_rr_b
				? kLazyFlagsAddB : kLazyFlagsSubB);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_asr_w:
		if (op->y.v[0].AMd == kAMdDat4)
			wr_ri(e, WR_I_MOV, WR_R6, op->src_reg);
		else
			wr_load_guest_l(e, WR_R6, op->src_reg);
		wr_ri(e, WR_I_MOV, WR_R7, op->reg);
		wr_addi(e, WR_R1, 2, falseblnr);
		wr_store_machine(e);
		wr_xcall(e, (ui5r)wr_jit_asr_word);
		wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
			(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
		wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
			(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
		break;
	case wr_jit_btst_b:
		{
			ui4r dst_mode = op->y.v[1].AMd;
			ui5r displacement = op->y.v[0].AMd == kAMdImmedB
				? op->extension2 : op->extension;
			wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
			if (dst_mode == kAMdRegB) {
				wr_mem(e, WR_O_LDB, WR_R3, WR_R0, reg_offset);
			} else {
				wr_load_guest_l(e, WR_R6, op->reg);
				if (dst_mode == kAMdAPosIncB
					|| dst_mode == kAMdAPosInc7B) {
					wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
					wr_addi(e, WR_R4,
						dst_mode == kAMdAPosInc7B ? 2 : 1,
						falseblnr);
					wr_store_guest_l(e, WR_R4, op->reg);
				} else if (dst_mode == kAMdAPreDecB
					|| dst_mode == kAMdAPreDec7B) {
					wr_addi(e, WR_R6,
						dst_mode == kAMdAPreDec7B ? 2 : 1,
						trueblnr);
					wr_store_guest_l(e, WR_R6, op->reg);
				} else if (dst_mode == kAMdADispB) {
					wr_addi(e, WR_R6,
						(ui5r)(si5r)(si4b)displacement, falseblnr);
				}
				wr_store_machine(e);
				wr_xcall(e, (ui5r)get_byte);
				wr_rr(e, WR_O_MOV, WR_R3, WR_R4);
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
			/* A memory read returns in R4, and both it and the following
			 * guard may clobber scratch registers.  Keep the byte in R3
			 * and load the bit number directly into the argument register. */
			if (op->y.v[0].AMd == kAMdRegB)
				wr_mem(e, WR_O_LDB, WR_R6, WR_R0, src_reg_offset);
			else
				wr_ri(e, WR_I_MOV, WR_R6, op->extension & 0xff);
			wr_rr(e, WR_O_MOV, WR_R7, WR_R3);
			wr_store_machine(e);
			wr_xcall(e, (ui5r)wr_jit_set_z_from_bit);
			wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
				(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
			wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
				(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
		}
		break;
	case wr_jit_bcc:
		{
			ui3p fall = op->at + (op->check_extension ? 4 : 2);
			ui3p target = op->at + 2
				+ (op->check_extension
					? (si5r)(si4b)op->extension
					: (si5r)(si3b)(ui3b)op->opcode);
			ui5r not_taken = 0;

			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)fall);
			if (op->extension2 == kLazyFlagsTstL) {
				ui4r inverse = 0;
				switch (op->src_reg & 15) {
				case 0:
				case 4:
				case 8:
					wr_ri(e, WR_I_MOV, WR_R1, (ui5r)target);
					break;
				case 1:
				case 5:
				case 9:
					break;
				case 2:
				case 6:
					inverse = WR_B_EQ;
					break;
				case 3:
				case 7:
					inverse = WR_B_NE;
					break;
				case 10:
				case 12:
					inverse = WR_B_LT;
					break;
				case 11:
				case 13:
					inverse = WR_B_GE;
					break;
				case 14:
					inverse = WR_B_LE;
					break;
				default:
					inverse = WR_B_GT;
					break;
				}
				if (inverse != 0) {
					wr_mem(e, WR_O_LDW, WR_R4, WR_R0,
						(ui5r)((ui3p)&V_regs.LazyFlagArgDst
							- (ui3p)&V_regs));
					wr_ri(e, WR_I_CMP, WR_R4, 0);
					not_taken = wr_forward(e, inverse);
					wr_ri(e, WR_I_MOV, WR_R1, (ui5r)target);
					wr_land(e, not_taken);
				}
			} else {
				wr_ri(e, WR_I_MOV, WR_R6, op->src_reg);
				wr_xcall(e, (ui5r)(((op->src_reg & 15) == 6
						|| (op->src_reg & 15) == 7)
					? wr_jit_eq_true : wr_jit_cc_true));
				wr_ri(e, WR_I_CMP, WR_R4, 0);
				not_taken = wr_forward(e, WR_B_EQ);
				wr_ri(e, WR_I_MOV, WR_R1, (ui5r)target);
				wr_land(e, not_taken);
			}
			wr_guard_next(e, op->next);
		}
		break;
	case wr_jit_dbcc:
		{
			ui3p fall = op->at + 4;
			ui3p target = op->at + 2
				+ (si5r)(si4b)op->extension;
			ui5r condition_true;
			ui5r counter_live;

			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)fall);
			wr_ri(e, WR_I_MOV, WR_R6, op->src_reg);
			wr_xcall(e, (ui5r)(((op->src_reg & 15) == 6
					|| (op->src_reg & 15) == 7)
				? wr_jit_eq_true : wr_jit_cc_true));
			wr_ri(e, WR_I_CMP, WR_R4, 0);
			condition_true = wr_forward(e, WR_B_NE);
			wr_mem(e, WR_O_LDUH, WR_R4, WR_R0, reg_offset);
			wr_addi(e, WR_R4, 1, trueblnr);
			wr_mem(e, WR_O_STH, WR_R4, WR_R0, reg_offset);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)target);
			wr_ri(e, WR_I_CMP, WR_R4, (ui5r)-1);
			counter_live = wr_forward(e, WR_B_NE);
			wr_ri(e, WR_I_MOV, WR_R1, (ui5r)fall);
			wr_land(e, counter_live);
			wr_land(e, condition_true);
			wr_guard_next(e, op->next);
		}
		break;
	case wr_jit_movem_pre_l:
	case wr_jit_movem_post_l:
	case wr_jit_movem_store_l:
		wr_emit_movem_l(e, op);
		break;
	default:
		{
			ui5r y;
			MyMoveBytes((anyp)&op->y, (anyp)&y, sizeof y);
			wr_addi(e, WR_R1, 2, falseblnr);
			wr_mem(e, WR_O_STW, WR_R1, WR_R0,
				(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
			wr_mem(e, WR_O_STW, WR_R2, WR_R0,
				(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
			wr_ri(e, WR_I_MOV, WR_R4, y);
			wr_mem(e, WR_O_STW, WR_R4, WR_R0,
				(ui5r)((ui3p)&V_regs.CurDecOpY - (ui3p)&V_regs));
			wr_xcall(e, (ui5r)op->handler);
			wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
				(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
			wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
				(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
			wr_guard_next(e, op->next);
		}
		break;
	}
	if (wr_jit_emitted_op_may_exit(e, op)) {
		wr_ri(e, WR_I_CMP, WR_R2, 0);
		wr_note_exit(e, WR_B_LE);
	}
}

LOCALFUNC ui5r wr_jit_emit_block(struct wr_jit_block *block, ui3p base,
	blnr write)
{
	struct wr_emit e;
	ui3r i;
	ui3r rdw_reads = 0;
	ui5r exit_at;
	ui5r pending_cycles = 0;
	ui5r op_offset[WR_JIT_OPS];
	ui5r loop_cycles = 0;
	ui3r loop_index = WR_JIT_OPS;
	blnr region_safe = trueblnr;
	blnr can_loop;
	blnr can_chain;

	e.base = base;
	e.off = 0;
	e.nexits = 0;
	if (MINIVMAC_JIT_REG_CACHE != 0)
		wr_jit_choose_cached_aregs(block, e.cached_guest);
	else
		e.cached_guest[0] = e.cached_guest[1] = 0xff;
	e.cache_rdw = falseblnr;
	e.direct_ram = MINIVMAC_JIT_DIRECT_RAM != 0
		&& block->memory_overlay == 0
		&& block->direct_guest_reg == 0xff;
	e.write = write;
	e.block = block;

	/* Cache MATCrdW only when its setup can be amortized and no operation in
	 * the trace can invalidate the address space behind our back. Ordinary
	 * word reads can replace this same MATC only on a miss; that path reloads
	 * the four live registers before continuing. */
	if ((MINIVMAC_JIT_INLINE_MEMORY_MASK & 2) != 0) {
		blnr cache_safe = trueblnr;
		for (i = 0; i < block->count; ++i) {
			const struct wr_jit_op *op = &block->op[i];
			if (op->kind == wr_jit_interpret
				|| op->kind == wr_jit_movem_pre_l
				|| op->kind == wr_jit_movem_post_l
				|| op->kind == wr_jit_movem_store_l)
			{
				cache_safe = falseblnr;
				break;
			}
			if (op->kind == wr_jit_move_rr_b
				|| op->kind == wr_jit_move_rr_w
				|| op->kind == wr_jit_move_rr_l)
			{
				ui4r reg_mode = op->kind == wr_jit_move_rr_b
					? kAMdRegB : (op->kind == wr_jit_move_rr_w
						? kAMdRegW : kAMdRegL);
				if (op->y.v[1].AMd != reg_mode) {
					cache_safe = falseblnr;
					break;
				}
				if (op->kind == wr_jit_move_rr_w
					&& op->y.v[0].AMd != kAMdRegW)
					++rdw_reads;
			}
		}
		e.cache_rdw = cache_safe && rdw_reads >= 2;
	}
	if (block->direct_guest_reg != 0xff || e.direct_ram
		|| e.cached_guest[0] != 0xff)
		e.cache_rdw = falseblnr;
	wr_w(&e, 0x0203); /* pushn %r3 */
	wr_ri(&e, WR_I_MOV, WR_R0, (ui5r)&V_regs);
	wr_mem(&e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(&e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
	/* Chained traces enter here with R0/R1/R2 and the original pushn frame
	 * still live. Keep this fixed so the predecessor can skip the redundant
	 * prologue without growing the native stack. */
	if (e.off != WR_JIT_CHAIN_ENTRY_BYTES)
		return WR_JIT_MAX_BLOCK_BYTES;
	if (e.cache_rdw)
		wr_load_cached_rdw(&e);
	if (e.direct_ram) {
		wr_ri(&e, WR_I_MOV, WR_R10, 0x00c00000);
		wr_ri(&e, WR_I_MOV, WR_R11, 0x003fffff);
		wr_ri(&e, WR_I_MOV, WR_R12, (ui5r)RAM);
	} else if (block->direct_guest_reg != 0xff) {
		wr_ri(&e, WR_I_MOV, WR_R12, (ui5r)block->direct_host_addr);
	}
	wr_reload_guest_cache(&e);

	/* A recorded trace can contain several trips around a small loop.  Use
	 * the last matching backedge so the steady path executes only one copy
	 * of the loop body, keeping the guest PC, cycle budget, and native frame
	 * live.  ROM regions are immutable.  A RAM region is also safe for the
	 * duration of one scheduler slice when every operation is register/control
	 * only: it cannot modify its code or reach MMIO, and the dispatcher
	 * revalidates it before the next entry. */
	if (block->validate != 0) {
		for (i = 0; i < block->count; ++i) {
			if (wr_jit_op_may_exit(&block->op[i])) {
				region_safe = falseblnr;
				break;
			}
		}
	}
	if (block->count != 0 && region_safe) {
		ui3p successor = block->op[block->count - 1].next;
		for (i = block->count; i != 0; ) {
			--i;
			if (block->op[i].at == successor) {
				loop_index = i;
				break;
			}
		}
		if (loop_index < block->count) {
			for (i = loop_index; i < block->count; ++i)
				loop_cycles += block->op[i].cycles;
		}
	}
	can_loop = loop_index < block->count
		&& base >= WR_JIT_FAST_BASE
		&& base < WR_JIT_FAST_BASE + WR_JIT_FAST_BYTES;
	for (i = 0; i < block->count; ++i) {
		op_offset[i] = e.off;
		pending_cycles += block->op[i].cycles;
		if (wr_jit_op_needs_cycle_commit(&block->op[i])
			&& !wr_jit_op_direct_rdw(&e, &block->op[i])) {
			wr_addi(&e, WR_R2, pending_cycles, trueblnr);
			pending_cycles = 0;
		}
		wr_emit_op(&e, &block->op[i]);
	}
	if (pending_cycles != 0)
		wr_addi(&e, WR_R2, pending_cycles, trueblnr);

	/* The recorded path has guards at every operation which can choose a
	 * different PC. On that path the final PC is therefore a single known
	 * successor. Promoted IVRAM traces may tail directly into another promoted
	 * trace's post-prologue entry. SDRAM traces omit this machinery entirely:
	 * their extra row traffic costs more than the dispatcher trip it saves.
	 * Slot identity, IVRAM residency, and cycle admission are rechecked every
	 * time, so collision, cache rebuild, and end-of-slice cases retain the
	 * ordinary dispatcher path. */
	/* A native backedge is the smallest useful region: unlike trace chaining
	 * it performs no cache-slot loads.  It is enabled only after IVRAM
	 * promotion; the cycle guard is its only mandatory recorded-path exit. */
	if (can_loop) {
		ui5r no_budget;
		wr_ri(&e, WR_I_CMP, WR_R2, loop_cycles);
		no_budget = wr_forward(&e, WR_B_ULT);
		wr_jump(&e, (ui5r)base + op_offset[loop_index]);
		wr_land(&e, no_budget);
	}

	can_chain = !can_loop
		&& MINIVMAC_JIT_CHAIN != 0
		&& base >= WR_JIT_FAST_BASE
		&& base < WR_JIT_FAST_BASE + WR_JIT_FAST_BYTES
		&& block->count != 0
		&& block->op[block->count - 1].next >= ROM
		&& block->op[block->count - 1].next < ROM + kROM_Size;
	if (can_chain) {
		struct wr_jit_block *next = wr_jit_slot(
			block->op[block->count - 1].next);
		wr_ri(&e, WR_I_MOV, WR_R7, (ui5r)next);
		wr_ind(&e, WR_O_LDW, WR_R4, WR_R7,
			(ui5r)((ui3p)&next->fast_code - (ui3p)next));
		wr_ri(&e, WR_I_CMP, WR_R4, 0);
		wr_note_exit(&e, WR_B_EQ);
		wr_ind(&e, WR_O_LDW, WR_R3, WR_R7,
			(ui5r)((ui3p)&next->start - (ui3p)next));
		wr_rr(&e, WR_O_CMP, WR_R1, WR_R3);
		wr_note_exit(&e, WR_B_NE);
		wr_ind(&e, WR_O_LDUB, WR_R3, WR_R7,
			(ui5r)((ui3p)&next->memory_overlay - (ui3p)next));
		wr_ri(&e, WR_I_CMP, WR_R3, block->memory_overlay);
		wr_note_exit(&e, WR_B_NE);
		wr_ind(&e, WR_O_LDW, WR_R3, WR_R7,
			(ui5r)((ui3p)&next->cycles - (ui3p)next));
		wr_rr(&e, WR_O_CMP, WR_R2, WR_R3);
		wr_note_exit(&e, WR_B_ULT);
		wr_flush_guest_cache(&e);
		wr_addi(&e, WR_R4, WR_JIT_CHAIN_ENTRY_BYTES, falseblnr);
		wr_jp_reg(&e, WR_R4);
	}

	exit_at = e.off;
	wr_flush_guest_cache(&e);
	wr_store_machine(&e);
	wr_w(&e, 0x0243); /* popn %r3 */
	wr_w(&e, 0x0640); /* ret */
	for (i = 0; i < e.nexits; ++i)
		wr_land_far(&e, e.exits[i], exit_at);

	return (e.off + 15) & ~15;
}

LOCALFUNC blnr wr_jit_compile(struct wr_jit_block *block)
{
	ui5r measured;
	ui5r emitted;

	/* The common path reserves the conservative maximum, emits once, then
	 * returns the unused tail directly to the coalescing allocator.  Only a
	 * heavily fragmented arena pays for a dry sizing pass. */
	block->code = wr_jit_code_alloc(WR_JIT_MAX_BLOCK_BYTES);
	if (block->code == nullpr) {
		measured = wr_jit_emit_block(block, wr_jit_code, falseblnr);
		if (measured > WR_JIT_MAX_BLOCK_BYTES)
			return falseblnr;
		block->code = wr_jit_code_alloc(measured);
		if (block->code == nullpr)
			return falseblnr;
	}
	emitted = wr_jit_emit_block(block, block->code, trueblnr);
	if (emitted > WR_JIT_MAX_BLOCK_BYTES) {
		wr_jit_code_free(block->code);
		block->code = nullpr;
		return falseblnr;
	}
	wr_jit_code_trim(block->code, emitted);
	block->code_bytes = emitted;
	return trueblnr;
}

LOCALFUNC blnr wr_jit_promote(struct wr_jit_block *block)
{
	ui5r emitted;

	if (block->code_bytes + WR_JIT_CHAIN_MAX_BYTES > WR_JIT_FAST_BYTES)
		return falseblnr;
	if (wr_jit_fast_used + block->code_bytes + WR_JIT_CHAIN_MAX_BYTES
		> WR_JIT_FAST_BYTES)
		return falseblnr;
	block->fast_code = WR_JIT_FAST_BASE + wr_jit_fast_used;
	emitted = wr_jit_emit_block(block, block->fast_code, trueblnr);
	if (emitted > block->code_bytes + WR_JIT_CHAIN_MAX_BYTES) {
		block->fast_code = nullpr;
		return falseblnr;
	}
	wr_jit_fast_used += emitted;
	WR_JIT_PROFILE_INC(WR_JIT_PROFILE_PROMOTIONS);
	return trueblnr;
}

/* Keep adapting the small IVRAM cache when a displaced trace remains hot
 * long after the current generation filled it. */
LOCALPROC wr_jit_rebuild_fast(void)
{
	ui5r i;
	WR_JIT_PROFILE_INC(WR_JIT_PROFILE_REBUILDS);
	for (i = 0; i < WR_JIT_SLOTS; ++i) {
		wr_jit_blocks[i].fast_code = nullpr;
		if (wr_jit_blocks[i].hits >= WR_JIT_PROMOTE_HITS)
			wr_jit_blocks[i].hits = WR_JIT_PROMOTE_HITS - 1;
	}
	wr_jit_fast_used = 0;
	wr_jit_fast_rebuilt = trueblnr;
}

LOCALPROC wr_jit_touch_fast(struct wr_jit_block *block)
{
	if (block->hits == WR_JIT_PROMOTE_HITS) {
		(void)wr_jit_promote(block);
	} else if (wr_jit_fast_used + block->code_bytes
			+ WR_JIT_CHAIN_MAX_BYTES > WR_JIT_FAST_BYTES)
	{
		wr_jit_rebuild_fast();
		(void)wr_jit_promote(block);
	}
}

/* Find a repeated, invariant word-read address. The specialization records
 * both the guest register value and the resolved host pointer; wr_jit_valid
 * rechecks them before every dispatch, while a native region can reuse the
 * host pointer across all of its internal backedges. */
LOCALPROC wr_jit_find_direct_rdw(struct wr_jit_block *block)
{
	ui3b uses[16] = {0};
	ui3b dirty[16] = {0};
	ui3b disp_state[16] = {0};
	ui5r displacement[16];
	ui3r i;
	ui3r best = 1;
	ui3r best_reg = 0xff;

	block->direct_guest_reg = 0xff;
	for (i = 0; i < block->count; ++i) {
		const struct wr_jit_op *op = &block->op[i];
		switch (op->kind) {
		case wr_jit_interpret:
		case wr_jit_movem_pre_l:
		case wr_jit_movem_post_l:
		case wr_jit_movem_store_l:
			return;
		case wr_jit_dbf:
		case wr_jit_dbcc:
		case wr_jit_moveq:
		case wr_jit_add_rr_l:
		case wr_jit_sub_rr_l:
		case wr_jit_addq_a:
		case wr_jit_subq_a:
		case wr_jit_addq_w:
		case wr_jit_subq_w:
		case wr_jit_add_rr_w:
		case wr_jit_sub_rr_w:
		case wr_jit_addq_b:
		case wr_jit_subq_b:
		case wr_jit_add_rr_b:
		case wr_jit_sub_rr_b:
		case wr_jit_adda:
		case wr_jit_suba:
		case wr_jit_logic_rr:
		case wr_jit_not_r:
		case wr_jit_asr_w:
			dirty[op->reg] = 1;
			break;
		case wr_jit_move_rr_b:
		case wr_jit_move_rr_w:
		case wr_jit_move_rr_l:
			{
			ui4r reg_mode = op->kind == wr_jit_move_rr_b
				? kAMdRegB : (op->kind == wr_jit_move_rr_w
					? kAMdRegW : kAMdRegL);
			ui5r disp = op->y.v[0].AMd == kAMdADispW
				? (ui5r)(si5r)(si4b)op->extension : 0;
			if (op->y.v[1].AMd != reg_mode)
				return;
			dirty[op->reg] = 1;
			if (op->y.v[0].AMd == kAMdAPosIncB
				|| op->y.v[0].AMd == kAMdAPosInc7B
				|| op->y.v[0].AMd == kAMdAPosIncW
				|| op->y.v[0].AMd == kAMdAPosIncL
				|| op->y.v[0].AMd == kAMdAPreDecB
				|| op->y.v[0].AMd == kAMdAPreDec7B
				|| op->y.v[0].AMd == kAMdAPreDecW
				|| op->y.v[0].AMd == kAMdAPreDecL)
				dirty[op->src_reg] = 1;
			if (op->kind == wr_jit_move_rr_w
				&& (op->y.v[0].AMd == kAMdIndirectW
					|| op->y.v[0].AMd == kAMdADispW)) {
				if (disp_state[op->src_reg] == 0) {
					displacement[op->src_reg] = disp;
					disp_state[op->src_reg] = 1;
				} else if (displacement[op->src_reg] != disp) {
					disp_state[op->src_reg] = 2;
				}
				++uses[op->src_reg];
			}
			}
			break;
		case wr_jit_movea_w:
		case wr_jit_movea_l:
			dirty[op->reg] = 1;
			if (op->y.v[0].AMd == kAMdAPosIncW
				|| op->y.v[0].AMd == kAMdAPosIncL
				|| op->y.v[0].AMd == kAMdAPreDecW
				|| op->y.v[0].AMd == kAMdAPreDecL)
				dirty[op->src_reg] = 1;
			if (op->kind == wr_jit_movea_w
				&& (op->y.v[0].AMd == kAMdIndirectW
					|| op->y.v[0].AMd == kAMdADispW)) {
				ui5r disp = op->y.v[0].AMd == kAMdADispW
					? (ui5r)(si5r)(si4b)op->extension : 0;
				if (disp_state[op->src_reg] == 0) {
					displacement[op->src_reg] = disp;
					disp_state[op->src_reg] = 1;
				} else if (displacement[op->src_reg] != disp) {
					disp_state[op->src_reg] = 2;
				}
				++uses[op->src_reg];
			}
			break;
		case wr_jit_clr_b:
		case wr_jit_clr_w:
		case wr_jit_clr_l:
			if (op->y.v[1].AMd != kAMdRegB
				&& op->y.v[1].AMd != kAMdRegW
				&& op->y.v[1].AMd != kAMdRegL)
				return;
			dirty[op->reg] = 1;
			break;
		default:
			break;
		}
	}
	for (i = 0; i < 16; ++i) {
		if (!dirty[i] && disp_state[i] == 1 && uses[i] > best) {
			best = uses[i];
			best_reg = i;
		}
	}
	if (best_reg != 0xff) {
		ui5r value = V_regs.regs[best_reg];
		ui5r addr = value + displacement[best_reg];
		if ((addr & V_regs.MATCrdW.cmpmask)
			== V_regs.MATCrdW.cmpvalu)
		{
			block->direct_reg_value = value;
			block->direct_guest_addr = addr;
			block->direct_host_addr = (addr & V_regs.MATCrdW.usemask)
				+ V_regs.MATCrdW.usebase;
			block->direct_guest_reg = best_reg;
		}
	}
}

LOCALPROC wr_jit_build(struct wr_jit_block *block)
{
	ui3r i;
	ui3r known_flags = 0xff;
	ui5r slot = (ui5r)(block - wr_jit_blocks);
	WR_JIT_PROFILE_INC(WR_JIT_PROFILE_BUILDS);
	if (block->start == nullpr) {
		++wr_jit_active_slots;
	} else if (block->start != V_pc_p) {
		WR_JIT_PROFILE_INC(WR_JIT_PROFILE_EVICTIONS);
	}
	if (block->code != nullpr)
		wr_jit_code_free(block->code);
	wr_jit_replace[slot / WR_JIT_WAYS]
		= (ui3b)((slot + 1) & (WR_JIT_WAYS - 1));
	block->start = V_pc_p;
	block->code = nullpr;
	block->code_bytes = 0;
	block->fast_code = nullpr;
	block->hits = 0;
	block->profile_runs = 0;
	block->cycles = 0;
	block->count = 0;
	block->fallback_count = 0;
	block->direct_guest_reg = 0xff;
	block->guest_start = V_regs.pc + (V_pc_p - V_regs.pc_pLo);
	block->memory_overlay = MemOverlay;
	block->validate = 0;
	for (i = 0; i < WR_JIT_OPS; ++i) {
		blnr preserves_flags = falseblnr;
		wr_jit_record_one(&block->op[i]);
		if (block->op[i].kind == wr_jit_bcc_pending) {
			preserves_flags = trueblnr;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CC) != 0
				|| known_flags == kLazyFlagsTstL
				|| (block->op[i].src_reg & 15) == 6
				|| (block->op[i].src_reg & 15) == 7) {
				block->op[i].kind = wr_jit_bcc;
				/* Bcc has at most one instruction extension, leaving
				 * extension2 available for the proven incoming flag form. */
				block->op[i].extension2 = known_flags;
			} else {
				block->op[i].kind = wr_jit_interpret;
			}
		}
		switch (block->op[i].kind) {
		case wr_jit_moveq:
		case wr_jit_move_rr_b:
		case wr_jit_move_rr_w:
		case wr_jit_move_rr_l:
		case wr_jit_clr_b:
		case wr_jit_clr_w:
		case wr_jit_clr_l:
		case wr_jit_tst_r_l:
		case wr_jit_logic_rr:
		case wr_jit_not_r:
			known_flags = kLazyFlagsTstL;
			break;
		case wr_jit_bra:
		case wr_jit_dbf:
		case wr_jit_nop:
		case wr_jit_movea_w:
		case wr_jit_movea_l:
		case wr_jit_addq_a:
		case wr_jit_subq_a:
		case wr_jit_adda:
		case wr_jit_suba:
		case wr_jit_bcc:
		case wr_jit_movem_pre_l:
		case wr_jit_movem_post_l:
		case wr_jit_movem_store_l:
		case wr_jit_jsr:
		case wr_jit_jmp:
		case wr_jit_rts:
		case wr_jit_link:
		case wr_jit_unlk:
			break;
		default:
			if (!preserves_flags)
				known_flags = 0xff;
			break;
		}
		block->count = i + 1;
		WR_JIT_PROFILE_INC(WR_JIT_PROFILE_GUEST_OPS_BUILT);
		if (block->op[i].kind == wr_jit_interpret)
			++block->fallback_count;
		if (block->op[i].kind == wr_jit_interpret)
			WR_JIT_PROFILE_INC(WR_JIT_PROFILE_FALLBACK_OPS_BUILT);
		block->cycles += block->op[i].cycles;
		if (block->op[i].at < ROM || block->op[i].at >= ROM + kROM_Size)
			block->validate = 1;
		if (V_MaxCyclesToGo <= 0)
			break;
	}
	if (MINIVMAC_JIT_DIRECT_RAM == 0)
		wr_jit_find_direct_rdw(block);
	if (block->direct_guest_reg != 0xff)
		WR_JIT_PROFILE_INC(WR_JIT_PROFILE_DIRECT_BUILDS);
}

/* A0 contains the lookup/runtime loop.  All translations have an SDRAM copy;
 * repeatedly executed blocks are re-emitted into the IVRAM promotion cache. */
LOCALPROC MINIVMAC_FAST_M68K MiniVMac_JIT_MaxCycles(void)
{
	while (V_MaxCyclesToGo > 0) {
		struct wr_jit_block *block = wr_jit_slot(V_pc_p);

		if (block->start != V_pc_p || !wr_jit_valid(block))
		{
			block = wr_jit_slot(V_pc_p);
			wr_jit_build(block);
			continue;
		}
		/* Recording a trace already executes its guest instructions.  Delay
		 * native-code emission until execution returns to the same trace, so
		 * one-shot startup paths do not pay the relatively large C33 compiler
		 * cost or consume code-cache space. */
		if (block->code == nullpr) {
			if (!wr_jit_compile(block)) {
				wr_jit_flush();
				continue;
			}
		}

		{
			void (*volatile run)(void);
			if ((ui5r)V_MaxCyclesToGo < block->cycles) {
				m68k_go_MaxCycles();
				return;
			}
			if (block->fast_code == nullpr) {
				ui5r hits = ++block->hits;
				if (hits == WR_JIT_PROMOTE_HITS
					|| hits == WR_JIT_REBUILD_HITS)
					wr_jit_touch_fast(block);
			}
#if MINIVMAC_PROFILE
			++block->profile_runs;
#endif
			run = (void (*)(void))(block->fast_code != nullpr
				? block->fast_code : block->code);
			run();
		}
	}
}

#endif
