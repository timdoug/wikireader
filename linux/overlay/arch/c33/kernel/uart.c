// SPDX-License-Identifier: GPL-2.0
#include <linux/console.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/tty_port.h>

#include <asm/wikireader.h>

#define C33_UART0_TXD       0x00300b00UL
#define C33_UART0_RXD       0x00300b01UL
#define C33_UART0_STATUS    0x00300b02UL
#define C33_UART0_CTL       0x00300b03UL
#define C33_UART0_IRDA      0x00300b04UL
#define C33_UART0_BRTRUN    0x00300b05UL
#define C33_UART0_BRTRDL    0x00300b06UL
#define C33_UART0_BRTRDM    0x00300b07UL
#define C33_UART0_IRQ_PRIO  0x0030026aUL
#define C33_UART0_IRQ_EN    0x00300276UL
#define C33_UART0_IRQ_FLAGS 0x00300286UL
#define C33_UART_RX_READY   1
#define C33_UART_TX_READY   2
#define C33_UART_RX_IRQ     (1 << 1)
#define C33_UART_BAUD       115200UL

static struct tty_driver *c33_tty_driver;
static struct tty_port c33_tty_port;
static bool c33_tty_ready;
static bool c33_tty_opened;

struct tty_driver *c33_console_device(struct console *console, int *index);
void c33_uart_rx_interrupt(void);

static void c33_uart_hw_init(void)
{
	unsigned long divisor =
		(c33_mclk_hz() + C33_UART_BAUD * 8) /
		(C33_UART_BAUD * 16) - 1;

	*(volatile unsigned char *)C33_UART0_CTL = 0xcb;
	*(volatile unsigned char *)C33_UART0_IRDA = 0x10;
	*(volatile unsigned char *)C33_UART0_BRTRUN = 0;
	*(volatile unsigned char *)C33_UART0_BRTRDM = divisor >> 8;
	*(volatile unsigned char *)C33_UART0_BRTRDL = divisor;
	*(volatile unsigned char *)C33_UART0_BRTRUN = 1;
	*(volatile unsigned char *)C33_UART0_IRQ_FLAGS = 7;
	*(volatile unsigned char *)C33_UART0_IRQ_PRIO =
		(*(volatile unsigned char *)C33_UART0_IRQ_PRIO & 0x8f) | 0x50;
}

static void c33_uart_putc(unsigned char ch)
{
	volatile unsigned char *tx = (void *)C33_UART0_TXD;
	volatile unsigned char *status = (void *)C33_UART0_STATUS;

	while (!(*status & C33_UART_TX_READY))
		cpu_relax();
	*tx = ch;
}

static int c33_tty_open(struct tty_struct *tty, struct file *file)
{
	int ret;

	tty->driver_data = &c33_tty_port;
	ret = tty_port_open(&c33_tty_port, tty, file);
	if (!ret && !READ_ONCE(c33_tty_opened)) {
		WRITE_ONCE(c33_tty_opened, true);
		*(volatile unsigned char *)C33_UART0_IRQ_FLAGS = C33_UART_RX_IRQ;
		*(volatile unsigned char *)C33_UART0_IRQ_EN |= C33_UART_RX_IRQ;
	}
	return ret;
}

static void c33_tty_close(struct tty_struct *tty, struct file *file)
{
	/* The built-in console remains the RX interrupt target after first open. */
	tty_port_close(&c33_tty_port, tty, file);
}

static ssize_t c33_tty_write(struct tty_struct *tty, const u8 *buf,
			     size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		c33_uart_putc(buf[i]);
	c33_lcd_write(buf, count);
	c33_lcd_checkpoint(5);
	return count;
}

static unsigned int c33_tty_write_room(struct tty_struct *tty)
{
	return 65536;
}

static unsigned int c33_tty_chars_in_buffer(struct tty_struct *tty)
{
	return 0;
}

static void c33_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&c33_tty_port);
}

static const struct tty_operations c33_tty_ops = {
	.open = c33_tty_open,
	.close = c33_tty_close,
	.write = c33_tty_write,
	.write_room = c33_tty_write_room,
	.chars_in_buffer = c33_tty_chars_in_buffer,
	.hangup = c33_tty_hangup,
};

static const struct tty_port_operations c33_tty_port_ops = {
};

struct tty_driver *c33_console_device(struct console *console, int *index)
{
	*index = 0;
	return c33_tty_driver;
}

void c33_uart_rx_interrupt(void)
{
	volatile unsigned char *rx = (void *)C33_UART0_RXD;
	volatile unsigned char *status = (void *)C33_UART0_STATUS;
	bool inserted = false;
	int limit = 16;

	*(volatile unsigned char *)C33_UART0_IRQ_FLAGS = C33_UART_RX_IRQ;
	if (!READ_ONCE(c33_tty_ready) || !READ_ONCE(c33_tty_opened))
		return;
	pr_info_once("C33 UART: received vector 57 interrupt\n");

	while ((*status & C33_UART_RX_READY) && limit--) {
		tty_insert_flip_char(&c33_tty_port, *rx, TTY_NORMAL);
		inserted = true;
	}
	if (inserted)
		tty_flip_buffer_push(&c33_tty_port);
	if (inserted)
		c33_lcd_checkpoint(6);
}

bool c33_tty_inject_char(u8 ch)
{
	if (!READ_ONCE(c33_tty_ready) || !READ_ONCE(c33_tty_opened))
		return false;
	if (tty_insert_flip_char(&c33_tty_port, ch, TTY_NORMAL) != 1)
		return false;
	tty_flip_buffer_push(&c33_tty_port);
	return true;
}

static int __init c33_tty_init(void)
{
	struct tty_driver *driver;
	int ret;

	c33_uart_hw_init();
	driver = tty_alloc_driver(1, TTY_DRIVER_RESET_TERMIOS |
				     TTY_DRIVER_REAL_RAW);
	if (IS_ERR(driver))
		return PTR_ERR(driver);

	tty_port_init(&c33_tty_port);
	c33_tty_port.ops = &c33_tty_port_ops;
	driver->driver_name = "c33-uart";
	driver->name = "ttyC33";
	driver->type = TTY_DRIVER_TYPE_SERIAL;
	driver->subtype = SERIAL_TYPE_NORMAL;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_iflag = 0;
	driver->init_termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
	driver->init_termios.c_oflag = OPOST | ONLCR;
	driver->init_termios.c_lflag = 0;
	tty_set_operations(driver, &c33_tty_ops);
	tty_port_link_device(&c33_tty_port, driver, 0);

	ret = tty_register_driver(driver);
	if (ret) {
		tty_port_destroy(&c33_tty_port);
		tty_driver_kref_put(driver);
		return ret;
	}

	c33_tty_driver = driver;
	WRITE_ONCE(c33_tty_ready, true);
	c33_lcd_checkpoint(4);
	pr_info("C33 UART: registered /dev/ttyC330\n");
	return 0;
}
device_initcall(c33_tty_init);
