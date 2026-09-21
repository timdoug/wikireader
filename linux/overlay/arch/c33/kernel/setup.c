// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/start_kernel.h>
#include <linux/string.h>

#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/wikireader.h>

#define C33_WATCHDOG_WRITE_PROTECT ((volatile u16 *)0x00300660UL)
#define C33_WATCHDOG_ENABLE        ((volatile u16 *)0x00300662UL)

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

void __init setup_arch(char **cmdline_p)
{
	memory_start = PAGE_ALIGN((unsigned long)_end);
	memory_end = CONFIG_PHYSICAL_START + CONFIG_C33_MEMORY_SIZE;

	setup_initial_init_mm(_stext, _etext, _edata, _end);
	memblock_add(CONFIG_PHYSICAL_START, CONFIG_C33_MEMORY_SIZE);
	memblock_reserve(CONFIG_PHYSICAL_START,
			 memory_start - CONFIG_PHYSICAL_START);

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
