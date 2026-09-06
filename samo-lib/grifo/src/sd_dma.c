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
 * rest of the session.  A healthy boot leaves no trace; a fallback or a
 * failure is printed and written to dma.txt on the boot volume for a
 * device without a serial cable.
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
 * section .dstram), together
 * with the dummy transmit byte: every byte of a block makes the IDMA read
 * its descriptor and the dummy byte and write the count back, and the HSDMA
 * write the received byte.  In SDRAM those hit a different row from the
 * receive buffer, so each byte cost two row changes on top of the SPI shift
 * time; internal RAM has no rows.  The section is NOLOAD, so the dummy byte
 * is set at initialisation.
 */
struct sd_dma_ram {
	struct idma_descriptor table[SPI_IDMA_CHANNEL + 1];
	BYTE dummy;
};
static struct sd_dma_ram dma_ram __attribute__((section(".dstram"), aligned(16)));

static unsigned long dma_blocks;
static int dma_given_up;
static char dma_status[120] = "dma: not used";

static void stop_engines(void)
{
	REG_HS3_EN = DMA_DISABLED;
	REG_IDMAEN_DELCDC_DESIF2_DESPI &= ~SPI_IDMA_ENABLE;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI &= ~SPI_IDMA_ENABLE;
	REG_IDMA_EN = 0;
}

static int receive_dma(BYTE *buff, UINT byte_count)
{
	struct idma_descriptor *descriptor = &dma_ram.table[SPI_IDMA_CHANNEL];
	DWORD table_address = (DWORD)dma_ram.table;
	DWORD buffer_address = (DWORD)buff;
	unsigned long start;
	unsigned long polls = 0;
	int complete = 0;
	UINT drained;
	UINT transmitted;

	if (dma_given_up || byte_count < 2)
		return 0;
	/* Only SDRAM is a known-good HSDMA destination.  The ELF loader reads
	 * an application's .fastcode section straight into A0 RAM; that and
	 * any other internal-RAM buffer take the byte-at-a-time path. */
	if ((uintptr_t)buff < 0x10000000u)
		return 0;

	/* HSDMA3 drains byte-wide SPI RXD into incrementing memory. */
	REG_HS_CNTLMODE = HSDMAADV;
	REG_HS3_EN = DMA_DISABLED;
	REG_HS3_ADVMODE = 0;
	REG_HS3_CNT = byte_count;
	REG_HS3_CTRL = 0x8000;
	REG_HS3_SADR_L = 0;
	REG_HS3_SADR_H = 0;
	REG_HS3_DADR_L = 0;
	REG_HS3_DADR_H = 0x2000;
	REG_HS3_ADV_SADR_L = SPI_RXD_ADDRESS & 0xffff;
	REG_HS3_ADV_SADR_H = SPI_RXD_ADDRESS >> 16;
	REG_HS3_ADV_DADR_L = buffer_address & 0xffff;
	REG_HS3_ADV_DADR_H = buffer_address >> 16;
	REG_HSDMA_HTGR2 = (REG_HSDMA_HTGR2 & 0x0f) | 0x90;
	REG_INT_FSIF2_FSPI = 0x30;
	REG_HS3_TF = 1;

	/* IDMA supplies every dummy transmit byte after the CPU's first one. */
	REG_IDMA_EN = 0;
	REG_IDMABASE0 = table_address & 0xffff;
	REG_IDMABASE1 = table_address >> 16;
	descriptor->control = 0;
	descriptor->count = byte_count - 1;
	descriptor->source = (DWORD)&dma_ram.dummy;
	descriptor->destination = SPI_TXD_ADDRESS;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI |= SPI_IDMA_ENABLE;
	REG_IDMAEN_DELCDC_DESIF2_DESPI |= SPI_IDMA_ENABLE;
	REG_IDMA_EN = 1;
	REG_INT_FDMA = HSDMA3_INTERRUPT;
	REG_HS3_EN = DMA_ENABLED;

	start = Timer_get();
	REG_SPI_TXD = 0xff;
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

	if (complete) {
		stop_engines();
		dma_blocks++;
		return (int)byte_count;
	}

	/*
	 * Timed out.  Stop the engines, let a byte in flight finish, then work
	 * out how much of the block is in the buffer.  Every byte the SPI
	 * clocked out produced one received byte: either HSDMA moved it into
	 * the buffer, or it still sits in RXD.  IDMA writes its remaining count
	 * back to the descriptor, so the transmitted total is known exactly.
	 */
	drained = byte_count - (REG_HS3_CNT & 0xffff);
	stop_engines();
	while (REG_SPI_STAT & BSYF)
		;
	transmitted = 1 + (byte_count - 1 - (UINT)descriptor->count);
	if ((REG_SPI_STAT & RDFF) && drained < byte_count) {
		buff[drained++] = (BYTE)REG_SPI_RXD;
	}
	dma_given_up = 1;
	if ((REG_SPI_STAT & RDOF) || drained != transmitted ||
	    transmitted > byte_count) {
		Serial_printf("SD DMA: unrecoverable after %lu blocks: drained %u transmitted %u stat %08lx\n",
			      dma_blocks, drained, transmitted,
			      (unsigned long)REG_SPI_STAT);
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
}

const char *SD_DMA_status(void)
{
	if (!dma_given_up)
		snprintf(dma_status, sizeof(dma_status), "dma: ok, %lu blocks",
			 dma_blocks);
	return dma_status;
}

/* A healthy boot removes any old note; a fallback or failure leaves one. */
void SD_DMA_report(void)
{
	const char *status = SD_DMA_status();
	int handle;

	Serial_printf("%s\n", status);
	if (!dma_given_up) {
		File_delete("dma.txt");
		return;
	}
	handle = File_create("dma.txt", FILE_OPEN_WRITE);
	if (handle < 0)
		return;
	File_write(handle, (void *)status, strlen(status));
	File_write(handle, "\n", 1);
	File_close(handle);
}

void SD_DMA_initialise(void)
{
	dma_ram.dummy = 0xff;
	mmc_set_spi_receive_dma(receive_dma);
}
