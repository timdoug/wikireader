#as: -mc33pe
#objdump: -d
#name: C33 PE retains documented instructions

.*: +file format elf32-c33

Disassembly of section .text:

0+ <.text>:
 +0:\t.*jpr +%r1.*
 +2:\t.*swaph +%r0,%r1.*
 +4:\t.*adc +%r0,%r1.*
 +6:\t.*sbc +%r0,%r1.*
 +8:\t.*ld.w +%pc,%r0.*
