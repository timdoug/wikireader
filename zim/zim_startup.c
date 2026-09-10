#include <grifo.h>
#include <stdio.h>
#include <string.h>

#include "zim_startup.h"

/* All hot-path measurements stay in RAM. No formatting or card writes until
 * after the keyboard has been drawn and all counters have been frozen. */
static int enabled, started, phase, pending;
static unsigned long app_start, ticks[4];
static file_io_stats_t stats[4];
static const char *path;

int __attribute__((section(".copycode"))) zim_startup_logging(void)
{ return enabled; }

void zim_startup_init(void)
{
	unsigned long size;
	app_start = timer_get();
	enabled = file_size("0:/zimlog.on", &size) == FILE_ERROR_OK;
}

void zim_startup_begin(const char *archive_path)
{
	if (!enabled || started)
		return;
	started = 1;
	path = archive_path ? archive_path : "wiki.zim (fallback path)";
	file_profile(&stats[0], true);
	ticks[0] = timer_get();
}

static void checkpoint(int next)
{
	if (!enabled || !started || phase != next - 1)
		return;
	ticks[next] = timer_get();
	file_profile(&stats[next], next != 3);
	phase = next;
}

void zim_startup_file_ready(void) { checkpoint(1); }
void zim_startup_archive_ready(void) { checkpoint(2); }

void zim_startup_keyboard_ready(void)
{
	checkpoint(3);
	if (phase == 3)
		pending = 1;
}

/* Emit one bounded record per boot, append across boots. A close commits the
 * record without relying on the user eventually taking the power-off path. */
void zim_startup_flush(void)
{
	static char record[2048];
	static const char *const names[] = { "file", "indexes", "ui" };
	unsigned long length = 0;
	int handle, used, i, wrote;
	if (!pending)
		return;
	pending = 0;
	used = snprintf(record, sizeof(record),
		"ZIMBOOT v1 build=%s %s dma_bits=%lu\narchive=%s\n"
		"open_to_keyboard_us=%lu app_to_keyboard_us=%lu\n",
		__DATE__, __TIME__, (unsigned long)stats[3].dma_bits, path,
		(ticks[3] - ticks[0]) / TIMER_CountsPerMicroSecond,
		(ticks[3] - app_start) / TIMER_CountsPerMicroSecond);
	for (i = 1; i <= 3 && used > 0 && used < (int)sizeof(record); i++) {
		const file_io_stats_t *a = &stats[i - 1], *b = &stats[i];
#define DELTA(field) ((unsigned long)(b->field - a->field))
		int count = snprintf(record + used, sizeof(record) - used,
			"%s elapsed_us=%lu read_calls=%lu read_sectors=%lu "
			"read_us=%lu read_errors=%lu dma32_bytes=%lu dma8_bytes=%lu "
			"dma_wait_us=%lu bypass_bytes=%lu timeouts=%lu dma_errors=%lu\n",
			names[i - 1], (ticks[i] - ticks[i - 1]) / TIMER_CountsPerMicroSecond,
			DELTA(read_calls), DELTA(read_sectors),
			DELTA(read_ticks) / TIMER_CountsPerMicroSecond, DELTA(read_errors),
			DELTA(dma32_bytes), DELTA(dma8_bytes),
			DELTA(dma_wait_ticks) / TIMER_CountsPerMicroSecond,
			DELTA(dma_bypass_bytes), DELTA(dma_timeouts), DELTA(dma_errors));
#undef DELTA
		if (count < 0)
			return;
		used += count;
	}
	if (used <= 0 || used >= (int)sizeof(record))
		return;
	used += snprintf(record + used, sizeof(record) - used,
		"dma_disabled_at_start=%lu dma_disabled_at_end=%lu\nEND ZIMBOOT\n\n",
		(unsigned long)stats[0].dma_disabled, (unsigned long)stats[3].dma_disabled);
	if (used <= 0 || used >= (int)sizeof(record))
		return;
	if (file_size("0:/zimboot.log", &length) == FILE_ERROR_OK && length < 65536) {
		handle = file_open("0:/zimboot.log", FILE_OPEN_READ | FILE_OPEN_WRITE);
		if (handle >= 0 && file_lseek(handle, length) != FILE_ERROR_OK) {
			file_close(handle);
			return;
		}
	} else {
		handle = file_create("0:/zimboot.log", FILE_OPEN_WRITE);
	}
	if (handle < 0) {
		debug_printf("ZIM startup log: cannot open (%d)\n", handle);
		return;
	}
	wrote = file_write(handle, record, used);
	i = file_close(handle);
	debug_printf("ZIM startup log: %s (%d bytes)\n",
		wrote == used && i == FILE_ERROR_OK ? "saved" : "write failed", wrote);
}
