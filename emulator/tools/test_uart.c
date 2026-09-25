/* EFSIF0 FIFO, interrupt causes, and reset via the MMIO interface. */
#include <assert.h>
#include <stdio.h>
#include "../src/mem.h"
#include "../src/uart.h"
#include "../src/itc.h"
#include "../src/cmu.h"
#define REG 0x00300000u
int main(void)
{
    struct mem m;
    struct uart u;
    struct itc itc;
    unsigned vector, priority;
    assert(mem_init(&m));
    itc_attach(&m, &itc);
    uart_attach(&m, &u, NULL);
    u.itc = &itc;
    assert(!uart_receive(&u, 'X'));
    mem_write(&m, REG + 0xb03, 1, 0xcb);
    mem_write(&m, REG + 0x26a, 1, 0x50);
    mem_write(&m, REG + 0x276, 1, 3);
    for (unsigned i = 0; i < 4; i++) assert(uart_receive(&u, 'A' + i));
    assert((mem_read(&m, REG + 0xb02, 1) & 0xc1) == 0xc1);
    assert(itc_next_irq(&itc, &vector, &priority) && vector == 57 && priority == 5);
    assert(!uart_receive(&u, 'E'));
    assert(mem_read(&m, REG + 0xb02, 1) & 4);
    assert(itc_next_irq(&itc, &vector, &priority) && vector == 56);
    mem_write(&m, REG + 0xb02, 1, 0);
    mem_write(&m, REG + 0x286, 1, 3);
    uart_poll(&u);
    assert(itc_next_irq(&itc, &vector, &priority) && vector == 57);
    for (unsigned i = 0; i < 4; i++) assert(mem_read(&m, REG + 0xb01, 1) == 'A' + i);
    mem_write(&m, REG + 0x286, 1, 3);
    uart_poll(&u);
    assert(!itc_next_irq(&itc, &vector, &priority));
    assert(mem_read(&m, REG + 0xb02, 1) == 2);
    assert(uart_receive(&u, 'Z'));
    uart_reset(&u);
    assert(u.rx_count == 0 && !uart_can_receive(&u));

    /* Transmit takes a frame a byte once the shift register is busy.  At
       57600 baud, DIVMD 8, BRTRD 64: a bit is 2 * 65 * 8 = 1040 cycles and
       a frame 10,400.  The first byte starts at once and the second waits in
       the buffer, so TDBE clears until the first frame is done. */
    uint64_t clock = 1000;
    uart_set_clock(&u, &clock);
    mem_write(&m, REG + 0xb04, 1, 0x10);          /* IRDA: DIVMD 1/8 */
    mem_write(&m, REG + 0xb06, 1, 64);            /* BRTRDL */
    mem_write(&m, REG + 0xb07, 1, 0);             /* BRTRDM */
    assert(mem_read(&m, REG + 0xb02, 1) & 2);     /* TDBE: idle */
    mem_write(&m, REG + 0xb00, 1, 'a');
    assert(mem_read(&m, REG + 0xb02, 1) & 2);     /* shifting, buffer free */
    assert(mem_read(&m, REG + 0xb02, 1) & 0x20);  /* TEND: busy */
    mem_write(&m, REG + 0xb00, 1, 'b');
    assert(!(mem_read(&m, REG + 0xb02, 1) & 2));  /* buffer full */
    clock = 1000 + 10399;
    assert(!(mem_read(&m, REG + 0xb02, 1) & 2));
    clock = 1000 + 10400;
    assert(mem_read(&m, REG + 0xb02, 1) & 2);     /* 'b' has started */
    assert(mem_read(&m, REG + 0xb02, 1) & 0x20);
    clock = 1000 + 20800;
    assert(!(mem_read(&m, REG + 0xb02, 1) & 0x20)); /* the line is idle */
    uart_set_clock(&u, NULL);
    mem_write(&m, REG + 0xb00, 1, 'c');
    assert(mem_read(&m, REG + 0xb02, 1) & 2);     /* untimed: at once */

    /* With either EFSIO gate off the port is dead: writes are dropped,
       reads are zero, input is refused and no interrupt is raised.  Both
       gates are on at reset; the kernel has to keep them that way. */
    {
        struct cmu cmu;
        unsigned long sent = u.tx_count;

        cmu_attach(&m, &cmu);
        uart_set_cmu(&u, &cmu);
        mem_write(&m, REG + 0xb03, 1, 0xcb);           /* RX enable */
        assert(uart_can_receive(&u));
        mem_write(&m, REG + 0x1b24, 4, 0x96);          /* PROTECT off */
        mem_write(&m, REG + 0x1b04, 4, mem_read(&m, REG + 0x1b04, 4) & ~(1u << 5));
        assert(!uart_can_receive(&u));
        assert(!uart_receive(&u, 'Q'));
        mem_write(&m, REG + 0xb00, 1, 'd');
        assert(u.tx_count == sent);
        assert(mem_read(&m, REG + 0xb02, 1) == 0);
        assert(u.gated_accesses == 2);
        mem_write(&m, REG + 0x1b04, 4, mem_read(&m, REG + 0x1b04, 4) | (1u << 5));
        mem_write(&m, REG + 0x1b04, 4, mem_read(&m, REG + 0x1b04, 4) & ~(1u << 25));
        mem_write(&m, REG + 0xb00, 1, 'e');
        assert(u.tx_count == sent && u.gated_accesses == 3);
        mem_write(&m, REG + 0x1b04, 4, mem_read(&m, REG + 0x1b04, 4) | (1u << 25));
        mem_write(&m, REG + 0xb00, 1, 'f');
        assert(u.tx_count == sent + 1 && u.gated_accesses == 3);
        assert(uart_receive(&u, 'R'));
        uart_set_cmu(&u, NULL);
    }
    mem_free(&m);
    puts("UART FIFO/IRQ/reset: PASS");
    return 0;
}
