#source: removed-words.s
#as: -mc33pe
#objdump: -d
#name: C33 PE disassembles removed instructions as data

.*: +file format elf32-c33

Disassembly of section .text:

0+ <.text>:
 +0:\t.*\.short\t0x8b10.*
 +2:\t.*\.short\t0x8f10.*
 +4:\t.*\.short\t0x9310.*
 +6:\t.*\.short\t0x9710.*
 +8:\t.*\.short\t0x9b00.*
 +a:\t.*\.short\t0xb210.*
 +c:\t.*\.short\t0x9610.*
 +e:\t.*\.short\t0x8a10.*
 +10:\t.*\.short\t0x8e10.*
