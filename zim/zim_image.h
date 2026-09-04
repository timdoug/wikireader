/* Device-sized WebP decoding and one-bit Atkinson dithering. */
#ifndef WIKIREADER_ZIM_IMAGE_H
#define WIKIREADER_ZIM_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define ZIM_IMAGE_MAX_WIDTH 226
#define ZIM_IMAGE_MAX_HEIGHT 280

int zim_image_fit_dimensions(unsigned int source_width,
			     unsigned int source_height,
			     unsigned int requested_width,
			     unsigned int requested_height,
			     uint8_t *width, uint16_t *height,
			     size_t *bitmap_size);

int zim_webp_to_bitmap(const unsigned char *webp, size_t webp_size,
		       unsigned int requested_width,
		       unsigned int requested_height,
		       unsigned char *bitmap, size_t capacity,
		       uint8_t *width, uint16_t *height,
		       size_t *bitmap_size);

#endif
