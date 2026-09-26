/*
 * Unmodified Peanut-GB timing and memory paths, under other names, for
 * gb.c's GBW_LOCKSTEP build to step beside the fast ones.
 * SPDX-License-Identifier: MIT
 */
#include "fast_types.h"

#define __gb_draw_line ref___gb_draw_line
#define __gb_execute_cb ref___gb_execute_cb
#define __gb_read ref___gb_read
#define __gb_step_cpu ref___gb_step_cpu
#define __gb_tick ref___gb_tick
#define __gb_write ref___gb_write
#define gb_colour_hash ref_gb_colour_hash
#define gb_get_rom_name ref_gb_get_rom_name
#define gb_get_save_size ref_gb_get_save_size
#define gb_get_save_size_s ref_gb_get_save_size_s
#define gb_init ref_gb_init
#define gb_init_lcd ref_gb_init_lcd
#define gb_init_serial ref_gb_init_serial
#define gb_reset ref_gb_reset
#define gb_run_frame ref_gb_run_frame
#define gb_set_bootrom ref_gb_set_bootrom
#define gb_set_rtc ref_gb_set_rtc
#define gb_tick_rtc ref_gb_tick_rtc

#define ENABLE_SOUND 0
#define PEANUT_GB_12_COLOUR 0
#include "peanut_gb.h"
