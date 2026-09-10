/* EFSIF0 FIFO, interrupt causes, and reset via the MMIO interface. */
#include <assert.h>
#include <stdio.h>
#include "../src/mem.h"
#include "../src/uart.h"
#include "../src/itc.h"
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
    mem_free(&m);
    puts("UART FIFO/IRQ/reset: PASS");
    return 0;
}
