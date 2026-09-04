#include <stdio.h>
#include <string.h>

#include "lcd_buf_draw.h"
#include "zim_article.h"
#include "zim_html.h"
#include "zim_link.h"

pcffont_bmf_t pcfFonts[FONT_COUNT];

int get_UTF8_char_width(int font, const unsigned char **text, long *length,
			int *bytes)
{
	(void)font;
	if (!*length) {
		*bytes = 0;
		return 0;
	}
	(*text)++;
	(*length)--;
	*bytes = 1;
	return 10;
}

static uint32_t resolve_link(void *opaque, const unsigned char *path,
			     size_t length)
{
	const char expected[] = "../New%20York";
	(void)opaque;
	return length == sizeof(expected) - 1 &&
		!memcmp(path, expected, sizeof(expected) - 1) ? 1234 : 0;
}

int main(void)
{
	static const unsigned char html[] =
		"<body><main><h1>Title</h1><p>See "
		"<a href=\"../New%20York\">New York has a deliberately long link"
		"</a> after <a href=\"https://example.com\">external</a>."
		"</p></main></body>";
	unsigned char normalized[512];
	unsigned char article[1024];
	size_t normalized_size;
	size_t article_size;
	ARTICLE_HEADER header;
	ARTICLE_LINK links[8];
	char path[512];
	unsigned int i;
	int height;

	for (i = 0; i < FONT_COUNT; i++)
		pcfFonts[i].Fmetrics.linespace = 10;
	if (zim_html_to_text_images(html, sizeof(html) - 1, normalized,
				    sizeof(normalized), &normalized_size))
		return 1;
	if (!memchr(normalized, ZIM_TEXT_LINK_START_MARKER, normalized_size) ||
	    !memchr(normalized, ZIM_TEXT_LINK_END_MARKER, normalized_size))
		return 2;
	if (zim_text_to_article_images_links(normalized, normalized_size,
			article, sizeof(article), &article_size, NULL, NULL,
			resolve_link, NULL, &height))
		return 3;
	if (height <= 0 ||
	    height != zim_article_stream_height(article, article_size))
		return 7;
	memcpy(&header, article, sizeof(header));
	if (header.article_link_count < 2 || header.article_link_count > 8 ||
	    header.offset_article != sizeof(header) +
		header.article_link_count * sizeof(ARTICLE_LINK) ||
	    header.offset_article >= article_size)
		return 4;
	memcpy(links, article + sizeof(header),
	       header.article_link_count * sizeof(ARTICLE_LINK));
	for (i = 0; i < header.article_link_count; i++) {
		if (links[i].article_id != 1234 ||
		    (links[i].start_xy >> 8) > (links[i].end_xy >> 8) ||
		    (links[i].start_xy & 0xff) > (links[i].end_xy & 0xff))
			return 5;
		if (i && (links[i - 1].start_xy >> 8) >
			 (links[i].start_xy >> 8))
			return 6;
	}
	if (zim_link_normalize("Paris/1st_arrondissement",
			       (const unsigned char *)"../%C3%8Ele-de-France#Map",
			       strlen("../%C3%8Ele-de-France#Map"), path,
			       sizeof(path)) ||
	    strcmp(path, "\xc3\x8ele-de-France"))
		return 7;
	if (zim_link_normalize("A/B/C", (const unsigned char *)"../../D&amp;E",
			       strlen("../../D&amp;E"), path, sizeof(path)) ||
	    strcmp(path, "D&E"))
		return 8;
	puts("PASS: HTML links become wrapped WikiReader link rectangles");
	return 0;
}
