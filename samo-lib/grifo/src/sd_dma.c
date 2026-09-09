/* S1C33E07 SPI receive DMA backend for the WikiReader kernel. */

#include "standard.h"

#include <integer.h>
#include <mmc.h>
#include <regs.h>
#include <stdio.h>
#include <string.h>

#include "file.h"
#include "interrupt.h"
#include "sd_dma.h"
#include "serial.h"
#include "timer.h"

#define SPI_IDMA_CHANNEL 0x24
#define SPI_IDMA_ENABLE  (1 << 4)
#define HSDMA3_INTERRUPT (1 << 3)
#define SPI_RXD_ADDRESS  (REG_BASE + 0x1700)
#define SPI_TXD_ADDRESS  (REG_BASE + 0x1704)

#ifndef SD_DMA_BITS
#define SD_DMA_BITS 32
#endif
#ifndef SD_DMA_TX_HSDMA
#define SD_DMA_TX_HSDMA 1
#endif

/* A 512-byte block at MCLK/4 takes about 0.3 ms; give the engines 20 ms. */
#define DMA_TIMEOUT_TICKS (20 * 60000UL)
#define DMA_TIMEOUT_POLLS 4000UL

/*
 * Waiting for the transfer: the first backend slept in HALT for the HSDMA
 * channel-3 terminal-count cause.  In the emulator that cause woke the
 * core; on a real WikiReader (stock 2009 flash, 2026-09-05) it never did,
 * and the kernel hung on the boot splash.  Polling the flag works on the
 * hardware, as does the register sequence of the 2009 Epson Shanghai driver
 * in samo-lib/drivers/src/sd_spi.c, which differs only in never writing the
 * IDMA enable register.  The wait is bounded; on a timeout the engines are
 * stopped and the caller is told how many bytes of the block are complete
 * so the byte-at-a-time SPI loop can finish it, and DMA stays off for the
 * rest of the session. Without the diagnostic marker a healthy boot leaves
 * no trace; a fallback or failure is printed and written to dma.txt on the
 * boot volume for a device without a serial cable.
 */
struct idma_descriptor {
	DWORD control;
	DWORD count;
	DWORD source;
	DWORD destination;
};

/*
 * IDMA control information must be 16-byte aligned in DSTRAM or SDRAM.  It
 * lives in DSTRAM, the descriptor RAM the linker script reserves (input
 * section .dstram), together with the dummy transmit word. The IDMA path
 * reads its descriptor and dummy data and writes the count back per unit,
 * and HSDMA writes the received data. In SDRAM those hit a different row
 * from the receive buffer, so each character cost two row changes on top of
 * the SPI shift time; internal RAM has no rows. The section is NOLOAD, so the
 * dummy word is set at initialisation. HSDMA2 uses the same dummy word;
 * retain the table for the byte path and the IDMA comparison build.
 */
struct sd_dma_ram {
	struct idma_descriptor table[SPI_IDMA_CHANNEL + 1];
	DWORD dummy;
};
static struct sd_dma_ram dma_ram __attribute__((section(".dstram"), aligned(16)));

static unsigned long dma_blocks;
static int dma_given_up;
static bool dma_word_enabled;
static char dma_status[120] = "dma: not used";
static bool profile_enabled;
static File_IOStats profile;

void SD_DMA_profile(File_IOStats *out, bool enabled)
{
	if (enabled && !profile_enabled)
		memset(&profile, 0, sizeof(profile));
	profile_enabled = enabled;
	out->dma_bits = dma_word_enabled ? 32 : 8;
	out->dma32_bytes = profile.dma32_bytes;
	out->dma8_bytes = profile.dma8_bytes;
	out->dma_wait_ticks = profile.dma_wait_ticks;
	out->dma_bypass_bytes = profile.dma_bypass_bytes;
	out->dma_timeouts = profile.dma_timeouts;
	out->dma_errors = profile.dma_errors;
	out->dma_disabled = dma_given_up;
}

static void stop_transmit(void)
{
	REG_HS2_EN = DMA_DISABLED;
	REG_IDMAEN_DELCDC_DESIF2_DESPI &= ~SPI_IDMA_ENABLE;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI &= ~SPI_IDMA_ENABLE;
	REG_IDMA_EN = 0;
}

static void stop_engines(void)
{
	stop_transmit();
	REG_HS3_EN = DMA_DISABLED;
}

/* V.2.8 forbids even reading CTL1 while BSYF is set. Completion of the
 * receive DMA is not a substitute for checking the SPI's own busy flag. */
static int wait_spi_idle(void)
{
	unsigned long polls;
	for (polls = 0; polls < DMA_TIMEOUT_POLLS; polls++)
		if (!(REG_SPI_STAT & BSYF))
			return 1;
	return 0;
}

/* Caller has established idle. V.2.8 also requires SPI_INT to be zero
 * before clearing ENA, even if its master IRQ enable bit is already zero.
 * Stock hardware leaves SPI_INT=0x14; a reset-state emulator hid this. */
static void set_spi_control(DWORD control)
{
	Interrupt_type state = Interrupt_disable();
	BYTE mux = REG_P6_47_CFP;
	BYTE direction = REG_P6_IOC6;
	BYTE data = REG_P6_P6D;
	bool hold_clock = (mux & 0xc0) == 0x40;
	unsigned settle;
	DWORD interrupts = REG_SPI_INT;
	/* The on-device sector probe found one lost bit per ENA cycle, even
	 * with unchanged BPT and CPU reads. Keep P67 at CPOL while SPI is
	 * disabled/re-enabled; reconnect only after its clock divider settles.
	 * Width/rate changes here preserve CPOL. The GPIO latch and direction
	 * are prepared before changing the mux, and restored behind SPI. */
	if (hold_clock) {
		REG_P6_P6D = (data & ~0x80) | ((control & CPOL) ? 0x80 : 0);
		REG_P6_IOC6 = direction | 0x80;
		REG_P6_47_CFP = mux & ~0xc0;
	}
	REG_SPI_INT = 0;
	REG_SPI_CTL1 &= ~ENA;
	REG_SPI_CTL1 = control & ~ENA;
	REG_SPI_CTL1 = control;
	if (hold_clock) {
		settle = 4u << ((control >> 4) & 7);
		while (settle--) asm volatile ("nop");
		REG_P6_47_CFP = mux;
		REG_P6_IOC6 = direction;
		REG_P6_P6D = data;
	}
	REG_SPI_INT = interrupts;
	Interrupt_enable(state);
}

static void unswap_words(BYTE *buff, UINT bytes)
{
	DWORD *words = (DWORD *)buff;
	/* SPI receives the MSB first; SDRAM stores the low byte first. */
	asm volatile ("" : : : "memory");
	while (bytes) {
		DWORD value = *words;
		asm ("swap\t%0,%1" : "=r" (value) : "r" (value));
		*words++ = value;
		bytes -= 4;
	}
}

static int receive_dma(BYTE *buff, UINT byte_count)
{
	volatile struct idma_descriptor *descriptor = &dma_ram.table[SPI_IDMA_CHANNEL];
	DWORD table_address = (DWORD)dma_ram.table;
	DWORD buffer_address = (DWORD)buff;
	unsigned long start;
	unsigned long polls = 0;
	int complete = 0;
	UINT drained;
	UINT transmitted;
	UINT unit = 1;
	UINT transfers;
	DWORD spi_control;
	DWORD status;
	bool hs_transmit;

	/* Only SDRAM is a known-good HSDMA destination.  The ELF loader reads
	 * an application's .fastcode section straight into A0 RAM; that and
	 * any other internal-RAM buffer take the byte-at-a-time path. */
	if (dma_given_up || byte_count < 2 || (uintptr_t)buff < 0x10000000u) {
		if (profile_enabled)
			profile.dma_bypass_bytes += byte_count;
		return 0;
	}

#if SD_DMA_BITS == 32
	if (dma_word_enabled && ((buffer_address | byte_count) & 3) == 0)
		unit = 4;
#endif
	transfers = byte_count / unit;
	/* Keep the proven receive-paced IDMA path for byte transfers.
	 * HSDMA2 pipelines only aligned 32-bit payloads. */
	hs_transmit = SD_DMA_TX_HSDMA && unit == 4;
	spi_control = 0;
	if (unit == 4) {
		if (!wait_spi_idle())
			goto spi_stuck;
		spi_control = REG_SPI_CTL1;
		set_spi_control((spi_control & ~BPT_32_BITS) | BPT_32_BITS);
	}

	/* HSDMA3 drains one SPI character into incrementing memory. A word
	 * payload needs 128 receive requests per sector instead of 512. */
	REG_HS_CNTLMODE = HSDMAADV;
	REG_HS3_EN = DMA_DISABLED;
	REG_HS3_ADVMODE = unit == 4 ? 1 : 0; /* WORDSIZE3, II.1.8 */
	REG_HS3_CNT = transfers;
	REG_HS3_CTRL = 0x8000;
	REG_HS3_SADR_L = 0;
	REG_HS3_SADR_H = 0;
	REG_HS3_DADR_L = 0;
	REG_HS3_DADR_H = 0x2000;
	REG_HS3_ADV_SADR_L = SPI_RXD_ADDRESS & 0xffff;
	REG_HS3_ADV_SADR_H = SPI_RXD_ADDRESS >> 16;
	REG_HS3_ADV_DADR_L = buffer_address & 0xffff;
	REG_HS3_ADV_DADR_H = buffer_address >> 16;
	stop_transmit();
	REG_HSDMA_HTGR2 = hs_transmit ? 0x99 : 0x90;
	REG_INT_FSIF2_FSPI = 0x30;
	REG_HS2_TF = 1;
	REG_HS3_TF = 1;

	if (hs_transmit) {
		/* II.1.5: HSDMA2 selector 9 is SPI TX-empty. V.2.5: TXD
		 * becomes empty at shift START, so this queues the next word
		 * while the current one is on the wire. Count includes queued
		 * words; only HSDMA3 completion means the payload has arrived. */
		DWORD dummy_address = (DWORD)&dma_ram.dummy;
		REG_HS2_ADVMODE = 1;
		REG_HS2_CNT = transfers - 1;
		REG_HS2_CTRL = 0x8000;
		REG_HS2_SADR_L = 0;
		REG_HS2_SADR_H = 0;
		REG_HS2_DADR_L = 0;
		REG_HS2_DADR_H = 0;
		REG_HS2_ADV_SADR_L = dummy_address & 0xffff;
		REG_HS2_ADV_SADR_H = dummy_address >> 16;
		REG_HS2_ADV_DADR_L = SPI_TXD_ADDRESS & 0xffff;
		REG_HS2_ADV_DADR_H = SPI_TXD_ADDRESS >> 16;
	} else {
		/* IDMA channel 0x24 is SPI RX-full: send after each received unit. */
		REG_IDMA_EN = 0;
		REG_IDMABASE0 = table_address & 0xffff;
		REG_IDMABASE1 = table_address >> 16;
		descriptor->control = unit == 4 ? 2UL << 16 : 0; /* DATSIZ, II.2.2 */
		descriptor->count = transfers - 1;
		descriptor->source = (DWORD)&dma_ram.dummy;
		descriptor->destination = SPI_TXD_ADDRESS;
		REG_IDMAREQ_RLCDC_RSIF2_RSPI |= SPI_IDMA_ENABLE;
		REG_IDMAEN_DELCDC_DESIF2_DESPI |= SPI_IDMA_ENABLE;
		REG_IDMA_EN = transfers > 1;
	}
	REG_INT_FDMA = HSDMA3_INTERRUPT | (1 << 2);
	REG_HS3_EN = DMA_ENABLED;
	if (hs_transmit && transfers > 1)
		REG_HS2_EN = DMA_ENABLED;

	start = Timer_get();
	REG_SPI_TXD = 0xffffffffUL;
	for (;;) {
		/* The inner poll is a few instructions, so it runs out of the
		 * instruction queue and the SDRAM sees nothing but the engines'
		 * sequential writes while the block arrives. */
		unsigned spins = 256;
		while (--spins && !(REG_INT_FDMA & HSDMA3_INTERRUPT))
			;
		if (REG_INT_FDMA & HSDMA3_INTERRUPT) {
			REG_INT_FDMA = HSDMA3_INTERRUPT;
			complete = 1;
			break;
		}
		if (Timer_get() - start > DMA_TIMEOUT_TICKS ||
		    ++polls > DMA_TIMEOUT_POLLS)
			break;
	}

	if (profile_enabled)
		profile.dma_wait_ticks += Timer_get() - start;
	if (complete) {
		stop_engines();
		if (unit == 4) {
			if (!wait_spi_idle())
				goto spi_stuck;
			set_spi_control(spi_control);
			unswap_words(buff, byte_count);
		}
		dma_blocks++;
		if (profile_enabled) {
			if (unit == 4)
				profile.dma32_bytes += byte_count;
			else
				profile.dma8_bytes += byte_count;
		}
		return (int)byte_count;
	}

	/*
	 * Timed out. Stop transmit requests, but keep RX DMA running until
	 * both the shifting word and any queued TXD word finish. Every unit SPI
	 * clocked out produced one received character: either HSDMA moved it into
	 * the buffer, or it still sits in RXD. The transmit engine's remaining
	 * count includes writes to TXD, including a word that was still queued.
	 */
	stop_transmit();
	polls = 0;
	while ((REG_SPI_STAT & BSYF) && ++polls < DMA_TIMEOUT_POLLS)
		;
	stop_engines();
	drained = (transfers - (REG_HS3_CNT & 0xffff)) * unit;
	transmitted = (transfers - (hs_transmit ? REG_HS2_CNT :
				   (UINT)descriptor->count)) * unit;
	status = REG_SPI_STAT;
	if (!(status & BSYF) && (status & RDFF) && drained < byte_count) {
		if (unit == 4)
			*(DWORD *)(buff + drained) = REG_SPI_RXD;
		else
			buff[drained] = (BYTE)REG_SPI_RXD;
		drained += unit;
	}
	if (unit == 4 && !(status & BSYF)) {
		set_spi_control(spi_control);
		if (drained <= byte_count)
			unswap_words(buff, drained);
	}
	dma_given_up = 1;
	if (profile_enabled)
		profile.dma_timeouts++;
	if ((status & (BSYF | RDOF)) || drained != transmitted ||
	    transmitted > byte_count) {
		if (profile_enabled)
			profile.dma_errors++;
		Serial_printf("SD DMA: unrecoverable after %lu blocks: drained %u transmitted %u stat %08lx\n",
			      dma_blocks, drained, transmitted,
			      (unsigned long)status);
		snprintf(dma_status, sizeof(dma_status),
			 "dma: FAILED after %lu blocks, drained %u transmitted %u, block lost",
			 dma_blocks, drained, transmitted);
		return -1;
	}
	Serial_printf("SD DMA: timeout after %lu blocks with %u of %u bytes; PIO from now on\n",
		      dma_blocks, transmitted, byte_count);
	snprintf(dma_status, sizeof(dma_status),
		 "dma: fallback after %lu blocks, %u of %u bytes arrived",
		 dma_blocks, transmitted, byte_count);
	return (int)transmitted;

spi_stuck:
	stop_engines();
	dma_given_up = 1;
	if (profile_enabled) {
		profile.dma_timeouts++;
		profile.dma_errors++;
	}
	snprintf(dma_status, sizeof(dma_status),
		 "dma: FAILED, SPI stayed busy; control registers left untouched");
	Serial_printf("%s\n", dma_status);
	return -1;
}

const char *SD_DMA_status(void)
{
	if (!dma_given_up)
		snprintf(dma_status, sizeof(dma_status), "dma: ok, %lu blocks",
			 dma_blocks);
	return dma_status;
}

/* With zimlog.on, save a checkpoint before loading applications: an app-load
 * failure can power off before the application's startup log exists. Record
 * the inherited SPI configuration to diagnose hardware handoff issues.
 * Without the marker retain the existing failure-only behavior. */
void SD_DMA_report(void)
{
	const char *status = SD_DMA_status();
	static char detail[192];
	unsigned long marker_size;
	int size;
	int handle;

	Serial_printf("%s\n", status);
	if (!dma_given_up && File_size("0:/zimlog.on", &marker_size) != FILE_ERROR_OK) {
		File_delete("dma.txt");
		return;
	}
	size = snprintf(detail, sizeof(detail),
		"kernel filesystem init complete; payload_bits=%u configured_bits=%u word_tx=%s\n"
		"spi_ctl1=%08lx spi_rxmask=%08lx spi_interrupt=%08lx\n",
		dma_word_enabled ? 32 : 8, SD_DMA_BITS,
		SD_DMA_TX_HSDMA ? "HSDMA2" : "IDMA24", (unsigned long)REG_SPI_CTL1,
		(unsigned long)REG_SPI_RXMK, (unsigned long)REG_SPI_INT);
	handle = File_create("dma.txt", FILE_OPEN_WRITE);
	if (handle < 0)
		return;
	File_write(handle, (void *)status, strlen(status));
	File_write(handle, "\n", 1);
	if (size > 0 && size < (int)sizeof(detail))
		File_write(handle, detail, size);
	File_close(handle);
}

void SD_DMA_initialise(void)
{
	dma_word_enabled = false;
	dma_ram.dummy = 0xffffffffUL;
	mmc_set_spi_receive_dma(receive_dma);
}

/* Mount and save the early diagnostic using the proven byte path first. */
void SD_DMA_enable_wide(void)
{
#if SD_DMA_BITS == 32
	dma_word_enabled = true;
#endif
}
