/*
 * romdump: copy the WikiReader's boot FLASH and the S1C33E07's boot ROM to
 * the card, for emulators that want the real contents.
 *
 * Copyright (c) 2026 Tim Douglas
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read-only by construction: the FLASH only ever sees READ DATA (0x03),
 * READ STATUS (0x05) and JEDEC ID (0x9f), so nothing here can erase or
 * program it.  Every file is created with FatFs's create-new
 * mode under the first number no earlier run has used, so a run can never
 * overwrite a dump, whichever device made it.
 *
 * The FLASH dump is written and closed before the boot ROM is touched: the
 * manual reserves that area for Epson's firmware, and if reading it faults
 * the FLASH is already safe on the card, as is the report so far, which is
 * synced at every step.
 *
 * Plain loads read 0x2c000-0x2ffff as zeros although the ROM's own code
 * calls into it, so version 2 tries that area other ways, each saved
 * before the next: halfword and byte loads, a load made from inside the
 * ROM by its own "ld.w %r1,[%r10]; ret", and last, as the one that could
 * conceivably stop the machine, the reserved 0x30000-0x3ffff above it.
 * All of them read zeros.
 *
 * Version 3 stops varying the load and goes around it:
 *
 *  - HSDMA, whose read is not the CPU's.  The chapter note says area 1
 *    cannot be a DMA source at all, but the source-address register's own
 *    note excludes area 1 only in single-address mode (II.1.9), so a
 *    dual-address transfer is worth a try.  A transfer from 0x20000, which
 *    loads can read, says whether the engine reaches area 1 at all.
 *  - How long a load from the area takes, against one from 0x20000: the
 *    same time means something answered for it and returned zeros.
 *  - pMISC_COROM (0x30001A), the COROM Switch Register: one protected
 *    "test bit" for Epson's factory tests, named after the core ROM this
 *    is.  A production test has to read the mask, so this is the likeliest
 *    switch there is.  Also tried together with the DMA.
 *  - pMISC_BOOT D1 (BOOT_ENA) set again, in case the lock follows the boot
 *    state.
 *
 * Nothing here writes anything that outlives the run: the two Misc bits are
 * put back and the DMA channel is left disabled.
 */

#include <grifo.h>
#include <regs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	FLASH_BYTES = 0x10000,          // Pm25LV512, 512 Kbit
	FLASH_PROBE_BYTES = 2 * FLASH_BYTES, // past the end: does it wrap?
	FLASH_READS = 3,
	FLASH_CHUNK = 1024,             // interrupts are off for one of these
	SERIAL_ADDRESS = 0x1fe0,        // samo-lib/drivers/include/FLASH.h
	SERIAL_SIZE = 32,

	// S1C33E07 Technical Manual I.6: the 64 KB specific ROM in area 1
	// holds the boot code (SPI-EEPROM boot starts at 0x20010), and a
	// 4-word gate ROM at 0xc00000 holds the reset vectors
	BOOT_ROM_BASE = 0x20000,
	BOOT_ROM_BYTES = 0x10000,
	GATE_ROM_BASE = 0xc00000,
	GATE_ROM_BYTES = 16,
	HIDDEN_OFFSET = 0xc000,         // 0x2c000: where plain loads read zeros
	HIDDEN_BYTES = BOOT_ROM_BYTES - HIDDEN_OFFSET,
	ROM_LOAD_GADGET = 0x20456,      // ld.w %r1,[%r10]; ret
	MIRROR_BASE = 0x30000,

	TIMED_LOADS = 4096,             // one timing sample, in words
	DMA_WAIT = 1000000,             // polls of the transfer counter

	MAX_RUNS = 999,
};

// pMISC_PROT (0x300020): this removes the write protection from the Misc
// registers 0x300010-0x30001A, and anything else restores it (III.4)
enum {
	MISC_UNLOCK = 0x96,
	MISC_BOOT_ENA = 1 << 1,         // pMISC_BOOT D1: #CE10 area boot enable
	MISC_COROM_SW = 1 << 0,         // pMISC_COROM D0: the test bit
};

// FLASH commands, all of them reads
enum {
	FLASH_READ_DATA = 0x03,
	FLASH_READ_STATUS = 0x05,
	FLASH_JEDEC_ID = 0x9f,
};

// the board's lines (samo-lib/include/boards/samo_a1.h)
enum {
	P3_VCCEN = 1 << 2,              // SD card supply, active low
	P3_BUFFER = 1 << 3,             // SD card bus buffer, active high
	P5_SDCARD_CS = 1 << 0,
	P5_FLASH_CS = 1 << 2,
};

static uint8_t flash[FLASH_READS][FLASH_PROBE_BYTES];
static uint32_t boot_rom[2][BOOT_ROM_BYTES / 4];
static uint32_t gate_rom[2][GATE_ROM_BYTES / 4];
static char report[4096];
static size_t report_length, report_written;
static int report_handle = -1;


static void say(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
static void say(const char *format, ...)
{
	char line[128];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	lcd_print(line);
	lcd_print("\n");
	debug_print(line);
	debug_print("\n");
	if (report_length + strlen(line) + 2 < sizeof(report)) {
		strcpy(report + report_length, line);
		report_length += strlen(line);
		report[report_length++] = '\n';
		report[report_length] = '\0';
	}
}


// Put what has been said since last time into the report file, durably.
static void report_flush(void)
{
	if (report_handle < 0 || report_written == report_length)
		return;
	file_write(report_handle, report + report_written, report_length - report_written);
	file_sync(report_handle);
	report_written = report_length;
}


static uint32_t crc32(const void *data, size_t length)
{
	const uint8_t *bytes = data;
	uint32_t crc = 0xffffffff;

	while (length--) {
		crc ^= *bytes++;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}


// The SPI is the SD card's too: take it from Grifo's driver for one
// transaction with interrupts off, as samo-lib/drivers/src/FLASH.c does, and
// leave every register and line as it was.
typedef struct {
	critical_t interrupts;
	uint32_t ctl1, ctl2, wait, rxmask;
	uint8_t p3, p5, p5_direction;
} spi_saved_t;

static void spi_claim(spi_saved_t *saved)
{
	saved->interrupts = critcal_enter();
	while (REG_SPI_STAT & BSYF) {
	}
	saved->ctl1 = REG_SPI_CTL1;
	saved->ctl2 = REG_SPI_CTL2;
	saved->wait = REG_SPI_WAIT;
	saved->rxmask = REG_SPI_RXMK;
	saved->p3 = REG_P3_P3D;
	saved->p5 = REG_P5_P5D;
	saved->p5_direction = REG_P5_IOC5;

	REG_P5_P5D |= P5_SDCARD_CS | P5_FLASH_CS;
	REG_P5_IOC5 |= P5_FLASH_CS;
	// a powered card's buffer would drive MISO against the FLASH
	if ((saved->p3 & (P3_VCCEN | P3_BUFFER)) == P3_BUFFER)
		REG_P3_P3D = saved->p3 & ~(P3_VCCEN | P3_BUFFER);

	// 8-bit characters at MCLK/16, no DMA requests: the boot ROM reads
	// this part at OSC3/16
	REG_SPI_CTL1 = BPT_8_BITS | MCBR_MCLK_DIV_16 | MODE_MASTER | ENA;
	REG_SPI_CTL2 = 0;
	REG_SPI_WAIT = 0;
	REG_SPI_RXMK = 0;
	(void)REG_SPI_RXD;
}

static void spi_release(const spi_saved_t *saved)
{
	while (REG_SPI_STAT & BSYF) {
	}
	REG_P5_P5D = (REG_P5_P5D & ~(P5_SDCARD_CS | P5_FLASH_CS)) | (saved->p5 & (P5_SDCARD_CS | P5_FLASH_CS));
	REG_P5_IOC5 = saved->p5_direction;
	REG_P3_P3D = saved->p3;
	(void)REG_SPI_RXD;
	REG_SPI_CTL1 = saved->ctl1;
	REG_SPI_CTL2 = saved->ctl2;
	REG_SPI_WAIT = saved->wait;
	REG_SPI_RXMK = saved->rxmask;
	critical_exit(saved->interrupts);
}

static uint8_t spi_exchange(uint8_t out)
{
	REG_SPI_TXD = out;
	while (!(REG_SPI_STAT & RDFF)) {
	}
	return REG_SPI_RXD;
}

// one command: its bytes out, then `length` bytes in
static void flash_command(const uint8_t *command, size_t command_length, uint8_t *in, size_t length)
{
	spi_saved_t saved;

	spi_claim(&saved);
	delay_us(10);
	REG_P5_P5D &= ~P5_FLASH_CS;
	for (size_t i = 0; i < command_length; ++i)
		(void)spi_exchange(command[i]);
	for (size_t i = 0; i < length; ++i)
		in[i] = spi_exchange(0x00);
	REG_P5_P5D |= P5_FLASH_CS;
	spi_release(&saved);
}

static void flash_read(uint8_t *buffer, uint32_t address, size_t length)
{
	while (length) {
		size_t chunk = length < FLASH_CHUNK ? length : FLASH_CHUNK;
		uint8_t command[4] = {
			FLASH_READ_DATA, address >> 16, address >> 8, address
		};

		flash_command(command, sizeof(command), buffer, chunk);
		buffer += chunk;
		address += chunk;
		length -= chunk;
		watchdog(WATCHDOG_KEY);
	}
}


static bool all_bytes(const uint8_t *data, size_t length, uint8_t value)
{
	for (size_t i = 0; i < length; ++i)
		if (data[i] != value)
			return false;
	return true;
}


// The first run number none of whose files exist; 0 if all are taken.
enum { INFO, FLASH, FLASH_HIGH, FLASH_REREAD, BOOT, GATE, BOOT16, BOOT8, BOOT_GADGET, BOOT_MIRROR,
       BOOT_DMA, BOOT_COROM, BOOT_COROM_DMA };
static const char *const KINDS[] = { "info", "flash", "fhigh", "fread", "boot", "gate", "bth", "btb", "btg", "btm",
				     "btdma", "btcro", "btcdm" };
static const char *const SUFFIXES[] = { "txt", "rom", "bin", "rom", "rom", "bin", "rom", "rom", "rom", "rom",
					"bin", "rom", "bin" };

static void dump_name(char *name, size_t size, unsigned kind, unsigned run)
{
	snprintf(name, size, "0:/dumps/%s%03u.%s", KINDS[kind], run, SUFFIXES[kind]);
}

static unsigned first_free_run(void)
{
	for (unsigned run = 1; run <= MAX_RUNS; ++run) {
		bool taken = false;
		for (unsigned kind = 0; kind < sizeof(KINDS) / sizeof(KINDS[0]); ++kind) {
			char name[40];
			unsigned long length;

			dump_name(name, sizeof(name), kind, run);
			if (FILE_ERROR_NO_FILE != file_size(name, &length)) {
				taken = true;
				break;
			}
		}
		if (!taken)
			return run;
	}
	return 0;
}

// A new file, never replacing one: create-new fails if it exists.
static int create_new(unsigned kind, unsigned run)
{
	char name[40];

	dump_name(name, sizeof(name), kind, run);
	int handle = file_open(name, FILE_OPEN_WRITE | FILE_OPEN_CREATE);
	if (handle < 0)
		say("%s: not created (%d)", name + 9, handle);
	return handle;
}

static bool write_new(unsigned kind, unsigned run, const void *data, size_t length)
{
	char name[40];

	dump_name(name, sizeof(name), kind, run);
	int handle = create_new(kind, run);
	if (handle < 0)
		return false;
	ssize_t written = file_write(handle, (void *)data, length);
	file_error_t synced = file_sync(handle);
	file_error_t closed = file_close(handle);
	if (written != (ssize_t)length || FILE_ERROR_OK != synced || FILE_ERROR_OK != closed) {
		say("%s: write failed (%ld of %lu)", name + 9, (long)written, (unsigned long)length);
		return false;
	}
	say("wrote %s", name + 9);
	report_flush();
	return true;
}


// a load whose PC is inside the boot ROM: its own "ld.w %r1,[%r10]; ret"
static uint32_t rom_load(uint32_t address)
{
	register uint32_t pointer asm("r10") = address;
	register uint32_t routine asm("r4") = ROM_LOAD_GADGET;
	register uint32_t value asm("r1");

	asm volatile ("call %2" : "=r" (value) : "r" (pointer), "r" (routine) : "memory");
	return value;
}


static uint16_t dma_counter, dma_flag;  // how the last transfer ended


// An HSDMA dual-address transfer of words from anywhere to RAM, by one
// software trigger (II.1.9).  Channel 1: Grifo's card driver uses 2 and 3.
// Returns false if the transfer counter never reached zero.
static bool dma_read(uint32_t source, void *destination, unsigned words)
{
	uint32_t target = (uint32_t)destination;
	uint16_t mode = REG_HS_CNTLMODE, trigger = REG_HSDMA_HTGR1;
	bool done;

	REG_HS1_EN = 0;
	REG_HS_CNTLMODE = HSDMAADV;     // 32-bit transfers and 32-bit addresses
	REG_HS1_ADVMODE = 1;            // WORDSIZE1
	REG_HS1_CNT = words;
	REG_HS1_CTRL = 0x8000;          // DUALM1, counter bits [23:16] zero
	REG_HS1_SADR_L = 0;
	REG_HS1_SADR_H = 0x3000;        // S1IN: increment, no initialisation
	REG_HS1_DADR_L = 0;
	REG_HS1_DADR_H = 0x7000;        // D1MOD successive, D1IN increment
	REG_HS1_ADV_SADR_L = source & 0xffff;
	REG_HS1_ADV_SADR_H = source >> 16;
	REG_HS1_ADV_DADR_L = target & 0xffff;
	REG_HS1_ADV_DADR_H = target >> 16;
	REG_HSDMA_HTGR1 = trigger & 0x0f;       // channel 1: software trigger
	REG_HS1_TF = 1;
	REG_HS1_EN = 1;
	REG_HSDMA_HSOFTTGR = 1 << 1;
	for (unsigned i = 0; i < DMA_WAIT && REG_HS1_CNT; ++i)
		continue;
	done = !REG_HS1_CNT;
	dma_counter = REG_HS1_CNT;
	dma_flag = REG_HS1_TF;

	REG_HS1_EN = 0;
	REG_HS1_TF = 1;
	REG_HSDMA_HTGR1 = trigger;
	REG_HS_CNTLMODE = mode;
	return done;
}


// Nanoseconds for a word load from an address, the best of three passes
// with interrupts off.  One copy of the loop serves every address: inlined
// copies would sit at different offsets in their fetch lines, which on this
// core is worth more than the memory being measured.
static unsigned __attribute__((noinline)) load_time(uint32_t address)
{
	const volatile uint32_t *from = (const volatile uint32_t *)address;
	unsigned long best = 0;

	for (unsigned pass = 0; pass < 3; ++pass) {
		critical_t interrupts = critcal_enter();
		uint32_t sink = 0;
		unsigned long start = timer_get();

		for (unsigned i = 0; i < TIMED_LOADS; ++i)
			sink += from[i & 0x3ff];
		unsigned long elapsed = timer_get() - start;
		critical_exit(interrupts);
		asm volatile ("" :: "r" (sink));
		if (!pass || elapsed < best)
			best = elapsed;
	}
	return (unsigned)(best * 1000 / (TIMER_CountsPerMicroSecond * TIMED_LOADS));
}


// how much of a read of the hidden area came back as something
static void report_probe(const char *how, const void *data, size_t length)
{
	const uint8_t *bytes = data;
	unsigned nonzero = 0;

	for (size_t i = 0; i < length; ++i)
		nonzero += bytes[i] != 0;
	say("%s: crc32 %08lx, %u nonzero", how, (unsigned long)crc32(data, length), nonzero);
}


// how much of the area plain loads read as zeros came back otherwise
static void report_hidden(const char *how, const uint8_t *rom)
{
	const uint8_t *hidden = rom + HIDDEN_OFFSET;
	unsigned nonzero = 0;

	for (unsigned i = 0; i < HIDDEN_BYTES; ++i)
		nonzero += hidden[i] != 0;
	say("%s: crc32 %08lx, 2c000+ %u nonzero", how,
	    (unsigned long)crc32(rom, BOOT_ROM_BYTES), nonzero);
}


static void wait_for_press(void)
{
	event_flush();
	for (;;) {
		event_t event;
		event_item_t item = event_wait(&event, NULL, NULL);

		if (EVENT_BUTTON_DOWN == item || EVENT_TOUCH_DOWN == item || EVENT_KEY == item)
			return;
	}
}


int grifo_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	say("romdump 3: FLASH and boot ROM");

	int revision = 0x08 ^ (REG_PA_DATA & 0x0f);
	say("board revision V%d", revision);

	if (FILE_ERROR_OK != directory_create("0:/dumps") && !directory_exists("0:/dumps")) {
		say("cannot make 0:/dumps");
		wait_for_press();
		return 1;
	}
	unsigned run = first_free_run();
	if (!run) {
		say("all %u run numbers used", MAX_RUNS);
		wait_for_press();
		return 1;
	}
	report_handle = create_new(INFO, run);
	say("run %03u", run);
	report_flush();

	// the part's identity: informational, all reads
	uint8_t status, jedec[3];
	static const uint8_t read_status[] = { FLASH_READ_STATUS };
	static const uint8_t jedec_id[] = { FLASH_JEDEC_ID };
	flash_command(read_status, sizeof(read_status), &status, 1);
	flash_command(jedec_id, sizeof(jedec_id), jedec, sizeof(jedec));
	say("flash status %02x jedec %02x%02x%02x",
	    status, jedec[0], jedec[1], jedec[2]);

	// three reads of the part and as far again, then compare
	for (unsigned i = 0; i < FLASH_READS; ++i)
		flash_read(flash[i], 0, FLASH_PROBE_BYTES);
	unsigned matching = 1;
	for (unsigned i = 1; i < FLASH_READS; ++i)
		if (!memcmp(flash[0], flash[i], FLASH_PROBE_BYTES))
			++matching;
	uint32_t flash_crc = crc32(flash[0], FLASH_BYTES);
	say("flash crc32 %08lx, %u of %u reads agree",
	    (unsigned long)flash_crc, matching, FLASH_READS);

	// the serial number, up to the first byte that is not printable
	char serial[SERIAL_SIZE + 1];
	unsigned serial_length = 0;
	while (serial_length < SERIAL_SIZE) {
		uint8_t c = flash[0][SERIAL_ADDRESS + serial_length];
		if (c < ' ' || c > '~')
			break;
		serial[serial_length++] = c;
	}
	serial[serial_length] = '\0';
	say("serial \"%s\"", serial);

	const uint8_t *high = flash[0] + FLASH_BYTES;
	const char *beyond =
		!memcmp(flash[0], high, FLASH_BYTES) ? "mirrors 0-64K" :
		all_bytes(high, FLASH_BYTES, 0xff) ? "reads ff" :
		all_bytes(high, FLASH_BYTES, 0x00) ? "reads 00" : "differs";
	say("flash 64K-128K %s", beyond);

	report_flush();
	bool ok = report_handle >= 0;
	ok = write_new(FLASH, run, flash[0], FLASH_BYTES) && ok;
	if (!strcmp(beyond, "differs"))
		ok = write_new(FLASH_HIGH, run, high, FLASH_BYTES) && ok;
	// keep the first read that disagrees, so the difference can be seen
	for (unsigned i = 1; i < FLASH_READS; ++i)
		if (memcmp(flash[0], flash[i], FLASH_PROBE_BYTES)) {
			say("read %u differs from read 1", i + 1);
			ok = write_new(FLASH_REREAD, run, flash[i], FLASH_PROBE_BYTES) && ok;
			break;
		}

	// the boot ROM, now that the FLASH is safe
	say("reading boot ROM %06x", BOOT_ROM_BASE);
	report_flush();
	for (unsigned pass = 0; pass < 2; ++pass) {
		const volatile uint32_t *rom = (const volatile uint32_t *)BOOT_ROM_BASE;
		for (unsigned i = 0; i < BOOT_ROM_BYTES / 4; ++i)
			boot_rom[pass][i] = rom[i];
		const volatile uint32_t *gate = (const volatile uint32_t *)GATE_ROM_BASE;
		for (unsigned i = 0; i < GATE_ROM_BYTES / 4; ++i)
			gate_rom[pass][i] = gate[i];
		watchdog(WATCHDOG_KEY);
	}
	bool boot_same = !memcmp(boot_rom[0], boot_rom[1], BOOT_ROM_BYTES);
	bool gate_same = !memcmp(gate_rom[0], gate_rom[1], GATE_ROM_BYTES);
	const char *boot_kind =
		all_bytes((const uint8_t *)boot_rom[0], BOOT_ROM_BYTES, 0x00) ? " (all 00)" :
		all_bytes((const uint8_t *)boot_rom[0], BOOT_ROM_BYTES, 0xff) ? " (all ff)" : "";
	say("boot rom crc32 %08lx%s, reads %s",
	    (unsigned long)crc32(boot_rom[0], BOOT_ROM_BYTES), boot_kind,
	    boot_same ? "agree" : "DIFFER");
	say("gate %08lx %08lx %08lx %08lx%s",
	    (unsigned long)gate_rom[0][0], (unsigned long)gate_rom[0][1],
	    (unsigned long)gate_rom[0][2], (unsigned long)gate_rom[0][3],
	    gate_same ? "" : " DIFFER");
	ok = write_new(BOOT, run, boot_rom[0], BOOT_ROM_BYTES) && ok;
	ok = write_new(GATE, run, gate_rom[0], GATE_ROM_BYTES) && ok;

	// the same 64K other ways, one buffer at a time, each saved before the next
	uint8_t *scratch = (uint8_t *)boot_rom[1];
	{
		const volatile uint16_t *rom = (const volatile uint16_t *)BOOT_ROM_BASE;
		for (unsigned i = 0; i < BOOT_ROM_BYTES / 2; ++i)
			((uint16_t *)scratch)[i] = rom[i];
		report_hidden("halfword loads", scratch);
		ok = write_new(BOOT16, run, scratch, BOOT_ROM_BYTES) && ok;
	}
	{
		const volatile uint8_t *rom = (const volatile uint8_t *)BOOT_ROM_BASE;
		for (unsigned i = 0; i < BOOT_ROM_BYTES; ++i)
			scratch[i] = rom[i];
		report_hidden("byte loads", scratch);
		ok = write_new(BOOT8, run, scratch, BOOT_ROM_BYTES) && ok;
	}
	say("loading from inside the ROM");
	report_flush();
	for (unsigned i = 0; i < BOOT_ROM_BYTES / 4; ++i) {
		((uint32_t *)scratch)[i] = rom_load(BOOT_ROM_BASE + 4 * i);
		if (!(i & 0x3ff))
			watchdog(WATCHDOG_KEY);
	}
	report_hidden("ROM's own load", scratch);
	ok = write_new(BOOT_GADGET, run, scratch, BOOT_ROM_BYTES) && ok;
	say("reading %05x, reserved", MIRROR_BASE);
	report_flush();
	{
		const volatile uint32_t *rom = (const volatile uint32_t *)MIRROR_BASE;
		for (unsigned i = 0; i < BOOT_ROM_BYTES / 4; ++i)
			((uint32_t *)scratch)[i] = rom[i];
		report_hidden("30000 loads", scratch);
		ok = write_new(BOOT_MIRROR, run, scratch, BOOT_ROM_BYTES) && ok;
	}

	// Around the CPU's load.  The first transfer is RAM to RAM, so that a
	// failure from area 1 is the area and not the channel; the second is
	// from the part of the ROM loads can read, and says whether HSDMA
	// reaches area 1 at all.
	say("HSDMA from area 1");
	report_flush();
	memset(scratch, 0, BOOT_ROM_BYTES);
	bool dma_ran = dma_read((uint32_t)flash[0], scratch, HIDDEN_BYTES / 4);
	say("ram to ram by HSDMA: %s, %s, counter %u flag %u",
	    dma_ran ? "transferred" : "NEVER FINISHED",
	    memcmp(scratch, flash[0], HIDDEN_BYTES) ? "WRONG DATA" : "right data",
	    dma_counter, dma_flag);
	report_flush();
	memset(scratch, 0, BOOT_ROM_BYTES);
	dma_ran = dma_read(BOOT_ROM_BASE, scratch, HIDDEN_BYTES / 4);
	bool dma_reads_rom = dma_ran && !memcmp(scratch, boot_rom[0], HIDDEN_BYTES);
	say("20000 by HSDMA: %s, %s, counter %u flag %u",
	    dma_ran ? "transferred" : "NEVER FINISHED",
	    dma_reads_rom ? "matches the loads" : "differs from the loads",
	    dma_counter, dma_flag);
	report_flush();
	memset(scratch, 0, BOOT_ROM_BYTES);
	dma_ran = dma_read(BOOT_ROM_BASE + HIDDEN_OFFSET, scratch, HIDDEN_BYTES / 4);
	report_probe(dma_ran ? "2c000 by HSDMA" : "2c000 by HSDMA (NEVER FINISHED)",
		     scratch, HIDDEN_BYTES);
	ok = write_new(BOOT_DMA, run, scratch, HIDDEN_BYTES) && ok;

	// a load that something answers takes as long as any other
	say("load ns: 20000 %u, 2c000 %u, 30000 %u, sdram %u",
	    load_time(BOOT_ROM_BASE), load_time(BOOT_ROM_BASE + HIDDEN_OFFSET),
	    load_time(MIRROR_BASE + HIDDEN_OFFSET), load_time((uint32_t)flash[0]));
	report_flush();

	// the boot register's gate bit, in case the lock follows the boot state
	{
		const volatile uint32_t *hidden =
			(const volatile uint32_t *)(BOOT_ROM_BASE + HIDDEN_OFFSET);
		uint8_t boot = REG_MISC_BOOT;
		// 0xc00000 is TTBR's reset value, and the gate ROM comes back
		// there while this bit is set.  Grifo moves TTBR into A0RAM,
		// but take no interrupt over the window in any case.
		critical_t interrupts = critcal_enter();

		REG_MISC_PROT = MISC_UNLOCK;
		REG_MISC_BOOT = boot | MISC_BOOT_ENA;
		uint32_t first = hidden[0], second = hidden[1];
		uint32_t gate = *(const volatile uint32_t *)GATE_ROM_BASE;
		REG_MISC_BOOT = boot;
		REG_MISC_PROT = 0;
		critical_exit(interrupts);
		say("BOOT_ENA set (boot %02x): 2c000 %08lx %08lx, gate %08lx",
		    boot, (unsigned long)first, (unsigned long)second, (unsigned long)gate);
	}
	report_flush();

	// pMISC_COROM, the one test bit named after this ROM
	say("COROM_SW set");
	report_flush();
	{
		uint8_t corom = REG_MISC_COROM;
		uint8_t readback;
		critical_t interrupts = critcal_enter();

		REG_MISC_PROT = MISC_UNLOCK;
		REG_MISC_COROM = corom | MISC_COROM_SW;
		readback = REG_MISC_COROM;
		const volatile uint32_t *rom = (const volatile uint32_t *)BOOT_ROM_BASE;
		for (unsigned i = 0; i < BOOT_ROM_BYTES / 4; ++i)
			((uint32_t *)scratch)[i] = rom[i];
		REG_MISC_COROM = corom;
		REG_MISC_PROT = 0;
		critical_exit(interrupts);
		watchdog(WATCHDOG_KEY);
		say("pMISC_COROM was %02x, read back %02x with the bit set", corom, readback);
		say("COROM_SW: 20000-2bfff %s the plain read",
		    memcmp(scratch, boot_rom[0], HIDDEN_OFFSET) ? "DIFFERS from" : "matches");
		report_hidden("COROM_SW loads", scratch);
		ok = write_new(BOOT_COROM, run, scratch, BOOT_ROM_BYTES) && ok;
	}
	report_flush();
	{
		uint8_t corom = REG_MISC_COROM;

		memset(scratch, 0, BOOT_ROM_BYTES);
		critical_t interrupts = critcal_enter();
		REG_MISC_PROT = MISC_UNLOCK;
		REG_MISC_COROM = corom | MISC_COROM_SW;
		dma_ran = dma_read(BOOT_ROM_BASE + HIDDEN_OFFSET, scratch, HIDDEN_BYTES / 4);
		REG_MISC_COROM = corom;
		REG_MISC_PROT = 0;
		critical_exit(interrupts);
		report_probe(dma_ran ? "COROM_SW and HSDMA" : "COROM_SW and HSDMA (NEVER FINISHED)",
			     scratch, HIDDEN_BYTES);
		ok = write_new(BOOT_COROM_DMA, run, scratch, HIDDEN_BYTES) && ok;
	}

	say("%s; press any button", ok ? "done" : "FINISHED WITH ERRORS");
	report_flush();
	if (report_handle >= 0)
		file_close(report_handle);
	wait_for_press();
	return ok ? 0 : 1;
}
