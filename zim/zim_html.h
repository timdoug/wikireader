/* Small HTML-to-text adapter for offline ZIM article payloads. */
#ifndef WIKIREADER_ZIM_HTML_H
#define WIKIREADER_ZIM_HTML_H

#include <stddef.h>

/* Extract readable UTF-8 text from an article. Structural elements become
 * newlines; scripts, styles, markup, and images are omitted. */
int zim_html_to_text(const unsigned char *html, size_t html_size,
		     unsigned char *text, size_t capacity,
		     size_t *text_size);

#endif
