#as: -mc33pe
#objdump: -d
#name: C33 backtick statement separator and .previous

.*: +file format elf32-c33

Disassembly of section .text:

0+ <separator_a>:
 +0:\t.*swaph +%r0,%r1.*

0+2 <separator_b>:
 +2:\t.*adc +%r0,%r1.*
