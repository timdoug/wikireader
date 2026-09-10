#ifndef ZIM_COPY_H
#define ZIM_COPY_H
#include <stddef.h>
#if defined(__c33__)
void *zim_copy(void *dst, const void *src, size_t size);
#else
#include <string.h>
#define zim_copy memmove
#endif
typedef struct {
    unsigned long cpu_bytes, dma_bytes, dma_calls, dma_errors;
} ZIM_COPY_STATS;
extern ZIM_COPY_STATS zim_copy_stats;
/* Cumulative diagnostics include work before the first timed retrieval.
 * Snapshots are read-only and retain the first guard rejection/failure. */
typedef struct {
    unsigned long attempt, dst, src, bytes, enabled, tf0, fdma, edma;
    unsigned long select, count0, control0, source0, dest0, adv0;
    unsigned long mode, acctime, gate, idma;
} ZIM_COPY_STATE;
typedef struct {
    unsigned long large, unaligned, identical, overlap, source, layout, eligible, bank;
    unsigned long attempts, latched, busy, trigger, irq, failed, reset_irq;
    ZIM_COPY_STATE first_guard, first_fault, first_reset;
} ZIM_COPY_DIAG;
extern ZIM_COPY_DIAG zim_copy_diag;
#endif
