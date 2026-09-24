// SPDX-License-Identifier: GPL-2.0-only
/*
 * Epson S1C33E07 16-bit timer block.
 *
 * Channel 0 counts MCLK directly and, on the WikiReader board, its timer
 * output pin is routed to channel 5's external clock input.  Channel 5
 * therefore advances once per channel 0 overflow and the pair reads as one
 * free-running 32-bit counter: the clocksource, the scheduler clock, and the
 * reference for udelay().  Channel 2 keeps its own prescaler and provides the
 * clock event through the interrupt controller.
 */
#include <linux/bitops.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched_clock.h>
#include <linux/suspend.h>

#include <linux/clocksource/timer-s1c33.h>

#include <asm/clock.h>

#define S1C33_T16_BASE		0x00300780UL
#define S1C33_T16_CRA(n)	(S1C33_T16_BASE + (n) * 8)
#define S1C33_T16_CRB(n)	(S1C33_T16_BASE + (n) * 8 + 2)
#define S1C33_T16_TC(n)		(S1C33_T16_BASE + (n) * 8 + 4)
#define S1C33_T16_CTL(n)	(S1C33_T16_BASE + (n) * 8 + 6)
#define S1C33_T16_PAUSE		0x003007dcUL
#define S1C33_T16_ADVMODE	0x003007deUL
#define S1C33_T16_CLKCTL(n)	(0x003007e0UL + (n) * 2)

#define S1C33_ADVMODE_ON	BIT(0)
#define S1C33_CTL_PRUN		BIT(0)
#define S1C33_CTL_PRESET	BIT(1)
#define S1C33_CTL_PTM		BIT(2)
#define S1C33_CTL_CKSL		BIT(3)
#define S1C33_CTL_OUTINV	BIT(4)
#define S1C33_CLKCTL_ON		BIT(3)
#define S1C33_CLKCTL_DIV1	0
#define S1C33_CLKCTL_DIV64	4
#define S1C33_CLKCTL_DIV4096	7


/* TM0 output on P12 and the EXCL5 input on P74 carry the cascade. */
#define S1C33_P1_03_CFP		0x003003a2UL
#define S1C33_P1_TM0		0x01
#define S1C33_P7_4_CFP		0x003003afUL
#define S1C33_P7_EXCL5		0x02
#define S1C33_CFP_MASK		0x03

#define S1C33_ITC_PRIORITY	0x00300267UL
#define S1C33_ITC_FLAGS		0x00300283UL
#define S1C33_ITC_T2_FLAGS	0x0c
#define S1C33_ITC_T2_PRIORITY	4

#define S1C33_COUNT_LOW		0
#define S1C33_COUNT_HIGH	5
#define S1C33_EVENT		2
#define S1C33_EVENT_DIVISOR	64
#define S1C33_WAKE		3
#define S1C33_WAKE_DIVISOR	4096
/*
 * A touch does wake this core out of HALT on its own -- measured on the
 * device with the poll disabled -- so this is insurance rather than the wake
 * path, and it is slow on purpose. It bounds how long the machine can stay
 * asleep if some other wake source turns out to be one of the causes this
 * core ignores, the way it ignores an HSDMA completion.
 */
#define S1C33_WAKE_SECONDS	4
#define S1C33_ITC_WAKE_FLAGS	0xc0
#define S1C33_ITC_WAKE_PRIORITY	0x40

/* The counter reaches CRB inclusive, so a period of n counts programs n - 1. */
#define S1C33_MIN_DELTA		2
#define S1C33_MAX_DELTA		0xfffe

static bool s1c33_timer_ready;
static unsigned long s1c33_event_rate;

static u16 t16_read(unsigned long reg)
{
	return readw((void __iomem *)reg);
}

static void t16_write(u16 value, unsigned long reg)
{
	writew(value, (void __iomem *)reg);
}

bool s1c33_timer_running(void)
{
	return s1c33_timer_ready;
}

u32 s1c33_timer_cycles(void)
{
	u16 high, low, again;

	if (!s1c33_timer_ready)
		return 0;

	/*
	 * The two counters cannot be sampled together without pausing them,
	 * which would lose time.  Re-read the high half instead and retry
	 * when the low half wrapped underneath us.
	 */
	do {
		high = t16_read(S1C33_T16_TC(S1C33_COUNT_HIGH));
		low = t16_read(S1C33_T16_TC(S1C33_COUNT_LOW));
		again = t16_read(S1C33_T16_TC(S1C33_COUNT_HIGH));
	} while (high != again);

	return ((u32)again << 16) | low;
}

static u64 s1c33_clocksource_read(struct clocksource *cs)
{
	return s1c33_timer_cycles();
}

static struct clocksource s1c33_clocksource = {
	.name = "s1c33-t16",
	.rating = 300,
	.read = s1c33_clocksource_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static u64 notrace s1c33_read_sched_clock(void)
{
	return s1c33_timer_cycles();
}

static void s1c33_event_stop(void)
{
	t16_write(0, S1C33_T16_CTL(S1C33_EVENT));
}

static void s1c33_event_start(unsigned long counts)
{
	t16_write(counts - 1, S1C33_T16_CRB(S1C33_EVENT));
	t16_write(S1C33_CTL_PRESET, S1C33_T16_CTL(S1C33_EVENT));
	t16_write(S1C33_CTL_PRUN, S1C33_T16_CTL(S1C33_EVENT));
}

static int s1c33_set_next_event(unsigned long delta,
				struct clock_event_device *evt)
{
	s1c33_event_stop();
	s1c33_event_start(delta);
	return 0;
}

static int s1c33_set_state_periodic(struct clock_event_device *evt)
{
	s1c33_event_stop();
	s1c33_event_start(DIV_ROUND_CLOSEST(s1c33_event_rate, HZ));
	return 0;
}

static int s1c33_set_state_shutdown(struct clock_event_device *evt)
{
	s1c33_event_stop();
	return 0;
}

static struct clock_event_device s1c33_clockevent = {
	.name = "s1c33-t16-2",
	.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.rating = 300,
	.set_next_event = s1c33_set_next_event,
	.set_state_periodic = s1c33_set_state_periodic,
	.set_state_oneshot = s1c33_set_state_shutdown,
	.set_state_oneshot_stopped = s1c33_set_state_shutdown,
	.set_state_shutdown = s1c33_set_state_shutdown,
};

static irqreturn_t s1c33_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	/* Only the periodic mode wants the automatic restart at CRB. */
	if (!clockevent_state_periodic(evt))
		s1c33_event_stop();
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static void __init s1c33_timer_clocks_on(void)
{
	/*
	 * time_init() runs long before the clock framework has a provider, so
	 * the four channels this driver owns are gated by hand.
	 */
	c33_cmu_gate(C33_CMU_TM0 | C33_CMU_TM2 | C33_CMU_TM3 | C33_CMU_TM5,
		     true);

	writeb((readb((void __iomem *)S1C33_P1_03_CFP) & ~S1C33_CFP_MASK) |
	       S1C33_P1_TM0, (void __iomem *)S1C33_P1_03_CFP);
	writeb((readb((void __iomem *)S1C33_P7_4_CFP) & ~S1C33_CFP_MASK) |
	       S1C33_P7_EXCL5, (void __iomem *)S1C33_P7_4_CFP);
}

static void __init s1c33_counter_init(unsigned long rate)
{
	/* Advanced mode makes the pause register and counter writes work. */
	t16_write(S1C33_ADVMODE_ON, S1C33_T16_ADVMODE);

	/*
	 * The cascade must step the high half exactly when the low half
	 * wraps, or the pair would not read as one rising count.  OUTINV puts
	 * the rising output edge on the comparison B match, which is the wrap
	 * itself; comparison A only has to return the output low again, so it
	 * sits mid-period where the edge is unambiguous.
	 */
	t16_write(S1C33_CTL_PTM | S1C33_CTL_OUTINV,
		  S1C33_T16_CTL(S1C33_COUNT_LOW));
	t16_write(S1C33_CLKCTL_ON | S1C33_CLKCTL_DIV1,
		  S1C33_T16_CLKCTL(S1C33_COUNT_LOW));
	t16_write(0x8000, S1C33_T16_CRA(S1C33_COUNT_LOW));
	t16_write(0xffff, S1C33_T16_CRB(S1C33_COUNT_LOW));
	t16_write(S1C33_CTL_PTM | S1C33_CTL_OUTINV | S1C33_CTL_PRESET,
		  S1C33_T16_CTL(S1C33_COUNT_LOW));

	t16_write(S1C33_CTL_CKSL, S1C33_T16_CTL(S1C33_COUNT_HIGH));
	t16_write(S1C33_CLKCTL_ON | S1C33_CLKCTL_DIV1,
		  S1C33_T16_CLKCTL(S1C33_COUNT_HIGH));
	t16_write(0, S1C33_T16_CRA(S1C33_COUNT_HIGH));
	t16_write(0xffff, S1C33_T16_CRB(S1C33_COUNT_HIGH));
	t16_write(S1C33_CTL_CKSL | S1C33_CTL_PRESET,
		  S1C33_T16_CTL(S1C33_COUNT_HIGH));

	/* Start both halves against a paused clock so they cannot skew. */
	t16_write(BIT(S1C33_COUNT_LOW) | BIT(S1C33_COUNT_HIGH),
		  S1C33_T16_PAUSE);
	t16_write(S1C33_CTL_PTM | S1C33_CTL_OUTINV | S1C33_CTL_PRUN,
		  S1C33_T16_CTL(S1C33_COUNT_LOW));
	t16_write(S1C33_CTL_CKSL | S1C33_CTL_PRUN,
		  S1C33_T16_CTL(S1C33_COUNT_HIGH));
	t16_write(0, S1C33_T16_PAUSE);
	s1c33_timer_ready = true;

	sched_clock_register(s1c33_read_sched_clock, 32, rate);
	if (clocksource_register_hz(&s1c33_clocksource, rate))
		pr_err("s1c33-timer: cannot register the clocksource\n");
}

static void __init s1c33_clockevent_init(unsigned long rate, int irq)
{
	int ret;

	s1c33_event_rate = rate;
	s1c33_event_stop();
	t16_write(S1C33_CLKCTL_ON | S1C33_CLKCTL_DIV64,
		  S1C33_T16_CLKCTL(S1C33_EVENT));
	t16_write(0xffff, S1C33_T16_CRA(S1C33_EVENT));

	writeb((readb((void __iomem *)S1C33_ITC_PRIORITY) & 0xf8) |
	       S1C33_ITC_T2_PRIORITY, (void __iomem *)S1C33_ITC_PRIORITY);
	writeb(S1C33_ITC_T2_FLAGS, (void __iomem *)S1C33_ITC_FLAGS);

	s1c33_clockevent.cpumask = cpumask_of(0);
	s1c33_clockevent.irq = irq;
	ret = request_irq(irq, s1c33_timer_interrupt, IRQF_TIMER, "s1c33-timer",
			  &s1c33_clockevent);
	if (ret)
		panic("s1c33-timer: cannot request IRQ %d: %d", irq, ret);

	clockevents_config_and_register(&s1c33_clockevent, rate,
					S1C33_MIN_DELTA, S1C33_MAX_DELTA);
}

/*
 * Suspend-to-idle stops the tick and then halts, which assumes the core leaves
 * HALT for whatever interrupt is meant to wake it. This one does not always:
 * the HSDMA completion cause demonstrably never woke it on silicon. Rather
 * than trust that a touch will, keep a slow timer running across suspend so
 * the core comes back regularly and takes whichever wake interrupt is already
 * pending. It costs a few hundred microseconds every couple of seconds.
 */
static unsigned long s1c33_wake_rate;
static unsigned int s1c33_wake_seconds = S1C33_WAKE_SECONDS;

/*
 * Whether this core leaves HALT for a peripheral cause at all is a property of
 * the silicon, not of Linux, and the only way to find out is to take the poll
 * away and see whether the machine still wakes. s1c33_wake=0 does that;
 * s1c33_wake=<seconds> tightens or loosens it.
 */
static int __init s1c33_wake_setup(char *options)
{
	unsigned int seconds;

	if (options && !kstrtouint(options, 0, &seconds) && seconds <= 4)
		s1c33_wake_seconds = seconds;
	return 0;
}
early_param("s1c33_wake", s1c33_wake_setup);

static irqreturn_t s1c33_wake_interrupt(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static int s1c33_timer_suspend(void)
{
	unsigned long counts = s1c33_wake_rate * s1c33_wake_seconds;

	if (!s1c33_wake_rate || !s1c33_wake_seconds)
		return 0;
	if (counts > 0xffff)
		counts = 0xffff;
	t16_write(0, S1C33_T16_CTL(S1C33_WAKE));
	t16_write(counts - 1, S1C33_T16_CRB(S1C33_WAKE));
	t16_write(S1C33_CTL_PRESET, S1C33_T16_CTL(S1C33_WAKE));
	t16_write(S1C33_CTL_PRUN, S1C33_T16_CTL(S1C33_WAKE));
	return 0;
}

static void s1c33_timer_resume(void)
{
	t16_write(0, S1C33_T16_CTL(S1C33_WAKE));
}

/*
 * Suspend-to-idle never reaches syscore_suspend(); it stops at the noirq
 * phase and idles. These are the hooks that bracket that idle loop.
 */
static const struct platform_s2idle_ops s1c33_s2idle_ops = {
	.prepare_late = s1c33_timer_suspend,
	.restore_early = s1c33_timer_resume,
};

static void __init s1c33_wake_init(unsigned long rate, int irq)
{
	s1c33_wake_rate = rate;
	t16_write(0, S1C33_T16_CTL(S1C33_WAKE));
	t16_write(S1C33_CLKCTL_ON | S1C33_CLKCTL_DIV4096,
		  S1C33_T16_CLKCTL(S1C33_WAKE));
	t16_write(0xffff, S1C33_T16_CRA(S1C33_WAKE));
	writeb((readb((void __iomem *)S1C33_ITC_PRIORITY) & 0x8f) |
	       S1C33_ITC_WAKE_PRIORITY, (void __iomem *)S1C33_ITC_PRIORITY);
	writeb(S1C33_ITC_WAKE_FLAGS, (void __iomem *)S1C33_ITC_FLAGS);
	if (request_irq(irq, s1c33_wake_interrupt, IRQF_TIMER | IRQF_NO_SUSPEND,
			"s1c33-wake", NULL)) {
		pr_warn("s1c33-timer: no suspend wake timer on IRQ %d\n", irq);
		s1c33_wake_rate = 0;
		return;
	}
	s2idle_set_ops(&s1c33_s2idle_ops);
}

void __init s1c33_timer_init(unsigned long mclk_hz, int event_irq, int wake_irq)
{
	s1c33_timer_clocks_on();
	s1c33_counter_init(mclk_hz);
	s1c33_clockevent_init(mclk_hz / S1C33_EVENT_DIVISOR, event_irq);
	s1c33_wake_init(mclk_hz / S1C33_WAKE_DIVISOR, wake_irq);
	if (s1c33_wake_seconds)
		pr_info("s1c33-timer: %lu Hz counter, %lu Hz clock event on IRQ %d, %u s suspend wake on IRQ %d\n",
			mclk_hz, mclk_hz / S1C33_EVENT_DIVISOR, event_irq,
			s1c33_wake_seconds, wake_irq);
	else
		pr_info("s1c33-timer: %lu Hz counter, %lu Hz clock event on IRQ %d, no suspend wake poll\n",
			mclk_hz, mclk_hz / S1C33_EVENT_DIVISOR, event_irq);
}
