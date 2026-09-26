/*
 * The host reference for the WikiReader Game Boy: runs gb.c natively over
 * the same script and prints the hashes the device prints, so a C33 build
 * can be checked frame for frame.
 *
 *   host ROM FRAMES [SCRIPT] [-p FRAME,FRAME,...] [-k]
 *
 * -p writes the panel as it would look at those frames to frame-N.pgm;
 * -k turns frame skipping on, as the device's default does.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"

static uint8_t panel[GBW_STRIDE * GBW_ROWS];

void gbw_serial_out(uint8_t byte)
{
	putchar(byte);
}

void gbw_error(const char *what, unsigned address)
{
	fprintf(stderr, "gb: %s at %04x\n", what, address);
	exit(1);
}

static void write_pgm(unsigned frame)
{
	char name[32];
	FILE *out;

	snprintf(name, sizeof name, "frame-%u.pgm", frame);
	out = fopen(name, "wb");
	fprintf(out, "P5\n240 208\n255\n");
	for (int y = 0; y < GBW_ROWS; ++y)
		for (int x = 0; x < 240; ++x)
			fputc(panel[y * GBW_STRIDE + x / 8] & (0x80 >> (x & 7))
			      ? 0 : 255, out);
	fclose(out);
}

int main(int argc, char **argv)
{
	const char *script = NULL, *pictures = "";
	int skip = 0;
	FILE *file;
	long size;
	uint8_t header[0x150];
	uint8_t *rom, *ram;
	unsigned frames;

#ifdef GBW_CHECK_RENDER
	if (argc == 3 && strcmp(argv[1], "-fuzz") == 0) {
		extern void gbw_fuzz_render(unsigned rounds);
		static uint8_t blank[0x8000];

		blank[0x147] = 0;		/* a plain 32 KiB cartridge */
		for (unsigned i = 0x134, x = 0; i <= 0x14c; ++i)
			blank[0x14d] = (uint8_t)(x = x - blank[i] - 1);
		gbw_init(blank, sizeof blank);
		gbw_set_framebuffer(panel);
		gbw_fuzz_render((unsigned)strtoul(argv[2], NULL, 0));
		printf("gb: render fuzz passed\n");
		return 0;
	}
#endif
	if (argc < 3) {
		fprintf(stderr, "usage: host ROM FRAMES [SCRIPT] [-p F,F] [-k]\n");
		return 2;
	}
	for (int i = 3; i < argc; ++i) {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			pictures = argv[++i];
		else if (strcmp(argv[i], "-k") == 0)
			skip = 1;
		else
			script = argv[i];
	}
	frames = (unsigned)strtoul(argv[2], NULL, 0);
	file = fopen(argv[1], "rb");
	if (!file) {
		perror(argv[1]);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	if (size < (long)sizeof header || fread(header, 1, sizeof header, file)
	    != sizeof header) {
		fprintf(stderr, "%s: too short\n", argv[1]);
		return 1;
	}
	rom = malloc(gbw_rom_bytes(header, (size_t)size));
	rewind(file);
	if (fread(rom, 1, (size_t)size, file) != (size_t)size)
		return 1;
	fclose(file);

	const char *failure = gbw_init(rom, (size_t)size);
	if (failure) {
		fprintf(stderr, "gb: %s\n", failure);
		return 1;
	}
	size_t ram_bytes = gbw_save_bytes();
	ram = calloc(1, ram_bytes ? ram_bytes : 1);
	gbw_set_cart_ram(ram, ram_bytes);
	gbw_set_framebuffer(panel);
	gbw_set_frame_skip(skip);
	printf("gb: %s, %ld bytes, %zu bytes of cartridge RAM\n", gbw_title(),
	       size, ram_bytes);

	for (unsigned frame = 1; frame <= frames; ++frame) {
		gbw_set_buttons(gbw_script_buttons(script, frame));
		gbw_run_frame();
		if (frame % 60 == 0 || frame == frames)
			printf("gb: frame %u hash %08x\n", frame, gbw_hash(panel));
		for (const char *p = pictures; *p; ) {
			if (strtoul(p, NULL, 10) == frame)
				write_pgm(frame);
			p = strchr(p, ',');
			p = p ? p + 1 : "";
		}
	}
	return 0;
}
