############################################################################
# apps/system/bench/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_SYSTEM_BENCH_PROGNAME) ubench selftest
PRIORITY  = $(CONFIG_SYSTEM_BENCH_PRIORITY) $(CONFIG_SYSTEM_BENCH_PRIORITY) $(CONFIG_SYSTEM_BENCH_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_BENCH_STACKSIZE) $(CONFIG_SYSTEM_BENCH_STACKSIZE) $(CONFIG_SYSTEM_BENCH_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_BENCH)

MAINSRC = bench_main.c ubench_main.c selftest_main.c
ASRCS   = ubench.S

include $(APPDIR)/Application.mk
