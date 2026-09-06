#ifndef MMC_TEST_REGS_H
#define MMC_TEST_REGS_H
#include <stdint.h>

enum { MOCK_TXD, MOCK_RXD, MOCK_STAT, MOCK_CTL1, MOCK_CTL2, MOCK_WAIT, MOCK_GATE };
uint32_t *mmc_test_register(unsigned reg);
#define REG_SPI_TXD (*mmc_test_register(MOCK_TXD))
#define REG_SPI_RXD (*mmc_test_register(MOCK_RXD))
#define REG_SPI_STAT (*mmc_test_register(MOCK_STAT))
#define REG_SPI_CTL1 (*mmc_test_register(MOCK_CTL1))
#define REG_SPI_CTL2 (*mmc_test_register(MOCK_CTL2))
#define REG_SPI_WAIT (*mmc_test_register(MOCK_WAIT))
#define REG_CMU_GATEDCLK1 (*mmc_test_register(MOCK_GATE))
#define RDFF (1u << 2)
#define DMA_CKE (1u << 1)
#define BPT_8_BITS (7u << 10)
#define MCBR_MCLK_DIV_4 0
#define TXDE (1u << 3)
#define RXDE (1u << 2)
#define MODE_MASTER (1u << 1)
#define ENA 1u
#endif
