#include "search_fnd.h"

void init_search_fnd(void) {}

int copy_fnd_to_buf(long offset, unsigned char *buf, int len)
{
	(void)offset; (void)buf; (void)len;
	return 0;
}

long get_search_offset_fnd(char *text, int len)
{
	(void)text; (void)len;
	return -1;
}

void retrieve_titles_from_fnd(long offset, unsigned char *search,
			      unsigned char *actual)
{
	(void)offset;
	if (search) search[0] = '\0';
	if (actual) actual[0] = '\0';
}
