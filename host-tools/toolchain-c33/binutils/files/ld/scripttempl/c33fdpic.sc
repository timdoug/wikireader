# Linker script for C33 FDPIC executables and shared libraries.
#
# Two segments, which the kernel and ld.so place independently: text (the
# headers, dynamic linking tables, code, read-only data and the .rofixup
# list), then data.  -mfdpic code reaches its data segment through %r15,
# which holds the address of __dp, the first byte of that segment: the
# linker's .got comes first, then everything else the code addresses as
# [%r15 + doff], which cannot be negative.
#
# Copyright (C) 2026 Free Software Foundation, Inc.
#
# Copying and distribution of this file, with or without modification,
# are permitted in any medium without royalty provided the copyright
# notice and this notice are preserved.

# A final link drops the stack note, which only says what the code
# needs; ld -r keeps one, merged from all of them.
STACKNOTE=".note.GNU-stack 0 : { *(.note.GNU-stack) }"
# -Ttext-segment moves the whole image, headers and all.
TEXT_START="SEGMENT_START(\"text-segment\", 0)"
# -z separate-code gives the code pages of its own, which no other
# section shares; off by default, as it costs up to two pages.
CODE_ALIGN=
case "$LD_FLAG" in
  *textonly*) CODE_ALIGN=". = ALIGN(${MAXPAGESIZE});" ;;
esac

cat <<EOF
/* Copyright (C) 2026 Free Software Foundation, Inc.

   Copying and distribution of this script, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.  */

OUTPUT_FORMAT("${OUTPUT_FORMAT}")
OUTPUT_ARCH(${ARCH})
${RELOCATING+ENTRY(${ENTRY})}
${RELOCATING+${LIB_SEARCH_DIRS}}

SECTIONS
{
  ${RELOCATING+. = ${TEXT_START} + SIZEOF_HEADERS;}
  .note.gnu.build-id : { *(.note.gnu.build-id) }
  .interp         : { *(.interp) }
  .hash           : { *(.hash) }
  .gnu.hash       : { *(.gnu.hash) }
  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_d  : { *(.gnu.version_d) }
  .gnu.version_r  : { *(.gnu.version_r) }
  .rela.dyn       : { *(.rela.dyn) }
  .rela.plt       : { *(.rela.plt) }
  ${RELOCATING+${CODE_ALIGN}}
  .plt            : { *(.plt) }
  .text           :
  {
    ${RELOCATING+*(.text.unlikely .text.*_unlikely .text.unlikely.*)}
    ${RELOCATING+*(.text.exit .text.exit.*)}
    ${RELOCATING+*(.text.startup .text.startup.*)}
    ${RELOCATING+*(.text.hot .text.hot.*)}
    *(.text${RELOCATING+ .stub .text.* .gnu.linkonce.t.*})
  }
  .init           : { ${RELOCATING+KEEP (*(SORT_NONE(.init)))} ${RELOCATING-*(.init)} }
  .fini           : { ${RELOCATING+KEEP (*(SORT_NONE(.fini)))} ${RELOCATING-*(.fini)} }
  ${RELOCATING+${CODE_ALIGN}}
  .rodata         : { *(.rodata${RELOCATING+ .rodata.* .gnu.linkonce.r.*}) }
  .rodata1        : { *(.rodata1) }
  /* Unrelocated link-time search entries are shared with the text.  The
     FDPIC unwinder maps the selected frame in the separate data segment. */
  .eh_frame_hdr   : { *(.eh_frame_hdr) }
  /* The words the program relocates itself at startup, by their link-time
     addresses; the last is __dp's.  Read-only, and it keeps text and data
     apart for the load map lookup of a one-past-the-end address.  */
  .rofixup        :
  {
    ${RELOCATING+HIDDEN (__ROFIXUP_LIST__ = .);}
    *(.rofixup)
    ${RELOCATING+HIDDEN (__ROFIXUP_END__ = .);}
  }

  /* The data segment continues at the same offset in the next page, so the
     file needs no padding between them.  */
  ${RELOCATING+. = ALIGN(${MAXPAGESIZE}) + (. & (${MAXPAGESIZE} - 1));}
  .got            :
  {
    ${RELOCATING+HIDDEN (__dp = .);}
    ${RELOCATING+HIDDEN (_GLOBAL_OFFSET_TABLE_ = .);}
    *(.got.plt) *(.got)
  }
  .data.rel.ro    : { *(.data.rel.ro.local${RELOCATING+ .data.rel.ro.local.*}) *(.data.rel.ro${RELOCATING+ .data.rel.ro.*}) }
  .preinit_array  :
  {
    ${RELOCATING+PROVIDE_HIDDEN (__preinit_array_start = .);}
    KEEP (*(.preinit_array))
    ${RELOCATING+PROVIDE_HIDDEN (__preinit_array_end = .);}
  }
  .init_array     :
  {
    ${RELOCATING+PROVIDE_HIDDEN (__init_array_start = .);}
    ${RELOCATING+KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))}
    KEEP (*(.init_array${RELOCATING+ .ctors}))
    ${RELOCATING+PROVIDE_HIDDEN (__init_array_end = .);}
  }
  .fini_array     :
  {
    ${RELOCATING+PROVIDE_HIDDEN (__fini_array_start = .);}
    ${RELOCATING+KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))}
    KEEP (*(.fini_array${RELOCATING+ .dtors}))
    ${RELOCATING+PROVIDE_HIDDEN (__fini_array_end = .);}
  }
  .dynamic        : { *(.dynamic) }
  /* The ELF TLS template has a PT_TLS header.  __tdata_start lets the
     static libc find its initialized image after FDPIC relocation. */
  .tdata          :
  {
    ${RELOCATING+PROVIDE_HIDDEN (__tdata_start = .);}
    *(.tdata${RELOCATING+ .tdata.* .gnu.linkonce.td.*})
  }
  .tbss           : { *(.tbss${RELOCATING+ .tbss.* .gnu.linkonce.tb.*}) *(.tcommon) }
  .data           :
  {
    ${RELOCATING+PROVIDE (__data_start = .);}
    *(.data${RELOCATING+ .data.* .gnu.linkonce.d.*})
    *(.sdata${RELOCATING+ .sdata.*})
  }
  /* The unwind tables hold absolute addresses, so they are data, where
     the loader relocates them.  */
  .gcc_except_table : { *(.gcc_except_table${RELOCATING+ .gcc_except_table.*}) }
  .tm_clone_table : { *(.tm_clone_table) }
  .eh_frame       : { KEEP (*(.eh_frame)) }
  ${RELOCATING+_edata = .; PROVIDE (edata = .);}
  .bss            :
  {
    ${RELOCATING+__bss_start = .;}
    *(.dynbss)
    *(.bss${RELOCATING+ .bss.* .gnu.linkonce.b.*})
    *(.sbss${RELOCATING+ .sbss.*})
    *(COMMON)
    ${RELOCATING+. = ALIGN(4);}
  }
  ${RELOCATING+_end = .; PROVIDE (end = .);}

  .comment        0 (INFO) : { *(.comment); LINKER_VERSION; }
  ${RELOCATING-${STACKNOTE}}
EOF

source_sh $srcdir/scripttempl/DWARF.sc

cat <<EOF
  .gnu.attributes 0 : { KEEP (*(.gnu.attributes)) }
  /DISCARD/ : { ${RELOCATING+*(.note.GNU-stack)} *(.gnu_debuglink) *(.gnu.lto_*) }
}
EOF
