#define _GNU_SOURCE /* memmem */
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
	const char fragment[] = "#Later";
	(void)opaque;
	if (length == sizeof(fragment) - 1 &&
	    !memcmp(path, fragment, sizeof(fragment) - 1))
		return 1234;
	return length == sizeof(expected) - 1 &&
		!memcmp(path, expected, sizeof(expected) - 1) ? 1234 : 0;
}

static int anchor_calls;
static int anchor_y;

static void note_anchor(void *opaque, const unsigned char *id,
			size_t id_length, int y)
{
	(void)opaque;
	if (id_length == 5 && !memcmp(id, "Later", 5)) {
		anchor_calls++;
		anchor_y = y;
	}
}

int main(void)
{
	static const unsigned char html[] =
		"<body><main><h1>Title</h1><p>See "
		"<a href=\"../New%20York\">New York has a deliberately long link"
		"</a> after <a href=\"https://example.com\">external</a>"
		" and <a href=\"#Later\">below</a>."
		"</p><div class=\"navbox\"><table><tr><td>NAVJUNK</td></tr></table>"
		"</div><span style=\"display: none\">HIDDEN</span>"
		"<span class=\"mw-editsection\">[edit]</span>"
		"<div class=\"mw-heading\"><h2 id=\"Later\">Later</h2></div>"
		"<p id=\"mwAQ\">Body</p></main></body>";
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
	    !memchr(normalized, ZIM_TEXT_LINK_END_MARKER, normalized_size) ||
	    !memchr(normalized, ZIM_TEXT_ANCHOR_MARKER, normalized_size))
		return 2;
	if (memmem(normalized, normalized_size, "NAVJUNK", 7) ||
	    memmem(normalized, normalized_size, "HIDDEN", 6) ||
	    memmem(normalized, normalized_size, "[edit]", 6) ||
	    !memmem(normalized, normalized_size, "Later", 5) ||
	    !memmem(normalized, normalized_size, "Body", 4) ||
	    memmem(normalized, normalized_size, "mwAQ", 4))
		return 9;
	if (zim_text_to_article_images_links(normalized, normalized_size,
			article, sizeof(article), &article_size, NULL, NULL,
			resolve_link, NULL, note_anchor, NULL, &height))
		return 3;
	if (anchor_calls != 1 || anchor_y <= 0 || anchor_y >= height)
		return 10;
	if (height <= 0 ||
	    height != zim_article_stream_height(article, article_size))
		return 7;
	memcpy(&header, article, sizeof(header));
	if (header.article_link_count < 3 || header.article_link_count > 8 ||
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
	/* A buffer too small for the text yields a valid, marked, shorter
	 * stream rather than a failure. */
	{
		unsigned char small[100];
		size_t small_size;
		int small_height;

		if (zim_text_to_article_images_links(normalized, normalized_size,
				small, sizeof(small), &small_size, NULL, NULL,
				resolve_link, NULL, note_anchor, NULL, &small_height))
			return 11;
		memcpy(&header, small, sizeof(header));
		if (small_size > sizeof(small) || small_size < 40 ||
		    header.offset_article != sizeof(header) +
			header.article_link_count * sizeof(ARTICLE_LINK) ||
		    !memmem(small, small_size, "(article truncated)", 19) ||
		    small_height != zim_article_stream_height(small, small_size))
			return 12;
	}
	puts("PASS: HTML links, same-page anchors, skipped navigation markup, "
	     "and truncation");
	return 0;
}
