/* Convert normalized ZIM text to the WikiReader's pre-wrapped article stream. */
#include "zim_article.h"

#include <stdlib.h>
#include <string.h>

#include "lcd_buf_draw.h"
#include "zim_html.h"
#include "zim_overlay.h"

#if defined(__c33__)
void *zim_alloc_other_bank(size_t size, const void *avoid);
#else
#define zim_alloc_other_bank(size, avoid) malloc(size)
#endif

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
	unsigned char *destination;
	size_t k;

	if (count > capacity - *used)
		return -1;
	/* Words average a handful of bytes; a call into the library memcpy
	 * costs more than copying them here. */
	destination = output + *used;
	for (k = 0; k < count; k++)
		destination[k] = bytes[k];
	*used += count;
	return 0;
}

/* Bytes that end a word: space, newline, and the three record markers.  The
 * class value also tells the main loop which record follows. */
enum {
	CLASS_WORD = 0,
	CLASS_SPACE,
	CLASS_NEWLINE,
	CLASS_LINK_START,
	CLASS_LINK_END,
	CLASS_IMAGE,
	CLASS_ANCHOR
};
static unsigned char word_break_classes[256];
static int word_break_ready;

static void word_break_init(void)
{
	if (word_break_ready)
		return;
	word_break_classes[' '] = CLASS_SPACE;
	word_break_classes['\n'] = CLASS_NEWLINE;
	word_break_classes[ZIM_TEXT_LINK_START_MARKER] = CLASS_LINK_START;
	word_break_classes[ZIM_TEXT_LINK_END_MARKER] = CLASS_LINK_END;
	word_break_classes[ZIM_TEXT_IMAGE_MARKER] = CLASS_IMAGE;
	word_break_classes[ZIM_TEXT_ANCHOR_MARKER] = CLASS_ANCHOR;
	word_break_ready = 1;
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

/* Nearly every measured byte is ASCII, and measuring one through the generic
 * UTF-8 decode and glyph-record lookup costs about sixty instructions.  Font
 * metrics never change once loaded, so memoize the 7-bit widths per font. */
static signed char ascii_width_cache[FONT_COUNT][128];
static unsigned char ascii_width_ready[FONT_COUNT];

static const signed char *ascii_widths(int font)
{
	signed char *widths = ascii_width_cache[font - 1];
	unsigned int c;

	if (!ascii_width_ready[font - 1]) {
		for (c = 0; c < 128; c++) {
			unsigned char byte = (unsigned char)c;
			size_t size;
			int width = next_width(font, &byte, 1, &size);
			widths[c] = (signed char)(width > 127 ? 127 : width);
		}
		ascii_width_ready[font - 1] = 1;
	}
	return widths;
}

/* Mirror of the renderer's vertical bookkeeping, advanced as the stream is
 * emitted so the article height is known without a second pass over it.
 * Must stay identical to measure_newline() below. */
typedef struct {
	int y;
	int line_height;
	int actual_height;
} HEIGHT_TRACK;

static void track_newline(HEIGHT_TRACK *track, int new_line_height)
{
	track->y += track->actual_height;
	if (new_line_height >= 0)
		track->line_height = new_line_height;
	track->actual_height = track->line_height;
	if (track->y + track->line_height >= LCD_BUF_HEIGHT_PIXELS)
		track->y = LCD_BUF_HEIGHT_PIXELS - track->line_height - 1;
}

static int emit_newline(unsigned char *output, size_t capacity, size_t *used,
			int font, int explicit_font, HEIGHT_TRACK *track)
{
	int line_space = pcfFonts[font - 1].Fmetrics.linespace + LINE_SPACE_ADDON;
	if (line_space > 31)
		line_space = 31;
	if (explicit_font) {
		track_newline(track, line_space);
		return append_byte(output, capacity, used,
				   ESC_3_NEW_LINE_WITH_FONT) ||
			append_byte(output, capacity, used,
				    (unsigned char)((line_space << 3) | font));
	}
	track_newline(track, -1);
	return append_byte(output, capacity, used, ESC_2_NEW_LINE_SAME_FONT);
}

static int word_width(int font, const signed char *ascii,
		      const unsigned char *word, size_t length)
{
	const unsigned char *p = word;
	const unsigned char *end = word + length;
	int width = 0;

	while (p < end) {
		unsigned char c = *p;

		if (c < 0x80) {
			width += ascii[c];
			p++;
		} else {
			size_t character_size;

			width += next_width(font, p, (size_t)(end - p),
					    &character_size);
			p += character_size;
		}
	}
	return width;
}

static int finish_link_segment(unsigned char *article, size_t capacity,
			       size_t *used, ARTICLE_LINK *links,
			       size_t *link_count, uint32_t link_id,
			       int *segment_x, int x, int y, int line_height)
{
	ARTICLE_LINK *record;
	int width;

	if (*segment_x < 0 || !link_id)
		return 0;
	width = x - *segment_x;
	if (width <= 0) {
		*segment_x = -1;
		return 0;
	}
	if (*link_count >= MAX_ARTICLE_LINKS) {
		*segment_x = -1;
		return 0;
	}
	if (width > 255 ||
	    append_byte(article, capacity, used, ESC_10_HORIZONTAL_LINE) ||
	    append_byte(article, capacity, used, (unsigned char)width))
		return -1;
	record = &links[(*link_count)++];
	record->start_xy = (uint32_t)*segment_x | (uint32_t)y << 8;
	record->end_xy = (uint32_t)(x - 1) |
		(uint32_t)(y + line_height - 1) << 8;
	record->article_id = link_id;
	*segment_x = -1;
	return 0;
}

#define WRAP_PROGRESS_STEPS 24
#define WRAP_PROGRESS_MIN_STEP 4096

int zim_text_to_article_images_links(const unsigned char *text,
				     size_t text_size,
				     unsigned char *article, size_t capacity,
				     size_t *article_size,
				     ZIM_ARTICLE_IMAGE image,
				     void *image_opaque,
				     ZIM_ARTICLE_LINK link,
				     void *link_opaque,
				     ZIM_ARTICLE_ANCHOR anchor,
				     void *anchor_opaque,
				     int *stream_height)
{
	return zim_text_to_article_images_links_progress(text, text_size,
			article, capacity, article_size, image, image_opaque,
			link, link_opaque, anchor, anchor_opaque, stream_height,
			NULL, NULL);
}

/* The wrapper runs from the IVRAM overlay (zim_overlay.h); this is the
 * body, and the public function below copies it in and calls it. */
static int ZIM_OVERLAY_SECTION("ovlwrap")
wrap_body(const unsigned char *text, size_t text_size,
	  unsigned char *article, size_t capacity, size_t *article_size,
	  ZIM_ARTICLE_IMAGE image, void *image_opaque,
	  ZIM_ARTICLE_LINK link, void *link_opaque,
	  ZIM_ARTICLE_ANCHOR anchor, void *anchor_opaque,
	  int *stream_height,
	  ZIM_ARTICLE_PROGRESS progress, void *progress_opaque);

int zim_text_to_article_images_links_progress(const unsigned char *text,
					      size_t text_size,
					      unsigned char *article,
					      size_t capacity,
					      size_t *article_size,
					      ZIM_ARTICLE_IMAGE image,
					      void *image_opaque,
					      ZIM_ARTICLE_LINK link,
					      void *link_opaque,
					      ZIM_ARTICLE_ANCHOR anchor,
					      void *anchor_opaque,
					      int *stream_height,
					      ZIM_ARTICLE_PROGRESS progress,
					      void *progress_opaque)
{
	ZIM_OVERLAY_ENSURE(ovlwrap);
	return wrap_body(text, text_size, article, capacity, article_size,
			 image, image_opaque, link, link_opaque, anchor,
			 anchor_opaque, stream_height, progress, progress_opaque);
}

static int ZIM_OVERLAY_SECTION("ovlwrap")
wrap_body(const unsigned char *text, size_t text_size,
	  unsigned char *article, size_t capacity, size_t *article_size,
	  ZIM_ARTICLE_IMAGE image, void *image_opaque,
	  ZIM_ARTICLE_LINK link, void *link_opaque,
	  ZIM_ARTICLE_ANCHOR anchor, void *anchor_opaque,
	  int *stream_height,
	  ZIM_ARTICLE_PROGRESS progress, void *progress_opaque)
{
	ARTICLE_HEADER header;
	size_t progress_step = text_size / WRAP_PROGRESS_STEPS;
	size_t next_report;
	ARTICLE_LINK *links;
	HEIGHT_TRACK track = { 0, 0, 0 };
	const signed char *ascii;
#if defined(__c33__)
	/* Copies of the font's ASCII widths and of the word-break classes in
	 * A0 RAM (zim_fast_scratch): a cycle a load, and no SDRAM row for
	 * measuring a word or classifying its end.  On the stack they were in
	 * SDRAM, and the frame straddled two rows; in the DSTRAM below the
	 * decoder's stack they were overwritten during the wrap. */
	signed char *const ascii_local = (signed char *)zim_fast_scratch;
	unsigned char *const word_break = zim_fast_scratch + 128;
#else
	signed char ascii_local[128];
	unsigned char word_break[256];
#endif
	size_t input = 0;
	size_t used = sizeof(header);
	size_t link_count = 0;
	int font = TITLE_FONT_IDX;
	int x = 0;
	int y = 0;
	int line_height;
	int actual_height;
	int first_line = 1;
	uint32_t link_id = 0;
	int segment_x = -1;
	size_t committed;
	size_t committed_links;
	HEIGHT_TRACK committed_track = { 0, 0, 0 };
	int rc;

/* A link segment is only open while segment_x >= 0, so the common case
 * skips the call entirely. */
#define FINISH_LINK_SEGMENT() \
	(segment_x >= 0 && finish_link_segment(article, capacity, &used, links, \
					       &link_count, link_id, &segment_x, \
					       x, y, line_height))
/* Room kept back from the loop for the terminator and a truncation note. */
#define TRUNCATION_RESERVE 32
#define TRUNCATION_MARK "(article truncated)"

	if (!text || !article || !article_size ||
	    capacity < sizeof(header) + TRUNCATION_RESERVE + 8)
		return -1;
	capacity -= TRUNCATION_RESERVE;
	word_break_init();
	memcpy(word_break, word_break_classes, 256);
	/* The link table is moved into the article at the end, word by word
	 * between the two; from another bank that is one row each. */
	links = zim_alloc_other_bank(MAX_ARTICLE_LINKS * sizeof(*links), article);
	if (!links)
		return -1;
	memset(&header, 0, sizeof(header));
	header.offset_article = sizeof(header);
	memcpy(article, &header, sizeof(header));
	rc = emit_newline(article, capacity, &used, font, 1, &track);
	if (rc)
		goto error;
	line_height = pcfFonts[font - 1].Fmetrics.linespace + LINE_SPACE_ADDON;
	actual_height = line_height;
	memcpy(ascii_local, ascii_widths(font), 128);
	ascii = ascii_local;
	if (progress_step < WRAP_PROGRESS_MIN_STEP)
		progress_step = WRAP_PROGRESS_MIN_STEP;
	next_report = progress_step;

	while (input < text_size) {
		const unsigned char *scan;
		const unsigned char *limit;
		size_t word_start;
		size_t word_length;
		int width;
		int space_width = 0;
		int had_space = 0;
		unsigned char class = word_break[text[input]];

		committed = used;
		committed_links = link_count;
		committed_track = track;
		if (progress && input >= next_report) {
			progress(progress_opaque, input, text_size);
			next_report = input + progress_step;
		}
		if (class == CLASS_LINK_START) {
			size_t path_length;

			if (FINISH_LINK_SEGMENT())
				goto truncate;
			if (text_size - input < 3)
				goto error;
			path_length = text[input + 1] |
				(size_t)text[input + 2] << 8;
			if (path_length > text_size - input - 3)
				goto error;
			link_id = link ? link(link_opaque, text + input + 3,
					      path_length) : 0;
			input += 3 + path_length;
			continue;
		}
		if (class == CLASS_LINK_END) {
			if (FINISH_LINK_SEGMENT())
				goto truncate;
			link_id = 0;
			input++;
			continue;
		}
		if (class == CLASS_ANCHOR) {
			size_t id_length;

			if (text_size - input < 3)
				goto error;
			id_length = text[input + 1] | (size_t)text[input + 2] << 8;
			if (id_length > text_size - input - 3)
				goto error;
			if (anchor)
				anchor(anchor_opaque, text + input + 3, id_length,
				       x ? y + actual_height : y);
			input += 3 + id_length;
			continue;
		}

		if (class == CLASS_IMAGE) {
			unsigned int requested_width;
			unsigned int requested_height;
			size_t path_length;
			size_t record_length;

			if (text_size - input < 7)
				goto error;
			requested_width = text[input + 1] |
				(unsigned int)text[input + 2] << 8;
			requested_height = text[input + 3] |
				(unsigned int)text[input + 4] << 8;
			path_length = text[input + 5] |
				(size_t)text[input + 6] << 8;
			record_length = 7 + path_length;
			if (record_length > text_size - input)
				goto error;
			if (image && capacity - used >= 6) {
				uint8_t image_width;
				uint16_t image_height;
				size_t bitmap_size;

				if (x) {
					if (FINISH_LINK_SEGMENT() ||
					    emit_newline(article, capacity, &used,
							 font, 0, &track))
						goto truncate;
					y += actual_height;
					actual_height = line_height;
				}
				x = 0;
				if (!image(image_opaque, text + input + 7,
					   path_length, requested_width,
					   requested_height, article + used + 4,
					   capacity - used - 6, &image_width,
					   &image_height, &bitmap_size)) {
					if (bitmap_size > capacity - used - 6)
						goto truncate;
					article[used++] = ESC_14_BITMAP;
					article[used++] = image_width;
					article[used++] = (unsigned char)image_height;
					article[used++] = (unsigned char)(image_height >> 8);
					used += bitmap_size;
					if (line_height < (int)image_height + 1)
						actual_height = (int)image_height + 3;
					if (track.line_height < (int)image_height + 1)
						track.actual_height = (int)image_height + 3;
					if (emit_newline(article, capacity, &used,
							 font, 0, &track))
						goto truncate;
					y += actual_height;
					actual_height = line_height;
				}
			}
			input += record_length;
			continue;
		}

		if (class == CLASS_NEWLINE) {
			if (FINISH_LINK_SEGMENT())
				goto truncate;
			input++;
			y += actual_height;
			if (first_line) {
				font = DEFAULT_FONT_IDX;
				first_line = 0;
				rc = emit_newline(article, capacity, &used, font, 1,
						  &track);
				line_height = pcfFonts[font - 1].Fmetrics.linespace +
					LINE_SPACE_ADDON;
				memcpy(ascii_local, ascii_widths(font), 128);
			} else {
				rc = emit_newline(article, capacity, &used, font, 0,
						  &track);
			}
			if (rc)
				goto truncate;
			actual_height = line_height;
			x = 0;
			continue;
		}
		if (class == CLASS_SPACE) {
			had_space = 1;
			do
				input++;
			while (input < text_size && text[input] == ' ');
			if (input >= text_size || word_break[text[input]])
				continue;
		}
		word_start = input;
		scan = text + input;
		limit = text + text_size;
		/* Every word-break byte is below 0x21, so a word's bytes settle
		 * with one compare and the table is only read at the break. */
		while (scan < limit && (*scan > ' ' || !word_break[*scan]))
			scan++;
		input = (size_t)(scan - text);
		word_length = input - word_start;
		width = word_width(font, ascii, text + word_start, word_length);
		if (x && had_space)
			space_width = ascii[' '];
		if (x && x + space_width + width > ARTICLE_TEXT_WIDTH) {
			if (FINISH_LINK_SEGMENT() ||
			    emit_newline(article, capacity, &used, font, 0, &track))
				goto truncate;
			y += actual_height;
			actual_height = line_height;
			x = 0;
			space_width = 0;
		}
		if (space_width) {
			if (link_id && segment_x < 0)
				segment_x = x;
			if (append_byte(article, capacity, &used, ' '))
				goto truncate;
			x += space_width;
		}
		/* Most words fit intact after the line-break decision above.  Their
		 * width is already known, so copy the word once instead of measuring
		 * every character a second time. */
		if (width <= ARTICLE_TEXT_WIDTH - x) {
			if (link_id && segment_x < 0)
				segment_x = x;
			if (append_bytes(article, capacity, &used,
					 text + word_start, word_length))
				goto truncate;
			x += width;
			continue;
		}
		while (word_length) {
			size_t character_size;
			int character_width = next_width(font, text + word_start,
							 word_length,
							 &character_size);
			if (x && x + character_width > ARTICLE_TEXT_WIDTH) {
				if (FINISH_LINK_SEGMENT() ||
				    emit_newline(article, capacity, &used,
						 font, 0, &track))
					goto truncate;
				y += actual_height;
				actual_height = line_height;
				x = 0;
			}
			if (link_id && segment_x < 0)
				segment_x = x;
			if (append_bytes(article, capacity, &used, text + word_start,
					 character_size))
				goto truncate;
			x += character_width;
			word_start += character_size;
			word_length -= character_size;
		}
	}
	committed = used;
	committed_links = link_count;
	committed_track = track;
	if (FINISH_LINK_SEGMENT())
		goto truncate;
	capacity += TRUNCATION_RESERVE;
	goto finish;

truncate:
	/* Out of room: keep the stream up to the last complete token and say
	 * so, in the space reserved before the loop. */
	used = committed;
	link_count = committed_links;
	track = committed_track;
	segment_x = -1;
	capacity += TRUNCATION_RESERVE;
	if (!emit_newline(article, capacity, &used, font, 0, &track))
		append_bytes(article, capacity, &used,
			     (const unsigned char *)TRUNCATION_MARK,
			     sizeof(TRUNCATION_MARK) - 1);

finish:
	if (append_byte(article, capacity, &used, '\0'))
		goto error;
	while (link_count && link_count * sizeof(*links) > capacity - used)
		link_count--;
	memmove(article + sizeof(header) + link_count * sizeof(*links),
		article + sizeof(header), used - sizeof(header));
	header.article_link_count = (uint16_t)link_count;
	header.offset_article = (uint32_t)(sizeof(header) +
		link_count * sizeof(*links));
	memcpy(article, &header, sizeof(header));
	if (link_count)
		memcpy(article + sizeof(header), links,
		       link_count * sizeof(*links));
	used += link_count * sizeof(*links);
	*article_size = used;
	if (stream_height)
		*stream_height = track.y;
	free(links);
	return 0;

error:
	free(links);
	return -1;
#undef FINISH_LINK_SEGMENT
#undef TRUNCATION_RESERVE
#undef TRUNCATION_MARK
}

int zim_text_to_article_images(const unsigned char *text, size_t text_size,
			       unsigned char *article, size_t capacity,
			       size_t *article_size,
			       ZIM_ARTICLE_IMAGE image, void *image_opaque)
{
	return zim_text_to_article_images_links(text, text_size, article,
		capacity, article_size, image, image_opaque, NULL, NULL, NULL,
		NULL, NULL);
}

int zim_text_to_article(const unsigned char *text, size_t text_size,
			unsigned char *article, size_t capacity,
			size_t *article_size)
{
	return zim_text_to_article_images(text, text_size, article, capacity,
					  article_size, NULL, NULL);
}

static void measure_newline(int *y, int *line_height, int *actual_height,
			    int new_line_height)
{
	*y += *actual_height;
	if (new_line_height >= 0)
		*line_height = new_line_height;
	*actual_height = *line_height;
	if (*y + *line_height >= LCD_BUF_HEIGHT_PIXELS)
		*y = LCD_BUF_HEIGHT_PIXELS - *line_height - 1;
}

int zim_article_stream_height(const unsigned char *article,
			      size_t article_size)
{
	ARTICLE_HEADER header;
	size_t offset;
	int y = 0;
	int line_height = 0;
	int actual_height = 0;

	if (!article || article_size < sizeof(header))
		return -1;
	memcpy(&header, article, sizeof(header));
	if (header.offset_article < sizeof(header) ||
	    header.offset_article >= article_size)
		return -1;
	offset = header.offset_article;
	while (offset < article_size) {
		unsigned int code = article[offset++];
		unsigned int value;

		if (!code)
			return y;
		if (code > MAX_ESC_CHAR)
			continue;
		switch (code) {
		case ESC_0_SPACE_LINE:
			if (offset >= article_size) return -1;
			measure_newline(&y, &line_height, &actual_height,
					article[offset++]);
			break;
		case ESC_1_NEW_LINE_DEFAULT_FONT:
			measure_newline(&y, &line_height, &actual_height,
				pcfFonts[DEFAULT_FONT_IDX - 1].Fmetrics.linespace +
				LINE_SPACE_ADDON);
			break;
		case ESC_2_NEW_LINE_SAME_FONT:
			measure_newline(&y, &line_height, &actual_height, -1);
			break;
		case ESC_3_NEW_LINE_WITH_FONT:
			if (offset >= article_size) return -1;
			value = article[offset++];
			measure_newline(&y, &line_height, &actual_height,
					(int)(value >> 3));
			break;
		case ESC_4_CHANGE_FONT:
		case ESC_7_FORWARD:
		case ESC_8_BACKWARD:
		case ESC_9_Y_ADJUSTMENT:
		case ESC_10_HORIZONTAL_LINE:
		case ESC_11_VERTICAL_LINE:
			if (offset >= article_size) return -1;
			offset++;
			break;
		case ESC_12_FULL_HORIZONTAL_LINE:
			y += actual_height;
			line_height = 1;
			actual_height = 1;
			break;
		case ESC_14_BITMAP: {
			unsigned int width;
			unsigned int height;
			size_t bytes;

			if (article_size - offset < 3) return -1;
			width = article[offset++];
			height = article[offset++];
			height |= (unsigned int)article[offset++] << 8;
			bytes = (size_t)((width + 7) / 8) * height;
			if (bytes > article_size - offset) return -1;
			offset += bytes;
			if (line_height < (int)height + 1)
				actual_height = (int)height + 3;
			break;
		}
		default:
			break;
		}
	}
	return -1;
}
