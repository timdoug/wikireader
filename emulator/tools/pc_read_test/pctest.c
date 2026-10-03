/* What ld.w %rd,%pc reads on silicon in a delayed branch's slot.  The PE
 * manual (section 5.14.2, and the caution on printed page 119) allows the
 * read only there and defines it as the address of the instruction after
 * the slot; GCC's nested-function trampoline (jp.d .+4; ld.w %r12,%pc)
 * depends on it.  A read outside a slot returned a stale value on the
 * device (2026-09-26), so this reads the PC after each kind of delayed
 * branch, at sixteen alignments from SDRAM and two from A0 RAM, alongside
 * the plain read, and calls a real trampoline.  Results go to pctest.log
 * and the serial port.  See README.md. */
#include <grifo.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RUNS 1000

typedef unsigned long read_fn(unsigned long *want);

/* Each case pads with n register moves, so the read sits at a different
 * offset in its 32-byte-aligned block, then sets pc from the read and want
 * from the address the manual says it gives. */
#define CASE(name, where, n, body)					\
static unsigned long __attribute__((noinline, aligned(32), section(where)))\
name(unsigned long *want)						\
{									\
	unsigned long pc, label, t = 0x5a5a5a5aUL;			\
	asm volatile (".rept " #n "\n\tld.w %[t],%[t]\n\t.endr\n\t"	\
		      body						\
		      : [pc] "=&r" (pc), [label] "=&r" (label),		\
			[t] "+&r" (t)					\
		      : : "memory", "cc");				\
	*want = label;							\
	return pc;							\
}

/* GCC's trampoline: jump to the instruction after the slot. */
#define JP_NEXT "jp.d 1f\n\tld.w %[pc],%%pc\n1:\n\txld.w %[label],1b\n"
/* Jump past it: is the read the next instruction or the target? */
#define JP_FAR "jp.d 2f\n\tld.w %[pc],%%pc\n1:\n\tld.w %[t],%[t]\n\t"	\
	"ld.w %[t],%[t]\n2:\n\txld.w %[label],1b\n"
#define JP_REG "xld.w %[t],1f\n\tjp.d %[t]\n\tld.w %[pc],%%pc\n1:\n\t"	\
	"xld.w %[label],1b\n"
#define JR_TAKEN "ld.w %[t],1\n\tcmp %[t],0\n\tjrne.d 1f\n\t"		\
	"ld.w %[pc],%%pc\n1:\n\txld.w %[label],1b\n"
#define JR_NOT "ld.w %[t],0\n\tcmp %[t],0\n\tjrne.d 1f\n\t"		\
	"ld.w %[pc],%%pc\n1:\n\txld.w %[label],1b\n"
#define CALL "call.d 3f\n\tld.w %[pc],%%pc\n1:\n\txld.w %[label],1b\n\t"	\
	"jp 4f\n3:\n\tret\n4:\n"
/* What the September probe did: no delayed branch at all. */
#define PLAIN "ld.w %[pc],%%pc\n1:\n\txld.w %[label],1b\n"

#define SIXTEEN(kind, BODY)						\
	CASE(kind##_0, ".text", 0, BODY) CASE(kind##_1, ".text", 1, BODY)	\
	CASE(kind##_2, ".text", 2, BODY) CASE(kind##_3, ".text", 3, BODY)	\
	CASE(kind##_4, ".text", 4, BODY) CASE(kind##_5, ".text", 5, BODY)	\
	CASE(kind##_6, ".text", 6, BODY) CASE(kind##_7, ".text", 7, BODY)	\
	CASE(kind##_8, ".text", 8, BODY) CASE(kind##_9, ".text", 9, BODY)	\
	CASE(kind##_10, ".text", 10, BODY) CASE(kind##_11, ".text", 11, BODY)\
	CASE(kind##_12, ".text", 12, BODY) CASE(kind##_13, ".text", 13, BODY)\
	CASE(kind##_14, ".text", 14, BODY) CASE(kind##_15, ".text", 15, BODY)\
	CASE(kind##_a0, ".fastcode", 0, BODY)				\
	CASE(kind##_a5, ".fastcode", 5, BODY)				\
	static read_fn *const kind##_fns[18] = {			\
		kind##_0, kind##_1, kind##_2, kind##_3, kind##_4,	\
		kind##_5, kind##_6, kind##_7, kind##_8, kind##_9,	\
		kind##_10, kind##_11, kind##_12, kind##_13, kind##_14,	\
		kind##_15, kind##_a0, kind##_a5 };

SIXTEEN(jp_next, JP_NEXT)
SIXTEEN(jp_far, JP_FAR)
SIXTEEN(jp_reg, JP_REG)
SIXTEEN(jr_taken, JR_TAKEN)
SIXTEEN(jr_not, JR_NOT)
SIXTEEN(call_slot, CALL)
SIXTEEN(plain, PLAIN)

/* ret.d with the read in its slot: the manual's answer is the address
 * after the slot, retslot_*_end. */
unsigned long retslot_sdram(void), retslot_a0(void);
extern char retslot_sdram_end[], retslot_a0_end[];
asm (".section .text.retslot,\"ax\"\n\t.balign 32\n"
     "\t.global retslot_sdram\nretslot_sdram:\n\tret.d\n\tld.w %r4,%pc\n"
     "\t.global retslot_sdram_end\nretslot_sdram_end:\n\t.previous\n"
     ".section .fastcode.retslot,\"ax\"\n\t.balign 32\n"
     "\t.global retslot_a0\nretslot_a0:\n\tret.d\n\tld.w %r4,%pc\n"
     "\t.global retslot_a0_end\nretslot_a0_end:\n\t.previous\n");

static unsigned long ret_sdram(unsigned long *want)
{
	*want = (unsigned long)retslot_sdram_end;
	return retslot_sdram();
}
static unsigned long ret_a0(unsigned long *want)
{
	*want = (unsigned long)retslot_a0_end;
	return retslot_a0();
}
static read_fn *const ret_fns[2] = { ret_sdram, ret_a0 };

static const struct {
	const char *name;
	read_fn *const *fns;
	unsigned count;
} kinds[] = {
	{ "jp.d-next", jp_next_fns, 18 }, { "jp.d-far", jp_far_fns, 18 },
	{ "jp.d-reg", jp_reg_fns, 18 }, { "jrne.d-taken", jr_taken_fns, 18 },
	{ "jrne.d-not", jr_not_fns, 18 }, { "call.d", call_slot_fns, 18 },
	{ "ret.d", ret_fns, 2 }, { "plain", plain_fns, 18 },
};

static char log_text[24576];
static size_t log_used;

static void out(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(log_text + log_used, sizeof(log_text) - log_used,
		      format, ap);
	va_end(ap);
	if (n > 0) {
		debug_print(log_text + log_used);
		log_used += (size_t)n < sizeof(log_text) - log_used ?
			    (size_t)n : sizeof(log_text) - log_used - 1;
	}
}

/* A real GCC trampoline: taking a nested function's address puts one on
 * the stack, and calling through it is the delayed read in use. */
static int __attribute__((noinline)) apply(int (*volatile f)(int), int v)
{
	return f(v);
}

static int __attribute__((noinline)) through_trampoline(int x)
{
	int nested(int y) { return x + 3 * y; }
	return apply(nested, 5);
}

int grifo_main(int argc, char **argv)
{
	int power = 0, plain_too = 1;

	/* "off" powers off at the end, for the emulator, which also needs
	   "noplain": it stops at a PC read outside a delay slot. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "off") == 0)
			power = 1;
		if (strcmp(argv[i], "noplain") == 0)
			plain_too = 0;
	}
	unsigned bad_kinds = 0, tramp_bad = 0;
	event_t event;
	int h;

	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_print("PC read test\nplease wait...\n");
	out("PCTEST v1 build=%s %s runs=%u\n", __DATE__, __TIME__, RUNS);
	for (unsigned k = 0; k < sizeof(kinds) / sizeof(*kinds); k++) {
		if (!plain_too && strcmp(kinds[k].name, "plain") == 0)
			continue;
		unsigned long match = 0, total = 0, first_bad = 0;
		long first_delta = 0;
		int have_bad = 0;

		for (unsigned i = 0; i < kinds[k].count; i++) {
			unsigned long hits = 0, want = 0, got = 0;
			int sdram = kinds[k].count == 2 ? i == 0 : i < 16;
			unsigned offset = kinds[k].count == 2 ? 0 :
					  i < 16 ? i * 2 : i == 16 ? 0 : 10;

			for (unsigned r = 0; r < RUNS; r++) {
				got = kinds[k].fns[i](&want);
				if (got == want) {
					hits++;
				} else if (!have_bad) {
					have_bad = 1;
					first_bad = got;
					first_delta = (long)(got - want);
				}
			}
			out("CASE kind=%s place=%s offset=%u want=%08lx last=%08lx "
			    "delta=%+ld match=%lu/%u\n", kinds[k].name,
			    sdram ? "sdram" : "a0", offset, want, got,
			    (long)(got - want), hits, RUNS);
			match += hits;
			total += RUNS;
		}
		if (match != total)
			bad_kinds++;
		out("RESULT kind=%s match=%lu/%lu first_bad=%08lx first_delta=%+ld\n",
		    kinds[k].name, match, total, first_bad, first_delta);
	}
	for (int x = -3; x <= 3; x++)
		for (unsigned r = 0; r < 100; r++)
			if (through_trampoline(x) != x + 15)
				tramp_bad++;
	out("RESULT kind=trampoline match=%u/700\n", 700 - tramp_bad);
	out("END PCTEST bad_kinds=%u trampoline_bad=%u\n", bad_kinds, tramp_bad);

	h = file_create("pctest.log", FILE_OPEN_WRITE);
	if (h >= 0) {
		file_write(h, log_text, log_used);
		file_close(h);
	}
	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_printf("PC read test done.\n%u kinds differ,\ntrampoline %s.\n"
		   "Results in pctest.log.\nTap to return.\n", bad_kinds,
		   tramp_bad ? "FAILED" : "works");
	if (power)
		power_off();
	event_flush();
	for (;;) {
		event_wait(&event, NULL, NULL);
		if (event.item_type == EVENT_TOUCH_DOWN ||
		    event.item_type == EVENT_BUTTON_DOWN)
			return 0;
	}
}
