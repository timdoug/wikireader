/*
 * Game Boy emulation for the WikiReader: the part that knows nothing about
 * Grifo, so the same code runs on a host for reference frame hashes.
 * SPDX-License-Identifier: MIT
 */
#ifndef WR_GB_H
#define WR_GB_H

#include <stddef.h>
#include <stdint.h>

enum {
	GBW_WIDTH = 160,
	GBW_HEIGHT = 144,
	/* The panel's framebuffer: 256 pixels a row, most significant bit
	   leftmost, 1 = black. */
	GBW_STRIDE = 32,
	GBW_ROWS = 208,
	/* Top right: the controls are to its left and below. */
	GBW_LEFT_BYTE = (240 - GBW_WIDTH) / 8,
	GBW_TOP = 0,
};

/* The Game Boy's buttons, active high here; gb.c inverts them. */
enum {
	GBW_A = 0x01,
	GBW_B = 0x02,
	GBW_SELECT = 0x04,
	GBW_START = 0x08,
	GBW_RIGHT = 0x10,
	GBW_LEFT = 0x20,
	GBW_UP = 0x40,
	GBW_DOWN = 0x80,
};

/* Supplied by the platform. */
void gbw_serial_out(uint8_t byte);
void gbw_error(const char *what, unsigned address);

/* rom must stay valid; it is padded to the size the header declares, so
   the buffer has to hold gbw_rom_bytes() bytes.  Returns 0 or an error
   string. */
size_t gbw_rom_bytes(const uint8_t *header, size_t file_bytes);
const char *gbw_init(uint8_t *rom, size_t file_bytes);
/* Battery RAM the cartridge declares, known once gbw_init has run. */
size_t gbw_save_bytes(void);
void gbw_set_cart_ram(uint8_t *ram, size_t bytes);
const char *gbw_title(void);

/* Lines are dithered into this framebuffer as they are drawn. */
void gbw_set_framebuffer(uint8_t *framebuffer);
void gbw_set_buttons(uint8_t buttons);
void gbw_set_frame_skip(int skip);
/* Gray as an ordered dither (the default), or thresholded at dark gray. */
void gbw_set_dither(int on);
/* Runs one Game Boy frame; returns the number of lines drawn. */
unsigned gbw_run_frame(void);
/* FNV-1a over a finished picture and the machine's RAM. */
uint32_t gbw_hash(const uint8_t *picture);
/* What the frame loop did, cumulative, when hot.s runs it. */
struct gbw_counts {
	unsigned long hot_calls, events, interrupts, given_back[256];
};
extern struct gbw_counts gbw_counts;

/* The buttons a scripted run holds at a frame; see gb.c. */
uint8_t gbw_script_buttons(const char *script, unsigned frame);

#endif
