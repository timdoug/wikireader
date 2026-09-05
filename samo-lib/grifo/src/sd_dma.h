#ifndef SD_DMA_H
#define SD_DMA_H

void SD_DMA_initialise(void);
/* One line describing whether block reads use DMA, fell back, or failed. */
const char *SD_DMA_status(void);
/* Print that line; write it to dma.txt on the boot volume only when DMA
 * fell back or failed, and remove a stale dma.txt when it did not. */
void SD_DMA_report(void);

#endif /* SD_DMA_H */
