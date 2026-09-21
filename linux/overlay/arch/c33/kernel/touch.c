// SPDX-License-Identifier: GPL-2.0-only
/* WikiReader resistive panel and minimal on-screen console keyboard. */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include <asm/wikireader.h>

#define C33_REG_BASE             0x00300000UL
#define C33_P0_DATA              (C33_REG_BASE + 0x380)
#define C33_P0_DIR               (C33_REG_BASE + 0x381)
#define C33_P0_FUNC47            (C33_REG_BASE + 0x3a1)
#define C33_TOUCH_RXD            (C33_REG_BASE + 0xb11)
#define C33_TOUCH_STATUS         (C33_REG_BASE + 0xb12)
#define C33_TOUCH_CTL            (C33_REG_BASE + 0xb13)
#define C33_TOUCH_IRDA           (C33_REG_BASE + 0xb14)
#define C33_TOUCH_BRTRUN         (C33_REG_BASE + 0xb15)
#define C33_TOUCH_BRTRDL         (C33_REG_BASE + 0xb16)
#define C33_TOUCH_BRTRDM         (C33_REG_BASE + 0xb17)
#define C33_SERIAL_IRQ_PRIORITY  (C33_REG_BASE + 0x26a)
#define C33_SERIAL_IRQ_ENABLE    (C33_REG_BASE + 0x276)
#define C33_SERIAL_IRQ_FLAGS     (C33_REG_BASE + 0x286)

#define C33_TOUCH_RX_READY       BIT(0)
#define C33_TOUCH_ERRORS         0x1c
#define C33_TOUCH_RX_IRQ         BIT(4)
#define C33_TOUCH_MCLK_HZ        60000000UL
#define C33_TOUCH_BAUD           9600UL
#define C33_TOUCH_DIVISOR        ((C33_TOUCH_MCLK_HZ + C33_TOUCH_BAUD * 8) / \
				   (C33_TOUCH_BAUD * 16) - 1)
#define C33_TOUCH_PACKET_START   0xaa
#define C33_TOUCH_KEYBOARD_Y     120
#define C33_TOUCH_KEY_WIDTH      24

static u8 touch_state;
static u16 touch_x;
static u16 touch_y;
static int touch_active_key = -1;
static bool touch_pressed;

static u8 touch_read(unsigned long address)
{
	return readb((void __iomem *)address);
}

static void touch_write(u8 value, unsigned long address)
{
	writeb(value, (void __iomem *)address);
}

static void touch_modify(unsigned long address, u8 clear, u8 set)
{
	touch_write((touch_read(address) & ~clear) | set, address);
}

static int touch_hit_key(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= 240 || y < C33_TOUCH_KEYBOARD_Y || y >= 208)
		return -1;
	row = (y - C33_TOUCH_KEYBOARD_Y) * 3 /
		(208 - C33_TOUCH_KEYBOARD_Y);
	return row * 10 + x / C33_TOUCH_KEY_WIDTH;
}

static u8 touch_key_character(int key)
{
	static const u8 keys[3][10] = {
		"qwertyuiop",
		"asdfghjkl\b",
		"zxcv  bnm\n",
	};

	return keys[key / 10][key % 10];
}

static void touch_report(bool pressed, bool valid)
{
	unsigned int x = touch_x >> 1;
	unsigned int y = touch_y >> 1;
	int key = valid ? touch_hit_key(x, y) : -1;
	int previous = touch_active_key;

	if (pressed && !touch_pressed) {
		touch_active_key = key;
		if (key >= 0)
			c33_lcd_keyboard_press(key, true);
	} else if (pressed && touch_pressed && key != touch_active_key) {
		touch_active_key = -1;
		if (previous >= 0)
			c33_lcd_keyboard_press(previous, false);
	} else if (!pressed && touch_pressed) {
		touch_active_key = -1;
		if (previous >= 0)
			c33_lcd_keyboard_press(previous, false);
		if (valid && key == previous && previous >= 0 &&
		    c33_tty_inject_char(touch_key_character(previous)))
			pr_info_once("C33 touch: on-screen keyboard injected console input\n");
	}
	touch_pressed = pressed;
}

static void touch_byte(u8 byte)
{
	if (byte == C33_TOUCH_PACKET_START) {
		touch_state = 1;
		return;
	}
	if ((byte & 0x80) && byte != 0xff) {
		touch_state = 0;
		return;
	}

	switch (touch_state++) {
	case 1:
		if (byte != 0xff)
			touch_x = (touch_x & 0x7f) | ((u16)byte << 7);
		break;
	case 2:
		if (byte != 0xff)
			touch_x = (touch_x & 0x3f80) | byte;
		break;
	case 3:
		if (byte != 0xff)
			touch_y = (touch_y & 0x7f) | ((u16)byte << 7);
		break;
	case 4:
		if (byte != 0xff)
			touch_y = (touch_y & 0x3f80) | byte;
		break;
	case 5:
		if (byte <= 1)
			touch_report(byte, true);
		touch_state = 0;
		break;
	default:
		touch_state = 0;
		break;
	}
}

void c33_touch_interrupt(void)
{
	u8 status = touch_read(C33_TOUCH_STATUS);
	int limit = 16;

	touch_write(C33_TOUCH_RX_IRQ, C33_SERIAL_IRQ_FLAGS);
	if (status & C33_TOUCH_ERRORS) {
		touch_state = 0;
		while ((touch_read(C33_TOUCH_STATUS) & C33_TOUCH_RX_READY) && limit--)
			touch_read(C33_TOUCH_RXD);
		if (touch_pressed)
			touch_report(false, false);
	} else {
		while ((touch_read(C33_TOUCH_STATUS) & C33_TOUCH_RX_READY) && limit--)
			touch_byte(touch_read(C33_TOUCH_RXD));
	}
	touch_write(0, C33_TOUCH_STATUS);
}

static int __init c33_touch_init(void)
{
	c33_lcd_keyboard_init();

	touch_modify(C33_P0_FUNC47, 3, 1);
	touch_write(0x4b, C33_TOUCH_CTL);
	touch_write(0x10, C33_TOUCH_IRDA);
	touch_write(0, C33_TOUCH_BRTRUN);
	touch_write((u8)(C33_TOUCH_DIVISOR >> 8), C33_TOUCH_BRTRDM);
	touch_write((u8)C33_TOUCH_DIVISOR, C33_TOUCH_BRTRDL);
	touch_write(1, C33_TOUCH_BRTRUN);

	/* P07 resets the panel. */
	touch_modify(C33_P0_DIR, 0, BIT(7));
	touch_modify(C33_P0_DATA, 0, BIT(7));
	fsleep(20);
	touch_modify(C33_P0_DATA, BIT(7), 0);

	touch_write(0, C33_TOUCH_STATUS);
	touch_write(C33_TOUCH_RX_IRQ, C33_SERIAL_IRQ_FLAGS);
	touch_modify(C33_SERIAL_IRQ_PRIORITY, 7, 6);
	touch_modify(C33_SERIAL_IRQ_ENABLE, 0, C33_TOUCH_RX_IRQ);
	pr_info("C33 touch: 9600-baud panel and on-screen keyboard ready\n");
	return 0;
}
device_initcall(c33_touch_init);
