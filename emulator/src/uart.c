/* EFSIF0 console UART. Register layout follows S1C33E07 V.1.8.2.
 * TX completes immediately; RX has the hardware four-byte FIFO and error
 * status. Host input is paced separately by main.c.
 */

#include <stdio.h>
#include <string.h>

#include "uart.h"

#define EFSIF0_BASE   0x0b00u
#define EFSIF0_LEN    0x0010u

#define OFF_TXD       0x00
#define OFF_RXD       0x01
#define OFF_STATUS    0x02
#define OFF_CTL       0x03

#define TENDx         (1u << 5)   /* 1 = transmitting, 0 = transmission done */
#define TDBEx         (1u << 1)   /* transmit data buffer empty */
#define RDBFx         (1u << 0)   /* receive data buffer full */

/* D[7:6]: 0 means "1 or 0" bytes, 1 means 2, 2 means 3, 3 means 4. */
#define RXDNUM(n)     ((uint32_t)((n) <= 1 ? 0 : (n) >= 4 ? 3 : (n) - 1) << 6)

static bool uart_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		      bool is_write)
{
	struct uart *u = ctx;
	uint32_t reg = off - EFSIF0_BASE;

	if (is_write) {
		switch (reg) {
		case OFF_TXD:
			u->tx_count++;
			if (u->itc) itc_set_flag(u->itc, 58);
			if (u->out) {
				fputc((int)(*val & 0xff), u->out);
				fflush(u->out);
			}
			if (u->capture_len + 1 < sizeof u->capture)
				u->capture[u->capture_len++] = (char)(*val & 0xff);
			return true;
		case OFF_STATUS:
			u->errors &= (uint8_t)*val;
			return true;
		case OFF_CTL:
			u->control = (uint8_t)*val;
			return true;
		default:
			return true;      /* other configuration registers */
		}
	}

	switch (reg) {
	case OFF_STATUS:
		*val = TDBEx | RXDNUM(u->rx_count) | u->errors |
		       (u->rx_count ? RDBFx : 0);
		return true;
	case OFF_RXD:
		*val = 0;
		if (u->rx_count) {
			*val = u->rx[u->rx_head];
			u->rx_head = (u->rx_head + 1) % sizeof u->rx;
			u->rx_count--;
		}
		return true;
	case OFF_CTL:
		*val = u->control;
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

void uart_reset(struct uart *u)
{
	u->rx_head = u->rx_count = u->control = u->errors = 0;
}

bool uart_can_receive(const struct uart *u)
{
	return (u->control & 0x40) && u->rx_count < sizeof u->rx;
}

bool uart_receive(struct uart *u, uint8_t byte)
{
	if (!(u->control & 0x40))
		return false;
	if (u->rx_count == sizeof u->rx) {
		u->errors |= 4; /* OER: discard the arriving byte on overflow. */
		uart_poll(u);
		return false;
	}
	u->rx[(u->rx_head + u->rx_count) % sizeof u->rx] = byte;
	u->rx_count++;
	uart_poll(u);
	return true;
}

void uart_poll(struct uart *u)
{
	if (!u->itc)
		return;
	if (u->rx_count)
		itc_set_flag(u->itc, 57);
	if (u->errors)
		itc_set_flag(u->itc, 56);
}
