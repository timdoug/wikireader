/* Convert normalized ZIM text to the WikiReader's pre-wrapped article stream. */
#include "zim_article.h"

#include <string.h>

#include "lcd_buf_draw.h"

#define ARTICLE_TEXT_WIDTH (LCD_BUF_WIDTH_PIXELS - LCD_LEFT_MARGIN * 2)

static int append_byte(unsigned char *output, size_t capacity, size_t *used,
		       unsigned char value)
{
	if (*used >= capacity)
		return -1;
	output[(*used)++] = value;
	return 0;
}

static int append_bytes(unsigned char *output, size_t capacity, size_t *used,
			const unsigned char *bytes, size_t count)
{
	if (count > capacity - *used)
		return -1;
	memcpy(output + *used, bytes, count);
	*used += count;
	return 0;
}

static int next_width(int font, const unsigned char *text, size_t remaining,
		      size_t *character_size)
{
	const unsigned char *next = text;
	long length = (long)remaining;
	int bytes = 0;
	int width = get_UTF8_char_width(font, &next, &length, &bytes);
	if (bytes <= 0 || (size_t)bytes > remaining)
		bytes = 1;
	*character_size = (size_t)bytes;
	return width;
}

static int emit_newline(unsigned char *output, size_t capacity, size_t *used,
			int font, int explicit_font)
{
	int line_space = pcfFonts[font - 1].Fmetrics.linespace + LINE_SPACE_ADDON;
	if (line_space > 31)
		line_space = 31;
	if (explicit_font) {
		return append_byte(output, capacity, used,
				   ESC_3_NEW_LINE_WITH_FONT) ||
			append_byte(output, capacity, used,
				    (unsigned char)((line_space << 3) | font));
	}
	return append_byte(output, capacity, used, ESC_2_NEW_LINE_SAME_FONT);
}

static int word_width(int font, const unsigned char *word, size_t length)
{
	int width = 0;
	size_t offset = 0;
	while (offset < length) {
		size_t character_size;
		width += next_width(font, word + offset, length - offset,
				    &character_size);
		offset += character_size;
	}
	return width;
}

int zim_text_to_article(const unsigned char *text, size_t text_size,
			unsigned char *article, size_t capacity,
			size_t *article_size)
{
	ARTICLE_HEADER header;
	size_t input = 0;
	size_t used = sizeof(header);
	int font = TITLE_FONT_IDX;
	int x = 0;
	int first_line = 1;
	int rc;

	if (!text || !article || !article_size || capacity <= sizeof(header))
		return -1;
	memset(&header, 0, sizeof(header));
	header.offset_article = sizeof(header);
	memcpy(article, &header, sizeof(header));
	rc = emit_newline(article, capacity, &used, font, 1);
	if (rc)
		return -1;

	while (input < text_size) {
		size_t word_start;
		size_t word_length;
		int width;
		int space_width = 0;

		if (text[input] == '\n') {
			input++;
			if (first_line) {
				font = DEFAULT_FONT_IDX;
				first_line = 0;
				rc = emit_newline(article, capacity, &used, font, 1);
			} else {
				rc = emit_newline(article, capacity, &used, font, 0);
			}
			if (rc)
				return -1;
			x = 0;
			continue;
		}
		while (input < text_size && text[input] == ' ')
			input++;
		if (input >= text_size || text[input] == '\n')
			continue;
		word_start = input;
		while (input < text_size && text[input] != ' ' && text[input] != '\n')
			input++;
		word_length = input - word_start;
		width = word_width(font, text + word_start, word_length);
		if (x)
			space_width = word_width(font, (const unsigned char *)" ", 1);
		if (x && x + space_width + width > ARTICLE_TEXT_WIDTH) {
			if (emit_newline(article, capacity, &used, font, 0))
				return -1;
			x = 0;
			space_width = 0;
		}
		if (space_width) {
			if (append_byte(article, capacity, &used, ' '))
				return -1;
			x += space_width;
		}
		while (word_length) {
			size_t character_size;
			int character_width = next_width(font, text + word_start,
							 word_length,
							 &character_size);
			if (x && x + character_width > ARTICLE_TEXT_WIDTH) {
				if (emit_newline(article, capacity, &used, font, 0))
					return -1;
				x = 0;
			}
			if (append_bytes(article, capacity, &used, text + word_start,
					 character_size))
				return -1;
			x += character_width;
			word_start += character_size;
			word_length -= character_size;
		}
	}
	if (append_byte(article, capacity, &used, '\0'))
		return -1;
	*article_size = used;
	return 0;
}
