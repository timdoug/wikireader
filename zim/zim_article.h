#ifndef WIKIREADER_ZIM_ARTICLE_H
#define WIKIREADER_ZIM_ARTICLE_H

#include <stddef.h>
#include <stdint.h>

typedef int (*ZIM_ARTICLE_IMAGE)(void *opaque,
				 const unsigned char *path, size_t path_length,
				 unsigned int requested_width,
				 unsigned int requested_height,
				 unsigned char *bitmap, size_t capacity,
				 uint8_t *width, uint16_t *height,
				 size_t *bitmap_size);

int zim_text_to_article(const unsigned char *text, size_t text_size,
			unsigned char *article, size_t capacity,
			size_t *article_size);

int zim_text_to_article_images(const unsigned char *text, size_t text_size,
			       unsigned char *article, size_t capacity,
			       size_t *article_size,
			       ZIM_ARTICLE_IMAGE image, void *image_opaque);

#endif
