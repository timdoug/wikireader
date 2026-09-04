#ifndef WIKIREADER_ZIM_LINK_H
#define WIKIREADER_ZIM_LINK_H

#include <stddef.h>

int zim_link_normalize(const char *base_path, const unsigned char *href,
		       size_t href_length, char *path, size_t capacity);

#endif
