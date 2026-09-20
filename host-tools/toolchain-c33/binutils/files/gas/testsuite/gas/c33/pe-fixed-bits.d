#as: -mc33pe
#objdump: -d
#name: C33 PE enforces fixed opcode bits

.*: +file format elf32-c33

Disassembly of section .text:

0+ <.text>:
 +0:\t92 00[ \t]+pushs[ \t]+%alr.*
 +2:\td3 00[ \t]+pops[ \t]+%ahr.*
 +4:\td0 01[ \t]+ld.cf.*
 +6:\t5f bf[ \t]+psrset[ \t]+0x1f.*
 +8:\t9f bf[ \t]+psrclr[ \t]+0x1f.*
 +a:\tb2 00[ \t]+\.short[ \t]+0x00b2
 +c:\tc2 00[ \t]+\.short[ \t]+0x00c2
 +e:\te2 00[ \t]+\.short[ \t]+0x00e2
 +10:\tf2 00[ \t]+\.short[ \t]+0x00f2
 +12:\td1 01[ \t]+\.short[ \t]+0x01d1
 +14:\t60 bf[ \t]+\.short[ \t]+0xbf60
 +16:\ta0 bf[ \t]+\.short[ \t]+0xbfa0
