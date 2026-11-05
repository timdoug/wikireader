/* Exercise the production event_wait/Suspend path through real Grifo. */
#include <grifo.h>
#include <regs.h>

static volatile unsigned retained[256];

void __attribute__((noinline)) suspend_input_anchor(void) { asm volatile("nop"); }

int grifo_main(int argc, char **argv)
{
    unsigned i, clock = REG_CMU_CLKCNTL;
    unsigned gate0 = REG_CMU_GATEDCLK0, gate1 = REG_CMU_GATEDCLK1;
    unsigned refresh = REG_SDRAMC_REF & ~(1u << 25);
    unsigned app = REG_SDRAMC_APP;
    event_t event;
    for (i = 0; i < 256; i++) retained[i] = 0x13579bdfu ^ (i * 0x10203u);
    event_flush();
    debug_print("SUSPEND_SMOKE_READY\n");
    suspend_input_anchor();
    do {
        event_wait(&event, NULL, NULL);
        if (event.item_type == EVENT_BUTTON_DOWN || event.item_type == EVENT_BUTTON_UP)
            if (event.button.code != BUTTON_HISTORY) panic("unexpected wake button\n");
    } while (event.item_type != EVENT_BUTTON_UP);
    if (REG_CMU_CLKCNTL != clock || REG_CMU_GATEDCLK0 != gate0 || REG_CMU_GATEDCLK1 != gate1 ||
        (REG_SDRAMC_REF & ~(1u << 25)) != refresh || REG_SDRAMC_APP != app)
        panic("suspend configuration restoration failed\n");
    for (i = 0; i < 256; i++)
        if (retained[i] != (0x13579bdfu ^ (i * 0x10203u))) panic("suspend retained data failed\n");
    debug_print("SUSPEND_SMOKE_PASS\n");
    power_off();
}
