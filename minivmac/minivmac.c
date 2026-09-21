/*
 * WikiReader operating-system glue for Mini vMac.
 *
 * Mini vMac is GPL-2.0; this port is GPL-3.0-or-later.  The combined
 * application is distributed under GPL-3.0-or-later.
 */
#include "OSGCOMUI.h"
#include "OSGCOMUD.h"
#include <stdio.h>

#ifdef WantOSGLUWR

GLOBALOSGLUPROC MyMoveBytes(anyp src, anyp dst, si5b count)
{
	memmove(dst, src, (size_t)count);
}

#define NeedCell2PlainAsciiMap 1
#include "INTLCHAR.h"

#define WantColorTransValid 0
#define Screen_OutputFrame MiniVMac_CommonScreen_OutputFrame
#include "COMOSGLU.h"
#undef Screen_OutputFrame
#include "CONTROLM.h"
#include "PROGMAIN.h"

extern blnr SCC_InterruptsEnabled(void);

enum {
	WR_VIEW_HEIGHT = 160,
	WR_LCD_STRIDE = LCD_BUFFER_WIDTH_BYTES,
	WR_TICK_CLOCKS = 997549, /* 60 MHz / 60.14742 Hz */
	WR_MAC_DATE_1986 = 2587766400UL,
	WR_INTERACTIVE_FRAMES = 30,
	WR_PAN_X_BORDER = 16,
	WR_PAN_Y_BORDER = 12,
	WR_PAN_X_STEP = 16,
	WR_PAN_Y_STEP = 12,
};

#define WR_FAST_CODE __attribute__((section(".fastcode"), noinline))

#if !MayFullScreen || !EnableFSMouseMotion
#error "WikiReader native viewport requires Mini vMac fullscreen scrolling"
#endif

#ifndef MINIVMAC_PROFILE
#define MINIVMAC_PROFILE 0
#endif

#if MINIVMAC_PROFILE
enum {
	WR_PROFILE_JIT_BUILDS,
	WR_PROFILE_JIT_PROMOTIONS,
	WR_PROFILE_JIT_REBUILDS,
	WR_PROFILE_JIT_FLUSHES,
	WR_PROFILE_JIT_DIRECT_BUILDS,
	WR_PROFILE_JIT_DIRECT_REG_INVALID,
	WR_PROFILE_JIT_DIRECT_MAP_INVALID,
	WR_PROFILE_JIT_CODE_INVALID,
	WR_PROFILE_JIT_GUEST_OPS_BUILT,
	WR_PROFILE_JIT_FALLBACK_OPS_BUILT,
	WR_PROFILE_JIT_EVICTIONS,
	WR_PROFILE_JIT_COUNT
};
#if MINIVMAC_JIT
extern void MiniVMac_JIT_GetProfile(ui5r *out);
extern void MiniVMac_JIT_GetProfileHot(ui5r *out);
extern void MiniVMac_JIT_GetCacheProfile(ui5r *out);
#endif
#endif

/* ------------------------------------------------------------------------- */
/* Persistent 68000 decode table                                             */

#define WR_DECODE_CACHE_PATH "/minivmac/m68k-v1.tbl"

LOCALVAR const ui3b WR_DecodeCacheHeader[16] = {
	'W', 'R', 'M', '6', '8', 'K', '1', 0,
	8, 0, 0, 0, 0, 0, 0, 0
};

LOCALFUNC blnr WR_FileTransferExact(int handle, ui3p data, ui5r size,
	blnr write)
{
	while (size != 0) {
		ui5r chunk = size > 4096 ? 4096 : size;
		ssize_t done = write ? file_write(handle, data, chunk)
			: file_read(handle, data, chunk);
		if (done != (ssize_t)chunk)
			return falseblnr;
		data += chunk;
		size -= chunk;
		watchdog(WATCHDOG_KEY);
	}
	return trueblnr;
}

GLOBALFUNC blnr MiniVMac_LoadDecodeTable(ui3p table, ui5r size)
{
	ui3b header[sizeof WR_DecodeCacheHeader];
	unsigned long file_bytes;
	ui3r i;
	int handle;
	blnr ok = trueblnr;

	if (file_size(WR_DECODE_CACHE_PATH, &file_bytes) != FILE_ERROR_OK
		|| file_bytes != sizeof WR_DecodeCacheHeader + size)
		return falseblnr;
	handle = file_open(WR_DECODE_CACHE_PATH, FILE_OPEN_READ);
	if (handle < 0)
		return falseblnr;
	if (!WR_FileTransferExact(handle, header, sizeof header, falseblnr))
		ok = falseblnr;
	for (i = 0; ok && i < sizeof header; ++i) {
		if (header[i] != WR_DecodeCacheHeader[i])
			ok = falseblnr;
	}
	if (ok && !WR_FileTransferExact(handle, table, size, falseblnr))
		ok = falseblnr;
	if (file_close(handle) != FILE_ERROR_OK)
		ok = falseblnr;
	return ok;
}

GLOBALPROC MiniVMac_SaveDecodeTable(ui3p table, ui5r size)
{
	int handle = file_create(WR_DECODE_CACHE_PATH, FILE_OPEN_WRITE);
	blnr ok;

	if (handle < 0)
		return;
	ok = WR_FileTransferExact(handle, (ui3p)WR_DecodeCacheHeader,
		sizeof WR_DecodeCacheHeader, trueblnr)
		&& WR_FileTransferExact(handle, table, size, trueblnr);
	if (file_close(handle) != FILE_ERROR_OK)
		ok = falseblnr;
	if (!ok)
		debug_printf("Mini vMac decode-table cache write failed\n");
}

/* ------------------------------------------------------------------------- */
/* Disk images and ROM                                                       */

typedef struct {
	int handle;
	ui5r size;
	blnr writable;
} wr_drive;

LOCALVAR wr_drive Drives[NumDrives];

LOCALPROC InitDrives(void)
{
	int i;
	for (i = 0; i < NumDrives; ++i) {
		Drives[i].handle = -1;
		Drives[i].size = 0;
		Drives[i].writable = falseblnr;
	}
}

GLOBALOSGLUFUNC tMacErr vSonyTransfer(blnr write, ui3p buffer,
	tDrive drive, ui5r start, ui5r count, ui5r *actual)
{
	wr_drive *d;
	ssize_t n;
#if MINIVMAC_PROFILE
	ui5b profile_start = timer_get();
#endif

	if (actual != nullpr)
		*actual = 0;
	if (drive >= NumDrives)
		return mnvm_nsDrvErr;
	d = &Drives[drive];
	if (d->handle < 0)
		return mnvm_offLinErr;
	if (write && !d->writable)
		return mnvm_wPrErr;
	if (file_lseek(d->handle, start) != FILE_ERROR_OK)
		return mnvm_miscErr;
	n = write ? file_write(d->handle, buffer, count)
		: file_read(d->handle, buffer, count);
	if (n < 0)
		return mnvm_miscErr;
	if (actual != nullpr)
		*actual = (ui5r)n;
	if (write && start + (ui5r)n > d->size)
		d->size = start + (ui5r)n;
	watchdog(WATCHDOG_KEY);
#if MINIVMAC_PROFILE
	extern ui5b WR_ProfileDiskTicks;
	WR_ProfileDiskTicks += timer_get() - profile_start;
#endif
	return (ui5r)n == count ? mnvm_noErr : mnvm_eofErr;
}

GLOBALOSGLUFUNC tMacErr vSonyGetSize(tDrive drive, ui5r *size)
{
	if (drive >= NumDrives || Drives[drive].handle < 0)
		return mnvm_offLinErr;
	*size = Drives[drive].size;
	return mnvm_noErr;
}

GLOBALOSGLUFUNC tMacErr vSonyEject(tDrive drive)
{
	wr_drive *d;
	if (drive >= NumDrives || Drives[drive].handle < 0)
		return mnvm_offLinErr;
	d = &Drives[drive];
#if MINIVMAC_PROFILE
	extern ui5b WR_ProfileEjects;
	++WR_ProfileEjects;
#endif
	DiskEjectedNotify(drive);
	if (d->writable)
		(void)file_sync(d->handle);
	(void)file_close(d->handle);
	d->handle = -1;
	return mnvm_noErr;
}

LOCALPROC UnInitDrives(void)
{
	tDrive i;
	for (i = 0; i < NumDrives; ++i)
		if (vSonyIsInserted(i))
			(void)vSonyEject(i);
}

LOCALFUNC blnr SonyInsertPath(const char *path)
{
	tDrive drive;
	unsigned long size;
	int handle;
	blnr locked = falseblnr;

	if (!FirstFreeDisk(&drive) || file_size(path, &size) != FILE_ERROR_OK)
		return falseblnr;
	handle = file_open(path, FILE_OPEN_READ | FILE_OPEN_WRITE);
	if (handle < 0) {
		handle = file_open(path, FILE_OPEN_READ);
		locked = trueblnr;
	}
	if (handle < 0)
		return falseblnr;
	Drives[drive].handle = handle;
	Drives[drive].size = (ui5r)size;
	Drives[drive].writable = !locked;
	DiskInsertNotify(drive, locked);
	return trueblnr;
}

LOCALFUNC blnr Sony_InsertIth(int i)
{
	static const char *const paths[] = {
		"/minivmac/disk1.dsk", "/minivmac/disk2.dsk",
		"/minivmac/disk3.dsk", "/minivmac/disk4.dsk",
		"/minivmac/disk5.dsk", "/minivmac/disk6.dsk",
	};
	if (i < 1 || i > (int)(sizeof(paths) / sizeof(paths[0])))
		return falseblnr;
	return SonyInsertPath(paths[i - 1]);
}

LOCALFUNC blnr LoadInitialImages(void)
{
	int i;
	if (!Sony_InsertIth(1))
		return falseblnr;
	for (i = 2; Sony_InsertIth(i); ++i)
		;
	return trueblnr;
}

LOCALFUNC blnr LoadMacRom(void)
{
	int handle;
	ssize_t n;

	handle = file_open("/minivmac/MacPlus.ROM", FILE_OPEN_READ);
	if (handle < 0) {
		debug_print("Mini vMac: /minivmac/MacPlus.ROM not found\n");
		return falseblnr;
	}
	n = file_read(handle, ROM, kROM_Size);
	(void)file_close(handle);
	if (n != kROM_Size) {
		debug_printf("Mini vMac: ROM must be exactly %lu bytes\n",
			(unsigned long)kROM_Size);
		return falseblnr;
	}
	if (ROM_IsValid() != mnvm_noErr) {
		debug_print("Mini vMac: unsupported or corrupt ROM\n");
		return falseblnr;
	}
	return trueblnr;
}

/* ------------------------------------------------------------------------- */
/* Native 240x160 viewport into the Macintosh's 512x342 display              */

LOCALVAR uint8_t *LCD;
LOCALVAR uint8_t *LCDBack;
LOCALVAR uint8_t *LCDBest;
LOCALVAR ui5b WR_LCDBuffers[3][LCD_BUFFER_SIZE_WORDS]
	__attribute__((aligned(4)));
LOCALVAR ui3p MacScreen;
LOCALVAR ui5b WR_FrameBestScore;
LOCALVAR ui5b WR_FrameFrontScore;
LOCALVAR ui3r WR_FrameBatchCount;
LOCALVAR ui3r WR_FrameHeldBatches;
LOCALVAR ui3r WR_InteractiveFrames;
LOCALVAR ui4r WR_FrameViewH;
LOCALVAR ui4r WR_FrameViewV;
LOCALVAR ui5b TouchDownCount;
LOCALVAR ui5b TouchMotionCount;
LOCALVAR ui5b TouchUpCount;

#if MINIVMAC_PROFILE
enum {
	WR_PROFILE_CLOCK_HZ = 60000000UL,
	WR_PROFILE_WINDOW_CLOCKS = 10UL * WR_PROFILE_CLOCK_HZ,
	WR_PROFILE_LOG_LIMIT = 256UL * 1024UL,
};

#define WR_PROFILE_LOG_PATH "0:/minivmac/perf.log"

LOCALVAR ui5b WR_ProfileStart;
LOCALVAR ui5b WR_ProfileFrames;
LOCALVAR ui5b WR_ProfileScreenTicks;
ui5b WR_ProfileDiskTicks;
ui5b WR_ProfileEjects;
LOCALVAR ui5b WR_ProfileWaitTicks;
LOCALVAR ui5b WR_ProfileOverheadTicks;
LOCALVAR ui5b WR_ProfileSequence;
LOCALVAR ui5b WR_ProfileJITPrevious[WR_PROFILE_JIT_COUNT];
LOCALVAR blnr WR_ProfileActive;
LOCALVAR blnr WR_ProfileLogFailed;

LOCALFUNC ui5b WR_ProfileTenths(ui5b ticks, ui5b elapsed)
{
	ui5b divisor = elapsed / 1000;
	return divisor != 0 ? ticks / divisor : 0;
}

LOCALPROC WR_ProfileLine(int row, const char *text)
{
	char padded[31];
	(void)snprintf(padded, sizeof(padded), "%-29.29s", text);
	lcd_at_xy(0, row);
	lcd_print(padded);
}

LOCALPROC WR_ProfileAppend(char *record, ui5b length)
{
	unsigned long size = 0;
	int status;
	int handle = -1;
	ssize_t wrote = -1;

	if (WR_ProfileLogFailed)
		return;
	status = file_size(WR_PROFILE_LOG_PATH, &size);
	if (status == FILE_ERROR_NO_FILE) {
		handle = file_create(WR_PROFILE_LOG_PATH, FILE_OPEN_WRITE);
	} else if (status == FILE_ERROR_OK && size + length <= WR_PROFILE_LOG_LIMIT) {
		handle = file_open(WR_PROFILE_LOG_PATH,
			FILE_OPEN_READ | FILE_OPEN_WRITE);
		if (handle >= 0 && file_lseek(handle, size) != FILE_ERROR_OK) {
			(void)file_close(handle);
			handle = -1;
		}
	}
	if (handle >= 0) {
		wrote = file_write(handle, record, length);
		status = file_close(handle);
	}
	if (handle < 0 || wrote != (ssize_t)length || status != FILE_ERROR_OK) {
		WR_ProfileLogFailed = trueblnr;
		debug_printf("Mini vMac profiler log failed: size=%lu len=%lu handle=%d write=%ld status=%d\n",
			size, (unsigned long)length, handle, (long)wrote, status);
	}
}

LOCALPROC WR_ProfileReport(ui5b now, blnr partial)
{
	ui5b elapsed = now - WR_ProfileStart;
	ui5b measured;
	ui5b core;
	ui5b hz100;
	ui5b realtime10;
	ui5b jit[WR_PROFILE_JIT_COUNT] = {0};
	ui5b hot[31] = {0};
	ui5b cache[12] = {0};
	ui5b delta[WR_PROFILE_JIT_COUNT];
	ui5b overhead_start;
	char line[40];
	char record[1024];
	int length;
	int i;

	if (elapsed == 0 || WR_ProfileFrames == 0)
		return;
	overhead_start = timer_get();
#if MINIVMAC_JIT
	MiniVMac_JIT_GetProfile(jit);
	MiniVMac_JIT_GetProfileHot(hot);
	MiniVMac_JIT_GetCacheProfile(cache);
#endif
	for (i = 0; i < WR_PROFILE_JIT_COUNT; ++i) {
		delta[i] = jit[i] - WR_ProfileJITPrevious[i];
		WR_ProfileJITPrevious[i] = jit[i];
	}
	measured = WR_ProfileScreenTicks + WR_ProfileDiskTicks
		+ WR_ProfileWaitTicks + WR_ProfileOverheadTicks;
	core = measured < elapsed ? elapsed - measured : 0;
	/* Keep the products inside 32 bits on a ten-second window. */
	hz100 = WR_ProfileFrames * 6000000UL / (elapsed / 1000UL);
	realtime10 = WR_ProfileFrames * WR_TICK_CLOCKS / (elapsed / 1000UL);

	memset(LCD + WR_VIEW_HEIGHT * WR_LCD_STRIDE, 0,
		(LCD_HEIGHT - WR_VIEW_HEIGHT) * WR_LCD_STRIDE);
	(void)snprintf(line, sizeof(line),
		"HW %lu.%02lu %lu.%lu C%lu.%lu L%lu.%lu",
		(unsigned long)(hz100 / 100), (unsigned long)(hz100 % 100),
		(unsigned long)(realtime10 / 10), (unsigned long)(realtime10 % 10),
		(unsigned long)(WR_ProfileTenths(core, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(core, elapsed) % 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileScreenTicks, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileScreenTicks, elapsed) % 10));
	WR_ProfileLine(12, line);
	(void)snprintf(line, sizeof(line), "SD %lu.%lu SYNC %lu.%lu OVH %lu.%lu",
		(unsigned long)(WR_ProfileTenths(WR_ProfileDiskTicks, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileDiskTicks, elapsed) % 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileWaitTicks, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileWaitTicks, elapsed) % 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileOverheadTicks, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileOverheadTicks, elapsed) % 10));
	WR_ProfileLine(13, line);
	(void)snprintf(line, sizeof(line), "JIT B%lu P%lu F%lu I%lu",
		(unsigned long)delta[WR_PROFILE_JIT_BUILDS],
		(unsigned long)delta[WR_PROFILE_JIT_PROMOTIONS],
		(unsigned long)delta[WR_PROFILE_JIT_FALLBACK_OPS_BUILT],
		(unsigned long)(delta[WR_PROFILE_JIT_DIRECT_REG_INVALID]
			+ delta[WR_PROFILE_JIT_DIRECT_MAP_INVALID]
			+ delta[WR_PROFILE_JIT_CODE_INVALID]));
	WR_ProfileLine(14, WR_ProfileLogFailed ? "JIT counters; LOG FAILED" : line);

	length = snprintf(record, sizeof(record),
		"MINIVMACPERF v=1 engine=v12-regions seq=%lu kind=%s elapsed_us=%lu frames=%lu hz_x100=%lu realtime_x10=%lu\n"
		"TIME core_us=%lu lcd_us=%lu disk_us=%lu sync_us=%lu profiler_us=%lu\n"
		"JIT builds=%lu promotions=%lu rebuilds=%lu flushes=%lu direct_builds=%lu direct_reg_invalid=%lu direct_map_invalid=%lu code_invalid=%lu guest_ops_built=%lu fallback_ops_built=%lu evictions=%lu"
		" delta_builds=%lu delta_promotions=%lu delta_rebuilds=%lu delta_flushes=%lu delta_direct_builds=%lu delta_direct_reg_invalid=%lu delta_direct_map_invalid=%lu delta_code_invalid=%lu delta_guest_ops_built=%lu delta_fallback_ops_built=%lu delta_evictions=%lu\n",
		(unsigned long)WR_ProfileSequence++, partial ? "partial" : "window",
		(unsigned long)(elapsed / 60), (unsigned long)WR_ProfileFrames,
		(unsigned long)hz100, (unsigned long)realtime10,
		(unsigned long)(core / 60),
		(unsigned long)(WR_ProfileScreenTicks / 60),
		(unsigned long)(WR_ProfileDiskTicks / 60),
		(unsigned long)(WR_ProfileWaitTicks / 60),
		(unsigned long)(WR_ProfileOverheadTicks / 60),
		(unsigned long)jit[0], (unsigned long)jit[1],
		(unsigned long)jit[2], (unsigned long)jit[3],
		(unsigned long)jit[4], (unsigned long)jit[5],
		(unsigned long)jit[6], (unsigned long)jit[7],
		(unsigned long)jit[8], (unsigned long)jit[9],
		(unsigned long)jit[10],
		(unsigned long)delta[0], (unsigned long)delta[1],
		(unsigned long)delta[2], (unsigned long)delta[3],
		(unsigned long)delta[4], (unsigned long)delta[5],
		(unsigned long)delta[6], (unsigned long)delta[7],
		(unsigned long)delta[8], (unsigned long)delta[9],
		(unsigned long)delta[10]);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	length = snprintf(record, sizeof(record),
		"FALLBACK o0=%04lx/%lu o1=%04lx/%lu o2=%04lx/%lu o3=%04lx/%lu o4=%04lx/%lu o5=%04lx/%lu o6=%04lx/%lu o7=%04lx/%lu\n",
		(unsigned long)hot[15], (unsigned long)hot[16],
		(unsigned long)hot[17], (unsigned long)hot[18],
		(unsigned long)hot[19], (unsigned long)hot[20],
		(unsigned long)hot[21], (unsigned long)hot[22],
		(unsigned long)hot[23], (unsigned long)hot[24],
		(unsigned long)hot[25], (unsigned long)hot[26],
		(unsigned long)hot[27], (unsigned long)hot[28],
		(unsigned long)hot[29], (unsigned long)hot[30]);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	length = snprintf(record, sizeof(record),
		"CACHE active=%lu slots=%lu ways=%lu code_used=%lu code_bytes=%lu fast_used=%lu largest_free=%lu free_chunks=%lu code_peak=%lu reclaimed=%lu free_bytes=%lu alloc_errors=%lu\n",
		(unsigned long)cache[0], (unsigned long)cache[1],
		(unsigned long)cache[2], (unsigned long)cache[3],
		(unsigned long)cache[4], (unsigned long)cache[5],
		(unsigned long)cache[6], (unsigned long)cache[7],
		(unsigned long)cache[8], (unsigned long)cache[9],
		(unsigned long)cache[10], (unsigned long)cache[11]);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	length = snprintf(record, sizeof(record),
		"HOT pc=%06lx p0=%06lx/%lu/%04lx p1=%06lx/%lu/%04lx p2=%06lx/%lu/%04lx p3=%06lx/%lu/%04lx exec_ops=%lu fallback_ops=%lu\n",
		(unsigned long)hot[0],
		(unsigned long)hot[1], (unsigned long)hot[2],
		(unsigned long)hot[3], (unsigned long)hot[4],
		(unsigned long)hot[5], (unsigned long)hot[6],
		(unsigned long)hot[7], (unsigned long)hot[8],
		(unsigned long)hot[9], (unsigned long)hot[10],
		(unsigned long)hot[11], (unsigned long)hot[12],
		(unsigned long)hot[13], (unsigned long)hot[14]);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	length = snprintf(record, sizeof(record),
		"INPUT down=%lu motion=%lu up=%lu queue_in=%lu queue_out=%lu mouse_enabled=%u cursor=%lu,%lu view=%lu,%lu\n",
		(unsigned long)TouchDownCount,
		(unsigned long)TouchMotionCount,
		(unsigned long)TouchUpCount,
		(unsigned long)MyEvtQIn, (unsigned long)MyEvtQOut,
		(unsigned)SCC_InterruptsEnabled(),
		(unsigned long)CurMouseH, (unsigned long)CurMouseV,
		(unsigned long)ViewHStart, (unsigned long)ViewVStart);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	debug_printf("Mini vMac profile: %lu.%02lu Hz, %lu.%lu%% realtime, core=%lu.%lu%% lcd=%lu.%lu%%\n",
		(unsigned long)(hz100 / 100), (unsigned long)(hz100 % 100),
		(unsigned long)(realtime10 / 10), (unsigned long)(realtime10 % 10),
		(unsigned long)(WR_ProfileTenths(core, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(core, elapsed) % 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileScreenTicks, elapsed) / 10),
		(unsigned long)(WR_ProfileTenths(WR_ProfileScreenTicks, elapsed) % 10));

	WR_ProfileStart = now;
	WR_ProfileFrames = 0;
	WR_ProfileScreenTicks = 0;
	WR_ProfileDiskTicks = 0;
	WR_ProfileWaitTicks = 0;
	WR_ProfileOverheadTicks = timer_get() - overhead_start;
}

LOCALPROC WR_ProfileInit(void)
{
	WR_ProfileStart = timer_get();
	WR_ProfileFrames = 0;
	WR_ProfileScreenTicks = 0;
	WR_ProfileDiskTicks = 0;
	WR_ProfileWaitTicks = 0;
	WR_ProfileOverheadTicks = 0;
	WR_ProfileSequence = 0;
	WR_ProfileEjects = 0;
	WR_ProfileActive = trueblnr;
	WR_ProfileLogFailed = falseblnr;
	memset(WR_ProfileJITPrevious, 0, sizeof(WR_ProfileJITPrevious));
	WR_ProfileLine(12, "HW PROFILE: warming up 10s");
	WR_ProfileLine(13, "C/L=core/LCD; SD/SYNC/OVH");
	WR_ProfileLine(14, "JIT B=build P=promote F=fallback");
}

LOCALPROC WR_ProfileFinish(void)
{
	char record[96];
	int length;
	if (WR_ProfileActive && WR_ProfileFrames != 0)
		WR_ProfileReport(timer_get(), trueblnr);
	length = snprintf(record, sizeof(record),
		"EXIT seq=%lu force_mac_off=%u disk_ejects=%lu log_failed=%u\n",
		(unsigned long)WR_ProfileSequence, (unsigned)ForceMacOff,
		(unsigned long)WR_ProfileEjects, (unsigned)WR_ProfileLogFailed);
	if (length > 0 && (size_t)length < sizeof(record))
		WR_ProfileAppend(record, (ui5b)length);
	WR_ProfileActive = falseblnr;
}
#endif

/* Preserve every source bit. ViewHStart moves in two-pixel increments, so
 * most positions need a byte shift; the rightmost shifted view still ends
 * inside the source's 64-byte scanline. */
static ui5r WR_FAST_CODE CopyMacViewport(ui3p src, uint8_t *lcd,
	ui4r view_x, ui4r view_y)
{
	int y;
	unsigned shift = view_x & 7;
	ui5r score = 0;

	for (y = 0; y < WR_VIEW_HEIGHT; ++y) {
		ui3p row = src + (view_y + y) * vMacScreenMonoByteWidth
			+ (view_x >> 3);
		uint8_t *line = lcd + y * WR_LCD_STRIDE;
		int x;

		if (shift == 0) {
			for (x = 0; x < LCD_WIDTH / 8; ++x) {
				ui3b value = row[x];
				line[x] = value;
				score += value != 0;
			}
		} else {
			for (x = 0; x < LCD_WIDTH / 8; ++x) {
				ui3b value = (ui3b)((row[x] << shift)
					| (row[x + 1] >> (8 - shift)));
				line[x] = value;
				score += value != 0;
			}
		}
		line[30] = 0;
		line[31] = 0;
	}
	return score;
}

LOCALPROC DrawMacScreen(void)
{
	ui5r (*volatile copy)(ui3p, uint8_t *, ui4r, ui4r) = CopyMacViewport;

	if (MacScreen != nullpr) {
		uint8_t *old_front = LCD;
		ui5r score = copy(MacScreen, LCDBack, ViewHStart, ViewVStart);
		if (ViewHStart != WR_FrameViewH || ViewVStart != WR_FrameViewV) {
			WR_FrameFrontScore = 0;
			WR_FrameHeldBatches = 0;
			WR_FrameBatchCount = 0;
		}

		/* The LCD controller continuously scans its framebuffer.  Drawing
		 * the viewport into that live buffer exposes each partly copied row
		 * as a large blank rectangle.  When emulation is badly behind, a
		 * normally imperceptible multi-frame QuickDraw repaint is also
		 * stretched into visible flashes.  Retain the most complete frame
		 * (the one with the most nonempty source bytes) over eight guest
		 * ticks, then publish it.  Realtime operation still swaps every tick. */
		/* Completeness scoring is valuable while QuickDraw is slowly
		 * assembling a boot screen, but it is the wrong tradeoff after an
		 * input event: cursor motion, highlighting and pull-down menus can
		 * all contain fewer black bytes than the old frame.  Publish input
		 * feedback immediately for a short guest-time window. */
		if (WR_InteractiveFrames != 0)
			--WR_InteractiveFrames;
		if (EmLagTime >= 4 && WR_InteractiveFrames == 0) {
			if (WR_FrameBatchCount == 0 || score > WR_FrameBestScore) {
				memcpy(LCDBest, LCDBack,
					WR_VIEW_HEIGHT * WR_LCD_STRIDE);
				WR_FrameBestScore = score;
			}
			if (++WR_FrameBatchCount < 8)
				return;
			if (WR_FrameBestScore >= WR_FrameFrontScore
				|| ++WR_FrameHeldBatches >= 4) {
				memcpy(LCDBest + WR_VIEW_HEIGHT * WR_LCD_STRIDE,
					LCD + WR_VIEW_HEIGHT * WR_LCD_STRIDE,
					(LCD_HEIGHT - WR_VIEW_HEIGHT) * WR_LCD_STRIDE);
				(void)lcd_set_framebuffer((uint32_t *)LCDBest);
				LCD = LCDBest;
				LCDBest = old_front;
				WR_FrameFrontScore = WR_FrameBestScore;
				WR_FrameHeldBatches = 0;
				WR_FrameViewH = ViewHStart;
				WR_FrameViewV = ViewVStart;
			}
			WR_FrameBatchCount = 0;
			WR_FrameBestScore = 0;
			return;
		}

		WR_FrameBatchCount = 0;
		WR_FrameBestScore = 0;
		memcpy(LCDBack + WR_VIEW_HEIGHT * WR_LCD_STRIDE,
			LCD + WR_VIEW_HEIGHT * WR_LCD_STRIDE,
			(LCD_HEIGHT - WR_VIEW_HEIGHT) * WR_LCD_STRIDE);
		(void)lcd_set_framebuffer((uint32_t *)LCDBack);
		LCD = LCDBack;
		LCDBack = old_front;
		WR_FrameFrontScore = score;
		WR_FrameHeldBatches = 0;
		WR_FrameViewH = ViewHStart;
		WR_FrameViewV = ViewVStart;
	}
}

LOCALPROC DrawControls(void)
{
	int x;
	memset(LCD + WR_VIEW_HEIGHT * WR_LCD_STRIDE, 0,
		(LCD_HEIGHT - WR_VIEW_HEIGHT) * WR_LCD_STRIDE);
	lcd_move_to(0, WR_VIEW_HEIGHT);
	lcd_line_to(LCD_WIDTH - 1, WR_VIEW_HEIGHT);
	for (x = 40; x < LCD_WIDTH; x += 40) {
		lcd_move_to(x, WR_VIEW_HEIGHT);
		lcd_line_to(x, LCD_HEIGHT - 1);
	}
	lcd_at_xy(0, 14);
	lcd_print(" CMD SHF ESC RET SPC QUIT");
}

LOCALFUNC blnr Screen_Init(void)
{
	lcd_window_disable();
	lcd_set_default_framebuffer();
	lcd_clear(LCD_WHITE);
	LCD = lcd_get_framebuffer();
	ViewHStart = 0;
	ViewVStart = 0;
	ViewHSize = LCD_WIDTH;
	ViewVSize = WR_VIEW_HEIGHT;
	SavedMouseH = 0;
	SavedMouseV = 0;
	DrawControls();
	memcpy(WR_LCDBuffers[0], LCD, LCD_BUFFER_SIZE_BYTES);
	memcpy(WR_LCDBuffers[1], LCD, LCD_BUFFER_SIZE_BYTES);
	memcpy(WR_LCDBuffers[2], LCD, LCD_BUFFER_SIZE_BYTES);
	LCD = (uint8_t *)WR_LCDBuffers[0];
	LCDBack = (uint8_t *)WR_LCDBuffers[1];
	LCDBest = (uint8_t *)WR_LCDBuffers[2];
	WR_FrameBestScore = 0;
	WR_FrameFrontScore = 0;
	WR_FrameBatchCount = 0;
	WR_FrameHeldBatches = 0;
	WR_InteractiveFrames = 0;
	WR_FrameViewH = ViewHStart;
	WR_FrameViewV = ViewVStart;
	(void)lcd_set_framebuffer((uint32_t *)LCD);
	return trueblnr;
}

GLOBALOSGLUPROC Screen_OutputFrame(ui3p screencurrentbuff)
{
	MacScreen = screencurrentbuff;
}

GLOBALOSGLUPROC DoneWithDrawingForTick(void)
{
#if MINIVMAC_PROFILE
	ui5b start = timer_get();
#endif
	DrawMacScreen();
#if MINIVMAC_PROFILE
	{
		ui5b now = timer_get();
		WR_ProfileScreenTicks += now - start;
		++WR_ProfileFrames;
		if (WR_ProfileActive
			&& now - WR_ProfileStart >= WR_PROFILE_WINDOW_CLOCKS)
			WR_ProfileReport(now, falseblnr);
	}
#endif
}

/* ------------------------------------------------------------------------- */
/* Touch, front buttons and timing                                            */

LOCALVAR blnr TouchMouse;
LOCALVAR blnr FrontMouse;
LOCALVAR ui4r TouchX;
LOCALVAR ui4r TouchY;
LOCALVAR ui3r StripKey = MKC_None;
LOCALVAR ui5b TrueEmulatedTime;
LOCALVAR ui5b TickAccumulator;
LOCALVAR ui5b DateAccumulator;
LOCALVAR ui5b LastClock;
LOCALVAR blnr CurSpeedStopped = trueblnr;

LOCALPROC SetMouseButton(void)
{
	MyMouseButtonSet(TouchMouse || FrontMouse);
}

LOCALPROC SetStripKey(ui3r key)
{
	if (StripKey != MKC_None)
		Keyboard_UpdateKeyMap2(StripKey, falseblnr);
	StripKey = key;
	if (StripKey != MKC_None)
		Keyboard_UpdateKeyMap2(StripKey, trueblnr);
}

LOCALPROC SetTouchMousePosition(void)
{
	MyMousePositionSet(ViewHStart + TouchX, ViewVStart + TouchY);
}

LOCALPROC PanViewportFromTouch(void)
{
	ui4r old_h = ViewHStart;
	ui4r old_v = ViewVStart;
	ui4r max_h = vMacScreenWidth - ViewHSize;
	ui4r max_v = vMacScreenHeight - ViewVSize;

	if (TouchX < WR_PAN_X_BORDER) {
		ViewHStart = ViewHStart > WR_PAN_X_STEP
			? ViewHStart - WR_PAN_X_STEP : 0;
	} else if (TouchX >= ViewHSize - WR_PAN_X_BORDER) {
		ViewHStart = ViewHStart + WR_PAN_X_STEP < max_h
			? ViewHStart + WR_PAN_X_STEP : max_h;
	}
	if (TouchY < WR_PAN_Y_BORDER) {
		ViewVStart = ViewVStart > WR_PAN_Y_STEP
			? ViewVStart - WR_PAN_Y_STEP : 0;
	} else if (TouchY >= ViewVSize - WR_PAN_Y_BORDER) {
		ViewVStart = ViewVStart + WR_PAN_Y_STEP < max_v
			? ViewVStart + WR_PAN_Y_STEP : max_v;
	}
	if (ViewHStart != old_h || ViewVStart != old_v) {
		WR_InteractiveFrames = WR_INTERACTIVE_FRAMES;
		ScreenChangedAll();
	}
}

LOCALPROC HandleEvent(event_t *event)
{
	switch (event->item_type) {
	case EVENT_TOUCH_DOWN:
	case EVENT_TOUCH_MOTION:
		WR_InteractiveFrames = WR_INTERACTIVE_FRAMES;
		if (event->item_type == EVENT_TOUCH_DOWN)
			++TouchDownCount;
		else
			++TouchMotionCount;
		if (event->touch.y < WR_VIEW_HEIGHT) {
			if (StripKey != MKC_None)
				SetStripKey(MKC_None);
			TouchX = event->touch.x < 0 ? 0
				: event->touch.x >= LCD_WIDTH ? LCD_WIDTH - 1
				: event->touch.x;
			TouchY = event->touch.y < 0 ? 0 : event->touch.y;
			PanViewportFromTouch();
			SetTouchMousePosition();
			TouchMouse = trueblnr;
			SetMouseButton();
		} else {
			int cell = event->touch.x / 40;
			TouchMouse = falseblnr;
			SetMouseButton();
			if (event->item_type == EVENT_TOUCH_DOWN) {
				static const ui3r keys[5] = {
					MKC_Command, MKC_Shift, MKC_Escape, MKC_Return, MKC_Space
				};
				if (cell < 5)
					SetStripKey(keys[cell]);
				else
					ForceMacOff = trueblnr;
			}
		}
		if (event->item_type == EVENT_TOUCH_DOWN) {
			debug_printf("Mini vMac touch down: %lu,%lu view=%lu,%lu queue=%lu/%lu mouse=%u\n",
				(unsigned long)TouchX, (unsigned long)TouchY,
				(unsigned long)ViewHStart, (unsigned long)ViewVStart,
				(unsigned long)MyEvtQIn, (unsigned long)MyEvtQOut,
				(unsigned)SCC_InterruptsEnabled());
		}
		break;
	case EVENT_TOUCH_UP:
		WR_InteractiveFrames = WR_INTERACTIVE_FRAMES;
		++TouchUpCount;
		TouchMouse = falseblnr;
		SetMouseButton();
		SetStripKey(MKC_None);
		debug_printf("Mini vMac touch up: queue=%lu/%lu guest=%lu,%lu view=%lu,%lu mouse=%u\n",
			(unsigned long)MyEvtQIn, (unsigned long)MyEvtQOut,
			(unsigned long)CurMouseH, (unsigned long)CurMouseV,
			(unsigned long)ViewHStart, (unsigned long)ViewVStart,
			(unsigned)SCC_InterruptsEnabled());
		break;
	case EVENT_BUTTON_DOWN:
	case EVENT_BUTTON_UP:
		if (event->button.code < 3) {
			blnr down = event->item_type == EVENT_BUTTON_DOWN;
			if (event->button.code == BUTTON_RANDOM) {
				FrontMouse = down;
				SetMouseButton();
			} else {
				Keyboard_UpdateKeyMap2(
					event->button.code == BUTTON_SEARCH ? MKC_Return : MKC_Escape,
					down);
			}
		}
		break;
	case EVENT_BATTERY_LOW:
		power_off();
		break;
	default:
		break;
	}
}

LOCALPROC PollEvents(void)
{
	event_t event;
	while (event_get(&event) != EVENT_NONE)
		HandleEvent(&event);
}

LOCALPROC UpdateClock(void)
{
	ui5b now = timer_get();
	ui5b elapsed = now - LastClock;
	LastClock = now;
	TickAccumulator += elapsed;
	DateAccumulator += elapsed;
	while (TickAccumulator >= WR_TICK_CLOCKS) {
		TickAccumulator -= WR_TICK_CLOCKS;
		++TrueEmulatedTime;
	}
	if (DateAccumulator >= 60000000UL) {
		DateAccumulator -= 60000000UL;
		++CurMacDateInSeconds;
	}
	watchdog(WATCHDOG_KEY);
}

LOCALFUNC blnr CheckDateTime(void)
{
	static ui5b previous;
	if (previous != CurMacDateInSeconds) {
		previous = CurMacDateInSeconds;
		return trueblnr;
	}
	return falseblnr;
}

LOCALFUNC blnr InitLocationDat(void)
{
	CurMacDateInSeconds = WR_MAC_DATE_1986;
	LastClock = timer_get();
	TickAccumulator = 0;
	DateAccumulator = 0;
	return trueblnr;
}

LOCALPROC LeaveSpeedStopped(void)
{
	LastClock = timer_get();
}

LOCALPROC EnterSpeedStopped(void)
{
}

LOCALPROC CheckSavedMacMsg(void)
{
	if (SavedBriefMsg != nullpr) {
		debug_print("Mini vMac: ");
		debug_print(SavedBriefMsg);
		debug_print("\n");
		SavedBriefMsg = nullpr;
		if (SavedFatalMsg)
			ForceMacOff = trueblnr;
	}
}

LOCALPROC CheckForSavedTasks(void)
{
	if (MyEvtQNeedRecover) {
		MyEvtQNeedRecover = falseblnr;
		MyEvtQTryRecoverFromFull();
	}
	if (RequestMacOff) {
		RequestMacOff = falseblnr;
		ForceMacOff = trueblnr;
	}
	if (CurSpeedStopped != SpeedStopped) {
		CurSpeedStopped = !CurSpeedStopped;
		if (CurSpeedStopped)
			EnterSpeedStopped();
		else
			LeaveSpeedStopped();
	}
	if (SavedBriefMsg != nullpr)
		CheckSavedMacMsg();
	if (NeedWholeScreenDraw) {
		NeedWholeScreenDraw = falseblnr;
		DrawMacScreen();
		DrawControls();
	}
#if NeedRequestIthDisk
	if (RequestIthDisk != 0) {
		(void)Sony_InsertIth(RequestIthDisk);
		RequestIthDisk = 0;
	}
#endif
}

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void)
{
	UpdateClock();
	PollEvents();
	return TrueEmulatedTime == OnTrueTime;
}

GLOBALOSGLUPROC WaitForNextTick(void)
{
#if MINIVMAC_PROFILE
	ui5b profile_start = timer_get();
#endif
	while (!ForceMacOff) {
		UpdateClock();
		PollEvents();
		CheckForSavedTasks();
		if (TouchMouse)
			SetTouchMousePosition();
		if (!CurSpeedStopped && TrueEmulatedTime != OnTrueTime)
			break;
	}
	if (CheckDateTime()) {
#if EnableDemoMsg
		DemoModeSecondNotify();
#endif
	}
	OnTrueTime = TrueEmulatedTime;
#if MINIVMAC_PROFILE
	WR_ProfileWaitTicks += timer_get() - profile_start;
#endif
}

/* ------------------------------------------------------------------------- */
/* Allocation and application entry                                          */

LOCALPROC ReserveAllocAll(void)
{
	ReserveAllocOneBlock(&ROM, kROM_Size, 5, falseblnr);
#if UseControlKeys
	ReserveAllocOneBlock(&CntrlDisplayBuff, vMacScreenNumBytes, 5, falseblnr);
#endif
	EmulationReserveAlloc();
}

LOCALFUNC blnr AllocMyMemory(void)
{
	uimr size;
	ReserveAllocOffset = 0;
	ReserveAllocBigBlock = nullpr;
	ReserveAllocAll();
	size = ReserveAllocOffset;
	ReserveAllocBigBlock = memory_allocate(size, "minivmac");
	if (ReserveAllocBigBlock == nullpr)
		return falseblnr;
	memset(ReserveAllocBigBlock, 0, size);
	ReserveAllocOffset = 0;
	ReserveAllocAll();
	return size == ReserveAllocOffset;
}

LOCALPROC UnallocMyMemory(void)
{
	if (ReserveAllocBigBlock != nullpr) {
		memory_free(ReserveAllocBigBlock, "minivmac");
		ReserveAllocBigBlock = nullpr;
	}
}

int grifo_main(int argc, char **argv)
{
	UnusedParam(argc);
	UnusedParam(argv);
	InitDrives();
	InitKeyCodes();

	if (!AllocMyMemory() || !Screen_Init() || !InitLocationDat() ||
		!LoadMacRom() || !LoadInitialImages()) {
		lcd_clear(LCD_WHITE);
		lcd_print("Mini vMac could not start.\n\nExpected:\n/minivmac/MacPlus.ROM\n/minivmac/disk1.dsk\n\nTouch to return.");
		for (;;) {
			event_t event;
			if (event_wait(&event, nullpr, nullpr) == EVENT_TOUCH_DOWN)
				break;
		}
	} else {
		ScreenChangedAll();
#if MINIVMAC_PROFILE
		WR_ProfileInit();
#endif
		(void)WaitForRom();
		ProgramMain();
#if MINIVMAC_PROFILE
		WR_ProfileFinish();
#endif
	}

	DisconnectKeyCodes2();
	UnInitDrives();
	UnallocMyMemory();
	lcd_set_default_framebuffer();
	chain("init.app");
}

#endif /* WantOSGLUWR */
