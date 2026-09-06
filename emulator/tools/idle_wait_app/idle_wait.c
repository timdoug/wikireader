/* Runs through the public syscall ABI and real timer/interrupt code. */
#include <grifo.h>
#include <regs.h>

static unsigned long gate, clock_control;
static unsigned int irq_enable, irq_priority;

static void require(int pass, const char *message)
{
	if (!pass)
		panic("idle wait FAIL: %s\n", message);
}

static void check_timer_cleanup(void)
{
	require(REG_CMU_GATEDCLK1 == gate, "clock gates restored");
	require(REG_CMU_CLKCNTL == clock_control, "clock frequency unchanged");
	require(REG_INT_E16T23 == irq_enable, "timer interrupt enable restored");
	require(REG_INT_P16T23 == irq_priority, "timer priority restored");
	require(!(REG_INT_F16T23 & (F16TC2 | F16TU2)), "timer flags cleared");
	require(!(REG_T16_CTL2 & PRUNx), "wake timer stopped");
}

static void timeout(unsigned long us)
{
	event_t event;
	unsigned long start = timer_get();
	event_item_t item = event_wait_timeout(&event, us);
	unsigned long ticks = timer_get() - start;
	unsigned long expected = (us > 1000000 ? 1000000 : us) * 60;
	require(item == EVENT_NONE, "timeout returns no event");
	require(ticks >= expected && ticks < expected + 60000, "timeout duration");
	check_timer_cleanup();
}

/* Anchor for -Z: scripted History press arrives during a timed wait. */
void __attribute__((noinline)) idle_input_start(void)
{
	asm volatile ("nop");
}

int grifo_main(int argc, char *argv[])
{
	event_t event;
	unsigned long start;
	unsigned int i;
	event_flush();
	gate = REG_CMU_GATEDCLK1;
	clock_control = REG_CMU_CLKCNTL;
	irq_enable = REG_INT_E16T23;
	irq_priority = REG_INT_P16T23;
	timeout(0);
	timeout(1);
	timeout(1000);
	timeout(~0UL); /* bounded even for a huge request */
	for (i = 0; i < 200; ++i)
		timeout(20000);

	/* Force the application's 32-bit clock to wrap during the next wait. */
	critical_t state = critcal_enter();
	REG_T16_CNT_PAUSE = PAUSE0 | PAUSE5;
	REG_T16_TC5 = 0xffff;
	REG_T16_TC0 = 0;
	REG_T16_CNT_PAUSE = 0;
	critical_exit(state);
	require(timer_get() >= 0xffff0000UL, "clock has not wrapped before wait");
	timeout(20000);
	require(timer_get() < 0x01000000UL, "clock wrapped during wait");
	debug_print("idle wait: repeated deadlines, wrap and timer cleanup pass\n");

	idle_input_start();
	start = timer_get();
	require(event_wait_timeout(&event, 1000000) == EVENT_BUTTON_DOWN,
		"input wakes wait");
	require(event.button.code == BUTTON_HISTORY, "History button");
	require(timer_get() - start < 60000000, "input before deadline");
	check_timer_cleanup();
	/* Script releases use retired-instruction time, so explicitly wait
	 * until the release is queued before testing the no-wait path. */
	start = timer_get();
	while (event_peek(&event) == EVENT_NONE && timer_get() - start < 60000000)
		delay_us(1000);
	require(event.item_type == EVENT_BUTTON_UP, "release arrived in queue");
	start = timer_get();
	require(event_wait_timeout(&event, 1000000) == EVENT_BUTTON_UP,
		"queued release");
	require(timer_get() - start < 60000, "queued input returns immediately");
	check_timer_cleanup();
	debug_print("idle wait: ALL TESTS PASSED\n");
	power_off();
	return 0;
}
