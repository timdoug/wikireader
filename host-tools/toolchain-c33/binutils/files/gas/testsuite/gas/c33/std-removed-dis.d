#source: removed-words.s
#objdump: -d
#name: C33 STD retains instructions removed only from PE

.*: +file format elf32-c33

Disassembly of section .text:

0+ <.text>:
 +0:\t.*div0s +%r1.*
 +2:\t.*div0u +%r1.*
 +4:\t.*div1 +%r1.*
 +6:\t.*div2s +%r1.*
 +8:\t.*div3s.*
 +a:\t.*mac +%r1.*
 +c:\t.*mirror +%r0,%r1.*
 +e:\t.*scan0 +%r0,%r1.*
 +10:\t.*scan1 +%r0,%r1.*
