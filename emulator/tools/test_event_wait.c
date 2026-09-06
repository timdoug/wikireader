/* Real event queue/deadline code, with deterministic interrupt scheduling.
 * Register programming and 32-bit timer wrap are covered by idle_wait_app. */
#include <assert.h>
#include <stdio.h>

#define _INTERRUPT_H_ 1
typedef enum { Interrupt_disabled, Interrupt_enabled } Interrupt_type;
static Interrupt_type Interrupt_disable(void);
static void Interrupt_enable(Interrupt_type state);

#include "../../samo-lib/grifo/src/event.c"

static Interrupt_type irq = Interrupt_enabled;
static unsigned long now, waited;
static unsigned waits, reads, partial_wakes;
static bool race_before_idle, input_on_wake, pending_input;

static void input(void)
{
	event_t event = { .item_type = EVENT_BUTTON_UP };
	event.button.code = BUTTON_HISTORY;
	assert(Event_put(&event));
}

static Interrupt_type Interrupt_disable(void)
{
	Interrupt_type old = irq;
	irq = Interrupt_disabled;
	return old;
}

static void Interrupt_enable(Interrupt_type state)
{
	if (state == Interrupt_enabled) {
		irq = state;
		if (pending_input) {
			pending_input = false;
			input();
		}
	}
}

void Timer_initialise(void) {}
void Suspend_initialise(void) {}
void Suspend(Standard_BoolCallBackType *callback, void *arg) { assert(0); }
void Watchdog_KeepAlive(Watchdog_type key) { assert(key == WATCHDOG_KEY); }

unsigned long Timer_get(void)
{
	if (++reads == 2 && race_before_idle) {
		race_before_idle = false;
		input();
	}
	return now;
}

void Timer_wait(unsigned long ticks)
{
	assert(irq == Interrupt_disabled);
	assert(ticks > 0 && ticks <= 60000000);
	++waits;
	if (partial_wakes || input_on_wake) {
		if (ticks > 60000)
			ticks = 60000;
		if (partial_wakes)
			--partial_wakes;
		else {
			input_on_wake = false;
			pending_input = true;
		}
	}
	now += ticks;
	waited += ticks;
}

static void reset(void)
{
	Event_flush();
	now = waited = waits = reads = partial_wakes = 0;
	race_before_idle = input_on_wake = pending_input = false;
	assert(irq == Interrupt_enabled);
}

int main(void)
{
	event_t event;
	reset();
	assert(Event_wait_timeout(&event, 0) == EVENT_NONE && waits == 0);
	input();
	assert(Event_wait_timeout(&event, 1000000) == EVENT_BUTTON_UP);
	assert(event.button.code == BUTTON_HISTORY && waits == 0);
	assert(Event_get(&event) == EVENT_NONE);

	reset();
	assert(Event_wait_timeout(&event, 20000) == EVENT_NONE);
	assert(waited == 1200000 && waits == 1);
	assert(irq == Interrupt_enabled);

	reset();
	partial_wakes = 6; /* UART bytes without a complete event */
	assert(Event_wait_timeout(&event, 20000) == EVENT_NONE);
	assert(waited == 1200000 && waits == 7); /* deadline was not restarted */

	reset();
	race_before_idle = true;
	assert(Event_wait_timeout(&event, 20000) == EVENT_BUTTON_UP);
	assert(waits == 0); /* event after empty read, before masked recheck */

	reset();
	partial_wakes = 3;
	input_on_wake = true;
	assert(Event_wait_timeout(&event, 20000) == EVENT_BUTTON_UP);
	assert(waited == 240000 && waits == 4);
	assert(irq == Interrupt_enabled);
	assert(Event_get(&event) == EVENT_NONE);

	reset();
	assert(Event_wait_timeout(&event, ~0UL) == EVENT_NONE);
	assert(waited == 60000000 && waits == 1);
	puts("event wait: deadlines, partial input, queue races and bounds pass");
	return 0;
}
