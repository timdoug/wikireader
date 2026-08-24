/*
 * EFSIF0 serial port -- the console grifo's Serial_print writes to.
 *
 * Register map from samo-lib/include/regs.h:
 *   REG_BASE+0xb00  TXD     transmit data
 *   REG_BASE+0xb01  RXD     receive data
 *   REG_BASE+0xb02  STATUS  bit1 TDBEx (tx buffer empty), bit0 RDBFx (rx full)
 *   REG_BASE+0xb03  CTL
 *   ...             baud rate divisors
 *
 * grifo polls STATUS until TDBEx is set, then stores to TXD
 * (samo-lib/grifo/src/serial.c:82-89). We report the transmitter as always
 * ready, which is correct for an emulator with an infinitely fast UART.
 */

#include <stdio.h>
#include <string.h>

#include "uart.h"

#define EFSIF0_BASE   0x0b00u
#define EFSIF0_LEN    0x0010u

#define OFF_TXD       0x00
#define OFF_RXD       0x01
#define OFF_STATUS    0x02

#define TDBEx         (1u << 1)   /* transmit data buffer empty */
#define RDBFx         (1u << 0)   /* receive data buffer full */

static bool uart_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		      bool is_write)
{
	struct uart *u = ctx;
	uint32_t reg = off - EFSIF0_BASE;

	if (is_write) {
		switch (reg) {
		case OFF_TXD:
			u->tx_count++;
			if (u->out) {
				fputc((int)(*val & 0xff), u->out);
				fflush(u->out);
			}
			if (u->capture_len + 1 < sizeof u->capture)
				u->capture[u->capture_len++] = (char)(*val & 0xff);
			return true;
		default:
			return true;      /* config registers: accept silently */
		}
	}

	switch (reg) {
	case OFF_STATUS:
		/* Transmitter always ready; nothing ever arrives on rx. */
		*val = TDBEx;
		return true;
	case OFF_RXD:
		*val = 0;
		return true;
	default:
		*val = 0;
		return true;
	}
}

void uart_attach(struct mem *m, struct uart *u, FILE *out)
{
	memset(u, 0, sizeof *u);
	u->out = out;
	mem_add_mmio(m, "efsif0", EFSIF0_BASE, EFSIF0_LEN, uart_mmio, u);
}
