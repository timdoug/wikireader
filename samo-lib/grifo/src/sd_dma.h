#ifndef SD_DMA_H
#define SD_DMA_H

void SD_DMA_initialise(void);
/* One line describing whether block reads use DMA, fell back, or failed. */
const char *SD_DMA_status(void);
/* Print that line and write it to dma.txt on the boot volume. */
void SD_DMA_report(void);

#endif /* SD_DMA_H */
