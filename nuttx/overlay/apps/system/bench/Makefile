############################################################################
# apps/system/bench/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_BENCH_PROGNAME) ubench selftest cardb
PRIORITY  = $(CONFIG_SYSTEM_BENCH_PRIORITY) $(CONFIG_SYSTEM_BENCH_PRIORITY) $(CONFIG_SYSTEM_BENCH_PRIORITY) $(CONFIG_SYSTEM_BENCH_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_BENCH_STACKSIZE) $(CONFIG_SYSTEM_BENCH_STACKSIZE) $(CONFIG_SYSTEM_BENCH_STACKSIZE) $(CONFIG_SYSTEM_BENCH_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_BENCH)

CSRCS   = bench_card.c
MAINSRC = bench_main.c ubench_main.c selftest_main.c cardb_main.c
ASRCS   = ubench.S

include $(APPDIR)/Application.mk
