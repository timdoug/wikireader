#ifndef MMC_PROFILE_H
#define MMC_PROFILE_H

/* Independent of the legacy/modern FatFs typedefs. result -1 starts a read
 * of sectors blocks; a nonnegative result ends it. Synchronous, never ISR. */
typedef void (*mmc_read_observer_fn)(unsigned int sectors, int result);
void mmc_set_read_observer(mmc_read_observer_fn observer);

#endif
