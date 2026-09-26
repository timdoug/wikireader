#!/usr/bin/env python3
"""Write statpoll.gb: a test ROM that reads STAT and LY at every phase of
the scanline and logs what it saw to work RAM.

The frame hashes cover work RAM, so a C33 build that reports a mode or line
a cycle early or late shows up against the host build (run.py).  It waits a
different number of cycles each pass so the reads walk across the line, and
every 256 passes it toggles the STAT mode 0 interrupt enable, which changes
when the emulator schedules its LCD events.  Interrupts stay disabled.

    statpoll.py OUTPUT
"""
import sys

code = bytearray()
labels, fixups = {}, []


def emit(*b):
    code.extend(b)


def label(name):
    labels[name] = 0x150 + len(code)


def jr(op, name):
    emit(op, 0)
    fixups.append((len(code) - 1, name))


emit(0xf3)                      # di
emit(0x21, 0x00, 0xc0)          # ld hl,$c000
emit(0x06, 0x00)                # ld b,0
label('loop')
emit(0xf0, 0x41)                # ldh a,(STAT)
emit(0x22)                      # ld (hl+),a
emit(0xf0, 0x44)                # ldh a,(LY)
emit(0x22)                      # ld (hl+),a
emit(0x7c)                      # ld a,h
emit(0xfe, 0xe0)                # cp $e0
jr(0x20, 'inside')              # jr nz,inside
emit(0x26, 0xc0)                # ld h,$c0
label('inside')
emit(0x04)                      # inc b
emit(0x78)                      # ld a,b
emit(0xe6, 0x0f)                # and $0f
emit(0x3c)                      # inc a
emit(0x4f)                      # ld c,a
label('wait')
emit(0x0d)                      # dec c
jr(0x20, 'wait')                # jr nz,wait
emit(0x78)                      # ld a,b
emit(0xa7)                      # and a
jr(0x20, 'loop')                # jr nz,loop
emit(0xf0, 0x41)                # ldh a,(STAT)
emit(0xee, 0x08)                # xor $08: mode 0 interrupt enable
emit(0xe0, 0x41)                # ldh (STAT),a
emit(0x18, 0)                   # jr loop
fixups.append((len(code) - 1, 'loop'))

for at, name in fixups:
    code[at] = (labels[name] - (0x150 + at + 1)) & 0xff

rom = bytearray(0x8000)
rom[0x100:0x104] = bytes((0x00, 0xc3, 0x50, 0x01))      # nop; jp $0150
rom[0x134:0x13c] = b'STATPOLL'
rom[0x150:0x150 + len(code)] = code
checksum = 0
for b in rom[0x134:0x14d]:
    checksum = (checksum - b - 1) & 0xff
rom[0x14d] = checksum
open(sys.argv[1], 'wb').write(rom)
