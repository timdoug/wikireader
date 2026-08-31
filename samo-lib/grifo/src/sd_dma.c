/* S1C33E07 SPI receive DMA backend for the WikiReader kernel. */

#include "standard.h"

#include <integer.h>
#include <mmc.h>
#include <regs.h>

#include "sd_dma.h"

#define SPI_IDMA_CHANNEL 0x24
#define SPI_IDMA_ENABLE  (1 << 4)
#define SPI_RXD_ADDRESS  (REG_BASE + 0x1700)
#define SPI_TXD_ADDRESS  (REG_BASE + 0x1704)

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

static void receive_dma(BYTE *buff, UINT byte_count)
{
	struct idma_descriptor *descriptor = &idma_table[SPI_IDMA_CHANNEL];
	DWORD table_address = (DWORD)idma_table;
	DWORD buffer_address = (DWORD)buff;

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
	descriptor->source = (DWORD)&dma_dummy;
	descriptor->destination = SPI_TXD_ADDRESS;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI |= SPI_IDMA_ENABLE;
	REG_IDMAEN_DELCDC_DESIF2_DESPI |= SPI_IDMA_ENABLE;
	REG_IDMA_EN = 1;

	REG_HS3_EN = DMA_ENABLED;
	REG_SPI_TXD = 0xff;
	while (REG_HS3_EN & DMA_ENABLED)
		;

	REG_IDMAEN_DELCDC_DESIF2_DESPI &= ~SPI_IDMA_ENABLE;
	REG_IDMAREQ_RLCDC_RSIF2_RSPI &= ~SPI_IDMA_ENABLE;
	REG_IDMA_EN = 0;
}

void SD_DMA_initialise(void)
{
	mmc_set_spi_receive_dma(receive_dma);
}
