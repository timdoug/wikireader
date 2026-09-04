/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zim_html.h"
#include "zim_archive.h"

#include <string.h>

typedef struct {
	unsigned char *text;
	size_t capacity;
	size_t used;
	unsigned char last;
	unsigned char previous;
	int pending_space;
} TEXT_OUTPUT;

static int ascii_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static unsigned char ascii_lower(unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int name_equal(const unsigned char *name, size_t length,
		      const char *literal)
{
	size_t i;
	for (i = 0; i < length && literal[i]; i++)
		if (ascii_lower(name[i]) != (unsigned char)literal[i])
			return 0;
	return i == length && !literal[i];
}

static void put_byte(TEXT_OUTPUT *out, unsigned char c)
{
	if (out->used + 1 < out->capacity)
		out->text[out->used] = c;
	out->previous = out->last;
	out->last = c;
	out->used++;
}

static void put_space_if_needed(TEXT_OUTPUT *out)
{
	if (!out->pending_space)
		return;
	out->pending_space = 0;
	if (out->used && out->last != '\n' && out->last != ' ')
		put_byte(out, ' ');
}

static void put_newline(TEXT_OUTPUT *out, int paragraph)
{
	out->pending_space = 0;
	while (out->used && out->last == ' ') {
		out->used--;
		out->last = out->previous;
	}
	if (out->used && out->last != '\n')
		put_byte(out, '\n');
	if (paragraph && out->used && out->previous != '\n')
		put_byte(out, '\n');
}

static void put_bytes(TEXT_OUTPUT *out, const unsigned char *bytes,
		      size_t count)
{
	while (count--)
		put_byte(out, *bytes++);
}

static int attribute_value(const unsigned char *attributes, size_t length,
			   const char *wanted,
			   const unsigned char **value, size_t *value_length)
{
	size_t i = 0;

	while (i < length) {
		size_t name_start;
		size_t name_end;
		size_t start;
		unsigned char quote = 0;

		while (i < length && ascii_space(attributes[i])) i++;
		name_start = i;
		while (i < length && !ascii_space(attributes[i]) &&
		       attributes[i] != '=' && attributes[i] != '>') i++;
		name_end = i;
		while (i < length && ascii_space(attributes[i])) i++;
		if (i >= length || attributes[i] != '=') {
			while (i < length && !ascii_space(attributes[i])) i++;
			continue;
		}
		i++;
		while (i < length && ascii_space(attributes[i])) i++;
		if (i < length && (attributes[i] == '\'' || attributes[i] == '"'))
			quote = attributes[i++];
		start = i;
		if (quote) {
			while (i < length && attributes[i] != quote) i++;
		} else {
			while (i < length && !ascii_space(attributes[i]) &&
			       attributes[i] != '>') i++;
		}
		if (name_equal(attributes + name_start, name_end - name_start,
			       wanted)) {
			*value = attributes + start;
			*value_length = i - start;
			return 1;
		}
		if (quote && i < length) i++;
	}
	return 0;
}

static unsigned int decimal_attribute(const unsigned char *value,
				      size_t length)
{
	unsigned int result = 0;
	size_t i;

	if (!length)
		return 0;
	for (i = 0; i < length; i++) {
		if (value[i] < '0' || value[i] > '9' || result > 6553)
			return 0;
		result = result * 10 + value[i] - '0';
	}
	return result <= 65535 ? result : 0;
}

static void put_image(TEXT_OUTPUT *out, const unsigned char *attributes,
		      size_t length)
{
	const unsigned char *src;
	const unsigned char *dimension;
	size_t src_length;
	size_t dimension_length;
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned char record[7];

	if (!attribute_value(attributes, length, "src", &src, &src_length) ||
	    !src_length || src_length > 65535 ||
	    (src_length >= 5 && !memcmp(src, "data:", 5)))
		return;
	if (attribute_value(attributes, length, "width", &dimension,
			    &dimension_length))
		width = decimal_attribute(dimension, dimension_length);
	if (attribute_value(attributes, length, "height", &dimension,
			    &dimension_length))
		height = decimal_attribute(dimension, dimension_length);

	put_newline(out, 0);
	record[0] = ZIM_TEXT_IMAGE_MARKER;
	record[1] = (unsigned char)width;
	record[2] = (unsigned char)(width >> 8);
	record[3] = (unsigned char)height;
	record[4] = (unsigned char)(height >> 8);
	record[5] = (unsigned char)src_length;
	record[6] = (unsigned char)(src_length >> 8);
	put_bytes(out, record, sizeof(record));
	put_bytes(out, src, src_length);
	put_newline(out, 0);
}

static size_t encode_utf8(uint32_t value, unsigned char bytes[4])
{
	if (value <= 0x7f) {
		bytes[0] = (unsigned char)value;
		return 1;
	}
	if (value <= 0x7ff) {
		bytes[0] = 0xc0 | (unsigned char)(value >> 6);
		bytes[1] = 0x80 | (unsigned char)(value & 0x3f);
		return 2;
	}
	if (value >= 0xd800 && value <= 0xdfff)
		return 0;
	if (value <= 0xffff) {
		bytes[0] = 0xe0 | (unsigned char)(value >> 12);
		bytes[1] = 0x80 | (unsigned char)((value >> 6) & 0x3f);
		bytes[2] = 0x80 | (unsigned char)(value & 0x3f);
		return 3;
	}
	if (value <= 0x10ffff) {
		bytes[0] = 0xf0 | (unsigned char)(value >> 18);
		bytes[1] = 0x80 | (unsigned char)((value >> 12) & 0x3f);
		bytes[2] = 0x80 | (unsigned char)((value >> 6) & 0x3f);
		bytes[3] = 0x80 | (unsigned char)(value & 0x3f);
		return 4;
	}
	return 0;
}

static size_t decode_entity(const unsigned char *input, size_t length,
			    unsigned char bytes[4])
{
	uint32_t value = 0;
	size_t i;
	int base = 10;

	if (length > 1 && input[0] == '#') {
		i = 1;
		if (i < length && (input[i] == 'x' || input[i] == 'X')) {
			base = 16;
			i++;
		}
		if (i == length)
			return 0;
		for (; i < length; i++) {
			unsigned int digit;
			if (input[i] >= '0' && input[i] <= '9')
				digit = input[i] - '0';
			else if (base == 16 && ascii_lower(input[i]) >= 'a' &&
				 ascii_lower(input[i]) <= 'f')
				digit = ascii_lower(input[i]) - 'a' + 10;
			else
				return 0;
			if (value > (0x10ffffU - digit) / (unsigned int)base)
				return 0;
			value = value * (unsigned int)base + digit;
		}
		return encode_utf8(value, bytes);
	}
	if (name_equal(input, length, "amp")) bytes[0] = '&';
	else if (name_equal(input, length, "lt")) bytes[0] = '<';
	else if (name_equal(input, length, "gt")) bytes[0] = '>';
	else if (name_equal(input, length, "quot")) bytes[0] = '"';
	else if (name_equal(input, length, "apos")) bytes[0] = '\'';
	else if (name_equal(input, length, "nbsp")) bytes[0] = ' ';
	else if (name_equal(input, length, "ndash")) return encode_utf8(0x2013, bytes);
	else if (name_equal(input, length, "mdash")) return encode_utf8(0x2014, bytes);
	else if (name_equal(input, length, "hellip")) return encode_utf8(0x2026, bytes);
	else if (name_equal(input, length, "middot")) return encode_utf8(0x00b7, bytes);
	else return 0;
	return 1;
}

static int block_tag(const unsigned char *name, size_t length)
{
	return name_equal(name, length, "p") || name_equal(name, length, "div") ||
		name_equal(name, length, "section") ||
		name_equal(name, length, "table") || name_equal(name, length, "tr") ||
		name_equal(name, length, "ul") || name_equal(name, length, "ol") ||
		name_equal(name, length, "dl") || name_equal(name, length, "dd") ||
		name_equal(name, length, "dt") || name_equal(name, length, "blockquote") ||
		(length == 2 && ascii_lower(name[0]) == 'h' &&
		 name[1] >= '1' && name[1] <= '6');
}

static int html_to_text(const unsigned char *html, size_t html_size,
			unsigned char *text, size_t capacity,
			size_t *text_size, int include_images)
{
	TEXT_OUTPUT out;
	size_t i = 0;
	int in_body = 0;
	int in_main = 0;
	int suppress = 0;

	if (!html || (!text && capacity) || !text_size)
		return ZIM_ERR_RANGE;
	out.text = text;
	out.capacity = capacity;
	out.used = 0;
	out.last = 0;
	out.previous = 0;
	out.pending_space = 0;
	while (i < html_size) {
		if (html[i] == '<') {
			size_t tag_start;
			size_t tag_end;
			size_t attributes_start;
			size_t attributes_end;
			int closing = 0;

			if (i + 3 < html_size && !memcmp(html + i, "<!--", 4)) {
				size_t end = i + 4;
				while (end + 2 < html_size && memcmp(html + end, "-->", 3))
					end++;
				if (end + 2 >= html_size)
					break;
				i = end + 3;
				continue;
			}
			i++;
			while (i < html_size && ascii_space(html[i])) i++;
			if (i < html_size && html[i] == '/') {
				closing = 1;
				i++;
			}
			tag_start = i;
			while (i < html_size &&
			       ((html[i] >= 'a' && html[i] <= 'z') ||
				(html[i] >= 'A' && html[i] <= 'Z') ||
				(html[i] >= '0' && html[i] <= '9')))
				i++;
			tag_end = i;
			attributes_start = i;
			while (i < html_size && html[i] != '>') i++;
			attributes_end = i;
			if (i < html_size) i++;
			if (tag_start == tag_end)
				continue;
			if (name_equal(html + tag_start, tag_end - tag_start, "body")) {
				in_body = !closing;
				continue;
			}
			if (!in_body)
				continue;
			if (name_equal(html + tag_start, tag_end - tag_start, "main")) {
				if (closing && in_main)
					break;
				in_main = !closing;
				continue;
			}
			if (!in_main)
				continue;
			if (name_equal(html + tag_start, tag_end - tag_start, "style") ||
			    name_equal(html + tag_start, tag_end - tag_start, "script")) {
				if (closing && suppress)
					suppress--;
				else if (!closing)
					suppress++;
				continue;
			}
			if (suppress)
				continue;
			if (include_images && !closing &&
			    name_equal(html + tag_start, tag_end - tag_start, "img")) {
				put_image(&out, html + attributes_start,
					  attributes_end - attributes_start);
			} else if (!closing && name_equal(html + tag_start, tag_end - tag_start, "li")) {
				put_newline(&out, 0);
				put_byte(&out, '*');
				put_byte(&out, ' ');
			} else if (name_equal(html + tag_start, tag_end - tag_start, "br")) {
				put_newline(&out, 0);
			} else if (!closing &&
				   (name_equal(html + tag_start, tag_end - tag_start, "td") ||
				    name_equal(html + tag_start, tag_end - tag_start, "th"))) {
				put_space_if_needed(&out);
				if (out.used && out.last != '\n') {
					put_byte(&out, '|');
					put_byte(&out, ' ');
				}
			} else if (block_tag(html + tag_start, tag_end - tag_start)) {
				put_newline(&out, closing &&
					    (name_equal(html + tag_start, tag_end - tag_start, "p") ||
					     (tag_end - tag_start == 2 &&
					      ascii_lower(html[tag_start]) == 'h')));
			}
			continue;
		}
		if (!in_body || !in_main || suppress) {
			i++;
			continue;
		}
		if (html[i] == '&') {
			size_t end = i + 1;
			unsigned char bytes[4];
			size_t count;
			while (end < html_size && end - i <= 16 && html[end] != ';' &&
			       !ascii_space(html[end]) && html[end] != '<')
				end++;
			if (end < html_size && html[end] == ';' &&
			    (count = decode_entity(html + i + 1, end - i - 1, bytes)) != 0) {
				size_t j;
				if (count == 1 && ascii_space(bytes[0]))
					out.pending_space = 1;
				else {
					put_space_if_needed(&out);
					for (j = 0; j < count; j++) put_byte(&out, bytes[j]);
				}
				i = end + 1;
				continue;
			}
		}
		if (ascii_space(html[i]))
			out.pending_space = 1;
		else if (html[i] >= 0x20) {
			put_space_if_needed(&out);
			put_byte(&out, html[i]);
		}
		i++;
	}
	while (out.used && out.used < out.capacity &&
	       (out.text[out.used - 1] == ' ' || out.text[out.used - 1] == '\n'))
		out.used--;
	*text_size = out.used;
	if (capacity) {
		size_t terminator = out.used < capacity ? out.used : capacity - 1;
		text[terminator] = '\0';
	}
	return out.used + 1 > capacity ? ZIM_ERR_TRUNCATED : ZIM_OK;
}

int zim_html_to_text(const unsigned char *html, size_t html_size,
		     unsigned char *text, size_t capacity,
		     size_t *text_size)
{
	return html_to_text(html, html_size, text, capacity, text_size, 0);
}

int zim_html_to_text_images(const unsigned char *html, size_t html_size,
			    unsigned char *text, size_t capacity,
			    size_t *text_size)
{
	return html_to_text(html, html_size, text, capacity, text_size, 1);
}
