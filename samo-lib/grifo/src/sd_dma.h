#ifndef SD_DMA_H
#define SD_DMA_H

#include "file.h"

void SD_DMA_initialise(void);
/* Enable the configured payload width after the early boot report. */
void SD_DMA_enable_wide(void);
/* One line describing whether block reads use DMA, fell back, or failed. */
const char *SD_DMA_status(void);
/* Print status. Save dma.txt on fallback/failure, or when zimlog.on requests
 * an early boot checkpoint and SPI register values. Otherwise remove it. */
void SD_DMA_report(void);
void SD_DMA_profile(File_IOStats *out, bool enabled);

#endif /* SD_DMA_H */
