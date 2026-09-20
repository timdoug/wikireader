/*
 * WikiReader operating-system glue for Mini vMac.
 *
 * Mini vMac is GPL-2.0; this port is GPL-3.0-or-later.  The combined
 * application is distributed under GPL-3.0-or-later.
 */
#include "OSGCOMUI.h"
#include "OSGCOMUD.h"

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

enum {
	WR_VIEW_HEIGHT = 160,
	WR_LCD_STRIDE = LCD_BUFFER_WIDTH_BYTES,
	WR_TICK_CLOCKS = 997549, /* 60 MHz / 60.14742 Hz */
	WR_MAC_DATE_1986 = 2587766400UL,
};

#define WR_FAST_CODE __attribute__((section(".fastcode"), noinline))
#define WR_FAST_DATA __attribute__((section(".fastbss"), aligned(4)))

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

	handle = file_open("/minivmac/Mac128K.ROM", FILE_OPEN_READ);
	if (handle < 0) {
		debug_print("Mini vMac: /minivmac/Mac128K.ROM not found\n");
		return falseblnr;
	}
	n = file_read(handle, ROM, kROM_Size);
	(void)file_close(handle);
	if (n != kROM_Size) {
		debug_print("Mini vMac: ROM must be exactly 65536 bytes\n");
		return falseblnr;
	}
	if (ROM_IsValid() != mnvm_noErr) {
		debug_print("Mini vMac: unsupported or corrupt ROM\n");
		return falseblnr;
	}
	return trueblnr;
}

/* ------------------------------------------------------------------------- */
/* 512x342 Macintosh display to the 240x160 upper panel                       */

LOCALVAR uint8_t *LCD;
LOCALVAR ui3p MacScreen;
LOCALVAR ui4b SourceY[WR_VIEW_HEIGHT] WR_FAST_DATA;
LOCALVAR ui3b SampleEven[256] WR_FAST_DATA;
LOCALVAR ui3b SampleOdd[256] WR_FAST_DATA;

/* 512 input pixels become 240 output pixels exactly as sixteen groups of
 * 32 -> 15.  The lookup tables gather alternating source bits four at a
 * time, avoiding a coordinate lookup and branch for every output pixel. */
static void WR_FAST_CODE ConvertMacScreen(ui3p src, uint8_t *lcd)
{
	int y;

	for (y = 0; y < WR_VIEW_HEIGHT; ++y) {
		ui3p row = src + SourceY[y] * vMacScreenMonoByteWidth;
		uint8_t *line = lcd + y * WR_LCD_STRIDE;
		uint8_t *out = line;
		ui5b pending = 0;
		unsigned have = 0;
		int group;

		for (group = 0; group < 16; ++group) {
			ui5b bits = ((ui5b)SampleEven[row[0]] << 11)
				| ((ui5b)SampleEven[row[1]] << 7)
				| ((ui5b)SampleOdd[row[2]] << 3)
				| ((ui5b)SampleOdd[row[3]] >> 1);

			pending = (pending << 15) | bits;
			have += 15;
			while (have >= 8) {
				have -= 8;
				*out++ = (ui3b)(pending >> have);
			}
			row += 4;
		}
		line[30] = 0;
		line[31] = 0;
	}
}

LOCALPROC DrawMacScreen(void)
{
	void (*volatile convert)(ui3p, uint8_t *) = ConvertMacScreen;

	if (MacScreen != nullpr)
		convert(MacScreen, LCD);
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
	int x, bit;
	lcd_window_disable();
	lcd_set_default_framebuffer();
	lcd_clear(LCD_WHITE);
	LCD = lcd_get_framebuffer();
	for (x = 0; x < WR_VIEW_HEIGHT; ++x)
		SourceY[x] = (ui4b)((long)x * vMacScreenHeight / WR_VIEW_HEIGHT);
	for (x = 0; x < 256; ++x) {
		unsigned even = 0;
		unsigned odd = 0;
		for (bit = 0; bit < 4; ++bit) {
			even = (even << 1) | ((x >> (7 - bit * 2)) & 1);
			odd = (odd << 1) | ((x >> (6 - bit * 2)) & 1);
		}
		SampleEven[x] = (ui3b)even;
		SampleOdd[x] = (ui3b)odd;
	}
	DrawControls();
	return trueblnr;
}

GLOBALOSGLUPROC Screen_OutputFrame(ui3p screencurrentbuff)
{
	MacScreen = screencurrentbuff;
}

GLOBALOSGLUPROC DoneWithDrawingForTick(void)
{
	DrawMacScreen();
}

/* ------------------------------------------------------------------------- */
/* Touch, front buttons and timing                                            */

LOCALVAR blnr TouchMouse;
LOCALVAR blnr FrontMouse;
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

LOCALPROC HandleEvent(event_t *event)
{
	switch (event->item_type) {
	case EVENT_TOUCH_DOWN:
	case EVENT_TOUCH_MOTION:
		if (event->touch.y < WR_VIEW_HEIGHT) {
			if (StripKey != MKC_None)
				SetStripKey(MKC_None);
			MyMousePositionSet(
				(ui4r)((long)event->touch.x * vMacScreenWidth / LCD_WIDTH),
				(ui4r)((long)event->touch.y * vMacScreenHeight / WR_VIEW_HEIGHT));
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
		break;
	case EVENT_TOUCH_UP:
		TouchMouse = falseblnr;
		SetMouseButton();
		SetStripKey(MKC_None);
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
	while (!ForceMacOff) {
		UpdateClock();
		PollEvents();
		CheckForSavedTasks();
		if (!CurSpeedStopped && TrueEmulatedTime != OnTrueTime)
			break;
	}
	if (CheckDateTime()) {
#if EnableDemoMsg
		DemoModeSecondNotify();
#endif
	}
	OnTrueTime = TrueEmulatedTime;
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
		lcd_print("Mini vMac could not start.\n\nExpected:\n/minivmac/Mac128K.ROM\n/minivmac/disk1.dsk\n\nTouch to return.");
		for (;;) {
			event_t event;
			if (event_wait(&event, nullpr, nullpr) == EVENT_TOUCH_DOWN)
				break;
		}
	} else {
		ScreenChangedAll();
		(void)WaitForRom();
		ProgramMain();
	}

	DisconnectKeyCodes2();
	UnInitDrives();
	UnallocMyMemory();
	chain("init.app");
}

#endif /* WantOSGLUWR */
