/* Device-sized WebP decoding and one-bit Atkinson dithering. */
#ifndef WIKIREADER_ZIM_IMAGE_H
#define WIKIREADER_ZIM_IMAGE_H

#include <stddef.h>
#include <stdint.h>

int zim_webp_to_bitmap(const unsigned char *webp, size_t webp_size,
		       unsigned int requested_width,
		       unsigned int requested_height,
		       unsigned char *bitmap, size_t capacity,
		       uint8_t *width, uint16_t *height,
		       size_t *bitmap_size);

#endif
