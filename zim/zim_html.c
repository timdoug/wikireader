/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "zim_html.h"
#include "zim_archive.h"
#include "zim_overlay.h"

#include <string.h>

typedef struct {
	unsigned char *text;
	size_t capacity;
	size_t used;
	unsigned char last;
	unsigned char previous;
	int pending_space;
} TEXT_OUTPUT;

/* Paths the converter takes once per image, link, anchor, entity,
 * newline, or skipped element stay out of line so the loop that runs per byte and per
 * tag fits the IVRAM overlay (zim_overlay.h). */
#if defined(__c33__)
#define HTML_RARE __attribute__((noinline))
#else
#define HTML_RARE
#endif

static int ascii_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/* The same test with one comparison for the usual printable byte; the byte
 * scanners below run it on most of the page. */
#define IS_SPACE(c) ((c) <= ' ' && ascii_space(c))

static unsigned char ascii_lower(unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive equality with a literal made of letters, digits, '-' and
 * ':' (every name the converter looks for).  Compared four bytes at a time
 * as words built in registers against constants, so the literal is never
 * read from .rodata: on the C33 each byte of the old loop changed SDRAM row
 * three times, between the code, the literal and the page.  Setting bit 5
 * lower-cases a letter and leaves the other bytes of those literals alone;
 * `literal` must be a string constant so the words fold at compile time. */
static inline __attribute__((always_inline)) int
name_is(const unsigned char *name, size_t length, const char *literal,
	size_t literal_length)
{
	size_t i;

	if (length != literal_length)
		return 0;
	for (i = 0; i + 4 <= literal_length; i += 4) {
		unsigned int word = (unsigned int)name[i] |
			(unsigned int)name[i + 1] << 8 |
			(unsigned int)name[i + 2] << 16 |
			(unsigned int)name[i + 3] << 24;
		unsigned int key = (unsigned int)(unsigned char)literal[i] |
			(unsigned int)(unsigned char)literal[i + 1] << 8 |
			(unsigned int)(unsigned char)literal[i + 2] << 16 |
			(unsigned int)(unsigned char)literal[i + 3] << 24;
		if ((word | 0x20202020u) != key)
			return 0;
	}
	for (; i < literal_length; i++)
		if ((name[i] | 0x20u) != (unsigned char)literal[i])
			return 0;
	return 1;
}
#define NAME_IS(name, length, literal) \
	name_is(name, length, literal, sizeof(literal) - 1)

static void put_byte(TEXT_OUTPUT *out, unsigned char c)
{
	if (out->used + 1 < out->capacity)
		out->text[out->used] = c;
	out->previous = out->last;
	out->last = c;
	out->used++;
}

/* Records (anchors, links, images) do not count as text: out->last stays 0
 * until a visible byte has been written, so a record ahead of the title does
 * not push the title off the first line. */
static void put_space_if_needed(TEXT_OUTPUT *out)
{
	if (!out->pending_space)
		return;
	out->pending_space = 0;
	if (out->last && out->last != '\n' && out->last != ' ')
		put_byte(out, ' ');
}

static HTML_RARE void put_newline(TEXT_OUTPUT *out, int paragraph)
{
	out->pending_space = 0;
	while (out->used && out->last == ' ') {
		out->used--;
		out->last = out->previous;
	}
	if (out->last && out->last != '\n')
		put_byte(out, '\n');
	if (paragraph && out->last && out->previous != '\n')
		put_byte(out, '\n');
}

static void put_bytes(TEXT_OUTPUT *out, const unsigned char *bytes,
		      size_t count)
{
	while (count--)
		put_byte(out, *bytes++);
}

/* Append a run of plain text bytes with a single bookkeeping update.  Matches
 * put_byte: bytes are stored while they leave room for the terminator, and
 * the length keeps counting so truncation is still detected. */
static void put_text(TEXT_OUTPUT *out, const unsigned char *bytes,
		     size_t count)
{
	size_t room = out->used + 1 < out->capacity ?
		out->capacity - 1 - out->used : 0;
	size_t store = count < room ? count : room;
	unsigned char *destination = out->text + out->used;
	size_t k;

	for (k = 0; k < store; k++)
		destination[k] = bytes[k];
	if (count >= 2) {
		out->previous = bytes[count - 2];
		out->last = bytes[count - 1];
	} else {
		out->previous = out->last;
		out->last = bytes[0];
	}
	out->used += count;
}

static void put_record_bytes(TEXT_OUTPUT *out, const unsigned char *bytes,
			     size_t count)
{
	size_t available = out->used < out->capacity ?
		out->capacity - out->used : 0;

	if (count <= available) {
		unsigned char *destination = out->text + out->used;
		size_t k;

		for (k = 0; k < count; k++)
			destination[k] = bytes[k];
	}
	out->used += count;
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

/* The attributes the converter reacts to, found in one pass over a tag
 * rather than one scan per name: id for anchors, class and style for
 * skipping, href for links, src and the dimensions for images.  A bit in
 * `have` says which were seen; the first occurrence of each counts. */
enum {
	ATTR_ID = 1,
	ATTR_CLASS = 2,
	ATTR_STYLE = 4,
	ATTR_HREF = 8,
	ATTR_SRC = 16,
	ATTR_WIDTH = 32,
	ATTR_HEIGHT = 64
};
typedef struct {
	unsigned int have;
	const unsigned char *id;
	size_t id_length;
	const unsigned char *class_value;
	size_t class_length;
	const unsigned char *style;
	size_t style_length;
	const unsigned char *href;
	size_t href_length;
	const unsigned char *src;
	size_t src_length;
	const unsigned char *width;
	size_t width_length;
	const unsigned char *height;
	size_t height_length;
} TAG_ATTRIBUTES;

static HTML_RARE void put_image(TEXT_OUTPUT *out, const TAG_ATTRIBUTES *found)
{
	const unsigned char *src = found->src;
	size_t src_length = found->src_length;
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned char record[7];

	if (!(found->have & ATTR_SRC) ||
	    !src_length || src_length > 65535 ||
	    (src_length >= 5 && NAME_IS(src, 5, "data:")))
		return;
	if (found->have & ATTR_WIDTH)
		width = decimal_attribute(found->width, found->width_length);
	if (found->have & ATTR_HEIGHT)
		height = decimal_attribute(found->height, found->height_length);

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

static int internal_href(const unsigned char *href, size_t length)
{
	size_t i;

	if (!length || (length >= 2 && href[0] == '/' && href[1] == '/'))
		return 0;
	if (href[0] == '#')
		return length >= 2;
	for (i = 0; i < length && href[i] != '/' && href[i] != '?' &&
	     href[i] != '#'; i++)
		if (href[i] == ':')
			return 0;
	return 1;
}

static HTML_RARE int put_link_start(TEXT_OUTPUT *out, const TAG_ATTRIBUTES *found)
{
	const unsigned char *href = found->href;
	size_t href_length = found->href_length;
	unsigned char record[3];

	if (!(found->have & ATTR_HREF) ||
	    href_length > 65535 || !internal_href(href, href_length))
		return 0;
	record[0] = ZIM_TEXT_LINK_START_MARKER;
	record[1] = (unsigned char)href_length;
	record[2] = (unsigned char)(href_length >> 8);
	put_record_bytes(out, record, sizeof(record));
	put_record_bytes(out, href, href_length);
	return 1;
}

/* Parse a tag's attributes up to its closing '>' and return where that is
 * (or `limit`).  One pass: finding the '>' first and parsing the
 * attributes second read these bytes, most of a Kiwix page, twice, and a
 * '>' inside a quoted value ended the tag early.  A bare attribute is
 * passed over without swallowing the one after it. */
static const unsigned char *scan_tag_attributes(const unsigned char *p,
						const unsigned char *limit,
						TAG_ATTRIBUTES *found)
{
	found->have = 0;
	for (;;) {
		const unsigned char *name;
		size_t name_length;
		const unsigned char *start;
		unsigned char quote = 0;

		while (p < limit && IS_SPACE(*p)) p++;
		if (p >= limit || *p == '>')
			return p;
		name = p;
		while (p < limit && !IS_SPACE(*p) && *p != '=' && *p != '>') p++;
		name_length = (size_t)(p - name);
		while (p < limit && IS_SPACE(*p)) p++;
		if (p >= limit || *p != '=')
			continue;
		p++;
		while (p < limit && IS_SPACE(*p)) p++;
		if (p < limit && (*p == '\'' || *p == '"'))
			quote = *p++;
		start = p;
		if (quote) {
			/* Attribute values are most of the tag bytes. */
			while (p < limit && *p != quote)
				p++;
		} else {
			while (p < limit && !IS_SPACE(*p) && *p != '>') p++;
		}
#define TAKE_ATTRIBUTE(bit, literal, field, field_length) \
		if (!(found->have & (bit)) && \
		    NAME_IS(name, name_length, literal)) { \
			found->have |= (bit); \
			found->field = start; \
			found->field_length = (size_t)(p - start); \
		}
		switch (name_length) {
		case 2:
			TAKE_ATTRIBUTE(ATTR_ID, "id", id, id_length)
			break;
		case 3:
			TAKE_ATTRIBUTE(ATTR_SRC, "src", src, src_length)
			break;
		case 4:
			TAKE_ATTRIBUTE(ATTR_HREF, "href", href, href_length)
			break;
		case 5:
			TAKE_ATTRIBUTE(ATTR_CLASS, "class", class_value, class_length)
			else TAKE_ATTRIBUTE(ATTR_STYLE, "style", style, style_length)
			else TAKE_ATTRIBUTE(ATTR_WIDTH, "width", width, width_length)
			break;
		case 6:
			TAKE_ATTRIBUTE(ATTR_HEIGHT, "height", height, height_length)
			break;
		default:
			break;
		}
#undef TAKE_ATTRIBUTE
		if (quote && p < limit) p++;
	}
}

/* Record an element id so a same-page link can find its line later.  Parsoid
 * stamps every element with an id like "mwAQ"; those are never link targets
 * and would triple the record count, so they are left out. */
static HTML_RARE void put_anchor(TEXT_OUTPUT *out, const unsigned char *id,
		       size_t id_length)
{
	unsigned char record[3];
	size_t i;

	if (!id_length || id_length > 65535)
		return;
	if (id_length >= 3 && id[0] == 'm' && id[1] == 'w') {
		for (i = 2; i < id_length; i++)
			if (!((id[i] >= 'A' && id[i] <= 'Z') ||
			      (id[i] >= 'a' && id[i] <= 'z') ||
			      (id[i] >= '0' && id[i] <= '9')))
				break;
		if (i == id_length)
			return;
	}
	record[0] = ZIM_TEXT_ANCHOR_MARKER;
	record[1] = (unsigned char)id_length;
	record[2] = (unsigned char)(id_length >> 8);
	put_record_bytes(out, record, sizeof(record));
	put_record_bytes(out, id, id_length);
}

/* Elements whose content is navigation, editing chrome, or hidden, and so
 * has no place on a small offline screen. */
static HTML_RARE int class_word_skipped(const unsigned char *word, size_t length)
{
	/* Dispatch on the first letter: most class words are checked against
	 * nothing at all, and the rest against one or two candidates. */
	switch (ascii_lower(word[0])) {
	case 'n':
		return NAME_IS(word, length, "navbox") ||
			NAME_IS(word, length, "navbox-styles") ||
			NAME_IS(word, length, "noprint");
	case 'v':
		return NAME_IS(word, length, "vertical-navbox");
	case 'm':
		return NAME_IS(word, length, "mw-editsection") ||
			NAME_IS(word, length, "mw-jump-link") ||
			NAME_IS(word, length, "mw-hidden-catlinks") ||
			NAME_IS(word, length, "mw-indicators") ||
			NAME_IS(word, length, "mw-empty-elt");
	case 'p':
		return NAME_IS(word, length, "printfooter");
	case 'c':
		return NAME_IS(word, length, "catlinks");
	case 's':
		return NAME_IS(word, length, "sistersitebox");
	default:
		return 0;
	}
}

static int element_skipped(const TAG_ATTRIBUTES *found)
{
	const unsigned char *value;
	size_t value_length;
	size_t i;

	if (found->have & ATTR_CLASS) {
		size_t start = 0;

		value = found->class_value;
		value_length = found->class_length;
		for (i = 0; i <= value_length; i++) {
			if (i == value_length || ascii_space(value[i])) {
				if (i > start &&
				    class_word_skipped(value + start, i - start))
					return 1;
				start = i + 1;
			}
		}
	}
	if (found->have & ATTR_STYLE) {
		/* "display:none", with optional space after the colon */
		value = found->style;
		value_length = found->style_length;
		for (i = 0; i + 12 <= value_length; i++) {
			if (NAME_IS(value + i, 8, "display:")) {
				size_t j = i + 8;

				while (j < value_length && ascii_space(value[j]))
					j++;
				if (j + 4 <= value_length &&
				    NAME_IS(value + j, 4, "none"))
					return 1;
			}
		}
	}
	return 0;
}

/* Elements that never have a closing tag. */
static HTML_RARE int void_element(const unsigned char *name, size_t length)
{
	switch (length) {
	case 2:
		return NAME_IS(name, 2, "br") || NAME_IS(name, 2, "hr");
	case 3:
		return NAME_IS(name, 3, "img") || NAME_IS(name, 3, "wbr") ||
			NAME_IS(name, 3, "col");
	case 4:
		return NAME_IS(name, 4, "meta") || NAME_IS(name, 4, "link") ||
			NAME_IS(name, 4, "area") || NAME_IS(name, 4, "base");
	case 5:
		return NAME_IS(name, 5, "input") || NAME_IS(name, 5, "embed") ||
			NAME_IS(name, 5, "param") || NAME_IS(name, 5, "track");
	case 6:
		return NAME_IS(name, 6, "source");
	default:
		return 0;
	}
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

static HTML_RARE size_t decode_entity(const unsigned char *input, size_t length,
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
	if (NAME_IS(input, length, "amp")) bytes[0] = '&';
	else if (NAME_IS(input, length, "lt")) bytes[0] = '<';
	else if (NAME_IS(input, length, "gt")) bytes[0] = '>';
	else if (NAME_IS(input, length, "quot")) bytes[0] = '"';
	else if (NAME_IS(input, length, "apos")) bytes[0] = '\'';
	else if (NAME_IS(input, length, "nbsp")) bytes[0] = ' ';
	else if (NAME_IS(input, length, "ndash")) return encode_utf8(0x2013, bytes);
	else if (NAME_IS(input, length, "mdash")) return encode_utf8(0x2014, bytes);
	else if (NAME_IS(input, length, "hellip")) return encode_utf8(0x2026, bytes);
	else if (NAME_IS(input, length, "middot")) return encode_utf8(0x00b7, bytes);
	else return 0;
	return 1;
}

/* Every element name the converter reacts to.  One classification per tag
 * replaces a chain of up to twenty string comparisons. */
enum html_tag {
	TAG_OTHER = 0,
	TAG_BODY,
	TAG_MAIN,
	TAG_STYLE,
	TAG_SCRIPT,
	TAG_A,
	TAG_IMG,
	TAG_LI,
	TAG_BR,
	TAG_TD,
	TAG_TH,
	TAG_P,
	TAG_HEADING,
	TAG_BLOCK
};

/* Two lower-cased name bytes as one switch key. */
#define NAME2(a, b) (((unsigned)(a) << 8) | (unsigned)(b))

static enum html_tag classify_tag(const unsigned char *name, size_t length)
{
	unsigned char first = ascii_lower(name[0]);

	switch (length) {
	case 1:
		if (first == 'a') return TAG_A;
		if (first == 'p') return TAG_P;
		return TAG_OTHER;
	case 2:
		if (first == 'h' && name[1] >= '1' && name[1] <= '6')
			return TAG_HEADING;
		switch (NAME2(first, ascii_lower(name[1]))) {
		case NAME2('l', 'i'): return TAG_LI;
		case NAME2('b', 'r'): return TAG_BR;
		case NAME2('t', 'd'): return TAG_TD;
		case NAME2('t', 'h'): return TAG_TH;
		case NAME2('t', 'r'):
		case NAME2('u', 'l'):
		case NAME2('o', 'l'):
		case NAME2('d', 'l'):
		case NAME2('d', 'd'):
		case NAME2('d', 't'):
			return TAG_BLOCK;
		default:
			return TAG_OTHER;
		}
	case 3:
		if (first == 'i' && NAME_IS(name, length, "img")) return TAG_IMG;
		if (first == 'd' && NAME_IS(name, length, "div")) return TAG_BLOCK;
		return TAG_OTHER;
	case 4:
		if (first == 'b' && NAME_IS(name, length, "body")) return TAG_BODY;
		if (first == 'm' && NAME_IS(name, length, "main")) return TAG_MAIN;
		return TAG_OTHER;
	case 5:
		if (NAME_IS(name, length, "style")) return TAG_STYLE;
		if (NAME_IS(name, length, "table")) return TAG_BLOCK;
		return TAG_OTHER;
	case 6:
		if (NAME_IS(name, length, "script")) return TAG_SCRIPT;
		return TAG_OTHER;
	case 7:
		if (NAME_IS(name, length, "section")) return TAG_BLOCK;
		return TAG_OTHER;
	case 10:
		if (NAME_IS(name, length, "blockquote")) return TAG_BLOCK;
		return TAG_OTHER;
	default:
		return TAG_OTHER;
	}
}

#define HTML_PROGRESS_STEPS 32
#define HTML_PROGRESS_MIN_STEP 4096

static int ZIM_OVERLAY_SECTION("ovlhtml")
html_to_text(const unsigned char *html, size_t html_size,
	     unsigned char *text, size_t capacity,
	     size_t *text_size, int include_images, int include_links,
	     ZIM_HTML_PROGRESS progress, void *progress_opaque)
{
	TEXT_OUTPUT out;
	size_t i = 0;
	size_t progress_step = html_size / HTML_PROGRESS_STEPS;
	size_t next_report;
	int in_body = 0;
	int in_main = 0;
	int suppress = 0;
	int skip_depth = 0;
	int in_link = 0;

	if (!html || (!text && capacity) || !text_size)
		return ZIM_ERR_RANGE;
	out.text = text;
	out.capacity = capacity;
	out.used = 0;
	out.last = 0;
	out.previous = 0;
	out.pending_space = 0;
	if (progress_step < HTML_PROGRESS_MIN_STEP)
		progress_step = HTML_PROGRESS_MIN_STEP;
	next_report = progress_step;
	while (i < html_size) {
		if (progress && i >= next_report) {
			progress(progress_opaque, i, html_size);
			next_report = i + progress_step;
		}
		if (html[i] == '<') {
			size_t tag_start;
			size_t tag_end;
			size_t attributes_start;
			size_t attributes_end;
			enum html_tag tag;
			int closing = 0;
			TAG_ATTRIBUTES found;

			found.have = 0;

			if (i + 3 < html_size && html[i + 1] == '!' &&
			    html[i + 2] == '-' && html[i + 3] == '-') {
				size_t end = i + 4;
				while (end + 2 < html_size &&
				       !(html[end] == '-' && html[end + 1] == '-' &&
					 html[end + 2] == '>'))
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
			tag = tag_start == tag_end ? TAG_OTHER :
				classify_tag(html + tag_start, tag_end - tag_start);
			if (tag_start != tag_end && !closing && in_body && in_main &&
			    !suppress && !skip_depth && tag != TAG_BODY &&
			    tag != TAG_MAIN && tag != TAG_STYLE && tag != TAG_SCRIPT) {
				/* An element whose attributes matter: parse them on
				 * the way to the '>'. */
				i = (size_t)(scan_tag_attributes(html + i, html + html_size,
								 &found) - html);
			} else {
				/* Attribute bytes are most of a Kiwix page; scan them
				 * with pointers so the loop stays a few instructions. */
				const unsigned char *scan = html + i;
				const unsigned char *limit = html + html_size;

				while (scan < limit && *scan != '>')
					scan++;
				i = (size_t)(scan - html);
			}
			attributes_end = i;
			if (i < html_size) i++;
			if (tag_start == tag_end)
				continue;
			if (tag == TAG_BODY) {
				in_body = !closing;
				continue;
			}
			if (!in_body)
				continue;
			if (tag == TAG_MAIN) {
				if (closing && in_main)
					break;
				in_main = !closing;
				continue;
			}
			if (!in_main)
				continue;
			if (tag == TAG_STYLE || tag == TAG_SCRIPT) {
				if (closing && suppress)
					suppress--;
				else if (!closing)
					suppress++;
				continue;
			}
			if (suppress)
				continue;
			/* Skipped elements swallow everything until their own
			 * closing tag, counting nested elements on the way. */
			if (skip_depth) {
				int self_closing = attributes_end > attributes_start &&
					html[attributes_end - 1] == '/';

				if (closing)
					skip_depth--;
				else if (!self_closing &&
					 !void_element(html + tag_start, tag_end - tag_start))
					skip_depth++;
				continue;
			}
			if (!closing && attributes_end > attributes_start) {
				/* found was filled on the way to the '>' above */
				if (element_skipped(&found)) {
					int self_closing =
						html[attributes_end - 1] == '/';

					if (!self_closing &&
					    !void_element(html + tag_start,
							  tag_end - tag_start))
						skip_depth = 1;
					continue;
				}
				if (include_links && (found.have & ATTR_ID))
					put_anchor(&out, found.id, found.id_length);
			}
			switch (tag) {
			case TAG_A:
				if (!include_links)
					break;
				if (closing) {
					if (in_link) {
						unsigned char marker = ZIM_TEXT_LINK_END_MARKER;
						put_record_bytes(&out, &marker, 1);
					}
					in_link = 0;
				} else {
					if (in_link) {
						unsigned char marker = ZIM_TEXT_LINK_END_MARKER;
						put_record_bytes(&out, &marker, 1);
					}
					in_link = put_link_start(&out, &found);
				}
				break;
			case TAG_IMG:
				if (include_images && !closing)
					put_image(&out, &found);
				break;
			case TAG_LI:
				if (!closing) {
					put_newline(&out, 0);
					put_byte(&out, '*');
					put_byte(&out, ' ');
				}
				break;
			case TAG_BR:
				put_newline(&out, 0);
				break;
			case TAG_TD:
			case TAG_TH:
				if (!closing) {
					put_space_if_needed(&out);
					if (out.used && out.last != '\n') {
						put_byte(&out, '|');
						put_byte(&out, ' ');
					}
				}
				break;
			case TAG_P:
			case TAG_HEADING:
				put_newline(&out, closing);
				break;
			case TAG_BLOCK:
				put_newline(&out, 0);
				break;
			default:
				break;
			}
			continue;
		}
		if (!in_body || !in_main || suppress || skip_depth) {
			/* Nothing outside <main> or inside style/script is emitted;
			 * jump straight to the next tag. */
			const unsigned char *scan = html + i + 1;
			const unsigned char *limit = html + html_size;

			while (scan < limit && *scan != '<')
				scan++;
			i = (size_t)(scan - html);
			continue;
		}
		if (html[i] == '&') {
			size_t end = i + 1;
			unsigned char bytes[4];
			size_t count;
			while (end < html_size && end - i <= 16 && html[end] != ';' &&
			       !IS_SPACE(html[end]) && html[end] != '<')
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
		{
			size_t start = i;
			/* A run of plain text ends at a tag or entity start, at
			 * whitespace, or at a control byte; see put_newline for
			 * why spaces never join a run.  Three compares per byte
			 * against registers beat a class table in SDRAM. */
			const unsigned char *scan = html + i;
			const unsigned char *limit = html + html_size;

			while (scan < limit) {
				unsigned char c = *scan;
				if (c <= ' ' || c == '<' || c == '&')
					break;
				scan++;
			}
			i = (size_t)(scan - html);
			if (i > start) {
				put_space_if_needed(&out);
				put_text(&out, html + start, i - start);
				continue;
			}
		}
		/* Whitespace, a control byte, or an ampersand that did not
		 * decode as an entity. */
		if (IS_SPACE(html[i]))
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
	ZIM_OVERLAY_ENSURE(ovlhtml);
	return html_to_text(html, html_size, text, capacity, text_size, 0, 0,
			    NULL, NULL);
}

int zim_html_to_text_images(const unsigned char *html, size_t html_size,
			    unsigned char *text, size_t capacity,
			    size_t *text_size)
{
	ZIM_OVERLAY_ENSURE(ovlhtml);
	return html_to_text(html, html_size, text, capacity, text_size, 1, 1,
			    NULL, NULL);
}

int zim_html_to_text_images_progress(const unsigned char *html,
				     size_t html_size,
				     unsigned char *text, size_t capacity,
				     size_t *text_size,
				     ZIM_HTML_PROGRESS progress,
				     void *progress_opaque)
{
	ZIM_OVERLAY_ENSURE(ovlhtml);
	return html_to_text(html, html_size, text, capacity, text_size, 1, 1,
			    progress, progress_opaque);
}
