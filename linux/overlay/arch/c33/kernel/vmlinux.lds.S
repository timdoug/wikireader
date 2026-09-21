/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/page.h>
#include <asm/thread_info.h>
#include <asm-generic/vmlinux.lds.h>

OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)

jiffies = jiffies_64;

SECTIONS
{
	. = CONFIG_C33_KERNEL_START;
	__dp = CONFIG_PHYSICAL_START;

	_text = .;
	_stext = .;
	HEAD_TEXT_SECTION
	.text : {
		/*
		 * S1C33 scall has a finite PC-relative range.  Keep cold text
		 * between ordinary text and init text instead of at the start of
		 * the image, so both sets of callers remain in range as the kernel
		 * grows.
		 */
		ALIGN_FUNCTION();
		*(.text.asan.* .text.tsan.*)
		*(.text.unknown .text.unknown.*)
		TEXT_SPLIT
		. = ALIGN(PAGE_SIZE);
		TEXT_HOT
		*(TEXT_MAIN .text.fixup)
		NOINSTR_TEXT
		*(.ref.text)
		TEXT_UNLIKELY
		SCHED_TEXT
		LOCK_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
	} = 0
	_etext = .;

	EXCEPTION_TABLE(4)
	. = ALIGN(1024);
	.vectors : { KEEP(*(.vectors)) }

	. = ALIGN(PAGE_SIZE);
	__init_begin = .;
	INIT_TEXT_SECTION(PAGE_SIZE)
	INIT_DATA_SECTION(PAGE_SIZE)
	PERCPU_SECTION(4)
	__init_end = .;

	_sdata = .;
	RO_DATA(PAGE_SIZE)
	RW_DATA(4, PAGE_SIZE, THREAD_SIZE)
	_edata = .;

	BSS_SECTION(0, 0, 0)
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	ELF_DETAILS
	DISCARDS
}
