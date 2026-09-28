# FDPIC ELF for no-MMU C33 Linux: executables and shared libraries whose
# text and data segments the kernel places independently.
SCRIPT_NAME=c33fdpic
OUTPUT_FORMAT="elf32-c33fdpic"
ARCH=c33
MACHINE=
MAXPAGESIZE=0x1000
COMMONPAGESIZE=0x1000
ENTRY=_start
TEMPLATE_NAME=elf
GENERATE_SHLIB_SCRIPT=yes
GENERATE_PIE_SCRIPT=yes
ELF_INTERPRETER_NAME=\"/lib/ld-uClibc.so.0\"
