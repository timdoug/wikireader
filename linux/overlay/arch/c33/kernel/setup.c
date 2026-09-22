// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/start_kernel.h>
#include <linux/string.h>

#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/wikireader.h>

#define C33_WATCHDOG_WRITE_PROTECT ((volatile u16 *)0x00300660UL)
#define C33_WATCHDOG_ENABLE        ((volatile u16 *)0x00300662UL)
#define C33_SDRAMC_CTL             ((volatile u32 *)0x00301604UL)
#define C33_SDRAMC_ADDRC_MASK      7

unsigned long memory_start;
unsigned long memory_end;
EXPORT_SYMBOL(memory_start);
EXPORT_SYMBOL(memory_end);

extern void paging_init(void);
asmlinkage __visible void __init c33_start(void);
void __init arch_cpu_finalize_init(void);

static void early_uart_putc(char c)
{
	volatile unsigned char *tx = (void *)0x00300b00;
	volatile unsigned char *status = (void *)0x00300b02;

	while (!(*status & 2))
		;
	*tx = c;
}

static void early_uart_puts(const char *s)
{
	while (*s)
		early_uart_putc(*s++);
}

asmlinkage __visible void __init c33_start(void)
{
	unsigned long *p;

	for (p = (unsigned long *)__bss_start;
	     p < (unsigned long *)__bss_stop; p++)
		*p = 0;

	/* Grifo arms a 20-second watchdog before starting an application. */
	*C33_WATCHDOG_WRITE_PROTECT = 0x96;
	*C33_WATCHDOG_ENABLE = 0;
	*C33_WATCHDOG_WRITE_PROTECT = 0;

	c33_lcd_init();
	early_uart_puts("\r\nC33 Linux: entry\r\n");
	start_kernel();
}

/*
 * Board revisions differ: early WikiReaders carry 32 MiB of SDRAM, production
 * rev 6 and V4 boards carry 16 MiB and alias the upper half.  The address
 * configuration the loader programmed into the SDRAM controller is the only
 * description of memory this machine has, so read it rather than trusting a
 * build-time size.
 */
static unsigned long __init c33_ram_size(void)
{
	static const unsigned long addrc_size[8] __initconst = {
		SZ_2M,  /*  1M x 16 bits x 1 */
		SZ_8M,  /*  4M x 16 bits x 1 */
		SZ_16M, /*  8M x 16 bits x 1 */
		SZ_32M, /* 16M x 16 bits x 1 */
		SZ_4M,  /*  2M x  8 bits x 2 */
		SZ_16M, /*  8M x  8 bits x 2 */
		SZ_32M, /* 16M x  8 bits x 2 */
		SZ_64M, /* 32M x 16 bits x 1 */
	};

	return addrc_size[*C33_SDRAMC_CTL & C33_SDRAMC_ADDRC_MASK];
}

void __init setup_arch(char **cmdline_p)
{
	unsigned long size = c33_ram_size();

	memory_start = PAGE_ALIGN((unsigned long)_end);
	if (memory_start - CONFIG_PHYSICAL_START > size) {
		/* The kernel itself does not fit: say so before faulting. */
		early_uart_puts("\r\nC33 Linux: SDRAM too small for kernel\r\n");
		size = CONFIG_C33_MEMORY_SIZE;
	}
	memory_end = CONFIG_PHYSICAL_START + size;

	setup_initial_init_mm(_stext, _etext, _edata, _end);
	memblock_add(CONFIG_PHYSICAL_START, size);
	memblock_reserve(CONFIG_PHYSICAL_START,
			 memory_start - CONFIG_PHYSICAL_START);
	pr_info("C33 memory: %lu MiB of SDRAM at %08x\n",
		size >> 20, CONFIG_PHYSICAL_START);

	strscpy(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
	*cmdline_p = boot_command_line;

	min_low_pfn = PFN_UP(memory_start);
	max_pfn = max_low_pfn = PFN_DOWN(memory_end);
	paging_init();
	c33_lcd_console_register();
	c33_lcd_checkpoint(1);
}

void __init arch_cpu_finalize_init(void)
{
}
