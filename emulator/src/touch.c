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
#include "lcd.h"

#define EFSIF1_BASE  0x0b10u
#define EFSIF1_LEN   0x0010u

#define OFF_RXD      0x01
#define OFF_STATUS   0x02
#define OFF_IRDA     0x04
#define OFF_BRTL     0x06
#define OFF_BRTH     0x07

#define DIVMD_8X     (1u << 4)   /* clear selects 16x */
#define FERx         (1u << 4)   /* framing error */

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
		switch (reg) {
		case OFF_STATUS:
			/* Writing the status register clears the bits written,
			   which is how the driver acknowledges an error. */
			t->errors &= (uint8_t)*val;
			break;
		case OFF_IRDA:
			t->irda = (uint8_t)*val;
			break;
		case OFF_BRTL:
			t->brt = (uint16_t)((t->brt & 0xff00u) | (*val & 0xffu));
			break;
		case OFF_BRTH:
			t->brt = (uint16_t)((t->brt & 0x00ffu) |
					    ((*val & 0xffu) << 8));
			break;
		default:
			break;
		}
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
		*val = TDBEx | (n ? RDBFx : 0) | RXDNUM(n) | t->errors;
		return true;
	}
	default:
		*val = 0;
		return true;
	}
}

void touch_set_clock(struct touch *t, uint32_t hz)
{
	t->clock_hz = hz;
}

void touch_set_cmu(struct touch *t, const struct cmu *cmu)
{
	t->cmu = cmu;
}

/*
 * The clock the baud generator is dividing at this moment.  It moves: grifo
 * drops the machine to MCLK/32 while it waits for something to happen and
 * reprograms both serial ports to match, so the divisor that means 9600
 * asleep is not the one that means 9600 awake.  Reading the rate out of the
 * registers at the time of the event is the only way to get both right.
 */
static uint32_t touch_clock(const struct touch *t)
{
	if (t->cmu && cmu_clock_selected(t->cmu))
		return cmu_mclk_hz(t->cmu);
	return t->clock_hz;
}

/*
 * The receiver's rate: the baud generator divides the clock by DIVMD (eight
 * or sixteen) and then by twice the reload plus one.  The original firmware
 * computes the reload with CALC_BAUD(PLL_CLK, 1, SERIAL_DIVMD, CTP_BPS) and
 * arrives at 97, which is 38265 baud against the panel's nominal 38400.
 */
uint32_t touch_baud(const struct touch *t)
{
	uint32_t divmd = (t->irda & DIVMD_8X) ? 8u : 16u;
	uint32_t hz = touch_clock(t);

	if (!hz)
		return 0;
	return hz / (divmd * 2u * ((uint32_t)t->brt + 1u));
}

static bool touch_listening(const struct touch *t)
{
	uint32_t baud = touch_baud(t);
	uint32_t slack = CTP_BPS * CTP_BAUD_TOLERANCE / 100u;

	/* Nothing configured yet is not a mismatch; it is a driver that has
	   not got there, and there is nothing for it to miss. */
	if (!baud)
		return true;
	return baud + slack >= CTP_BPS && baud <= CTP_BPS + slack;
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
	/*
	 * Clamp to the panel before packing.
	 *
	 * The coordinates go out as two 7-bit halves, so a negative value --
	 * which is what SDL reports once the pointer leaves the window during
	 * a drag -- wraps into a huge one: y = -10 packs as 0x7f,0x6c, which
	 * grifo decodes as 8182 on a 208-pixel screen. The application then
	 * jumps somewhere absurd. A real panel cannot report off-panel
	 * coordinates at all, so clamping is both the safe and the faithful
	 * answer.
	 */
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > LCD_WIDTH - 1)  x = LCD_WIDTH - 1;
	if (y > LCD_HEIGHT - 1) y = LCD_HEIGHT - 1;

	unsigned tx = (unsigned)(x << CTP_SHIFT);
	unsigned ty = (unsigned)(y << CTP_SHIFT);

	/*
	 * A receiver clocked at the wrong rate does not hear a quieter
	 * version of the packet: it samples the line in the wrong places and
	 * gets bytes that were never sent, with the stop bit missing. What
	 * reaches the driver is framing errors and rubbish, and no amount of
	 * tapping produces a coordinate -- which is exactly what a WikiReader
	 * does when its touch panel is set to 9600 and the panel is talking
	 * at 38400. Both interrupts still fire; there is a signal on the
	 * wire, it just does not mean anything.
	 */
	if (!touch_listening(t)) {
		t->errors |= FERx;
		push_byte(t, 0xff);
		push_byte(t, 0xff);
		t->garbled++;
		t->events++;
		itc_set_flag((struct itc *)t->itc, CTP_IRQ_VECTOR);
		c33_raise_irq(cpu, CTP_IRQ_VECTOR,
			      itc_priority(t->itc, CTP_IRQ_VECTOR));
		return;
	}

	push_byte(t, 0xaa);
	push_byte(t, (tx >> 7) & 0x7f);
	push_byte(t, tx & 0x7f);
	push_byte(t, (ty >> 7) & 0x7f);
	push_byte(t, ty & 0x7f);
	push_byte(t, pressed ? 0x01 : 0x00);

	t->events++;
	itc_set_flag((struct itc *)t->itc, CTP_IRQ_VECTOR);
	c33_raise_irq(cpu, CTP_IRQ_VECTOR, itc_priority(t->itc, CTP_IRQ_VECTOR));
}

/* Re-assert the interrupt while bytes remain, so the handler drains the FIFO. */
void touch_poll(struct touch *t, struct c33 *cpu)
{
	if (t->head != t->tail) {
		itc_set_flag((struct itc *)t->itc, CTP_IRQ_VECTOR);
		c33_raise_irq(cpu, CTP_IRQ_VECTOR,
			      itc_priority(t->itc, CTP_IRQ_VECTOR));
	}
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

void touch_reset(struct touch *t)
{
	const struct itc *keep = t->itc;
	const struct cmu *cmu = t->cmu;
	uint32_t clock = t->clock_hz;

	memset(t, 0, sizeof *t);
	t->itc = keep;
	t->cmu = cmu;
	t->clock_hz = clock;   /* a reset does not change the crystal */
}

void touch_attach(struct mem *m, struct touch *t, const struct itc *itc)
{
	t->itc = itc;
	touch_reset(t);
	mem_add_mmio(m, "efsif1/ctp", EFSIF1_BASE, EFSIF1_LEN, touch_mmio, t);
}
