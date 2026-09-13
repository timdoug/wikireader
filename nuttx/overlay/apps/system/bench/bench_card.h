/****************************************************************************
 * apps/system/bench/bench_card.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_BENCH_BENCH_CARD_H
#define __APPS_SYSTEM_BENCH_BENCH_CARD_H

#include <nuttx/config.h>

/* Where a measurement goes when nobody has redirected it.
 *
 * The panel is thirty-nine columns and does not scroll, so the card is how
 * anything leaves this device. Opening returns the descriptor to write to
 * -- standard output if the file cannot be opened, so the numbers are at
 * least somewhere -- and closing gets them onto the card and takes the
 * filesystem down, so that the line saying the card can be removed is true
 * when it is printed.
 */

int  bench_card_open(const char *path);
void bench_card_geometry(int fd);
void bench_card_close(int fd, const char *path);

#endif /* __APPS_SYSTEM_BENCH_BENCH_CARD_H */
