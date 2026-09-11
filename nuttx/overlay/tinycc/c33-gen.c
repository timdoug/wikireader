/* Epson C33 PE code generator.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * The allocator uses caller-saved r4-r9. r0 and r1 are private scratch, r3
 * is the frame pointer and r2 a copy of it biased for reaching locals. All
 * four are preserved; r15 (GCC's data pointer) is untouched. Instruction
 * encodings follow the C33 PE core manual.
 */
#ifdef TARGET_DEFS_ONLY
#define NB_REGS 8
#define RC_INT 1
#define RC_FLOAT 2
#define RC_R(n) (4 << (n))
#define RC_IRET RC_R(0)
#define RC_IRE2 RC_R(1)
#define RC_FRET RC_R(6)
#define REG_IRET 0
#define REG_IRE2 1
#define REG_FRET 6
#define PTR_SIZE 4
#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 4
#define MAX_ALIGN 4
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#else
#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char * const target_machine_defs =
    "__c33__\0__c33\0__NuttX__\0";
ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_R(0), RC_INT | RC_R(1), RC_INT | RC_R(2),
    RC_INT | RC_R(3), RC_INT | RC_R(4), RC_INT | RC_R(5),
    RC_FLOAT | RC_R(6), RC_FLOAT | RC_R(7)
};
static int c33_prolog;
/* How far below the frame pointer r2 points. Locals within this distance
 * reach their slot with a single 13-bit ext prefix, which is the whole
 * point of keeping the biased copy; deeper frames still work, more slowly.
 * The value is also the reserved prolog's business: see gfunc_epilog. */
#define C33_LOCBIAS 8192
/* Bytes the patched prolog writes back over gen_fill_nops(). */
#define C33_PROLOG_SIZE 18

ST_FUNC void o(unsigned c)
{
    if (nocode_wanted)
        return;
    if (ind + 2 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind + 2);
    write16le(cur_text_section->data + ind, c);
    ind += 2;
}
static void c33_rr(unsigned op, int d, int s) { o(op | s << 4 | d); }
static void c33_mov(int d, int s) { if (d != s) c33_rr(0x2e00, d, s); }
static void c33_imm(int d, uint32_t v)
{
    if ((int32_t)v < -32 || (int32_t)v > 31) {
        o(0xc000 | v >> 19);
        o(0xc000 | ((v >> 6) & 8191));
    }
    o(0x6c00 | (v & 63) << 4 | d);
}
static void c33_sym(int d, Sym *sym, int addend)
{
    greloca(cur_text_section, sym, ind, R_C33_H, addend);
    o(0xc000);
    greloca(cur_text_section, sym, ind, R_C33_M, addend);
    o(0xc000);
    greloca(cur_text_section, sym, ind, R_C33_L, addend);
    o(0x6c00 | d);
}
/* Form an address value. Memory accesses below separately preserve flags
 * during address formation for long long carry/borrow chains. */
static void c33_offset(int d, int base, int off)
{
    c33_imm(d, off);
    c33_rr(0x2200, d, base);
}
/* Add any 32-bit immediate, which always takes two prefixes. */
static void c33_add(int d, int v)
{
    o(0xc000 | ((uint32_t)v >> 19));
    o(0xc000 | (((uint32_t)v >> 6) & 8191));
    o(0x6000 | (v & 63) << 4 | d);
}
/* One prefix carries 19 more bits. add and sub read their immediate
 * unsigned, so a negative adjustment becomes the opposite mnemonic, the way
 * the assembler writes it; the ld.w immediate is sign extended instead. */
#define C33_IMM19_MAX (1 << 18)
static void c33_addsub19(int d, int v)
{
    unsigned m = v < 0 ? -(unsigned)v : (unsigned)v;
    if (m >= C33_IMM19_MAX)
        tcc_error("C33: stack frame is too large");
    o(0xc000 | (m >> 6));
    o((v < 0 ? 0x6400 : 0x6000) | (m & 63) << 4 | d);
}
static void c33_imm19(int d, unsigned v)
{
    if (v >= C33_IMM19_MAX)
        tcc_error("C33: stack frame is too large");
    o(0xc000 | (v >> 6));
    o(0x6c00 | (v & 63) << 4 | d);
}
/* The ext-prefixed [%rN+disp] displacement is unsigned, so an access below
 * the frame pointer would otherwise have to compute its address in a
 * register first. r2 holds the frame pointer biased down by C33_LOCBIAS,
 * which turns every local within that distance into a one-prefix
 * displacement that also leaves the flags alone. This puts r2 back after
 * something borrows it as scratch. */
static void c33_locbase(void)
{
    c33_mov(2, 3);
    c33_add(2, -C33_LOCBIAS);
}
static void c33_mem(unsigned op, int r, int base, int off)
{
    int borrowed = 0;
    if (base == 3 && off < 0 && off >= -C33_LOCBIAS) {
        base = 2;
        off += C33_LOCBIAS;
    }
    if (off < 0) {
        /* A frame deeper than the bias still has to form the address in a
         * register. Preserve the flags across it (64-bit carry chains); the
         * access itself leaves them alone, so r2 is rebuilt before psr is
         * put back. */
        c33_rr(0xa400, 0, 0); /* r0 = psr */
        c33_mov(2, base);
        c33_add(2, off);
        base = 2;
        off = 0;
        borrowed = 1;
    }
    if (off) {
        if ((unsigned)off >= (1u << 26))
            tcc_error("C33: memory displacement too large");
        if (off >= 8192) o(0xc000 | ((unsigned)off >> 13));
        o(0xc000 | (off & 8191));
    }
    c33_rr(op, r, base);
    if (borrowed) {
        c33_locbase();
        c33_rr(0xa000, 0, 0); /* psr = r0 */
    }
}
static unsigned c33_memop(int t, int wr)
{
    switch (t & VT_BTYPE) {
    case VT_BYTE: return wr ? 0x3400 : (t & VT_UNSIGNED) ? 0x2400 : 0x2000;
    case VT_BOOL: return wr ? 0x3400 : 0x2400;
    case VT_SHORT: return wr ? 0x3800 : (t & VT_UNSIGNED) ? 0x2c00 : 0x2800;
    default: return wr ? 0x3c00 : 0x3000;
    }
}
static void c33_check_type(int t)
{
    if ((t & VT_BTYPE) == VT_QFLOAT)
        tcc_error("C33: 128-bit floating point is unsupported");
    if (t & VT_TLS)
        tcc_error("C33: thread-local storage is not supported");
}
ST_FUNC void load(int r, SValue *sv)
{
    int v = sv->r & VT_VALMASK, d = r + 4, base, off = sv->c.i;
    int fp = is_float(sv->type.t), wide = fp && (sv->type.t & VT_BTYPE) != VT_FLOAT;
    if (fp) d = 10 + (r - 6) * 2;
    c33_check_type(sv->type.t);
    if (sv->r & VT_LVAL) {
        if (v == VT_LLOCAL) {
            c33_mem(0x3000, 1, 3, off);
            base = 1; off = 0;
        } else if (v == VT_LOCAL) base = 3;
        else if (v == VT_CONST) {
            if (sv->r & VT_SYM) c33_sym(1, sv->sym, off);
            else c33_imm(1, off);
            base = 1; off = 0;
        } else { base = v + 4; off = 0; }
        c33_mem(c33_memop(sv->type.t, 0), d, base, off);
        if (wide) c33_mem(0x3000, d + 1, base, off + 4);
    } else if (v == VT_CONST) {
        if (sv->r & VT_SYM) c33_sym(d, sv->sym, off);
        else c33_imm(d, off);
        if (wide) c33_imm(d + 1, (uint64_t)sv->c.i >> 32);
    } else if (v == VT_LOCAL) c33_offset(d, 3, off);
    else if (v == VT_LLOCAL) c33_mem(0x3000, d, 3, off);
    else if (v == VT_CMP) {
        int j = gjmp_cond(sv->cmp_op, 0);
        c33_imm(d, 0);
        o(0x1e02); /* skip the next 2-byte instruction */
        gsym(j);
        c33_imm(d, 1);
    } else if (v == VT_JMP || v == VT_JMPI) {
        c33_imm(d, v == VT_JMPI);
        o(0x1e02);
        gsym(off);
        c33_imm(d, v == VT_JMP);
    } else if (fp) {
        c33_mov(d, 10 + (v - 6) * 2);
        if (wide) c33_mov(d + 1, 11 + (v - 6) * 2);
    } else c33_mov(d, v + 4);
}
ST_FUNC void store(int r, SValue *sv)
{
    int v = sv->r & VT_VALMASK, off = sv->c.i, base;
    int fp = is_float(sv->type.t), d = fp ? 10 + (r - 6) * 2 : r + 4;
    c33_check_type(sv->type.t);
    if (v == VT_LOCAL) base = 3;
    else if (v == VT_LLOCAL) {
        c33_mem(0x3000, 1, 3, off);
        base = 1; off = 0;
    }
    else if (v == VT_CONST) {
        if (sv->r & VT_SYM) c33_sym(1, sv->sym, off);
        else c33_imm(1, off);
        base = 1; off = 0;
    } else { base = v + 4; off = 0; }
    c33_mem(c33_memop(sv->type.t, 1), d, base, off);
    if (fp && (sv->type.t & VT_BTYPE) != VT_FLOAT)
        c33_mem(0x3c00, d + 1, base, off + 4);
}

/* A branch is one ext prefix and the instruction, and the instruction PC is
 * the second halfword. Until the target is known those two halfwords carry
 * the next link of the patch chain in the very field the displacement will
 * occupy, so both a chain offset and a branch distance are 21 bits: 2 MiB of
 * text, and a reach of 2 MiB in either direction. */
#define C33_BRANCH_BITS 21
static void c33_write_branch(int at, uint32_t value)
{
    unsigned char *p = cur_text_section->data + at;
    write16le(p, 0xc000 | ((value >> 8) & 8191));
    write16le(p + 2, (read16le(p + 2) & 0xff00) | (value & 255));
}
static int c33_read_branch(int at)
{
    unsigned char *p = cur_text_section->data + at;
    return ((read16le(p) & 8191) << 8) | (read16le(p + 2) & 255);
}
static void c33_patch_branch(int at, int dest)
{
    int disp = dest - (at + 2);
    if (disp < -(1 << C33_BRANCH_BITS) || disp >= (1 << C33_BRANCH_BITS))
        tcc_error("C33: branch target is more than 2 MB away");
    c33_write_branch(at, (uint32_t)disp >> 1);
}
ST_FUNC void gsym_addr(int t, int a)
{
    while (t) {
        int next = c33_read_branch(t);
        c33_patch_branch(t, a);
        t = next;
    }
}
ST_FUNC int gjmp_append(int n, int t)
{
    int p = n, next;
    if (!n) return t;
    while ((next = c33_read_branch(p))) p = next;
    c33_write_branch(p, t);
    return n;
}
static int c33_branch(unsigned op, int t)
{
    int at = ind;
    if (nocode_wanted) return t;
    if (ind >= (1 << C33_BRANCH_BITS))
        tcc_error("C33: text section is too large for branch chains");
    o(0xc000); o(op);
    c33_write_branch(at, (uint32_t)t);
    return at;
}
ST_FUNC int gjmp(int t) { return c33_branch(0x1e00, t); }
ST_FUNC void gjmp_addr(int a)
{
    int at;
    if (nocode_wanted) return;
    /* A backward jump already knows its target, so it can skip the prefix
     * when the loop it closes is short enough. */
    if (a - ind >= -256 && a - ind <= 254) {
        o(0x1e00 | (((uint32_t)(a - ind) >> 1) & 255));
        return;
    }
    at = gjmp(0);
    c33_patch_branch(at, a);
}
ST_FUNC int gjmp_cond(int op, int t)
{
    unsigned ins;
    switch (op) {
    case TOK_EQ: ins = 0x1800; break;
    case TOK_NE: ins = 0x1a00; break;
    case TOK_LT: ins = 0x0c00; break;
    case TOK_LE: ins = 0x0e00; break;
    case TOK_GT: ins = 0x0800; break;
    case TOK_GE: ins = 0x0a00; break;
    case TOK_ULT: ins = 0x1400; break;
    case TOK_ULE: ins = 0x1600; break;
    case TOK_UGT: ins = 0x1000; break;
    case TOK_UGE: ins = 0x1200; break;
    default: tcc_error("C33: unsupported condition %d", op);
    }
    return c33_branch(ins, t);
}
ST_FUNC void gen_fill_nops(int bytes) { while (bytes > 0) o(0), bytes -= 2; }

static void c33_sp(int bytes)
{
    while (bytes) {
        int n = bytes > 0 ? bytes : -bytes;
        if (n > 4080) n = 4080;
        o((bytes > 0 ? 0x8000 : 0x8400) | (n >> 2));
        bytes += bytes > 0 ? -n : n;
    }
}
/* GCC uses registers for scalar-mode aggregates of 1,2,4,8 bytes. */
static int c33_argsize(CType *t)
{
    int align;
    /* The frontend can retain the array/function type on an argument whose
     * value has already decayed to an address, especially in varargs. */
    if ((t->t & VT_ARRAY) || (t->t & VT_BTYPE) == VT_FUNC) return PTR_SIZE;
    return type_size(t, &align);
}
static int c33_argreg(CType *t, int *reg, int named)
{
    int a, size = c33_argsize(t), bt = t->t & VT_BTYPE;
    c33_check_type(t->t);
    if (!named || *reg >= 4 || ((bt == VT_DOUBLE || bt == VT_LDOUBLE) && *reg >= 3) || (bt == VT_STRUCT &&
        size != 1 && size != 2 && size != 4 && size != 8)) return -1;
    a = *reg;
    *reg += (size + 3) / 4;
    return a + 6;
}
ST_FUNC int gfunc_sret(CType *t, int variadic, CType *ret, int *align, int *regsize)
{
    int size = type_size(t, align);
    *regsize = 4;
    if (size == 1 || size == 2 || size == 4 || size == 8) {
        ret->t = size == 8 ? VT_LLONG : VT_INT;
        return 1;
    }
    return 0;
}
/* True when every argument can go straight into its ABI register.
 *
 * The general path below first parks the arguments in an outgoing scratch
 * area, because loading one argument register can otherwise overwrite a
 * value a later argument still has to read. That cannot happen when each
 * argument is a word-sized scalar sitting in memory: the loads only read
 * through the frame registers and only write r4, r1 and their own r6-r9,
 * so the scratch area and its stack adjustment can both be skipped. */
static int c33_arg_in_memory(SValue *sv)
{
    int v = sv->r & VT_VALMASK;
    return v == VT_CONST || v == VT_LLOCAL || v == VT_LOCAL;
}
static int c33_direct_args(SValue *fn, int nb_args, const int *where,
                           const int *stackat)
{
    int i;
    if ((fn->type.ref->type.t & VT_BTYPE) == VT_STRUCT ||
        fn->type.ref->f.func_type == FUNC_ELLIPSIS || !c33_arg_in_memory(fn))
        return 0;
    for (i = 0; i < nb_args; i++) {
        int t = fn[i + 1].type.t, bt = t & VT_BTYPE;
        if (where[i] < 0 || stackat[i] >= 0 || !c33_arg_in_memory(&fn[i + 1]))
            return 0;
        if (bt == VT_STRUCT || bt == VT_LLONG || bt == VT_FUNC ||
            is_float(t) || (t & VT_ARRAY))
            return 0;
    }
    return 1;
}
ST_FUNC void gfunc_call(int nb_args)
{
    SValue *fn = vtop - nb_args;
    Sym *formal = fn->type.ref->next;
    int i, j, reg = 0, stack = 0, scratch, size, callsize;
    int *where = tcc_malloc(2 * (nb_args + 1) * sizeof(int));
    int *stackat = where + nb_args + 1;
    stk_push(&where);
    for (i = 0; i < nb_args; i++) {
        int named = formal != NULL || fn->type.ref->f.func_type == FUNC_OLD;
        /* Hidden structure-result pointer precedes the declared parameters. */
        int hidden = i == 0 && (fn->type.ref->type.t & VT_BTYPE) == VT_STRUCT;
        if (hidden) {
            CType ret; int ra, rs;
            hidden = !gfunc_sret(&fn->type.ref->type, 0, &ret, &ra, &rs);
            named |= hidden;
        }
        where[i] = c33_argreg(&fn[i + 1].type, &reg, named);
        stackat[i] = -1;
        if (where[i] < 0 || (fn->type.ref->f.func_type == FUNC_ELLIPSIS &&
                            (!formal || !formal->next) && !hidden)) {
            stackat[i] = stack;
            size = c33_argsize(&fn[i + 1].type);
            stack += (size + 3) & -4;
        }
        if (formal && !hidden) formal = formal->next;
    }
    /* Spill all live values before loading ABI registers. First collect the
     * arguments in a temporary stack area, then load argument registers.
     * This also handles overlapping registers and indirect function calls. */
    save_regs(0);
    if (c33_direct_args(fn, nb_args, where, stackat)) {
        load(0, fn);
        c33_mov(0, 4);
        for (i = 0; i < nb_args; i++)
            load(where[i] - 4, &fn[i + 1]);
        /* No GCC __builtin_apply forwarding descriptor is claimed. */
        c33_imm(5, 0);
        o(0x0600); /* call r0 */
        if (is_float(fn->type.ref->type.t)) {
            c33_mov(10, 4);
            c33_mov(11, 5);
        }
        stk_pop();
        tcc_free(where);
        vtop = fn - 1;
        return;
    }
    scratch = stack;
    for (i = 0; i < nb_args; i++) {
        size = c33_argsize(&fn[i + 1].type);
        scratch += (size + 3) & -4;
    }
    callsize = (scratch + 15) & -16;
    c33_sp(-callsize);
    scratch = stack;
    for (i = 0; i < nb_args; i++) {
        SValue sv = fn[i + 1];
        int dst = scratch;
        size = c33_argsize(&sv.type);
        if ((sv.type.t & VT_BTYPE) == VT_STRUCT) {
            sv.r &= ~VT_LVAL;
            load(0, &sv);
            for (j = 0; j < size; j++) {
                c33_mem(0x2400, 5, 4, j);
                c33_rr(0xa400, 1, 1); /* r1 = sp */
                c33_mem(0x3400, 5, 1, dst + j);
            }
        } else {
            if (is_float(sv.type.t)) {
                load(REG_FRET, &sv);
                c33_mov(4, 10);
                if (size == 8) {
                    c33_rr(0xa400, 1, 1);
                    c33_mem(0x3c00, 11, 1, dst + 4);
                }
            } else if ((sv.type.t & VT_BTYPE) == VT_LLONG) {
                if ((sv.r & VT_VALMASK) == VT_CONST && !(sv.r & VT_LVAL)) {
                    c33_imm(4, sv.c.i);
                    c33_imm(5, (uint64_t)sv.c.i >> 32);
                } else if ((sv.r & VT_VALMASK) == VT_LLOCAL) {
                    c33_mem(0x3000, 1, 3, sv.c.i);
                    c33_mem(0x3000, 4, 1, 0);
                    c33_mem(0x3000, 5, 1, 4);
                } else {
                    sv.type.t = VT_INT;
                    load(0, &sv);
                    sv.c.i += 4;
                    load(1, &sv);
                }
                c33_rr(0xa400, 1, 1);
                c33_mem(0x3c00, 5, 1, dst + 4);
            } else load(0, &sv);
            c33_rr(0xa400, 1, 1);
            c33_mem(0x3c00, 4, 1, dst);
        }
        scratch += (size + 3) & -4;
    }
    /* GCC's non-strict argument naming duplicates the last named variadic
     * parameter on the stack, as well as passing it in its normal register.
     * Anonymous arguments follow that stack slot. */
    c33_rr(0xa400, 1, 1);
    scratch = stack;
    for (i = 0; i < nb_args; i++) {
        size = c33_argsize(&fn[i + 1].type);
        if (stackat[i] >= 0)
            for (j = 0; j < (size + 3) / 4; j++) {
                c33_mem(0x3000, 4, 1, scratch + j * 4);
                c33_mem(0x3c00, 4, 1, stackat[i] + j * 4);
            }
        scratch += (size + 3) & -4;
    }
    load(0, fn);
    c33_mov(0, 4);
    c33_rr(0xa400, 1, 1);
    scratch = stack;
    for (i = 0; i < nb_args; i++) {
        size = c33_argsize(&fn[i + 1].type);
        if (where[i] >= 0)
            for (j = 0; j < (size + 3) / 4; j++)
                c33_mem(0x3000, where[i] + j, 1, scratch + j * 4);
        scratch += (size + 3) & -4;
    }
    /* No GCC __builtin_apply forwarding descriptor is claimed. */
    c33_imm(5, 0);
    o(0x0600); /* call r0 */
    if (is_float(fn->type.ref->type.t)) {
        c33_mov(10, 4);
        c33_mov(11, 5);
    }
    c33_sp(callsize);
    stk_pop();
    tcc_free(where);
    vtop = fn - 1;
}
ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    Sym *param = func_sym->type.ref->next;
    int reg = 0, stack = 20, size, align, r, j;
    loc = 0;
    func_vc = 0;
    c33_prolog = ind;
    gen_fill_nops(C33_PROLOG_SIZE);
    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        CType ret; int ra, rs;
        if (!gfunc_sret(&func_vt, 0, &ret, &ra, &rs)) {
            func_vc = loc = -4;
            c33_mem(0x3c00, 6, 3, loc);
            reg = 1;
        }
    }
    for (; param; param = param->next) {
        size = type_size(&param->type, &align);
        r = c33_argreg(&param->type, &reg, !func_var || param->next != NULL);
        if (r >= 0) {
            loc = (loc - ((size + 3) & -4)) & -4;
            for (j = 0; j < (size + 3) / 4; j++)
                c33_mem(0x3c00, r + j, 3, loc + j * 4);
            gfunc_set_param(param, loc, 0);
        } else {
            gfunc_set_param(param, stack, 0);
            stack += (size + 3) & -4;
        }
    }
}
ST_FUNC void gfunc_epilog(void)
{
    int end, frame = ((-loc + 19) & -16) - 4;
    if (func_vc) c33_mem(0x3000, 4, 3, func_vc);
    if (is_float(func_vt.t)) {
        c33_mov(4, 10);
        c33_mov(5, 11);
    }
    c33_rr(0xa000, 1, 3); /* sp = r3 */
    o(0x0243); /* popn r3 */
    o(0x0640); /* ret */
    end = ind;
    ind = c33_prolog;
    o(0x0203); /* pushn r3 */
    c33_rr(0xa400, 3, 1); /* r3 = sp */
    /* Fixed-size subtraction sequence, so the frame can be any size the
     * one-prefix immediates reach. */
    c33_imm19(1, frame);
    c33_mov(2, 3);
    c33_rr(0x2600, 2, 1);
    c33_rr(0xa000, 1, 2);
    c33_addsub19(2, frame - C33_LOCBIAS); /* r2 still holds r3 - frame */
    if (ind != c33_prolog + C33_PROLOG_SIZE)
        tcc_error("C33: prolog does not fill its reserved space");
    ind = end;
}
ST_FUNC void ggoto(void) { o(0x0680 | (gv(RC_INT) + 4)); vpop(); }

ST_FUNC void gen_opi(int op)
{
    int r, s, ins;
    /* Division uses GCC's soft arithmetic helpers. */
    if (op == '/' || op == TOK_UDIV || op == TOK_PDIV || op == '%' || op == TOK_UMOD) {
        const char *name = op == '%' ? "__modsi3" : op == TOK_UMOD ? "__umodsi3" :
            op == TOK_UDIV ? "__udivsi3" : "__divsi3";
        vpush_helper_func(tok_alloc_const(name));
        vrott(3);
        gfunc_call(2);
        vpushi(0);
        vtop->r = REG_IRET;
        return;
    }
    gv2(RC_INT, RC_INT);
    r = vtop[-1].r + 4; s = vtop->r + 4;
    switch (op) {
    case '+': case TOK_ADDC1: ins = 0x2200; break;
    case '-': case TOK_SUBC1: ins = 0x2600; break;
    case TOK_ADDC2: ins = 0xb800; break;
    case TOK_SUBC2: ins = 0xbc00; break;
    case '&': ins = 0x3200; break;
    case '|': ins = 0x3600; break;
    case '^': ins = 0x3a00; break;
    case TOK_SHL: ins = 0x8d00; break;
    case TOK_SHR: ins = 0x8900; break;
    case TOK_SAR: ins = 0x9100; break;
    case '*': ins = 0xaa00; break;
    case TOK_UMULL: ins = 0xae00; break;
    default:
        c33_rr(0x2a00, r, s);
        vtop--;
        vset_VT_CMP(op);
        return;
    }
    c33_rr(ins, r, s);
    vtop--;
    if (op == '*' || op == TOK_UMULL) {
        c33_rr(0xa400, r, 2); /* alr */
        if (op == TOK_UMULL) {
            int hi = get_reg(RC_INT);
            c33_rr(0xa400, hi + 4, 3); /* ahr */
            vtop->r2 = hi;
        }
    }
}
/* Software floating point uses GCC's libgcc ABI. Two allocator registers
 * represent the disjoint r10:r11 and r12:r13 pairs; even single precision
 * reserves a pair. Calls spill them and transfer the r4:r5 return value. */
static void c33_helper(const char *name, int args, int result)
{
    vpush_helper_func(tok_alloc_const(name));
    vrott(args + 1);
    gfunc_call(args);
    vpushi(0);
    vtop->type.t = result;
    PUT_R_RET(vtop, result);
    if (is_float(result)) {
        c33_mov(10, 4);
        c33_mov(11, 5);
    }
}
ST_FUNC void gen_opf(int op)
{
    int t = vtop->type.t, cmp = 0;
    const char *stem;
    char name[32];
    switch (op) {
    case '+': stem = "add"; break;
    case '-': stem = "sub"; break;
    case '*': stem = "mul"; break;
    case '/': stem = "div"; break;
    case TOK_NEG: stem = "neg"; break;
    case TOK_EQ: stem = "eq"; cmp = 1; break;
    case TOK_NE: stem = "ne"; cmp = 1; break;
    case TOK_LT: stem = "lt"; cmp = 1; break;
    case TOK_LE: stem = "le"; cmp = 1; break;
    case TOK_GT: stem = "gt"; cmp = 1; break;
    case TOK_GE: stem = "ge"; cmp = 1; break;
    default: tcc_error("C33: unsupported float operation %d", op);
    }
    snprintf(name, sizeof(name), "__%s%s%d", stem,
             (t & VT_BTYPE) == VT_FLOAT ? "sf" : "df", cmp || op == TOK_NEG ? 2 : 3);
    c33_helper(name, op == TOK_NEG ? 1 : 2, cmp ? VT_INT : t);
    if (cmp) {
        vpushi(0);
        gen_opi(op);
    }
}
ST_FUNC void gen_cvt_ftoi(int t)
{
    char name[32];
    snprintf(name, sizeof(name), "__fix%s%s%s", t & VT_UNSIGNED ? "uns" : "",
             (vtop->type.t & VT_BTYPE) == VT_FLOAT ? "sf" : "df",
             (t & VT_BTYPE) == VT_LLONG ? "di" : "si");
    c33_helper(name, 1, t);
}
ST_FUNC void gen_cvt_itof(int t)
{
    char name[32];
    snprintf(name, sizeof(name), "__float%s%s%s", vtop->type.t & VT_UNSIGNED ? "un" : "",
             (vtop->type.t & VT_BTYPE) == VT_LLONG ? "di" : "si",
             (t & VT_BTYPE) == VT_FLOAT ? "sf" : "df");
    c33_helper(name, 1, t);
}
ST_FUNC void gen_cvt_ftof(int t)
{
    int from = vtop->type.t & VT_BTYPE;
    if ((from == VT_FLOAT) != (t == VT_FLOAT))
        c33_helper(from == VT_FLOAT ? "__extendsfdf2" : "__truncdfsf2", 1, t);
}
ST_FUNC void gen_vla_sp_save(int addr)
{
    c33_rr(0xa400, 0, 1);
    c33_mem(0x3c00, 0, 3, addr);
}
ST_FUNC void gen_vla_sp_restore(int addr)
{
    c33_mem(0x3000, 0, 3, addr);
    c33_rr(0xa000, 1, 0);
}
ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    int r = gv(RC_INT) + 4;
    o(0x6000 | 15 << 4 | r);
    c33_imm(1, -16);
    c33_rr(0x3200, r, 1);
    c33_rr(0xa400, 0, 1);
    c33_rr(0x2600, 0, r);
    c33_rr(0xa000, 1, 0);
    vpop();
}
#endif
