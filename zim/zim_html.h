/* Small HTML-to-text adapter for offline ZIM article payloads. */
#ifndef WIKIREADER_ZIM_HTML_H
#define WIKIREADER_ZIM_HTML_H

#include <stddef.h>

/* An image in the image-aware normalized stream is encoded as this marker,
 * little-endian displayed width and height, little-endian path length, and
 * the non-NUL-terminated ZIM path. */
#define ZIM_TEXT_IMAGE_MARKER 0x1f
#define ZIM_TEXT_LINK_START_MARKER 0x1e
#define ZIM_TEXT_LINK_END_MARKER 0x1d

/* Extract readable UTF-8 text from an article. Structural elements become
 * newlines; scripts, styles, markup, and images are omitted. */
int zim_html_to_text(const unsigned char *html, size_t html_size,
		     unsigned char *text, size_t capacity,
		     size_t *text_size);

/* As above, preserving img elements and internal-link boundaries as compact
 * records in the normalized stream. Consumers which do not understand those
 * records should continue to use zim_html_to_text(). */
int zim_html_to_text_images(const unsigned char *html, size_t html_size,
			    unsigned char *text, size_t capacity,
			    size_t *text_size);

#endif
