#ifndef WIKIREADER_ZIM_ARTICLE_H
#define WIKIREADER_ZIM_ARTICLE_H

#include <stddef.h>

int zim_text_to_article(const unsigned char *text, size_t text_size,
			unsigned char *article, size_t capacity,
			size_t *article_size);

#endif
