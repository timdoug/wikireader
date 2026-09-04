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

typedef uint32_t (*ZIM_ARTICLE_LINK)(void *opaque,
				     const unsigned char *path,
				     size_t path_length);

int zim_text_to_article(const unsigned char *text, size_t text_size,
			unsigned char *article, size_t capacity,
			size_t *article_size);

int zim_text_to_article_images(const unsigned char *text, size_t text_size,
			       unsigned char *article, size_t capacity,
			       size_t *article_size,
			       ZIM_ARTICLE_IMAGE image, void *image_opaque);

/* Called for each anchor record with the element id and the vertical
 * position, in stream pixels, of the line it precedes. */
typedef void (*ZIM_ARTICLE_ANCHOR)(void *opaque, const unsigned char *id,
				   size_t id_length, int y);

/* stream_height, when non-NULL, receives the same value
 * zim_article_stream_height() would compute for the produced stream. */
int zim_text_to_article_images_links(const unsigned char *text,
				     size_t text_size,
				     unsigned char *article, size_t capacity,
				     size_t *article_size,
				     ZIM_ARTICLE_IMAGE image,
				     void *image_opaque,
				     ZIM_ARTICLE_LINK link,
				     void *link_opaque,
				     ZIM_ARTICLE_ANCHOR anchor,
				     void *anchor_opaque,
				     int *stream_height);

int zim_article_stream_height(const unsigned char *article,
			      size_t article_size);

#endif
