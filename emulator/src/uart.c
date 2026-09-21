/* EFSIF0 console UART. Register layout follows S1C33E07 V.1.8.2.
 * RX has the hardware four-byte FIFO and error status; host input is paced
 * separately by main.c.
 *
 * TX takes the time the line takes.  The hardware is a one-byte transmit
 * buffer in front of a shift register: a byte written while the shifter is
 * idle starts at once and the buffer stays empty; one written while it is
 * busy waits in the buffer, TDBE clears, and it starts when the shifter is
 * free.  Firmware polls TDBE before every write, so from the third byte of
 * a line on the CPU waits a frame a byte -- 10,400 cycles at 57600 baud --
 * and the device charges a Linux boot four hundred million cycles for its
 * console where this model charged nothing.  The frame comes from the baud
 * rate registers as the firmware programs them: a bit is 2 * (BRTRD + 1)
 * DIVMD clocks, and the EFSIF runs from the CPU's clock here.  Without a
 * clock (the tests) a write completes at once, as before.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uart.h"

#define EFSIF0_BASE   0x0b00u
#define EFSIF0_LEN    0x0010u

#define OFF_TXD       0x00
#define OFF_RXD       0x01
#define OFF_STATUS    0x02
#define OFF_CTL       0x03
#define OFF_IRDA      0x04
#define OFF_BRTRUN    0x05
#define OFF_BRTRDL    0x06
#define OFF_BRTRDM    0x07

#define TENDx         (1u << 5)   /* 1 = transmitting, 0 = transmission done */
#define TDBEx         (1u << 1)   /* transmit data buffer empty */
#define RDBFx         (1u << 0)   /* receive data buffer full */

/* D[7:6]: 0 means "1 or 0" bytes, 1 means 2, 2 means 3, 3 means 4. */
#define RXDNUM(n)     ((uint32_t)((n) <= 1 ? 0 : (n) >= 4 ? 3 : (n) - 1) << 6)

/* One frame, 8N1, in CPU cycles: ten bits of 2 * (BRTRD + 1) * DIVMD. */
static uint64_t uart_frame(const struct uart *u)
{
	unsigned brtrd = ((unsigned)u->brtrdm << 8) | u->brtrdl;
	unsigned divmd = (u->irda & 0x10) ? 8 : 16;

	return 10ull * 2 * (brtrd + 1) * divmd;
}

/* Bring the transmitter up to date: a byte waiting in the buffer starts
   shifting the moment the one before it is done. */
static void uart_settle(struct uart *u)
{
	if (u->clock && u->tx_buffered && *u->clock >= u->tx_done) {
		u->tx_done += uart_frame(u);
		u->tx_buffered = false;
	}
}

static void uart_trace_line(struct uart *u, uint8_t byte)
{
	const char *expected = getenv("WREMU_UART_TRACE");

	if (!expected)
		return;
	if (byte == '\r')
		return;
	if (byte != '\n') {
		if (u->trace_line_len + 1 < sizeof u->trace_line)
			u->trace_line[u->trace_line_len++] = (char)byte;
		return;
	}
	u->trace_line[u->trace_line_len] = '\0';
	if (!*expected || !strcmp(u->trace_line, expected))
		fprintf(stderr, "  [uart line at MCLK %llu] %s\n",
			u->clock ? (unsigned long long)*u->clock : 0,
			u->trace_line);
	u->trace_line_len = 0;
}

static bool uart_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		      bool is_write)
{
	struct uart *u = ctx;
	uint32_t reg = off - EFSIF0_BASE;

	if (is_write) {
		switch (reg) {
		case OFF_TXD:
			if (u->clock) {
				uart_settle(u);
				if (*u->clock >= u->tx_done)
					u->tx_done = *u->clock + uart_frame(u);
				else
					u->tx_buffered = true;
			}
			u->tx_count++;
			if (u->itc) itc_set_flag(u->itc, 58);
			if (u->out) {
				fputc((int)(*val & 0xff), u->out);
				fflush(u->out);
			}
			uart_trace_line(u, (uint8_t)*val);
			if (u->capture_len + 1 < sizeof u->capture)
				u->capture[u->capture_len++] = (char)(*val & 0xff);
			return true;
		case OFF_STATUS:
			u->errors &= (uint8_t)*val;
			return true;
		case OFF_CTL:
			u->control = (uint8_t)*val;
			return true;
		case OFF_IRDA:
			u->irda = (uint8_t)*val;
			return true;
		case OFF_BRTRDL:
			u->brtrdl = (uint8_t)*val;
			return true;
		case OFF_BRTRDM:
			u->brtrdm = (uint8_t)*val;
			return true;
		default:
			return true;      /* other configuration registers */
		}
	}

	switch (reg) {
	case OFF_STATUS:
		uart_settle(u);
		*val = (u->tx_buffered ? 0 : TDBEx) |
		       (u->tx_buffered || (u->clock && *u->clock < u->tx_done)
			? TENDx : 0) |
		       RXDNUM(u->rx_count) | u->errors |
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

void uart_set_clock(struct uart *u, const uint64_t *clock)
{
	u->clock = clock;
}

void uart_reset(struct uart *u)
{
	u->rx_head = u->rx_count = u->control = u->errors = 0;
	u->tx_buffered = false;
	u->tx_done = 0;
	u->trace_line_len = 0;
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
