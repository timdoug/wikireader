/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/page.h>
#include <asm/thread_info.h>
#include <uapi/asm/tls.h>
#include <asm/iram.h>
#include <asm-generic/vmlinux.lds.h>

OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)

jiffies = jiffies_64;
c33_current_tls = C33_TLS_SLOT_ADDRESS;

SECTIONS
{
	. = CONFIG_C33_KERNEL_START;
	__dp = CONFIG_PHYSICAL_START;

	_text = .;
	_stext = .;
	HEAD_TEXT_SECTION

	/*
	 * A0 RAM (asm/iram.h): the hot code and data, linked where they run,
	 * carried here and copied into place by head.S.  They come before
	 * .text and .data so that their lists, made from profiles by
	 * linux/a0-hot.py, claim those input sections first.  .a0.data ends
	 * with a word of its own so that it has contents to copy even when
	 * everything listed is zero-initialised.
	 */
	. = ALIGN(4);
	__a0_text_load = .;
	.a0.text C33_A0_CODE_START : AT(__a0_text_load) {
		__a0_text_start = .;
		__iram_text_start = .;
		*(.iram.text)
		__iram_text_end = .;
#include "a0-text.lds"
		. = ALIGN(4);
		__a0_text_end = .;
	}
	__a0_data_load = __a0_text_load + SIZEOF(.a0.text);
	.a0.data C33_A0_DATA_START : AT(__a0_data_load) {
		__a0_data_start = .;
#include "a0-data.lds"
		LONG(0)
		. = ALIGN(4);
		__a0_data_end = .;
	}
	. = __a0_data_load + SIZEOF(.a0.data);
	ASSERT(__a0_text_end <= C33_A0_CODE_END, "A0 RAM's code is too big")
	ASSERT(__a0_data_end <= C33_A0_DATA_END, "A0 RAM's data is too big")

	/* Loaded where it runs again, as everything after it is. */
	.text : AT(ADDR(.text)) {
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
