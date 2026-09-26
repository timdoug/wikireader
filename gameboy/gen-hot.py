#!/usr/bin/env python3
"""Generate hot.s: the Game Boy CPU's hot path in C33 assembly.

    int gb_hot_run(void);

Runs Game Boy instructions from gb_hot_state until one of them is something
this core does not do -- I/O other than plain registers, a banking write,
HALT, EI, DI, RETI, STOP, DAA, the stack in HRAM, an access to an unmapped
page -- or until the cycle budget runs out.  It returns 0 when the budget
is spent, with the instruction that spent it complete, and 1 when the
instruction at pc has not run and C must run it (through Peanut).  gb.c's
loop does both, and checks for interrupts between calls.

Registers, for the whole loop:

  %r0  A                 %r6  Z: zero iff the Game Boy's Z flag is set
  %r1  BC                %r7  C: the Game Boy's carry in bit 31
  %r2  DE                %r8  H in bit 4, N in bit 9
  %r3  HL                %r9  the host address two bytes before the end
  %r4  SP                     of the region pc is in
  %r5  p, the host address of the next byte of code
  %r10 cycles left before the next event; the loop leaves when it is <= 0
  %r11 the dispatch table, with the state block after it
  %r12 %r13 %r14 scratch

The flags are kept the way they are produced: Z is the result byte, C is
the ninth bit of an 8-bit sum or difference shifted to the top, H is bit 4
of a ^ b ^ result, which is the carry into bit 4 for an add and the borrow
for a subtract.  Subtracts set bit 9 for N.  Nothing computes the flags
byte except PUSH AF and C, at the boundary.

Regions are the host-contiguous stretches of the Game Boy's map (ROM bank
0, the switched bank, VRAM, cartridge RAM, work RAM, echo).  p runs freely
inside one; an instruction starting in the last two bytes of a region goes
to C, so operands never run off the end.
"""
import sys

A, BC, DE, HL, SP, P = '%r0', '%r1', '%r2', '%r3', '%r4', '%r5'
ZV, CF, HX, PEND, LEFT, TB = '%r6', '%r7', '%r8', '%r9', '%r10', '%r11'
T0, T1, T2 = '%r12', '%r13', '%r14'

# The state block, after the 1 KB table.  gb.c's struct gb_hot matches.
OFF = {}
_at = 1024
for name, size in (('READ', 64), ('WRITE', 64), ('REND', 64), ('RSIZE', 64),
                   ('A', 4), ('BC', 4), ('DE', 4), ('HL', 4), ('SP', 4),
                   ('PC', 4), ('ZV', 4), ('CF', 4), ('HX', 4), ('LEFT', 4),
                   ('HRAM', 4), ('BIAS', 4), ('SIZE', 4), ('STEPS', 4),
                   ('PARK', 4), ('GO', 4), ('HRAMB', 4), ('STAT0', 4),
                   ('IME', 4), ('IMEBIT', 4),
                   ('CBGET', 32), ('CBSET', 32), ('CBOP', 128)):
    OFF[name] = _at
    _at += size
STATE_END = _at

CHECK = '--check' in sys.argv       # count retired instructions for lockstep

REG8 = {'b': (BC, 'hi'), 'c': (BC, 'lo'), 'd': (DE, 'hi'), 'e': (DE, 'lo'),
        'h': (HL, 'hi'), 'l': (HL, 'lo')}
R8 = ['b', 'c', 'd', 'e', 'h', 'l', '(hl)', 'a']
R16 = [BC, DE, HL, SP]

# I/O registers a store to is a plain byte in Peanut (__gb_write stores
# it and nothing else) and that nothing deferred depends on: the scroll
# and window positions and LYC, which is only compared at line starts.
PLAIN_IO_STORES = (0x42, 0x43, 0x45, 0x4a, 0x4b)


class Handler:
    def __init__(self, name):
        self.name = name
        self.lines = []
        self.cold = []          # after the dispatch: decline tails
        self.decline_used = {}

    def e(self, text):
        self.lines.append('\t' + text)

    def label(self, name):
        self.lines.append(name + ':')

    def size(self):
        words = 0
        for line in self.lines + self.cold:
            s = line.strip()
            if not s or s.endswith(':') or s.startswith(';'):
                continue
            op = s.split()[0]
            words += 3 if op.startswith('xj') else 2 if op.startswith('x') else 1
        return words * 2

    # ---- pieces --------------------------------------------------------

    def get8(self, r, dst):
        """Game Boy register r into dst (0..255)."""
        if r == 'a':
            self.e(f'ld.w\t{dst},{A}')
            return
        reg, half = REG8[r]
        if half == 'lo':
            self.e(f'ld.ub\t{dst},{reg}')
        else:
            self.e(f'ld.w\t{dst},{reg}')
            self.e(f'srl\t{dst},8')

    def set8(self, r, src):
        """src (0..255, clobbered) into Game Boy register r."""
        if r == 'a':
            self.e(f'ld.w\t{A},{src}')
            return
        reg, half = REG8[r]
        if half == 'lo':
            self.e(f'srl\t{reg},8')
            self.e(f'sll\t{reg},8')
            self.e(f'or\t{reg},{src}')
        else:
            self.e(f'ld.ub\t{reg},{reg}')
            self.e(f'sll\t{src},8')
            self.e(f'or\t{reg},{src}')

    def decline(self, rewind, cond='jreq'):
        """Branch to a tail that rewinds p and gives the instruction to C."""
        lab = self.decline_used.get(rewind)
        if lab is None:
            lab = f'{self.name}_d{rewind}'
            self.decline_used[rewind] = lab
            self.cold.append(f'{lab}:')
            self.cold.append(f'\tld.w\t{T1},{rewind}')
            self.cold.append('\txjp\t.Ldecline')
        self.e(f'{cond}\t{lab}')

    def page(self, table, addr, dst, rewind):
        """dst = host address of Game Boy address `addr` (a register), or
        decline when the page has no direct mapping.  Through HL, HRAM
        (FF80-FFFE, plain bytes of Peanut's hram_io) is served too: games
        keep tables there."""
        self.e(f'ld.w\t{dst},{addr}')
        self.e(f'srl\t{dst},12')
        self.e(f'sll\t{dst},2')
        self.e(f'add\t{dst},{TB}')
        self.e(f'xld.w\t{dst},[{dst}+{OFF[table]}]')
        self.e(f'cmp\t{dst},0')
        if addr != HL:
            self.decline(rewind)
            self.e(f'add\t{dst},{addr}')
            return
        self.pages = getattr(self, 'pages', 0) + 1
        hram = f'{self.name}_hr{self.pages}'
        back = f'{self.name}_hb{self.pages}'
        self.e(f'jreq\t{hram}')
        self.e(f'add\t{dst},{addr}')
        self.label(back)
        c = self.cold.append
        no = f'{self.name}_hd{self.pages}'
        yes = f'{self.name}_hy{self.pages}'
        c(f'{hram}:')
        c(f'\tld.w\t{dst},{addr}')
        c(f'\txsub\t{dst},0xff00')
        c(f'\txcmp\t{dst},0xff')                 # FF00-FFFE, not IE
        c(f'\txjruge\t{no}')
        c(f'\txcmp\t{dst},0x80')                 # HRAM
        c(f'\txjruge\t{yes}')
        if table == 'READ':                       # plain I/O, as plain_io
            c(f'\txcmp\t{dst},0x41')             # STAT: C works out the mode
            c(f'\txjreq\t{no}')
            c(f'\tcmp\t{dst},4')
            c(f'\txjreq\t{no}')
            c(f'\tcmp\t{dst},5')
            c(f'\txjreq\t{no}')
            c(f'\tcmp\t{dst},0x10')
            c(f'\txjrult\t{yes}')
            c(f'\txcmp\t{dst},0x40')
            c(f'\txjrult\t{no}')
            c(f'\txjp\t{yes}')
        else:
            for reg in PLAIN_IO_STORES:
                c(f'\txcmp\t{dst},{reg:#x}')
                c(f'\txjreq\t{yes}')
            c(f'\txjp\t{no}')
        c(f'{yes}:')
        c(f'\txld.w\t{dst},[{TB}+{OFF["HRAMB"]}]')  # hram_io - 0xff00
        c(f'\tadd\t{dst},{addr}')
        c(f'\txjp\t{back}')
        c(f'{no}:')
        c(f'\tld.w\t{T1},{rewind}')
        c('\txjp\t.Ldecline')

    def imm16(self, dst, tmp):
        self.e(f'ld.ub\t{dst},[{P}]+')
        self.e(f'ld.ub\t{tmp},[{P}]+')
        self.e(f'sll\t{tmp},8')
        self.e(f'or\t{dst},{tmp}')

    def count(self):
        if CHECK:
            self.e(f'xld.w\t{T1},[{TB}+{OFF["STEPS"]}]')
            self.e(f'add\t{T1},1')
            self.e(f'xld.w\t[{TB}+{OFF["STEPS"]}],{T1}')

    def next(self, cycles):
        """Charge the instruction's cycles and dispatch the next one."""
        self.count()
        self.e(f'ld.ub\t{T0},[{P}]+')
        self.e(f'sll\t{T0},2')
        self.e(f'add\t{T0},{TB}')
        self.e(f'ld.w\t{T0},[{T0}]')
        self.e(f'sub\t{LEFT},{cycles}')
        self.e('jrle\t{SPENT}')
        self.e(f'cmp\t{P},{PEND}')
        self.e('jrugt\t{BOUND}')
        self.e(f'jp\t{T0}')

    def jump(self, target, cycles):
        """Continue at the Game Boy address in `target` (T0 or another
        register), charging `cycles`."""
        if target != T0:
            self.e(f'ld.w\t{T0},{target}')
        self.e(f'ld.w\t{T1},{cycles}')
        self.e('xjp\t.Lsetpc')

    def cond(self, cc, taken_label):
        """Branch to taken_label when condition cc holds."""
        if cc in ('nz', 'z'):
            self.e(f'cmp\t{ZV},0')
            self.e(f'{"jrne" if cc == "nz" else "jreq"}\t{taken_label}')
        else:
            self.e(f'cmp\t{CF},0')
            self.e(f'{"jrge" if cc == "nc" else "jrlt"}\t{taken_label}')

    def text(self, spent, bound):
        out = [f'{self.name}:']
        for line in self.lines + self.cold:
            out.append(line.replace('{SPENT}', spent).replace('{BOUND}', bound))
        return out


handlers = {}          # opcode -> Handler
cb_handlers = {}       # name -> Handler, the CB stages


def op(code, name=None):
    h = Handler(name or f'.Lop{code:02x}')
    handlers[code] = h
    return h


# ------------------------------------------------------------ ALU pieces --

def alu(h, kind, v):
    """A = A kind v, v a scratch register holding 0..255 (not T1/T2)."""
    if kind in ('add', 'adc'):
        if kind == 'adc':                  # H is bit 4 of a ^ b ^ res
            h.e(f'ld.w\t{T2},{CF}')        # with the carry in the sum too
            h.e(f'srl\t{T2},31')
        h.e(f'ld.w\t{HX},{A}')
        h.e(f'xor\t{HX},{v}')
        h.e(f'add\t{A},{v}')
        if kind == 'adc':
            h.e(f'add\t{A},{T2}')
        h.e(f'xor\t{HX},{A}')
        h.e(f'ld.w\t{CF},{A}')
        h.e(f'sll\t{CF},23')
        h.e(f'ld.ub\t{A},{A}')
        h.e(f'ld.w\t{ZV},{A}')
    elif kind in ('sub', 'sbc', 'cp'):
        res = A if kind != 'cp' else T1
        if kind == 'sbc':
            h.e(f'ld.w\t{T2},{CF}')
            h.e(f'srl\t{T2},31')
        if kind == 'cp':
            h.e(f'ld.w\t{T1},{A}')
        h.e(f'ld.w\t{HX},{A}')
        h.e(f'xor\t{HX},{v}')
        h.e(f'sub\t{res},{v}')
        if kind == 'sbc':
            h.e(f'sub\t{res},{T2}')
        h.e(f'xor\t{HX},{res}')
        h.e(f'xoor\t{HX},0x200')
        h.e(f'ld.w\t{CF},{res}')
        h.e(f'sll\t{CF},23')
        h.e(f'ld.ub\t{ZV},{res}')
        if kind != 'cp':
            h.e(f'ld.ub\t{A},{A}')
    else:
        h.e(f'{ {"and": "and", "xor": "xor", "or": "or"}[kind]}\t{A},{v}')
        h.e(f'ld.w\t{ZV},{A}')
        h.e(f'ld.w\t{HX},{0x10 if kind == "and" else 0}')
        h.e(f'ld.w\t{CF},0')


ALU = ['add', 'adc', 'sub', 'sbc', 'and', 'xor', 'or', 'cp']


def inc8(h, v, dec):
    """v = v +/- 1 with Z, H, N; C unchanged.  v holds 0..255."""
    h.e(f'ld.w\t{HX},{v}')
    h.e(f'{"sub" if dec else "add"}\t{v},1')
    h.e(f'xor\t{HX},{v}')
    h.e(f'xor\t{HX},1')
    if dec:
        h.e(f'xoor\t{HX},0x200')
    h.e(f'ld.ub\t{v},{v}')
    h.e(f'ld.w\t{ZV},{v}')


# ------------------------------------------------------------ opcodes -----

h = op(0x00)
h.next(4)

for i, reg in enumerate(R16):
    h = op(0x01 + 16 * i)                       # LD rr,nn
    h.imm16(T0, T1)
    h.e(f'ld.w\t{reg},{T0}')
    h.next(12)

    h = op(0x03 + 16 * i)                       # INC rr
    h.e(f'add\t{reg},1')
    h.e(f'ld.uh\t{reg},{reg}')
    h.next(8)

    h = op(0x0b + 16 * i)                       # DEC rr
    h.e(f'sub\t{reg},1')
    h.e(f'ld.uh\t{reg},{reg}')
    h.next(8)

    h = op(0x09 + 16 * i)                       # ADD HL,rr
    h.e(f'ld.w\t{HX},{HL}')
    h.e(f'xor\t{HX},{reg}')
    h.e(f'add\t{HL},{reg}')
    h.e(f'xor\t{HX},{HL}')
    h.e(f'srl\t{HX},8')                        # H from bit 12, N clear
    h.e(f'ld.w\t{CF},{HL}')
    h.e(f'sll\t{CF},15')                       # C from bit 16
    h.e(f'ld.uh\t{HL},{HL}')
    h.next(8)

for code, reg in ((0x02, BC), (0x12, DE)):      # LD (BC)/(DE),A
    h = op(code)
    h.page('WRITE', reg, T0, 1)
    h.e(f'ld.b\t[{T0}],{A}')
    h.next(8)
for code, reg in ((0x0a, BC), (0x1a, DE)):      # LD A,(BC)/(DE)
    h = op(code)
    h.page('READ', reg, T0, 1)
    h.e(f'ld.ub\t{A},[{T0}]')
    h.next(8)

for code, step, load in ((0x22, 1, False), (0x32, -1, False),
                         (0x2a, 1, True), (0x3a, -1, True)):
    h = op(code)                                # LD (HL+/-),A / LD A,(HL+/-)
    h.page('READ' if load else 'WRITE', HL, T0, 1)
    h.e(f'ld.ub\t{A},[{T0}]' if load else f'ld.b\t[{T0}],{A}')
    h.e(f'{"add" if step > 0 else "sub"}\t{HL},1')
    h.e(f'ld.uh\t{HL},{HL}')
    h.next(8)

for i, r in enumerate(R8):
    if r == '(hl)':
        for dec in (False, True):
            h = op(0x34 + dec)                  # INC/DEC (HL)
            h.page('WRITE', HL, T2, 1)          # RAM pages read as written
            h.e(f'ld.ub\t{T0},[{T2}]')
            inc8(h, T0, dec)
            h.e(f'ld.b\t[{T2}],{T0}')
            h.next(12)
        h = op(0x36)                            # LD (HL),n
        h.page('WRITE', HL, T1, 1)
        h.e(f'ld.ub\t{T0},[{P}]+')
        h.e(f'ld.b\t[{T1}],{T0}')
        h.next(12)
        continue
    for dec in (False, True):
        h = op(0x04 + 8 * i + dec)              # INC/DEC r
        if r == 'a':
            inc8(h, A, dec)
        else:
            h.get8(r, T0)
            inc8(h, T0, dec)
            h.set8(r, T0)
        h.next(4)
    h = op(0x06 + 8 * i)                        # LD r,n
    h.e(f'ld.ub\t{T0},[{P}]+')
    h.set8(r, T0)
    h.next(8)

# Rotates of A: Z is cleared, as are N and H.
h = op(0x07)                                    # RLCA
h.e(f'ld.w\t{CF},{A}')
h.e(f'sll\t{CF},24')
h.e(f'ld.w\t{T0},{A}')
h.e(f'srl\t{T0},7')
h.e(f'sll\t{A},1')
h.e(f'or\t{A},{T0}')
h.e(f'ld.ub\t{A},{A}')
h.e(f'ld.w\t{ZV},1')
h.e(f'ld.w\t{HX},0')
h.next(4)
h = op(0x0f)                                    # RRCA
h.e(f'ld.w\t{CF},{A}')
h.e(f'sll\t{CF},31')
h.e(f'ld.w\t{T0},{A}')
h.e(f'sll\t{T0},7')
h.e(f'srl\t{A},1')
h.e(f'or\t{A},{T0}')
h.e(f'ld.ub\t{A},{A}')
h.e(f'ld.w\t{ZV},1')
h.e(f'ld.w\t{HX},0')
h.next(4)
h = op(0x17)                                    # RLA
h.e(f'ld.w\t{T0},{CF}')
h.e(f'srl\t{T0},31')
h.e(f'ld.w\t{CF},{A}')
h.e(f'sll\t{CF},24')
h.e(f'sll\t{A},1')
h.e(f'or\t{A},{T0}')
h.e(f'ld.ub\t{A},{A}')
h.e(f'ld.w\t{ZV},1')
h.e(f'ld.w\t{HX},0')
h.next(4)
h = op(0x1f)                                    # RRA
h.e(f'ld.w\t{T0},{CF}')
h.e(f'srl\t{T0},31')
h.e(f'sll\t{T0},7')
h.e(f'ld.w\t{CF},{A}')
h.e(f'sll\t{CF},31')
h.e(f'srl\t{A},1')
h.e(f'or\t{A},{T0}')
h.e(f'ld.w\t{ZV},1')
h.e(f'ld.w\t{HX},0')
h.next(4)

h = op(0x2f)                                    # CPL
h.e(f'not\t{A},{A}')
h.e(f'ld.ub\t{A},{A}')
h.e(f'xld.w\t{HX},0x210')
h.next(4)
h = op(0x37)                                    # SCF
h.e(f'ld.w\t{CF},-1')
h.e(f'ld.w\t{HX},0')
h.next(4)
h = op(0x3f)                                    # CCF
h.e(f'not\t{CF},{CF}')
h.e(f'ld.w\t{HX},0')
h.next(4)


def jr_body(h):
    """p += the signed operand; leave the region through .Lsetpc."""
    h.e(f'ld.b\t{T0},[{P}]+')
    h.e(f'add\t{P},{T0}')
    h.e(f'ld.w\t{T1},{PEND}')
    h.e(f'sub\t{T1},{P}')
    h.e(f'add\t{T1},1')                    # (end - 2) - p + 1 = end - p - 1
    h.e(f'xld.w\t{T2},[{TB}+{OFF["SIZE"]}]')
    h.e(f'cmp\t{T1},{T2}')                 # 0 <= end - p - 1 < size
    h.e(f'jruge\t{h.name}_far')
    h.next(12)
    h.cold.append(f'{h.name}_far:')
    h.cold.append(f'\txld.w\t{T1},[{TB}+{OFF["BIAS"]}]')
    h.cold.append(f'\tld.w\t{T0},{P}')
    h.cold.append(f'\tsub\t{T0},{T1}')
    h.cold.append(f'\tld.w\t{T1},12')
    h.cold.append('\txjp\t.Lsetpc')


h = op(0x18)                                    # JR e
jr_body(h)
def idle_skip(h):
    """The busy-wait on an interrupt handler's flag in HRAM:

        wait: ldh a,(nn) ; and a ; jr z,wait      (or jr nz)

    Only the CPU writes HRAM and interrupts are taken outside this loop,
    so once A holds the byte and the flags are what `and a` leaves, every
    pass is the same 28 cycles until the budget runs out.  Charge all but
    the last pass at once; the last runs normally and leaves at the same
    instruction a pass at a time would have."""
    idle, back = f'{h.name}_idle', f'{h.name}_back'
    h.e(f'ld.ub\t{T0},[{P}]')
    h.e(f'xcmp\t{T0},0xfb')                   # jr back 5 bytes
    h.e(f'jreq\t{idle}')
    h.label(back)
    c = h.cold.append
    c(f'{idle}:')
    c(f'\tld.w\t{T1},{P}')
    c(f'\tsub\t{T1},4')
    c(f'\tld.ub\t{T0},[{T1}]+')
    c(f'\txcmp\t{T0},0xf0')                  # ldh a,(nn)
    c(f'\txjrne\t{back}')
    c(f'\tld.ub\t{T2},[{T1}]+')              # nn
    c(f'\tld.ub\t{T0},[{T1}]')
    c(f'\txcmp\t{T0},0xa7')                  # and a
    c(f'\txjrne\t{back}')
    c(f'\tld.w\t{T0},{T2}')
    c(f'\txsub\t{T0},0x80')
    c(f'\txcmp\t{T0},0x7f')                  # HRAM proper, not IE
    c(f'\txjruge\t{back}')
    c(f'\txld.w\t{T0},[{TB}+{OFF["HRAM"]}]')
    c(f'\tadd\t{T0},{T2}')
    c(f'\tld.ub\t{T0},[{T0}]')
    c(f'\tcmp\t{A},{T0}')                    # the fixed point: A is the
    c(f'\txjrne\t{back}')                    # byte, and the flags are
    c(f'\tcmp\t{ZV},{A}')                    # and a's
    c(f'\txjrne\t{back}')
    c(f'\tcmp\t{HX},0x10')
    c(f'\txjrne\t{back}')
    c(f'\tcmp\t{CF},0')
    c(f'\txjrne\t{back}')
    c('\txjp\t.Lidle')                         # P: the jr's operand


for code, cc in ((0x20, 'nz'), (0x28, 'z'), (0x30, 'nc'), (0x38, 'c')):
    h = op(code)
    h.cond(cc, f'{h.name}_taken')
    h.e(f'add\t{P},1')
    h.next(8)
    h.label(f'{h.name}_taken')
    if cc in ('z', 'nz'):
        idle_skip(h)
    jr_body(h)

# LD r,r' and HALT's hole
for d in range(8):
    for s in range(8):
        code = 0x40 + 8 * d + s
        if code == 0x76:
            continue
        rd, rs = R8[d], R8[s]
        h = op(code)
        if rd == '(hl)':
            h.page('WRITE', HL, T1, 1)
            if rs == 'a':
                h.e(f'ld.b\t[{T1}],{A}')
            else:
                h.get8(rs, T0)
                h.e(f'ld.b\t[{T1}],{T0}')
            h.next(8)
        elif rs == '(hl)':
            h.page('READ', HL, T1, 1)
            if rd == 'a':
                h.e(f'ld.ub\t{A},[{T1}]')
            else:
                h.e(f'ld.ub\t{T0},[{T1}]')
                h.set8(rd, T0)
            h.next(8)
        else:
            if rd != rs:
                if rd == 'a':
                    h.get8(rs, A)
                else:
                    h.get8(rs, T0)
                    h.set8(rd, T0)
            h.next(4)

# ALU A,r and A,n
for k, kind in enumerate(ALU):
    for s in range(8):
        h = op(0x80 + 8 * k + s)
        if R8[s] == '(hl)':
            h.page('READ', HL, T0, 1)
            h.e(f'ld.ub\t{T0},[{T0}]')
            alu(h, kind, T0)
            h.next(8)
        else:
            h.get8(R8[s], T0)
            alu(h, kind, T0)
            h.next(4)
    h = op(0xc6 + 8 * k)
    h.e(f'ld.ub\t{T0},[{P}]+')
    alu(h, kind, T0)
    h.next(8)


def push(h, value, rewind):
    """Push `value` (a register other than T0/T1/T2, or T0) -- declines
    before changing anything if the new top of stack is not plain RAM or
    the two bytes straddle a page."""
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'sub\t{T1},2')
    h.e(f'ld.uh\t{T1},{T1}')
    h.page('WRITE', T1, T2, rewind)            # T2 = host address of new SP
    h.e(f'ld.w\t{T1},{SP}')                    # SP - 1 starting a page: the
    h.e(f'sub\t{T1},1')                        # two bytes straddle it
    h.e(f'sll\t{T1},20')
    h.decline(rewind)
    h.e(f'sub\t{SP},2')
    h.e(f'ld.uh\t{SP},{SP}')
    if value != T0:
        h.e(f'ld.w\t{T0},{value}')
    h.e(f'ld.b\t[{T2}]+,{T0}')
    h.e(f'srl\t{T0},8')
    h.e(f'ld.b\t[{T2}],{T0}')


def pop(h, dst, rewind):
    """dst = the word at SP; SP += 2.  Declines like push."""
    h.page('READ', SP, T2, rewind)
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'add\t{T1},1')
    h.e(f'sll\t{T1},20')
    h.decline(rewind)
    h.e(f'ld.ub\t{dst},[{T2}]+')
    h.e(f'ld.ub\t{T1},[{T2}]')
    h.e(f'sll\t{T1},8')
    h.e(f'or\t{dst},{T1}')
    h.e(f'add\t{SP},2')
    h.e(f'ld.uh\t{SP},{SP}')


def return_address(h, dst):
    """dst = the Game Boy address p points at."""
    h.e(f'xld.w\t{dst},[{TB}+{OFF["BIAS"]}]')
    h.e(f'sub\t{dst},{P}')
    h.e(f'not\t{dst},{dst}')
    h.e(f'add\t{dst},1')


for i, (reg, af) in enumerate(((BC, False), (DE, False), (HL, False), (None, True))):
    h = op(0xc1 + 16 * i)                       # POP rr
    if af:
        pop(h, T0, 1)
        h.e(f'ld.w\t{A},{T0}')
        h.e(f'srl\t{A},8')
        h.e(f'ld.ub\t{T0},{T0}')               # F
        h.e(f'ld.w\t{ZV},{T0}')
        h.e(f'xand\t{ZV},0x80')
        h.e(f'xxor\t{ZV},0x80')
        h.e(f'ld.w\t{CF},{T0}')
        h.e(f'sll\t{CF},27')                   # bit 4 -> 31
        h.e(f'ld.w\t{HX},{T0}')
        h.e(f'srl\t{HX},1')
        h.e(f'and\t{HX},0x10')                 # bit 5 -> 4
        h.e(f'xand\t{T0},0x40')
        h.e(f'sll\t{T0},3')                    # bit 6 -> 9
        h.e(f'or\t{HX},{T0}')
    else:
        pop(h, reg, 1)
    h.next(12)

    h = op(0xc5 + 16 * i)                       # PUSH rr
    if af:
        h.e(f'ld.w\t{T0},0')
        h.e(f'cmp\t{ZV},0')
        h.e(f'jrne\t{h.name}_nz')
        h.e(f'xld.w\t{T0},0x80')
        h.label(f'{h.name}_nz')
        h.e(f'ld.w\t{T1},{HX}')
        h.e(f'srl\t{T1},3')
        h.e(f'xand\t{T1},0x40')
        h.e(f'or\t{T0},{T1}')
        h.e(f'ld.w\t{T1},{HX}')
        h.e(f'sll\t{T1},1')
        h.e(f'xand\t{T1},0x20')
        h.e(f'or\t{T0},{T1}')
        h.e(f'ld.w\t{T1},{CF}')
        h.e(f'srl\t{T1},31')
        h.e(f'sll\t{T1},4')
        h.e(f'or\t{T0},{T1}')
        h.e(f'ld.w\t{T1},{A}')
        h.e(f'sll\t{T1},8')
        h.e(f'or\t{T0},{T1}')
        h.e(f'xld.w\t[{TB}+{OFF["PARK"]}],{T0}')   # park it past STEPS
        # push() needs T0 free until the store
        h.lines.append('\t; F and A are parked in the scratch word')
        push_value = 'parked'
    else:
        push_value = reg
    if push_value == 'parked':
        h.e(f'ld.w\t{T1},{SP}')
        h.e(f'sub\t{T1},2')
        h.e(f'ld.uh\t{T1},{T1}')
        h.page('WRITE', T1, T2, 1)
        h.e(f'ld.w\t{T1},{SP}')
        h.e(f'sub\t{T1},1')
        h.e(f'sll\t{T1},20')
        h.decline(1)
        h.e(f'sub\t{SP},2')
        h.e(f'ld.uh\t{SP},{SP}')
        h.e(f'xld.w\t{T0},[{TB}+{OFF["PARK"]}]')
        h.e(f'ld.b\t[{T2}]+,{T0}')
        h.e(f'srl\t{T0},8')
        h.e(f'ld.b\t[{T2}],{T0}')
    else:
        push(h, push_value, 1)
    h.next(16)

def pending_irq(h, rewind):
    """Decline if an interrupt would be pending with IME set: C then takes
    the instruction, and Peanut dispatches it in its order."""
    h.e(f'xld.w\t{T0},[{TB}+{OFF["HRAM"]}]')
    h.e(f'xld.ub\t{T1},[{T0}+0x0f]')           # IF
    h.e(f'xld.ub\t{T2},[{T0}+0xff]')           # IE
    h.e(f'and\t{T1},{T2}')
    h.e(f'and\t{T1},0x1f')
    h.e(f'cmp\t{T1},0')
    h.decline(rewind, 'jrne')


def set_ime(h, on):
    """gb_ime is a bitfield of Peanut's struct: its byte and bit come in
    the state block."""
    h.e(f'xld.w\t{T0},[{TB}+{OFF["IME"]}]')
    h.e(f'ld.ub\t{T1},[{T0}]')
    h.e(f'xld.w\t{T2},[{TB}+{OFF["IMEBIT"]}]')
    if on:
        h.e(f'or\t{T1},{T2}')
    else:
        h.e(f'not\t{T2},{T2}')
        h.e(f'and\t{T1},{T2}')
    h.e(f'ld.b\t[{T0}],{T1}')


# No interrupt can become pending between events, which gb_hot_event
# checks; so EI and RETI only need to look once, when IME goes on.
h = op(0xf3)                                    # DI
set_ime(h, False)
h.next(4)
h = op(0xfb)                                    # EI
pending_irq(h, 1)
set_ime(h, True)
h.next(4)
h = op(0xd9)                                    # RETI
pending_irq(h, 1)
pop(h, T0, 1)
h.e(f'xld.w\t[{TB}+{OFF["PARK"]}],{T0}')     # set_ime takes T0-T2
set_ime(h, True)
h.e(f'xld.w\t{T0},[{TB}+{OFF["PARK"]}]')
h.jump(T0, 16)

h = op(0xc9)                                    # RET
pop(h, T0, 1)
h.jump(T0, 16)
for code, cc in ((0xc0, 'nz'), (0xc8, 'z'), (0xd0, 'nc'), (0xd8, 'c')):
    h = op(code)                                # RET cc
    h.cond(cc, f'{h.name}_taken')
    h.next(8)
    h.label(f'{h.name}_taken')
    pop(h, T0, 1)
    h.jump(T0, 20)

h = op(0xc3)                                    # JP nn
h.imm16(T0, T1)
h.jump(T0, 16)
for code, cc in ((0xc2, 'nz'), (0xca, 'z'), (0xd2, 'nc'), (0xda, 'c')):
    h = op(code)                                # JP cc,nn
    h.cond(cc, f'{h.name}_taken')
    h.e(f'add\t{P},2')
    h.next(12)
    h.label(f'{h.name}_taken')
    h.imm16(T0, T1)
    h.jump(T0, 16)
h = op(0xe9)                                    # JP (HL)
h.jump(HL, 4)


def call_body(h, cycles, rewind):
    """Push the address after the operand and continue at the operand."""
    # Check the stack before anything moves; then read the operand.
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'sub\t{T1},2')
    h.e(f'ld.uh\t{T1},{T1}')
    h.page('WRITE', T1, T2, rewind)
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'sub\t{T1},1')
    h.e(f'sll\t{T1},20')
    h.decline(rewind)
    h.e(f'sub\t{SP},2')
    h.e(f'ld.uh\t{SP},{SP}')
    h.e(f'xld.w\t[{TB}+{OFF["PARK"]}],{T2}')
    h.imm16(T0, T1)                            # the target
    return_address(h, T1)
    h.e(f'xld.w\t{T2},[{TB}+{OFF["PARK"]}]')
    h.e(f'ld.b\t[{T2}]+,{T1}')
    h.e(f'srl\t{T1},8')
    h.e(f'ld.b\t[{T2}],{T1}')
    h.jump(T0, cycles)


h = op(0xcd)                                    # CALL nn
call_body(h, 24, 1)
for code, cc in ((0xc4, 'nz'), (0xcc, 'z'), (0xd4, 'nc'), (0xdc, 'c')):
    h = op(code)                                # CALL cc,nn
    h.cond(cc, f'{h.name}_taken')
    h.e(f'add\t{P},2')
    h.next(12)
    h.label(f'{h.name}_taken')
    call_body(h, 24, 1)

for n in range(8):                              # RST n
    h = op(0xc7 + 8 * n)
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'sub\t{T1},2')
    h.e(f'ld.uh\t{T1},{T1}')
    h.page('WRITE', T1, T2, 1)
    h.e(f'ld.w\t{T1},{SP}')
    h.e(f'sub\t{T1},1')
    h.e(f'sll\t{T1},20')
    h.decline(1)
    h.e(f'sub\t{SP},2')
    h.e(f'ld.uh\t{SP},{SP}')
    return_address(h, T1)
    h.e(f'ld.b\t[{T2}]+,{T1}')
    h.e(f'srl\t{T1},8')
    h.e(f'ld.b\t[{T2}],{T1}')
    h.e(f'xld.w\t{T0},{8 * n}')
    h.jump(T0, 16)


def plain_io(h, n, rewind):
    """Decline unless FF00+n reads as a plain byte of Peanut's hram_io:
    DIV and TIMA fall behind while cycles are deferred, and the sound
    registers read through a mask."""
    h.e(f'ld.w\t{T1},{n}')
    h.e(f'sub\t{T1},4')
    h.e(f'cmp\t{T1},2')
    h.decline(rewind, 'jrult')
    h.e(f'sub\t{T1},12')
    h.e(f'xcmp\t{T1},0x30')
    h.decline(rewind, 'jrult')


def stat_mode(h):
    """A was read from FF00+T0: if that was STAT in mode 3 and the change
    to mode 0 is not an event (memory.h's hblank_end), work the mode out
    from the budget."""
    stat, back = f'{h.name}_stat', f'{h.name}_statb'
    h.e(f'xcmp\t{T0},0x41')
    h.e(f'jreq\t{stat}')
    h.label(back)
    c = h.cold.append
    c(f'{stat}:')
    c(f'\tld.w\t{T1},{A}')
    c(f'\tand\t{T1},3')
    c(f'\tcmp\t{T1},3')
    c(f'\txjrne\t{back}')
    c(f'\txld.w\t{T1},[{TB}+{OFF["STAT0"]}]')
    c(f'\tcmp\t{LEFT},{T1}')
    c(f'\txjrgt\t{back}')
    c(f'\tand\t{A},-4')                      # mode 0
    c(f'\txjp\t{back}')


h = op(0xf0)                                    # LDH A,(n)
h.e(f'ld.ub\t{T0},[{P}]+')
plain_io(h, T0, 2)
h.e(f'xld.w\t{T1},[{TB}+{OFF["HRAM"]}]')
h.e(f'add\t{T1},{T0}')
h.e(f'ld.ub\t{A},[{T1}]')
stat_mode(h)
h.next(12)
h = op(0xf2)                                    # LD A,(C)
h.e(f'ld.ub\t{T0},{BC}')
plain_io(h, T0, 1)
h.e(f'xld.w\t{T1},[{TB}+{OFF["HRAM"]}]')
h.e(f'add\t{T1},{T0}')
h.e(f'ld.ub\t{A},[{T1}]')
stat_mode(h)
h.next(8)


def hram_only(h, n, rewind):
    """Decline unless a store to FF00+n is to HRAM proper (not IE) or one
    of PLAIN_IO_STORES."""
    ok, io = f'{h.name}_st', f'{h.name}_io'
    h.e(f'ld.w\t{T1},{n}')
    h.e(f'xsub\t{T1},0x80')
    h.e(f'xcmp\t{T1},0x7f')
    h.e(f'jruge\t{io}')
    h.label(ok)
    c = h.cold.append
    c(f'{io}:')
    for reg in PLAIN_IO_STORES:
        c(f'\txcmp\t{n},{reg:#x}')
        c(f'\txjreq\t{ok}')
    c(f'\tld.w\t{T1},{rewind}')
    c('\txjp\t.Ldecline')


h = op(0xe0)                                    # LDH (n),A
h.e(f'ld.ub\t{T0},[{P}]+')
hram_only(h, T0, 2)
h.e(f'xld.w\t{T1},[{TB}+{OFF["HRAM"]}]')
h.e(f'add\t{T1},{T0}')
h.e(f'ld.b\t[{T1}],{A}')
h.next(12)
h = op(0xe2)                                    # LD (C),A
h.e(f'ld.ub\t{T0},{BC}')
hram_only(h, T0, 1)
h.e(f'xld.w\t{T1},[{TB}+{OFF["HRAM"]}]')
h.e(f'add\t{T1},{T0}')
h.e(f'ld.b\t[{T1}],{A}')
h.next(8)

h = op(0xea)                                    # LD (nn),A
h.imm16(T0, T1)
h.page('WRITE', T0, T1, 3)
h.e(f'ld.b\t[{T1}],{A}')
h.next(16)
h = op(0xfa)                                    # LD A,(nn)
h.imm16(T0, T1)
h.page('READ', T0, T1, 3)
h.e(f'ld.ub\t{A},[{T1}]')
h.next(16)

h = op(0xf9)                                    # LD SP,HL
h.e(f'ld.w\t{SP},{HL}')
h.next(8)

# ---------------------------------------------------------------- CB ------

h = op(0xcb)
h.e(f'ld.ub\t{T0},[{P}]+')                     # the CB opcode, kept in T0
h.e(f'ld.w\t{T1},{T0}')
h.e(f'and\t{T1},7')
h.e(f'sll\t{T1},2')
h.e(f'add\t{T1},{TB}')
h.e(f'xld.w\t{T1},[{T1}+{OFF["CBGET"]}]')
h.e(f'jp\t{T1}')

# Getters leave the operand in T1; (HL) also leaves its host address in
# the parking word.  Then the operation, by opcode >> 3.
CB_GET = []
for z, r in enumerate(R8):
    g = Handler(f'.Lcbget{z}')
    cb_handlers[g.name] = g
    CB_GET.append(g.name)
    if r == '(hl)':
        g.page('WRITE', HL, T2, 2)              # RAM pages read as written
        g.e(f'xld.w\t[{TB}+{OFF["PARK"]}],{T2}')
        g.e(f'ld.ub\t{T1},[{T2}]')
    else:
        g.get8(r, T1)
    g.e(f'ld.w\t{T2},{T0}')
    g.e(f'srl\t{T2},3')
    g.e(f'sll\t{T2},2')
    g.e(f'add\t{T2},{TB}')
    g.e(f'xld.w\t{T2},[{T2}+{OFF["CBOP"]}]')
    g.e(f'jp\t{T2}')

CB_SET = []
for z, r in enumerate(R8):
    s = Handler(f'.Lcbset{z}')
    cb_handlers[s.name] = s
    CB_SET.append(s.name)
    if r == '(hl)':
        s.e(f'xld.w\t{T2},[{TB}+{OFF["PARK"]}]')
        s.e(f'ld.b\t[{T2}],{T1}')
        s.next(16)
    else:
        s.set8(r, T1)
        s.next(8)


def to_setter(g):
    g.e(f'ld.w\t{T2},{T0}')
    g.e(f'and\t{T2},7')
    g.e(f'sll\t{T2},2')
    g.e(f'add\t{T2},{TB}')
    g.e(f'xld.w\t{T2},[{T2}+{OFF["CBSET"]}]')
    g.e(f'jp\t{T2}')


CB_OP = []
for kind in ('rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'swap', 'srl'):
    g = Handler(f'.Lcb{kind}')
    cb_handlers[g.name] = g
    CB_OP.append(g.name)
    v = T1
    if kind == 'rlc':
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},24')
        g.e(f'ld.w\t{T2},{v}'); g.e(f'srl\t{T2},7'); g.e(f'sll\t{v},1'); g.e(f'or\t{v},{T2}')
    elif kind == 'rrc':
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},31')
        g.e(f'ld.w\t{T2},{v}'); g.e(f'sll\t{T2},7'); g.e(f'srl\t{v},1'); g.e(f'or\t{v},{T2}')
    elif kind == 'rl':
        g.e(f'ld.w\t{T2},{CF}'); g.e(f'srl\t{T2},31')
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},24'); g.e(f'sll\t{v},1'); g.e(f'or\t{v},{T2}')
    elif kind == 'rr':
        g.e(f'ld.w\t{T2},{CF}'); g.e(f'srl\t{T2},31'); g.e(f'sll\t{T2},7')
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},31'); g.e(f'srl\t{v},1'); g.e(f'or\t{v},{T2}')
    elif kind == 'sla':
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},24'); g.e(f'sll\t{v},1')
    elif kind == 'sra':
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},31')
        g.e(f'ld.w\t{T2},{v}'); g.e(f'xand\t{T2},0x80'); g.e(f'srl\t{v},1'); g.e(f'or\t{v},{T2}')
    elif kind == 'swap':
        g.e(f'ld.w\t{T2},{v}'); g.e(f'srl\t{T2},4'); g.e(f'sll\t{v},4'); g.e(f'or\t{v},{T2}')
        g.e(f'ld.w\t{CF},0')
    elif kind == 'srl':
        g.e(f'ld.w\t{CF},{v}'); g.e(f'sll\t{CF},31'); g.e(f'srl\t{v},1')
    g.e(f'ld.ub\t{v},{v}')
    g.e(f'ld.w\t{ZV},{v}')
    g.e(f'ld.w\t{HX},0')
    to_setter(g)

g = Handler('.Lcbbit')                          # BIT b: Z from the bit, H set
cb_handlers[g.name] = g
g.e(f'ld.w\t{T2},{T0}')
g.e(f'srl\t{T2},3')
g.e(f'and\t{T2},7')
g.e(f'srl\t{T1},{T2}')
g.e(f'and\t{T1},1')
g.e(f'ld.w\t{ZV},{T1}')
g.e(f'ld.w\t{HX},0x10')
g.e(f'ld.w\t{T2},{T0}')
g.e(f'and\t{T2},7')
g.e(f'cmp\t{T2},6')
g.e(f'jreq\t.Lcbbit_hl')
g.next(8)
g.label('.Lcbbit_hl')
g.next(12)

for kind in ('res', 'set'):
    g = Handler(f'.Lcb{kind}')
    cb_handlers[g.name] = g
    g.e(f'ld.w\t{T2},{T0}')
    g.e(f'srl\t{T2},3')
    g.e(f'and\t{T2},7')
    g.e(f'ld.w\t%alr,{T0}')                    # T0 is needed for the mask
    g.e(f'ld.w\t{T0},1')
    g.e(f'sll\t{T0},{T2}')
    if kind == 'res':
        g.e(f'not\t{T0},{T0}')
        g.e(f'and\t{T1},{T0}')
    else:
        g.e(f'or\t{T1},{T0}')
    g.e(f'ld.w\t{T0},%alr')
    to_setter(g)

CB_TABLE = CB_OP + ['.Lcbbit'] * 8 + ['.Lcbres'] * 8 + ['.Lcbset'] * 8

# ------------------------------------------------------------- output -----

out = []
w = out.append
w('; hot.s -- generated by gen-hot.py; edit that.')
w(f'; {"check build: counts retired instructions" if CHECK else "normal build"}')
for name, value in OFF.items():
    w(f'\t.set\tOFF_{name}, {value}')
w('')
w('\t.section .ivram_code,"ax"')
w('\t.align\t2')
w('\t.globl\tgb_hot_table')
w('\t.globl\tgb_hot_state')
w('gb_hot_table:')
for code in range(256):
    w(f'\t.long\t{handlers[code].name if code in handlers else ".Lunimplemented"}')
w('gb_hot_state:')
w(f'\t.space\t{OFF["CBGET"] - 1024}')
for name in CB_GET:
    w(f'\t.long\t{name}')
for name in CB_SET:
    w(f'\t.long\t{name}')
for name in CB_TABLE:
    w(f'\t.long\t{name}')
w('')

# Entry and exits: rarely run, so in SDRAM.
w('\t.section .text')
w('\t.align\t1')
w('\t.globl\tgb_hot_run')
w('gb_hot_run:')
w('\tpushn\t%r3')
w(f'\txld.w\t{TB},gb_hot_table')
for reg, name in ((A, 'A'), (BC, 'BC'), (DE, 'DE'), (HL, 'HL'), (SP, 'SP'),
                  (ZV, 'ZV'), (CF, 'CF'), (HX, 'HX'), (LEFT, 'LEFT')):
    w(f'\txld.w\t{reg},[{TB}+{OFF[name]}]')
w(f'\txld.w\t{T0},[{TB}+{OFF["PC"]}]')
w(f'\tld.w\t{T1},0')
w('\txjp\t.Lsetpc_entry')
w('')
w('.Lunimplemented:')
w(f'\tld.w\t{T1},1')
w('.Ldecline:')                                 # p -= T1; C runs it
w(f'\tsub\t{P},{T1}')
w('.Lcpu:')
w(f'\tld.w\t{T0},1')
w('\tjp\t.Lexit')
# The budget is spent at an event.  C runs it (gb_hot_event) with every
# register pushed, and says whether to go on -- the usual case -- or to
# leave for an interrupt or the end of the frame.  Leaving and coming back
# in cost far more than the event: three of them a scanline.
w('.Lleave_after_event:')
w(f'\tld.w\t{T0},2')
w('\tjp\t.Lexit')
w('.Lbound_r:')
w(f'\tsub\t{P},1')
w(f'\tld.w\t{T0},1')
w('.Lexit:')
w(f'\txld.w\t{T1},[{TB}+{OFF["BIAS"]}]')
w(f'\tsub\t{P},{T1}')
w(f'\tld.uh\t{P},{P}')
for reg, name in ((A, 'A'), (BC, 'BC'), (DE, 'DE'), (HL, 'HL'), (SP, 'SP'),
                  (P, 'PC'), (ZV, 'ZV'), (CF, 'CF'), (HX, 'HX'), (LEFT, 'LEFT')):
    w(f'\txld.w\t[{TB}+{OFF[name]}],{reg}')
w(f'\tld.w\t%r4,{T0}')
w('\tpopn\t%r3')
w('\tret')
w('')
# A Game Boy address in T0 with T1 cycles to charge: find its region.
# Code in HRAM -- where games put the routine that waits out OAM DMA -- is
# its own region, FF80-FFFF; the page's data stays with C, for the I/O.
w('.Lsetpc_unmapped:')
w(f'\txcmp\t{T0},0xff80')
w('\tjrult\t.Lsetpc_out')
w(f'\txld.w\t{P},[{TB}+{OFF["HRAM"]}]')
w(f'\txsub\t{P},0xff00')                     # hram_io is FF00 up
w(f'\txld.w\t[{TB}+{OFF["BIAS"]}],{P}')
w(f'\txld.w\t{T1},0x80')
w(f'\txld.w\t[{TB}+{OFF["SIZE"]}],{T1}')
w(f'\tld.w\t{PEND},{P}')
w(f'\txadd\t{PEND},0xfffe')                  # 0x10000 - 2
w('\txjp\t.Lsetpc_go')
w('.Lsetpc_out:')                              # pc = T0, via bias 0
w(f'\tld.w\t{P},{T0}')
w(f'\tld.w\t{T1},0')
w(f'\txld.w\t[{TB}+{OFF["BIAS"]}],{T1}')
w(f'\tcmp\t{LEFT},0')
w('\tjrgt\t.Lcpu')
w(f'\tld.w\t{T0},0')
w('\tjp\t.Lexit')
w('')

hot_code = []
hc = hot_code.append
hc('\t.section .ivram_code,"ax"')
hc('\t.align\t1')
hc('.Lsetpc:')
if CHECK:
    hc(f'\txld.w\t{T2},[{TB}+{OFF["STEPS"]}]')
    hc(f'\tadd\t{T2},1')
    hc(f'\txld.w\t[{TB}+{OFF["STEPS"]}],{T2}')
hc('.Lsetpc_entry:')
hc(f'\tsub\t{LEFT},{T1}')
hc(f'\tld.uh\t{T0},{T0}')
hc(f'\tld.w\t{T2},{T0}')
hc(f'\tsrl\t{T2},12')
hc(f'\tsll\t{T2},2')
hc(f'\tadd\t{T2},{TB}')
hc(f'\txld.w\t{P},[{T2}+{OFF["READ"]}]')
hc(f'\tcmp\t{P},0')
hc('\txjreq\t.Lsetpc_unmapped')
hc(f'\txld.w\t[{TB}+{OFF["BIAS"]}],{P}')
hc(f'\txld.w\t{T1},[{T2}+{OFF["RSIZE"]}]')
hc(f'\txld.w\t[{TB}+{OFF["SIZE"]}],{T1}')
hc(f'\txld.w\t{PEND},[{T2}+{OFF["REND"]}]')
hc(f'\tadd\t{PEND},{P}')
hc(f'\tsub\t{PEND},2')
hc('.Lsetpc_go:')                              # P the region's bias
hc(f'\tadd\t{P},{T0}')
hc(f'\tld.ub\t{T0},[{P}]+')
hc(f'\tsll\t{T0},2')
hc(f'\tadd\t{T0},{TB}')
hc(f'\tld.w\t{T0},[{T0}]')
hc(f'\tcmp\t{LEFT},0')
hc('\txjrle\t.Lspent_r')
hc(f'\tcmp\t{P},{PEND}')
hc('\txjrugt\t.Lbound_r')
hc(f'\tjp\t{T0}')
# The idle loop, fast-forwarded (see idle_skip): the jr at P - 1, the ldh at
# P - 4, the and at P - 2, 28 cycles a pass, the machine the same at every
# instruction boundary.  Whole passes are charged at once and the last one
# stepped, to find the instruction each event falls after; the event runs,
# and the loop goes on from there until gb_hot_event reports an interrupt
# or the end of the frame.  T1 is where the loop stands: 0 before the jr,
# 1 before the ldh, 2 before the and.
hc('.Lidle:')
hc(f'\tld.w\t{T1},0')
hc('.Lidle_passes:')
hc(f'\tld.w\t{T0},{LEFT}')
hc(f'\tsub\t{T0},1')
hc(f'\tcmp\t{T0},28')
hc('\tjrlt\t.Lidle_step')
hc(f'\txld.w\t{T2},153391690')                 # k = (L - 1) / 28
hc(f'\tmltu.w\t{T0},{T2}')
hc(f'\tld.w\t{T0},%ahr')
if CHECK:
    hc(f'\txld.w\t{T2},[{TB}+{OFF["STEPS"]}]')
    hc(f'\tadd\t{T2},{T0}')
    hc(f'\tadd\t{T2},{T0}')
    hc(f'\tadd\t{T2},{T0}')
    hc(f'\txld.w\t[{TB}+{OFF["STEPS"]}],{T2}')
hc(f'\tld.w\t{T2},{T0}')
hc(f'\tsll\t{T2},5')
hc(f'\tsll\t{T0},2')
hc(f'\tsub\t{T2},{T0}')                        # 28k
hc(f'\tsub\t{LEFT},{T2}')
hc('.Lidle_step:')                               # one instruction: 12, 12, 4
if CHECK:
    hc(f'\txld.w\t{T2},[{TB}+{OFF["STEPS"]}]')
    hc(f'\tadd\t{T2},1')
    hc(f'\txld.w\t[{TB}+{OFF["STEPS"]}],{T2}')
hc(f'\tcmp\t{T1},2')
hc('\tjreq\t.Lidle_and')
hc(f'\tsub\t{LEFT},12')
hc(f'\tadd\t{T1},1')
hc('\tjp\t.Lidle_charged')
hc('.Lidle_and:')
hc(f'\tsub\t{LEFT},4')
hc(f'\tld.w\t{T1},0')
hc('.Lidle_charged:')
hc(f'\tcmp\t{LEFT},0')
hc('\tjrgt\t.Lidle_step')
hc(f'\txld.w\t[{TB}+{OFF["PARK"]}],{T1}')     # the call takes r12-r14
hc(f'\txld.w\t[{TB}+{OFF["LEFT"]}],{LEFT}')
hc('\tpushn\t%r11')
hc(f'\txld.w\t{T0},gb_hot_event')
hc(f'\tcall\t{T0}')
hc('\tpopn\t%r11')
hc(f'\txld.w\t{LEFT},[{TB}+{OFF["LEFT"]}]')
hc(f'\txld.w\t{T1},[{TB}+{OFF["PARK"]}]')
hc(f'\txld.w\t{T0},[{TB}+{OFF["GO"]}]')
hc(f'\tcmp\t{T0},0')
hc('\tjrne\t.Lidle_passes')
# Leave at the instruction the loop stands before: the jr at P - 1, the
# ldh at P - 4, the and at P - 2.
hc(f'\tsub\t{P},1')
hc(f'\tcmp\t{T1},0')
hc('\txjreq\t.Lleave_after_event')
hc(f'\tsub\t{P},1')
hc(f'\tcmp\t{T1},2')
hc('\txjreq\t.Lleave_after_event')
hc(f'\tsub\t{P},2')
hc('\txjp\t.Lleave_after_event')
hc('')
# The budget is spent at an event.  C runs it (gb_hot_event) with every
# register pushed, and says whether to go on -- the usual case -- or to
# leave for an interrupt or the end of the frame.  Leaving and coming back
# in cost far more than the event: two or three of them a scanline.
hc('.Lspent_r:')
hc(f'\tsub\t{P},1')                             # the next opcode, unrun
hc(f'\txld.w\t[{TB}+{OFF["LEFT"]}],{LEFT}')
hc('\tpushn\t%r11')
hc(f'\txld.w\t{T0},gb_hot_event')
hc(f'\tcall\t{T0}')
hc('\tpopn\t%r11')
hc(f'\txld.w\t{LEFT},[{TB}+{OFF["LEFT"]}]')
hc(f'\txld.w\t{T0},[{TB}+{OFF["GO"]}]')
hc(f'\tcmp\t{T0},0')
hc('\txjreq\t.Lleave_after_event')
hc(f'\tld.ub\t{T0},[{P}]+')
hc(f'\tsll\t{T0},2')
hc(f'\tadd\t{T0},{TB}')
hc(f'\tld.w\t{T0},[{T0}]')
hc(f'\tcmp\t{P},{PEND}')
hc('\txjrugt\t.Lbound_r')
hc(f'\tjp\t{T0}')
hc('')

# Placement: the heaviest handlers by opcode-weights.txt go in on-chip RAM
# -- the LCD window buffer in IVRAM after the table, then what A0 RAM the
# renderer leaves -- and the rest in SDRAM, where a dispatch costs a
# landing of 20-30 cycles instead of 6.  Each section gets exit stubs
# every so often so every dispatch's two exits are short branches.
def weights():
    here = __file__.rsplit('/', 1)[0] if '/' in __file__ else '.'
    table = {}
    for line in open(f'{here}/opcode-weights.txt'):
        line = line.split('#', 1)[0].split()
        if len(line) == 2:
            table[line[0]] = int(line[1])
    return table


WEIGHT = weights()
# The window buffer and A0 RAM also hold gb.c's code and data there; the
# Makefile names the compiled objects (--c-object) and objdump (--objdump).
# hot.s's own shared code (.Lsetpc, the event stub, the idle loop) takes
# about SHARED of the window buffer, and each section keeps some slack for
# alignment and the exit stubs' rounding.
def c_sections():
    objs = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--c-object=')]
    tool = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--objdump=')), None)
    sizes = {}
    for obj in objs if tool else []:
        import subprocess
        for line in subprocess.run([tool, '-h', obj], capture_output=True,
                                   text=True, check=True).stdout.splitlines():
            p = line.split()
            if len(p) >= 3 and p[0].isdigit():
                sizes[p[1]] = sizes.get(p[1], 0) + int(p[2], 16)
    return sizes


C_SECTIONS = c_sections()
SHARED, SLACK = 300, 40
IVRAM_CODE = 0x1600 - STATE_END - C_SECTIONS.get('.ivram_code', 2300) - SHARED - SLACK
A0_CODE = (0x13c0 - C_SECTIONS.get('.fastcode', 2400)
           - C_SECTIONS.get('.fastbss', 700) - SLACK)

order = []
for c in sorted(handlers, key=lambda c: -WEIGHT.get(f'{c:02x}', 0)):
    order.append((WEIGHT.get(f'{c:02x}', 0), handlers[c]))
    if c == 0xcb:                               # its stages ride with it
        for g in cb_handlers.values():
            order.append((WEIGHT.get('cb', 0), g))

# The default framebuffer after the machine's kilobyte (memory.lds).
FB_CODE = 0x81a00 - 0x80400 - 64
sections = {'.ivram_code': [], '.fastcode': [], '.fbcode': [], '.text': []}
room = {'.ivram_code': IVRAM_CODE, '.fastcode': A0_CODE, '.fbcode': FB_CODE,
        '.text': 1 << 30}
for weight, g in order:
    need = g.size() + 12                        # a share of the stubs
    for name in ('.ivram_code', '.fastcode', '.fbcode', '.text'):
        if weight and room[name] >= need or name == '.text':
            sections[name].append(g)
            room[name] -= need
            break

stub = 0
for name, members in sections.items():
    if name != '.ivram_code':
        hc(f'\t.section {name},"ax"')
        hc('\t.align\t1')
    pending_bytes = 0
    group = []

    def flush():
        global stub
        spent, bound = f'.Ls{stub}', f'.Lb{stub}'
        for g in group:
            hot_code.extend(g.text(spent, bound))
        hc(f'{spent}:')
        hc('\txjp\t.Lspent_r')
        hc(f'{bound}:')
        hc('\txjp\t.Lbound_r')
        stub += 1

    for g in members:
        if pending_bytes + g.size() > 180 and group:
            flush()
            group, pending_bytes = [], 0
        group.append(g)
        pending_bytes += g.size()
    if group:
        flush()
    sys.stderr.write(f'gen-hot: {name}: {len(members)} handlers, '
                     f'{sum(g.size() for g in members)} bytes\n')

sys.stdout.write('\n'.join(out + hot_code) + '\n')
sys.stderr.write(f'gen-hot: {len(handlers)} opcodes, state ends at {STATE_END}\n')
