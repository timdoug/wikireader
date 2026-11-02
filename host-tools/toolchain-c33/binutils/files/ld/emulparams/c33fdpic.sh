# FDPIC ELF for no-MMU C33 Linux: executables and shared libraries whose
# text and data segments the kernel and ld.so place independently.  The
# generic ELF script, as FRV's and Blackfin's FDPIC use it, with C33's
# one requirement: code reaches data as an unsigned offset from %r15,
# which holds __dp, so .got, where __dp is, opens the data segment.
SCRIPT_NAME=elf
OUTPUT_FORMAT="elf32-c33fdpic"
ARCH=c33
MACHINE=
TEXT_START_ADDR=0
# No COMMONPAGESIZE: without an MMU there is no RELRO to page-align, and
# leaving it unset makes ld report -z relro ignored, as FRV's and Blackfin's.
MAXPAGESIZE="CONSTANT (MAXPAGESIZE)"
ENTRY=_start
TEMPLATE_NAME=elf
GENERATE_SHLIB_SCRIPT=yes
GENERATE_PIE_SCRIPT=yes
ELF_INTERPRETER_NAME=\"/lib/ld-uClibc.so.0\"
NO_SMALL_DATA=" "
# .got is placed by INITIAL_DATA_SECTIONS, not where elf.sc puts a GOT.
GOT=" "
INITIAL_DATA_SECTIONS="
  .got          ${RELOCATING-0} :
  {
    ${RELOCATING+HIDDEN (__dp = .);}
    ${RELOCATING+HIDDEN (_GLOBAL_OFFSET_TABLE_ = .);}
    *(.got.plt) *(.got)
  }"
# The words the program relocates itself at startup, by their link-time
# addresses, as FRV's; the last is __dp's.
OTHER_READONLY_SECTIONS="
  .rofixup      ${RELOCATING-0} :
  {
    ${RELOCATING+HIDDEN (__ROFIXUP_LIST__ = .);}
    *(.rofixup)
    ${RELOCATING+HIDDEN (__ROFIXUP_END__ = .);}
  }"
DATA_START_SYMBOLS="PROVIDE (__data_start = .);"
