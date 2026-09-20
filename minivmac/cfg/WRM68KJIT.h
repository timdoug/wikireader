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
#define MINIVMAC_JIT_NATIVE_MASK 0x3df
#endif
#ifndef MINIVMAC_JIT_INLINE_MEMORY_MASK
#define MINIVMAC_JIT_INLINE_MEMORY_MASK (1 | 2 | 32)
#endif
#ifndef MINIVMAC_JIT_CHAIN
#define MINIVMAC_JIT_CHAIN 1
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

enum {
	WR_JIT_SLOTS = 1024,
	WR_JIT_OPS = 16,
	WR_JIT_CODE_BYTES = 1024 * 1024,
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
	wr_jit_tst_r_l,
	wr_jit_cmp_rr_l,
	wr_jit_add_rr_l,
	wr_jit_sub_rr_l,
	wr_jit_addq_a,
	wr_jit_subq_a,
	wr_jit_addq_w,
	wr_jit_subq_w,
	wr_jit_add_rr_w,
	wr_jit_sub_rr_w,
	wr_jit_logic_rr,
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
	/* Keep direct-map lookup to a single C33 shift. */
	ui3b lookup_padding[36];
};

typedef char wr_jit_block_must_be_512_bytes[
	(sizeof(struct wr_jit_block) == 512) ? 1 : -1];

LOCALVAR struct wr_jit_block wr_jit_blocks[WR_JIT_SLOTS];
LOCALVAR ui3b wr_jit_code[WR_JIT_CODE_BYTES]
	__attribute__((aligned(16)));
LOCALVAR ui5r wr_jit_code_used;
LOCALVAR ui5r wr_jit_fast_used;
LOCALVAR blnr wr_jit_fast_rebuilt;

LOCALINLINEFUNC struct wr_jit_block *wr_jit_slot(ui3p pc)
{
	return &wr_jit_blocks[((ui5r)pc >> 1) & (WR_JIT_SLOTS - 1)];
}

LOCALPROC wr_jit_flush(void)
{
	ui5r i;
	for (i = 0; i < WR_JIT_SLOTS; ++i) {
		wr_jit_blocks[i].start = nullpr;
		wr_jit_blocks[i].code = nullpr;
		wr_jit_blocks[i].fast_code = nullpr;
		wr_jit_blocks[i].hits = 0;
		wr_jit_blocks[i].count = 0;
	}
	wr_jit_code_used = 0;
	wr_jit_fast_used = 0;
	wr_jit_fast_rebuilt = falseblnr;
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
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CC) != 0
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
					&& wr_jit_simple_move_mode(op->y.v[0].AMd, size)
					&& wr_jit_simple_move_mode(op->y.v[1].AMd, size)))) {
				op->kind = size == 1 ? wr_jit_move_rr_b
					: (size == 2 ? wr_jit_move_rr_w : wr_jit_move_rr_l);
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				if (wr_jit_is_adisp(op->y.v[0].AMd)) {
					op->extension = do_get_mem_word(op->at + 2);
					op->check_extension = 1;
				}
				if (wr_jit_is_adisp(op->y.v[1].AMd)) {
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
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = wr_jit_tst_r_l;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindCmpL:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& op->y.v[0].AMd == kAMdRegL
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = wr_jit_cmp_rr_l;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
			}
			break;
		case kIKindAddL:
		case kIKindSubL:
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_REGL) != 0
				&& (op->y.v[0].AMd == kAMdRegL
					|| op->y.v[0].AMd == kAMdDat4)
				&& op->y.v[1].AMd == kAMdRegL) {
				op->kind = main_class == kIKindAddL
					? wr_jit_add_rr_l : wr_jit_sub_rr_l;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
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
					|| op->y.v[0].AMd == kAMdRegW)
				&& op->y.v[1].AMd == kAMdRegW) {
				if (op->y.v[0].AMd == kAMdDat4)
					op->kind = main_class == kIKindAddW
						? wr_jit_addq_w : wr_jit_subq_w;
				else
					op->kind = main_class == kIKindAddW
						? wr_jit_add_rr_w : wr_jit_sub_rr_w;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
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
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_LOGIC) != 0
				&& (op->y.v[0].AMd == kAMdRegB
					|| op->y.v[0].AMd == kAMdRegW
					|| op->y.v[0].AMd == kAMdRegL)
				&& op->y.v[1].AMd == op->y.v[0].AMd) {
				op->kind = wr_jit_logic_rr;
				op->src_reg = op->y.v[0].ArgDat;
				op->reg = op->y.v[1].ArgDat;
				op->extension = main_class == kIKindAndI
					|| main_class == kIKindAndEaD
					|| main_class == kIKindAndDEa ? 0
					: (main_class == kIKindOrI
						|| main_class == kIKindOrDEa
						|| main_class == kIKindOrEaD ? 1 : 2);
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
	if (!block->validate)
		return trueblnr;
	for (i = 0; i < block->count; ++i) {
		const struct wr_jit_op *op = &block->op[i];
		if (do_get_mem_word(op->at) != op->opcode)
			return falseblnr;
		if (op->check_extension
			&& do_get_mem_word(op->at + 2) != op->extension)
			return falseblnr;
		if (op->check_extension > 1
			&& do_get_mem_word(op->at + 4) != op->extension2)
			return falseblnr;
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

enum { WR_R0, WR_R1, WR_R2, WR_R3, WR_R4, WR_R5, WR_R6, WR_R7 };
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
};

LOCALINLINEPROC wr_w(struct wr_emit *e, ui4r word)
{
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

LOCALINLINEPROC wr_xcall(struct wr_emit *e, ui5r target)
{
	ui5r self = (ui5r)e->base + e->off + 4;
	ui5r distance = (ui5r)((si5r)(target - self) / 2);
	wr_ext(e, ((distance >> 21) & 0x3ff) << 3);
	wr_ext(e, distance >> 8);
	wr_w(e, (WR_B_CALL << 8) | (distance & 0xff));
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
	e->base[at] = (ui3b)(distance >> 8);
	e->base[at + 1] = (ui3b)(0xc0 | ((distance >> 16) & 0x1f));
	e->base[at + 2] = (ui3b)distance;
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

LOCALINLINEPROC wr_guard_next(struct wr_emit *e, ui3p next)
{
	wr_ri(e, WR_I_MOV, WR_R4, (ui5r)next);
	wr_rr(e, WR_O_CMP, WR_R1, WR_R4);
	wr_note_exit(e, WR_B_NE);
}

/* Inline the same one-entry address-translation cache used by get/put_*.
 * A miss takes the exact core helper, which also refreshes the cache; normal
 * Macintosh RAM traffic then stays entirely inside the translated block. */
LOCALPROC wr_emit_guest_read(struct wr_emit *e, ui3r size, ui3p next)
{
	MATCp matc = size == 1 ? &V_regs.MATCrdB : &V_regs.MATCrdW;
	ui5r miss1;
	ui5r miss2 = 0;
	ui5r done;

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
	wr_guard_next(e, next);
	wr_land(e, done);
}

LOCALPROC wr_emit_guest_write(struct wr_emit *e, ui3r size, ui3p next)
{
	MATCp matc = size == 1 ? &V_regs.MATCwrB : &V_regs.MATCwrW;
	ui5r miss1;
	ui5r miss2 = 0;
	ui5r done;

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
	wr_store_machine(e);
	wr_xcall(e, (ui5r)(size == 1 ? put_byte
		: (size == 2 ? put_word : put_long)));
	wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
		(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
	wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
		(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
	wr_guard_next(e, next);
	wr_land(e, done);
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
	ui5r reg_offset = (ui5r)((ui3p)&V_regs.regs[op->reg]
		- (ui3p)&V_regs);
	int z;

	for (z = 0; z < 16; ++z)
		if ((mask & ((ui5r)1 << z)) != 0)
			++count;
	bytes = count * 4;
	wr_mem(e, WR_O_LDW, WR_R6, WR_R0, reg_offset);
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
		ui5r value_offset;
		if ((mask & bit) == 0)
			continue;
		value_offset = (ui5r)((ui3p)&V_regs.regs[z] - (ui3p)&V_regs);
		if (load) {
			wr_ind(e, WR_O_LDUH, WR_R3, WR_R5, 0);
			wr_rr(e, WR_O_SWAPH, WR_R3, WR_R3);
			wr_shifti(e, WR_S_SLL, WR_R3, 16);
			wr_ind(e, WR_O_LDUH, WR_R4, WR_R5, 2);
			wr_rr(e, WR_O_SWAPH, WR_R4, WR_R4);
			wr_rr(e, WR_O_OR, WR_R3, WR_R4);
			wr_mem(e, WR_O_STW, WR_R3, WR_R0, value_offset);
		} else {
			wr_mem(e, WR_O_LDW, WR_R3, WR_R0, value_offset);
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
		wr_mem(e, WR_O_STW, WR_R6, WR_R0, reg_offset);
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
	case wr_jit_btst_b:
		return op->y.v[1].AMd != kAMdRegB;
	case wr_jit_movem_pre_l:
	case wr_jit_movem_post_l:
	case wr_jit_movem_store_l:
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
		wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
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
		ui5r dst_disp = wr_jit_is_adisp(op->y.v[0].AMd)
			? op->extension2 : op->extension;
		if (op->y.v[0].AMd == kAMdAPosInc7B
			|| op->y.v[0].AMd == kAMdAPreDec7B)
			src_step = 2;
		if (op->y.v[1].AMd == kAMdAPosInc7B
			|| op->y.v[1].AMd == kAMdAPreDec7B)
			dst_step = 2;
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		if (op->y.v[0].AMd == reg_mode) {
			wr_mem(e, size == 1 ? WR_O_LDB
				: (size == 2 ? WR_O_LDH : WR_O_LDW),
				WR_R3, WR_R0, src_reg_offset);
		} else {
			wr_mem(e, WR_O_LDW, WR_R6, WR_R0, src_reg_offset);
			if (op->y.v[0].AMd == post_mode
				|| op->y.v[0].AMd == kAMdAPosInc7B) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, src_step, falseblnr);
				wr_mem(e, WR_O_STW, WR_R4, WR_R0,
					src_reg_offset);
			} else if (op->y.v[0].AMd == pre_mode
				|| op->y.v[0].AMd == kAMdAPreDec7B) {
				wr_addi(e, WR_R6, src_step, trueblnr);
				wr_mem(e, WR_O_STW, WR_R6, WR_R0,
					src_reg_offset);
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
			wr_mem(e, size == 1 ? WR_O_STB
				: (size == 2 ? WR_O_STH : WR_O_STW),
				WR_R3, WR_R0, reg_offset);
		} else {
			wr_mem(e, WR_O_LDW, WR_R6, WR_R0, reg_offset);
			if (op->y.v[1].AMd == post_mode
				|| op->y.v[1].AMd == kAMdAPosInc7B) {
				wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
				wr_addi(e, WR_R4, dst_step, falseblnr);
				wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
			} else if (op->y.v[1].AMd == pre_mode
				|| op->y.v[1].AMd == kAMdAPreDec7B) {
				wr_addi(e, WR_R6, dst_step, trueblnr);
				wr_mem(e, WR_O_STW, WR_R6, WR_R0, reg_offset);
			} else if (wr_jit_is_adisp(op->y.v[1].AMd)) {
				wr_addi(e, WR_R6,
					(ui5r)(si5r)(si4b)dst_disp, falseblnr);
			}
			if ((MINIVMAC_JIT_INLINE_MEMORY_MASK
				& (size == 1 ? 8 : (size == 2 ? 16 : 32))) != 0) {
				wr_emit_guest_write(e, size, op->next);
			} else {
				wr_rr(e, WR_O_MOV, WR_R7, WR_R3);
				wr_store_machine(e);
				wr_xcall(e, (ui5r)(size == 1 ? put_byte
					: (size == 2 ? put_word : put_long)));
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
	case wr_jit_tst_r_l:
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsTstL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_cmp_rr_l:
		wr_mem(e, WR_O_LDW, WR_R3, WR_R0, src_reg_offset);
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0, reg_offset);
		wr_mem(e, WR_O_STW, WR_R3, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgSrc - (ui3p)&V_regs));
		wr_mem(e, WR_O_STW, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagArgDst - (ui3p)&V_regs));
		wr_ri(e, WR_I_MOV, WR_R4, kLazyFlagsCmpL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_add_rr_l:
	case wr_jit_sub_rr_l:
		if (op->y.v[0].AMd == kAMdDat4)
			wr_ri(e, WR_I_MOV, WR_R3, op->src_reg);
		else
			wr_mem(e, WR_O_LDW, WR_R3, WR_R0, src_reg_offset);
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0, reg_offset);
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
		wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
		wr_ri(e, WR_I_MOV, WR_R4,
			op->kind == wr_jit_add_rr_l
				? kLazyFlagsAddL : kLazyFlagsSubL);
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyFlagKind - (ui3p)&V_regs));
		wr_mem(e, WR_O_STB, WR_R4, WR_R0,
			(ui5r)((ui3p)&V_regs.LazyXFlagKind - (ui3p)&V_regs));
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_addq_a:
	case wr_jit_subq_a:
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0, reg_offset);
		wr_addi(e, WR_R4, op->src_reg,
			op->kind == wr_jit_subq_a);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_adda:
	case wr_jit_suba:
		if (op->y.v[0].AMd == kAMdRegW)
			wr_mem(e, WR_O_LDH, WR_R3, WR_R0, src_reg_offset);
		else if (op->y.v[0].AMd == kAMdRegL)
			wr_mem(e, WR_O_LDW, WR_R3, WR_R0, src_reg_offset);
		else if (op->y.v[0].AMd == kAMdImmedW)
			wr_ri(e, WR_I_MOV, WR_R3,
				(ui5r)(si5r)(si4b)op->extension);
		else
			wr_ri(e, WR_I_MOV, WR_R3,
				((ui5r)op->extension << 16) | op->extension2);
		wr_mem(e, WR_O_LDW, WR_R4, WR_R0, reg_offset);
		wr_rr(e, op->kind == wr_jit_adda ? WR_O_ADD : WR_O_SUB,
			WR_R4, WR_R3);
		wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
		wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
		break;
	case wr_jit_logic_rr:
		{
		ui3r size = op->y.v[1].AMd == kAMdRegB ? 1
			: (op->y.v[1].AMd == kAMdRegW ? 2 : 4);
		wr_mem(e, size == 1 ? WR_O_LDB
			: (size == 2 ? WR_O_LDH : WR_O_LDW),
			WR_R3, WR_R0, src_reg_offset);
		wr_mem(e, size == 1 ? WR_O_LDB
			: (size == 2 ? WR_O_LDH : WR_O_LDW),
			WR_R4, WR_R0, reg_offset);
		wr_rr(e, op->extension == 0 ? WR_O_AND
			: (op->extension == 1 ? WR_O_OR : WR_O_XOR),
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
	case wr_jit_addq_w:
	case wr_jit_subq_w:
	case wr_jit_add_rr_w:
	case wr_jit_sub_rr_w:
		wr_mem(e, WR_O_LDH, WR_R4, WR_R0, reg_offset);
		if (op->kind == wr_jit_add_rr_w || op->kind == wr_jit_sub_rr_w)
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
		wr_addi(e, WR_R1, 2, falseblnr);
		break;
	case wr_jit_asr_w:
		if (op->y.v[0].AMd == kAMdDat4)
			wr_ri(e, WR_I_MOV, WR_R6, op->src_reg);
		else
			wr_mem(e, WR_O_LDW, WR_R6, WR_R0, src_reg_offset);
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
			if (op->y.v[0].AMd == kAMdRegB)
				wr_mem(e, WR_O_LDB, WR_R3, WR_R0, src_reg_offset);
			else
				wr_ri(e, WR_I_MOV, WR_R3, op->extension & 0xff);
			wr_addi(e, WR_R1, (ui5r)(op->next - op->at), falseblnr);
			if (dst_mode == kAMdRegB) {
				wr_mem(e, WR_O_LDB, WR_R4, WR_R0, reg_offset);
			} else {
				wr_mem(e, WR_O_LDW, WR_R6, WR_R0, reg_offset);
				if (dst_mode == kAMdAPosIncB
					|| dst_mode == kAMdAPosInc7B) {
					wr_rr(e, WR_O_MOV, WR_R4, WR_R6);
					wr_addi(e, WR_R4,
						dst_mode == kAMdAPosInc7B ? 2 : 1,
						falseblnr);
					wr_mem(e, WR_O_STW, WR_R4, WR_R0, reg_offset);
				} else if (dst_mode == kAMdAPreDecB
					|| dst_mode == kAMdAPreDec7B) {
					wr_addi(e, WR_R6,
						dst_mode == kAMdAPreDec7B ? 2 : 1,
						trueblnr);
					wr_mem(e, WR_O_STW, WR_R6, WR_R0, reg_offset);
				} else if (dst_mode == kAMdADispB) {
					wr_addi(e, WR_R6,
						(ui5r)(si5r)(si4b)displacement, falseblnr);
				}
				wr_store_machine(e);
				wr_xcall(e, (ui5r)get_byte);
				wr_mem(e, WR_O_LDW, WR_R1, WR_R0,
					(ui5r)((ui3p)&V_regs.pc_p - (ui3p)&V_regs));
				wr_mem(e, WR_O_LDW, WR_R2, WR_R0,
					(ui5r)((ui3p)&V_regs.MaxCyclesToGo - (ui3p)&V_regs));
				wr_guard_next(e, op->next);
			}
			wr_rr(e, WR_O_MOV, WR_R6, WR_R3);
			wr_rr(e, WR_O_MOV, WR_R7, WR_R4);
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
				wr_xcall(e, (ui5r)wr_jit_cc_true);
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
			wr_xcall(e, (ui5r)wr_jit_cc_true);
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
	if (wr_jit_op_may_exit(op)) {
		wr_ri(e, WR_I_CMP, WR_R2, 0);
		wr_note_exit(e, WR_B_LE);
	}
}

LOCALFUNC ui5r wr_jit_emit_block(struct wr_jit_block *block, ui3p base)
{
	struct wr_emit e;
	ui3r i;
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
		if (wr_jit_op_needs_cycle_commit(&block->op[i])) {
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
		wr_ind(&e, WR_O_LDW, WR_R3, WR_R7,
			(ui5r)((ui3p)&next->cycles - (ui3p)next));
		wr_rr(&e, WR_O_CMP, WR_R2, WR_R3);
		wr_note_exit(&e, WR_B_ULT);
		wr_addi(&e, WR_R4, WR_JIT_CHAIN_ENTRY_BYTES, falseblnr);
		wr_jp_reg(&e, WR_R4);
	}

	exit_at = e.off;
	wr_store_machine(&e);
	wr_w(&e, 0x0243); /* popn %r3 */
	wr_w(&e, 0x0640); /* ret */
	for (i = 0; i < e.nexits; ++i)
		wr_land_far(&e, e.exits[i], exit_at);

	return (e.off + 15) & ~15;
}

LOCALPROC wr_jit_compile(struct wr_jit_block *block)
{
	block->code = wr_jit_code + wr_jit_code_used;
	block->code_bytes = wr_jit_emit_block(block, block->code);
	wr_jit_code_used += block->code_bytes;
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
	emitted = wr_jit_emit_block(block, block->fast_code);
	if (emitted > block->code_bytes + WR_JIT_CHAIN_MAX_BYTES) {
		block->fast_code = nullpr;
		return falseblnr;
	}
	wr_jit_fast_used += emitted;
	return trueblnr;
}

/* Keep adapting the small IVRAM cache when a displaced trace remains hot
 * long after the current generation filled it. */
LOCALPROC wr_jit_rebuild_fast(void)
{
	ui5r i;
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

LOCALPROC wr_jit_build(struct wr_jit_block *block)
{
	ui3r i;
	ui3r known_flags = 0xff;
	block->start = V_pc_p;
	block->code = nullpr;
	block->fast_code = nullpr;
	block->hits = 0;
	block->cycles = 0;
	block->count = 0;
	block->validate = 0;
	for (i = 0; i < WR_JIT_OPS; ++i) {
		blnr preserves_flags = falseblnr;
		wr_jit_record_one(&block->op[i]);
		if (block->op[i].kind == wr_jit_bcc_pending) {
			preserves_flags = trueblnr;
			if ((MINIVMAC_JIT_NATIVE_MASK & WR_JIT_NATIVE_CC) != 0
				|| known_flags == kLazyFlagsTstL) {
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
		case wr_jit_tst_r_l:
		case wr_jit_logic_rr:
			known_flags = kLazyFlagsTstL;
			break;
		case wr_jit_bra:
		case wr_jit_dbf:
		case wr_jit_nop:
		case wr_jit_addq_a:
		case wr_jit_subq_a:
		case wr_jit_adda:
		case wr_jit_suba:
		case wr_jit_bcc:
		case wr_jit_movem_pre_l:
		case wr_jit_movem_post_l:
		case wr_jit_movem_store_l:
			break;
		default:
			if (!preserves_flags)
				known_flags = 0xff;
			break;
		}
		block->count = i + 1;
		block->cycles += block->op[i].cycles;
		if (block->op[i].at < ROM || block->op[i].at >= ROM + kROM_Size)
			block->validate = 1;
		if (V_MaxCyclesToGo <= 0)
			break;
	}
	wr_jit_compile(block);
}

/* A0 contains the lookup/runtime loop.  All translations have an SDRAM copy;
 * repeatedly executed blocks are re-emitted into the IVRAM promotion cache. */
LOCALPROC MINIVMAC_FAST_M68K MiniVMac_JIT_MaxCycles(void)
{
	while (V_MaxCyclesToGo > 0) {
		struct wr_jit_block *block = wr_jit_slot(V_pc_p);

		if (block->start != V_pc_p || block->code == nullpr
			|| !wr_jit_valid(block))
		{
			if (wr_jit_code_used + WR_JIT_MAX_BLOCK_BYTES
				> WR_JIT_CODE_BYTES)
				wr_jit_flush();
			block = wr_jit_slot(V_pc_p);
			wr_jit_build(block);
			continue;
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
			run = (void (*)(void))(block->fast_code != nullpr
				? block->fast_code : block->code);
			run();
		}
	}
}

#endif
