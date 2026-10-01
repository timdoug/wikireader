#!/usr/bin/env python3
"""Static counts behind ABI-v2-proposal.md "Reported static image counts".

Usage: abi-static-counts.py IMAGE.elf [IMAGE.map]

Disassembles IMAGE with the C33 objdump (C33_OBJDUMP, default the toolchain
install) and prints the five measures in the proposal's table.  The map file,
when given, supplies function boundaries for the incoming-stack-argument count;
the images are stripped, so nothing else has them.

Classification rules.  All are static: they bound occurrence, not frequency.

* Call instructions: mnemonics call, call.d, xcall, scall, scall.d.  Tail
  calls are jumps and cannot be told from other jumps in stripped code, so
  they are not counted here.
* Descriptor loads: an `xld.w %r5,imm` whose first ext prefix is
  0x1860..0x187f, i.e. an immediate with top byte 0xc3: the __builtin_apply
  descriptor.  `xld.w %r5,0xc3000000` is the no-stack-words case.  The compiler
  emits one before every call and tail call, and the scheduler may hoist it
  well ahead of the transfer, so the count can exceed the call count.
* Prologue: a `pushn %rN` with N <= 3 starts a function.  A function extends
  to the next such pushn, so a following leaf without a prologue is merged
  into it.  That over-counts register use, so "body never touches" is a
  lower bound on unnecessary saves.
* Body never touches: among %r0..%r2 (with pushn %r3), the registers that
  appear nowhere in the segment other than the popn.  Implicit uses (none in
  compiler output) are not modelled.
* Incoming stack words (needs the map): function boundaries are the map's
  symbols plus, inside them, every `pushn %r0..3` prologue and every
  `sub %sp` directly after a `ret`, since static functions are not in the
  map.  Per function, the frame is 4*(N+1) for a leading pushn, plus 4*M
  after the function's single `sub %sp,M`, which the scheduler may place
  after early loads or in a delay slot.  A `ld.w %rX,[%sp+K]` with
  4K >= frame+4 reads incoming stack word (4K-frame-4)/4+1.  Functions with
  two `sub %sp` or a `ld.w %sp,...` (alloca, frame pointer, hand assembly)
  are skipped.
* Padding-only frames: a segment with one `sub %sp,M`, M <= 3, that never
  loads or stores below byte 4M of the frame (byte and halfword forms scale
  the offset by their size) and never copies %sp into a register, which
  would be an address-taken local.
"""
import bisect
import collections
import hashlib
import os
import re
import subprocess
import sys

INS = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(\S+)\s*(.*?)(?:\s{2,}\S.*)?$')
SYM = re.compile(r'^\s+0x([0-9a-f]{8,16})\s+([A-Za-z_$][\w.$]*)\s*$')
CALLS = ('call', 'xcall', 'call.d', 'scall', 'scall.d')
OBJDUMP = os.environ.get(
    'C33_OBJDUMP',
    os.path.join(os.path.dirname(__file__), '..', '..', 'work', 'install',
                 'bin', 'c33-epson-elf-objdump'))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def disassemble(image):
    out = subprocess.run([OBJDUMP, '-d', image], capture_output=True,
                         text=True, errors='replace', check=True).stdout
    ins = []
    for line in out.splitlines():
        m = INS.match(line)
        if m:
            ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return ins


def text_range(image):
    out = subprocess.run([OBJDUMP, '-h', image], capture_output=True,
                         text=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) > 3 and f[1] == '.text':
            return int(f[3], 16), int(f[3], 16) + int(f[2], 16)
    raise SystemExit('no .text section')


SIZE = {'ld.b': 1, 'ld.ub': 1, 'ld.h': 2, 'ld.uh': 2, 'ld.w': 4}


def frame_touched(seg, nbytes):
    """True if any instruction reads or writes [%sp+off] with off < nbytes,
    or takes %sp's value (an address-taken local)."""
    for _, op, a in seg:
        if op in SIZE:
            m = re.search(r'\[%sp\+0x([0-9a-f]+)\]', a)
            if m and SIZE[op] * int(m.group(1), 16) < nbytes:
                return True
            if '[%sp]' in a or re.search(r',%sp$', a.strip()):
                return True
        elif op == 'add' and re.search(r',%sp$', a.strip()):
            return True
    return False


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    image = sys.argv[1]
    mapfile = sys.argv[2] if len(sys.argv) == 3 else None
    ins = disassemble(image)
    print(f'{image}  sha256 {sha256(image)}')
    print(subprocess.run([OBJDUMP, '--version'], capture_output=True,
                         text=True).stdout.splitlines()[0])

    calls = sum(1 for _, op, _ in ins if op in CALLS)
    desc = zero = 0
    for i in range(2, len(ins)):
        if (ins[i][1] == 'ld.w' and ins[i][2].startswith('%r5,0x')
                and ins[i - 1][1] == 'ext' and ins[i - 2][1] == 'ext'
                and 0x1860 <= int(ins[i - 2][2], 16) <= 0x187f):
            desc += 1
            if ins[i - 2][2] == '0x1860' and ins[i - 1][2] == '0x0' \
                    and ins[i][2] == '%r5,0x0':
                zero += 1
    print(f'r5 descriptor loads: {desc} ({zero} for calls with no stack words); '
          f'call instructions: {calls}')

    starts = [i for i, (_, op, a) in enumerate(ins)
              if op == 'pushn' and re.fullmatch(r'%r[0-3]', a.strip())]
    hist = collections.Counter()
    wasted = 0
    padding = 0
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(ins)
        n = int(ins[s][2].strip()[2:])
        hist[n] += 1
        body = ' '.join(a for _, op, a in ins[s + 1:e] if op != 'popn')
        if n == 3:
            used = [r for r in ('%r0', '%r1', '%r2')
                    if re.search(re.escape(r) + r'(?![0-9])', body)]
            if len(used) < 3:
                wasted += 1
        seg = ins[s + 1:e]
        subs = [x for x in seg if x[1] == 'sub' and x[2].startswith('%sp,0x')]
        if len(subs) == 1:
            words = int(subs[0][2].split('0x')[1], 16)
            if words <= 3 and not frame_touched(seg, 4 * words):
                padding += 1
    total = sum(hist.values())
    print(f'prologues that are pushn %r3: {hist[3]} of {total}  '
          f'(pushn histogram {dict(sorted(hist.items()))})')
    print(f'of those, saving a register the body never touches: at least {wasted}')
    print(f'frames that exist only for sixteen-byte padding: {padding} of {total}')

    if not mapfile:
        return
    lo, hi = text_range(image)
    syms = {}
    for line in open(mapfile, errors='replace'):
        m = SYM.match(line)
        if m and lo <= int(m.group(1), 16) < hi:
            syms.setdefault(int(m.group(1), 16), m.group(2))
    for k, (a, op, arg) in enumerate(ins):
        if op == 'pushn' and re.fullmatch(r'%r[0-3]', arg.strip()):
            syms.setdefault(a, f'.L{a:x}')
        elif op == 'sub' and arg.startswith('%sp,0x') and k > 0 \
                and ins[k - 1][1] in ('ret', 'ret.d'):
            syms.setdefault(a, f'.L{a:x}')
    addrs = sorted(syms)
    funcs = collections.defaultdict(list)
    for a, op, arg in ins:
        i = bisect.bisect_right(addrs, a) - 1
        if lo <= a < hi and i >= 0:
            funcs[addrs[i]].append((op, arg))
    analysed = reading = skipped = 0
    words_hist = collections.Counter()
    for f in funcs.values():
        base = 4 * (int(f[0][1].strip()[2:]) + 1) if f and f[0][0] == 'pushn' else 0
        subs = [k for k, (op, a) in enumerate(f) if op == 'sub' and a.startswith('%sp,')]
        if len(subs) > 1 or any(op == 'ld.w' and a.startswith('%sp,') for op, a in f):
            skipped += 1
            continue
        analysed += 1
        words = set()
        for k, (op, a) in enumerate(f):
            m = re.match(r'%r\d+,\[%sp\+0x([0-9a-f]+)\]', a)
            if op == 'ld.w' and m:
                frame = base
                if subs and k > subs[0]:
                    frame += 4 * int(f[subs[0]][1].split('0x')[1], 16)
                off = 4 * int(m.group(1), 16)
                if off >= frame + 4:
                    words.add((off - frame - 4) // 4 + 1)
        if words:
            reading += 1
            words_hist[max(words)] += 1
    print(f'functions reading incoming stack argument words: {reading} of {analysed}  '
          f'(skipped {skipped}; max-words histogram {dict(sorted(words_hist.items()))})')


if __name__ == '__main__':
    main()
