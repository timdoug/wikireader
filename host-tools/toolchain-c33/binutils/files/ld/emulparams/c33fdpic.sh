# FDPIC ELF for no-MMU C33 Linux: executables and shared libraries whose
# text and data segments the kernel places independently.
SCRIPT_NAME=c33fdpic
OUTPUT_FORMAT="elf32-c33fdpic"
ARCH=c33
MACHINE=
# No COMMONPAGESIZE: without an MMU there is no RELRO to page-align, and
# leaving it unset makes ld report -z relro ignored, as FRV's and Blackfin's.
MAXPAGESIZE="CONSTANT (MAXPAGESIZE)"
ENTRY=_start
TEMPLATE_NAME=elf
GENERATE_SHLIB_SCRIPT=yes
GENERATE_PIE_SCRIPT=yes
ELF_INTERPRETER_NAME=\"/lib/ld-uClibc.so.0\"
