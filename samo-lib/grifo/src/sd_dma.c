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
#define DMA_TIMEOUT_POLLS 400000UL

/*
 * The register sequence comes from the 2009 driver samo-lib/drivers/src/
 * sd_spi.c, written by Epson Shanghai for this board.  It differs from the
 * first Grifo backend in two places, and a kernel with that backend never
 * reached init.app on a real WikiReader while a PIO kernel did:
 *
 *   - it never writes the IDMA enable register (0x301105); the request and
 *     enable bits in the interrupt controller alone let hardware IDMA feed
 *     the dummy transmit bytes
 *   - it waits by polling the HSDMA channel's enable bit, which the
 *     controller clears at terminal count, not by sleeping on the ITC flag
 *
 * SD_DMA_LEGACY_SEQUENCE=1 follows that driver exactly; 0 keeps the first
 * backend's sequence.  Either way the wait is bounded, and on a timeout the
 * engines are stopped and the caller is told how many bytes of the block
 * are complete so the byte-at-a-time SPI loop can finish it; DMA is then
 * left off for the rest of the session.  SD_DMA_status() describes what
 * happened, for the serial console and the dma.txt file on the boot volume.
 */
#ifndef SD_DMA_LEGACY_SEQUENCE
#define SD_DMA_LEGACY_SEQUENCE 0
#endif

struct idma_descriptor {
	DWORD control;
	DWORD count;
	DWORD source;
	DWORD destination;
};

/* IDMA control information must be 16-byte aligned in DSTRAM or SDRAM. */
static struct idma_descriptor idma_table[SPI_IDMA_CHANNEL + 1]
	__attribute__((aligned(16)));
static BYTE dma_dummy = 0xff;

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
	struct idma_descriptor *descriptor = &idma_table[SPI_IDMA_CHANNEL];
	DWORD table_address = (DWORD)idma_table;
	DWORD buffer_address = (DWORD)buff;
	unsigned long start;
	unsigned long polls = 0;
	int complete = 0;
	UINT drained;
	UINT transmitted;

	if (dma_given_up || byte_count < 2)
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
#if SD_DMA_LEGACY_SEQUENCE
	REG_HS3_EN = DMA_ENABLED;
#endif

	/* IDMA supplies every dummy transmit byte after the CPU's first one. */
	REG_IDMA_EN = 0;
	REG_IDMABASE0 = table_address & 0xffff;
	REG_IDMABASE1 = table_address >> 16;
	descriptor->control = 0;
	descriptor->count = byte_count - 1;
	descriptor->source = (DWORD)&dma_dummy;
	descriptor->destination = SPI_TXD_ADDRESS;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI |= SPI_IDMA_ENABLE;
	REG_IDMAEN_DELCDC_DESIF2_DESPI |= SPI_IDMA_ENABLE;
#if !SD_DMA_LEGACY_SEQUENCE
	REG_IDMA_EN = 1;
	REG_INT_FDMA = HSDMA3_INTERRUPT;
	REG_HS3_EN = DMA_ENABLED;
#endif

	start = Timer_get();
	REG_SPI_TXD = 0xff;
	for (;;) {
#if SD_DMA_LEGACY_SEQUENCE
		if (!(REG_HS3_EN & DMA_ENABLED)) {
			complete = 1;
			break;
		}
#else
		if (REG_INT_FDMA & HSDMA3_INTERRUPT) {
			REG_INT_FDMA = HSDMA3_INTERRUPT;
			complete = 1;
			break;
		}
#endif
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
		 "dma: fallback after %lu blocks, %u of %u bytes arrived (sequence %s)",
		 dma_blocks, transmitted, byte_count,
		 SD_DMA_LEGACY_SEQUENCE ? "2009" : "grifo");
	return (int)transmitted;
}

const char *SD_DMA_status(void)
{
	if (!dma_given_up)
		snprintf(dma_status, sizeof(dma_status),
			 "dma: ok, %lu blocks (sequence %s)", dma_blocks,
			 SD_DMA_LEGACY_SEQUENCE ? "2009" : "grifo");
	return dma_status;
}

/* Leave a note on the boot volume for a device without a serial cable. */
void SD_DMA_report(void)
{
	const char *status = SD_DMA_status();
	int handle = File_create("dma.txt", FILE_OPEN_WRITE);

	Serial_printf("%s\n", status);
	if (handle < 0)
		return;
	File_write(handle, (void *)status, strlen(status));
	File_write(handle, "\n", 1);
	File_close(handle);
}

void SD_DMA_initialise(void)
{
	mmc_set_spi_receive_dma(receive_dma);
}
