/* Opt-in power counters and KEEP-only persistent suspend checkpoints. */
#include "standard.h"

#include <stdio.h>
#include <regs.h>
#include <samo.h>

#include "file.h"
#include "power_log.h"
#include "sdram.h"
#include "serial.h"
#if SD_DMA_ENABLED
#include "sd_dma.h"
#endif

static bool enabled;
static unsigned long entries, resumes, timeouts;
static unsigned long sd_on_entries, sd_on_resumes;
static unsigned long last_refresh, refresh_mismatches;
static unsigned long card_inits, card_failures, card_max_ticks;
static uint64_t card_ticks;
static unsigned long idle_waits, idle_timeouts, idle_max_ticks;
static uint64_t idle_ticks;

#if !defined(CARD_POWER_OFF_ON_SUSPEND)
static bool trace_enabled;

/* Diagnostic KEEP builds only: file I/O here must not turn an OFF card
 * back on just before halt. Each closed checkpoint survives a later hang.
 * This deliberately adds I/O and is not a battery measurement mode. */
static void checkpoint(const char *phase)
{
	char report[480];
	int length, handle;
	if (!trace_enabled)
		return;
	length = snprintf(report, sizeof(report),
		"power trace v1; gcc %s\n"
		"policy card=KEEP; last completed checkpoint=%s\n"
		"suspend entries=%lu resumes=%lu timeouts=%lu\n"
		"sd_supply=%s gate1=0x%08lx clock=0x%08lx\n"
		"refresh=0x%08lx watchdog_count=%lu watchdog_limit=%lu\n"
		"This is a checkpoint, not a crash backtrace.\n",
		__VERSION__, phase,
		entries, resumes, timeouts,
		(REG_P3_P3D & P32_BIT) ? "OFF" : "ON",
		(unsigned long)REG_CMU_GATEDCLK1,
		(unsigned long)REG_CMU_CLKCNTL,
		(unsigned long)REG_SDRAMC_REF,
		(unsigned long)REG_WD_CNT, (unsigned long)REG_WD_COMP);
	if (length < 0 || (size_t)length >= sizeof(report))
		return;
	handle = File_create("pwrtrace.txt", FILE_OPEN_WRITE);
	if (handle < 0) {
		Serial_print("power trace: cannot create pwrtrace.txt\n");
		return;
	}
	ssize_t written = File_write(handle, report, (size_t)length);
	File_ErrorType closed = File_close(handle);
	if (written != length || closed != FILE_ERROR_OK)
		Serial_print("power trace: write failed\n");
}
#else
#define checkpoint(phase) ((void)0)
#endif

void PowerLog_initialise(void)
{
	unsigned long size;
	enabled = File_size("powerlog.on", &size) == FILE_ERROR_OK;
	if (enabled)
		Serial_print("power log: RAM counters enabled; power.txt on shutdown\n");
#if !defined(CARD_POWER_OFF_ON_SUSPEND)
	trace_enabled = enabled && File_size("pwrtrace.on", &size) == FILE_ERROR_OK;
	checkpoint("boot");
#endif
}

bool PowerLog_enabled(void)
{
	return enabled;
}

void PowerLog_suspend(void)
{
	if (!enabled)
		return;
	++entries;
	/* P32 controls the supply; P33 is the bus buffer, not the supply. */
	if (!(REG_P3_P3D & P32_BIT))
		++sd_on_entries;
	checkpoint("before suspend");
}

void PowerLog_resume(bool timeout)
{
	if (!enabled)
		return;
	++resumes;
	if (timeout)
		++timeouts;
	if (!(REG_P3_P3D & P32_BIT))
		++sd_on_resumes;
	last_refresh = (REG_SDRAMC_REF >> AURCO_SHIFT) & 0xfff;
	if (last_refresh != SDRAM_REFRESH)
		++refresh_mismatches;
	checkpoint("after resume");
}

void PowerLog_card_init(unsigned long ticks, bool ready)
{
	if (!enabled)
		return;
	++card_inits;
	if (!ready)
		++card_failures;
	card_ticks += ticks;
	if (ticks > card_max_ticks)
		card_max_ticks = ticks;
}

void PowerLog_report(void)
{
	char report[800];
	int length, handle;
	ssize_t written;
	File_ErrorType closed;
	if (!enabled)
		return;
	/* Freeze the snapshot and exclude this report's own card restart. */
	enabled = false;
	length = snprintf(report, sizeof(report),
		"power log v1; gcc %s\n"
		"policy card=%s auto_off=%us refresh=0x%lx\n"
		"suspend entries=%lu resumes=%lu timeouts=%lu\n"
		"sd_supply on_entries=%lu on_resumes=%lu\n"
		"refresh last=0x%lx mismatches=%lu\n"
		"card reinit=%lu failures=%lu total_ms=%lu max_ticks=%lu ticks_per_ms=%lu\n"
		"idle waits=%lu deadlines=%lu total_ms=%lu max_ticks=%lu\n"
		"%s\n",
		__VERSION__,
#if defined(CARD_POWER_OFF_ON_SUSPEND)
		"OFF",
#else
		"KEEP",
#endif
		(unsigned)SUSPEND_AUTO_POWER_OFF_SECONDS, (unsigned long)SDRAM_REFRESH,
		entries, resumes, timeouts, sd_on_entries, sd_on_resumes,
		last_refresh, refresh_mismatches,
		card_inits, card_failures, (unsigned long)(card_ticks / (PLL_CLK / 1000)),
		card_max_ticks,
		(unsigned long)(PLL_CLK / 1000),
		idle_waits, idle_timeouts, (unsigned long)(idle_ticks / (PLL_CLK / 1000)),
		idle_max_ticks,
#if SD_DMA_ENABLED
		SD_DMA_status()
#else
		"dma: disabled"
#endif
		);
	if (length < 0 || (size_t)length >= sizeof(report)) {
		Serial_print("power log: report overflow\n");
		return;
	}
	handle = File_create("power.txt", FILE_OPEN_WRITE);
	if (handle < 0) {
		Serial_print("power log: cannot create power.txt\n");
		return;
	}
	written = File_write(handle, report, (size_t)length);
	closed = File_close(handle);
	Serial_print(written == length && closed == FILE_ERROR_OK ?
		     "power log: saved power.txt\n" : "power log: write failed\n");
}

void PowerLog_idle(unsigned long ticks, bool timeout)
{
	if (!enabled)
		return;
	++idle_waits;
	if (timeout)
		++idle_timeouts;
	idle_ticks += ticks;
	if (ticks > idle_max_ticks)
		idle_max_ticks = ticks;
}
