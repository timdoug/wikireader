// SPDX-License-Identifier: GPL-2.0
#include <linux/console.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <linux/tty_driver.h>

#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/wikireader.h>

unsigned long memory_start;
unsigned long memory_end;
EXPORT_SYMBOL(memory_start);
EXPORT_SYMBOL(memory_end);

extern void paging_init(void);
asmlinkage __visible void __init c33_start(void);
void __init arch_cpu_finalize_init(void);
struct tty_driver *c33_console_device(struct console *console, int *index);

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

static void c33_console_write(struct console *console, const char *s,
			      unsigned int count)
{
	const char *start = s;
	unsigned int original_count = count;

	(void)console;
	while (count--) {
		if (*s == '\n')
			early_uart_putc('\r');
		early_uart_putc(*s++);
	}
	c33_lcd_write(start, original_count);
}

static struct console c33_console = {
	.name = "ttyC33",
	.write = c33_console_write,
	.device = c33_console_device,
	.flags = CON_PRINTBUFFER | CON_ENABLED,
	.index = 0,
};

asmlinkage __visible void __init c33_start(void)
{
	unsigned long *p;

	for (p = (unsigned long *)__bss_start;
	     p < (unsigned long *)__bss_stop; p++)
		*p = 0;

	c33_lcd_init();
	early_uart_puts("\r\nC33 Linux: entry\r\n");
	start_kernel();
}

void __init setup_arch(char **cmdline_p)
{
	register_console(&c33_console);

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
	c33_lcd_checkpoint(1);
}

void __init arch_cpu_finalize_init(void)
{
}
