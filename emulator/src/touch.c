/*
 * Resistive touch panel, seen by the firmware as serial port EFSIF1
 * (REG_BASE+0xb10) delivering 6-byte packets:
 *
 *     0xaa, x_high, x_low, y_high, y_low, pressed
 *
 * with 7 bits per coordinate byte -- any byte with bit 7 set resets the
 * decoder (samo-lib/drivers/src/ctp.c:113-118). Reception is interrupt
 * driven: grifo hooks CTP_interrupt to
 * VECTOR_Serial_interface_Ch_1_Receive_buffer_full (61) in
 * samo-lib/grifo/src/CTP.c:89, so each queued byte raises that vector.
 */

#include <string.h>

#include "touch.h"

#define EFSIF1_BASE  0x0b10u
#define EFSIF1_LEN   0x0010u

#define OFF_RXD      0x01
#define OFF_STATUS   0x02

/* D[7:6]: 0 means "1 or 0" bytes, 1 means 2, 2 means 3, 3 means 4. */
#define RXDNUM(n)     ((uint32_t)((n) <= 1 ? 0 : (n) >= 4 ? 3 : (n) - 1) << 6)
#define RDBFx        (1u << 0)   /* receive data buffer full */
#define TDBEx        (1u << 1)

#define CTP_IRQ_VECTOR 61

static bool touch_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		       bool is_write)
{
	struct touch *t = ctx;
	uint32_t reg = off - EFSIF1_BASE;

	if (is_write) {
		if (reg == OFF_STATUS)
			;                /* error-clear write, nothing to do */
		return true;
	}

	switch (reg) {
	case OFF_RXD:
		if (t->head != t->tail) {
			*val = t->fifo[t->head];
			t->head = (t->head + 1) % TOUCH_FIFO;
			t->bytes_read++;
		} else {
			*val = 0xff;
		}
		return true;
	case OFF_STATUS: {
		/*
		 * RDBFx (D0) is "receive buffer non-empty", and D[7:6] report
		 * the FIFO occupancy -- 0 for "1 or 0" bytes, then 2, 3, 4.
		 * CTP_interrupt drains with "while (0 != (REG_EFSIF1_STATUS &
		 * RDBFx))", so RDBFx is the bit that matters; RXDxNUM is
		 * reported for completeness and capped at the hardware FIFO
		 * depth of 4 (see the note on queue depth in touch.h).
		 */
		unsigned n = (t->tail - t->head + TOUCH_FIFO) % TOUCH_FIFO;
		*val = TDBEx | (n ? RDBFx : 0) | RXDNUM(n);
		return true;
	}
	default:
		*val = 0;
		return true;
	}
}

static void push_byte(struct touch *t, uint8_t b)
{
	unsigned next = (t->tail + 1) % TOUCH_FIFO;

	if (next == t->head)
		return;                  /* drop rather than corrupt the stream */
	t->fifo[t->tail] = b;
	t->tail = next;
}

/*
 * Queue one touch event. The panel reports in its own coordinate space, which
 * grifo scales; feeding it pixel coordinates shifted left by CTP_SHIFT keeps
 * the arithmetic in range while staying proportional.
 */
void touch_post(struct touch *t, struct c33 *cpu, int x, int y, bool pressed)
{
	unsigned tx = (unsigned)(x << CTP_SHIFT);
	unsigned ty = (unsigned)(y << CTP_SHIFT);

	push_byte(t, 0xaa);
	push_byte(t, (tx >> 7) & 0x7f);
	push_byte(t, tx & 0x7f);
	push_byte(t, (ty >> 7) & 0x7f);
	push_byte(t, ty & 0x7f);
	push_byte(t, pressed ? 0x01 : 0x00);

	t->events++;
	c33_raise_irq(cpu, CTP_IRQ_VECTOR, itc_priority(t->itc, CTP_IRQ_VECTOR));
}

/* Re-assert the interrupt while bytes remain, so the handler drains the FIFO. */
void touch_poll(struct touch *t, struct c33 *cpu)
{
	if (t->head != t->tail)
		c33_raise_irq(cpu, CTP_IRQ_VECTOR,
			      itc_priority(t->itc, CTP_IRQ_VECTOR));
}

/*
 * On-screen keyboard geometry, measured from the rendered framebuffer:
 * ten columns on a 24-pixel pitch centred at x = 12 + 24*i, and three rows
 * centred at y = 139, 168, 195. The space bar spans columns 4..5.
 */
static const char *const kb_rows[3] = {
	"QWERTYUIOP",
	"ASDFGHJKL<",       /* '<' is backspace */
	"ZXCV  BNM#",       /* '#' is the 123 key */
};

bool touch_key_pos(char ch, int *x, int *y)
{
	static const int row_y[3] = { 139, 168, 195 };

	if (ch >= 'a' && ch <= 'z')
		ch = (char)(ch - 'a' + 'A');

	if (ch == ' ') {                 /* space bar spans two columns */
		*x = 12 + 24 * 4 + 12;
		*y = row_y[2];
		return true;
	}
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 10; c++)
			if (kb_rows[r][c] == ch && ch != ' ') {
				*x = 12 + 24 * c;
				*y = row_y[r];
				return true;
			}
	return false;
}

void touch_attach(struct mem *m, struct touch *t, const struct itc *itc)
{
	memset(t, 0, sizeof *t);
	t->itc = itc;
	mem_add_mmio(m, "efsif1/ctp", EFSIF1_BASE, EFSIF1_LEN, touch_mmio, t);
}
