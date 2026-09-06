/* ZIM-backed search and retrieval for the WikiReader user interface. */
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <grifo.h>

#include "guilib.h"
#include "glyph.h"
#include "bigram.h"
#include "history.h"
#include "keyboard.h"
#include "lcd_buf_draw.h"
#include "search.h"
#include "ustring.h"
#include "wiki_info.h"
#include "wikilib.h"
#include "zim_archive.h"
#include "zim_article.h"
#include "zim_bench.h"

void *zim_alloc_in_bank(size_t size, unsigned bank);
#include "zim_blob.h"
#include "zim_catalog.h"
#include "zim_file.h"
#include "zim_html.h"
#include "zim_image.h"
#include "zim_link.h"
#include "zim_overlay.h"

#define ZIM_RAW_BUFFER_SIZE FILE_BUFFER_SIZE
#define ZIM_MIN_IMAGE_WIDTH 80
#define ZIM_MIN_IMAGE_HEIGHT 40
#define ZIM_MIN_IMAGE_STREAM_SIZE 5 /* four-byte header plus one bitmap byte */
#define ZIM_MAX_DEFERRED_IMAGES (FILE_BUFFER_SIZE / ZIM_MIN_IMAGE_STREAM_SIZE)
#define ZIM_INITIAL_DEFERRED_IMAGES 8
/* Bar segments, proportioned to where a big article's time goes: the
 * cluster decode dominates, converting the HTML is next, wrapping is short. */
#define ARTICLE_PROGRESS_LIMIT 100
#define ARTICLE_PROGRESS_BLOB_START 3
#define ARTICLE_PROGRESS_BLOB_END 70
#define ARTICLE_PROGRESS_HTML_END 90
#define ARTICLE_PROGRESS_WRAP_END 99
#define IMAGE_PROGRESS_BLOB_START 5
#define IMAGE_PROGRESS_BLOB_END 30
#define IMAGE_PROGRESS_DECODE_END 99
/* Unresolved links carry their deferred-table ordinal under a top nibble
 * no article id can have: ids keep the sign bit clear (lcd_buf_draw.h). */
#define ZIM_DEFERRED_LINK_TAG 0x80000000U
#define ZIM_DEFERRED_LINK_MASK 0xf0000000U
#define ZIM_DEFERRED_LINK_INDEX_MASK 0x0fffffffU
typedef struct {
	unsigned char title[NUMBER_OF_FIRST_PAGE_RESULTS][MAX_TITLE_ACTUAL];
	uint32_t article[NUMBER_OF_FIRST_PAGE_RESULTS];
	uint32_t count;
	int selected;
} ZIM_RESULTS;

static ZIM_ARCHIVE archive;
static ZIM_FILE archive_file;
static int archive_wiki = -1;
static ZIM_RESULTS results;
static unsigned char search_string[MAX_TITLE_SEARCH];
static int search_length;
static unsigned char *raw_buffer;
static unsigned char *text_buffer;
static uint32_t search_render_buffer[LCD_BUFFER_SIZE_WORDS];

typedef struct {
	unsigned char *stream;
	const unsigned char *path;
	size_t path_length;
	uint8_t width;
	uint16_t height;
	size_t bitmap_size;
} ZIM_DEFERRED_IMAGE;

typedef struct {
	const unsigned char *path;
	size_t path_length;
} ZIM_DEFERRED_LINK;

static ZIM_DEFERRED_IMAGE *deferred_images;
static size_t deferred_image_count;
static size_t deferred_image_capacity;
static size_t deferred_image_next;
static ZIM_IMAGE_DECODER *deferred_image_decoder;
static uint8_t deferred_decode_width;
static uint16_t deferred_decode_height;
static size_t deferred_decode_bitmap_size;
static unsigned char deferred_saved_bar[2 * LCD_BUFFER_WIDTH_BYTES];
static unsigned char *deferred_progress_framebuffer;
static size_t article_text_size;
static ZIM_DEFERRED_LINK *deferred_links;
static size_t deferred_link_count;
static size_t deferred_link_capacity;

/* Same-page link targets: element ids recorded by the wrapper together with
 * the stream position of the line they start. */
typedef struct {
	const unsigned char *id;
	size_t id_length;
	int y;
} ZIM_DEFERRED_ANCHOR;

#define ZIM_MAX_DEFERRED_ANCHORS 8192 /* Donald Trump has 2,143 ids */
static ZIM_DEFERRED_ANCHOR *deferred_anchors;
static size_t deferred_anchor_count;
static size_t deferred_anchor_capacity;
/* Fragment of a "Path#fragment" link being followed, applied once the
 * target article's anchors exist. */
static unsigned char pending_fragment[128];
static size_t pending_fragment_length;
extern int lcd_draw_cur_y_pos;
extern int article_start_y_pos;
static char current_article_path[ZIM_DIRENT_TEXT_MAX];

/*
 * Finished-article cache.  Leaving an article snapshots everything a revisit
 * needs: the wrapped stream (with whatever images have been decoded into
 * it), the normalized text the link and image paths point into, both
 * tables with their pointers turned into offsets, the path, and the height.
 * Coming back through history then costs a copy instead of a cluster decode,
 * conversion, wrap, and image decode.
 */
#define ARTICLE_CACHE_ENTRIES 4
#define ARTICLE_CACHE_BYTES (5u << 19) /* 2.5 MiB across all entries */

typedef struct {
	uint32_t stream_offset;
	uint32_t path_offset;
	uint32_t path_length;
	uint32_t bitmap_size;
	uint16_t height;
	uint8_t width;
} CACHED_IMAGE;

typedef struct {
	uint32_t path_offset;
	uint32_t path_length;
} CACHED_LINK;

typedef struct {
	uint32_t id_offset;
	uint32_t id_length;
	int32_t y;
} CACHED_ANCHOR;

typedef struct {
	uint32_t index;          /* article index as passed to retrieve_article; 0 = empty */
	unsigned int age;        /* larger is more recent */
	unsigned char *data;     /* stream, text, images, links */
	size_t bytes;
	size_t stream_size;
	size_t text_size;
	size_t image_count;
	size_t image_next;
	size_t link_count;
	size_t anchor_count;
	int height;
	char path[ZIM_DIRENT_TEXT_MAX];
} ARTICLE_CACHE_ENTRY;

static ARTICLE_CACHE_ENTRY article_cache[ARTICLE_CACHE_ENTRIES];
static unsigned int article_cache_clock;
static size_t article_cache_bytes;
/* The article currently in file_buffer, if retrieve_article completed it. */
static int current_article_valid;
static uint32_t current_article_index;
static size_t current_article_size;
static int current_article_height;
/* display_link_article zeroes file_buffer[0] before asking for the next
 * article, which is when the outgoing one is snapshotted; keep its header. */
static ARTICLE_HEADER current_article_header;
extern int display_first_page;
extern int finger_touched;
extern long finger_move_speed;

static void article_blob_progress(void *opaque, uint64_t completed,
				  uint64_t total)
{
	unsigned int progress = ARTICLE_PROGRESS_BLOB_START;
	(void)opaque;
	if (total) {
		if (completed >= total) {
			progress = ARTICLE_PROGRESS_BLOB_END;
		} else {
			progress += (unsigned int)
				(completed * (ARTICLE_PROGRESS_BLOB_END -
				 ARTICLE_PROGRESS_BLOB_START) / total);
		}
	}
	draw_progress_bar((int)progress, ARTICLE_PROGRESS_LIMIT);
}

static void progress_range(size_t completed, size_t total, int start, int end)
{
	int progress = start;

	if (total) {
		if (completed >= total)
			progress = end;
		else
			progress += (int)((uint64_t)completed * (uint64_t)(end - start) /
					  total);
	}
	draw_progress_bar(progress, ARTICLE_PROGRESS_LIMIT);
}

static void article_html_progress(void *opaque, size_t done, size_t total)
{
	(void)opaque;
	progress_range(done, total, ARTICLE_PROGRESS_BLOB_END,
		       ARTICLE_PROGRESS_HTML_END);
}

static void article_wrap_progress(void *opaque, size_t done, size_t total)
{
	(void)opaque;
	progress_range(done, total, ARTICLE_PROGRESS_HTML_END,
		       ARTICLE_PROGRESS_WRAP_END);
}

static void image_blob_progress(void *opaque, uint64_t completed,
				uint64_t total)
{
	(void)opaque;
	wikilib_service_pending_touch_events();
	progress_range((size_t)completed, (size_t)total,
		       IMAGE_PROGRESS_BLOB_START, IMAGE_PROGRESS_BLOB_END);
}

static void image_decode_progress(void *opaque, size_t completed,
				  size_t total)
{
	(void)opaque;
	wikilib_service_pending_touch_events();
	progress_range(completed, total, IMAGE_PROGRESS_BLOB_END,
			     IMAGE_PROGRESS_DECODE_END);
}

static int normalize_image_path(const unsigned char *source_path,
				size_t source_path_length,
				const unsigned char **path,
				size_t *path_length)
{
	const unsigned char *start = source_path;
	size_t length = source_path_length;
	size_t i;

	while (length >= 2 && start[0] == '.' && start[1] == '/') {
		start += 2;
		length -= 2;
	}
	while (length && *start == '/') {
		start++;
		length--;
	}
	for (i = 0; i < length; i++) {
		if (start[i] == '?' || start[i] == '#') {
			length = i;
			break;
		}
	}
	if (!length || length >= ZIM_DIRENT_TEXT_MAX)
		return -1;
	*path = start;
	*path_length = length;
	return 0;
}

static int grow_deferred_images(void)
{
	ZIM_DEFERRED_IMAGE *images;
	size_t capacity;

	if (deferred_image_count < deferred_image_capacity)
		return 0;
	if (deferred_image_capacity >= ZIM_MAX_DEFERRED_IMAGES)
		return -1;
	capacity = deferred_image_capacity ? deferred_image_capacity * 2 :
		ZIM_INITIAL_DEFERRED_IMAGES;
	if (capacity > ZIM_MAX_DEFERRED_IMAGES)
		capacity = ZIM_MAX_DEFERRED_IMAGES;
	images = memory_allocate(capacity * sizeof(*images), "zim-images");
	if (!images)
		return -1;
	if (deferred_image_count)
		memcpy(images, deferred_images,
		       deferred_image_count * sizeof(*images));
	if (deferred_images)
		memory_free(deferred_images, "zim-images");
	deferred_images = images;
	deferred_image_capacity = capacity;
	return 0;
}

static int grow_deferred_links(void)
{
	ZIM_DEFERRED_LINK *links;
	size_t capacity;

	if (deferred_link_count < deferred_link_capacity)
		return 0;
	if (deferred_link_capacity >= MAX_ARTICLE_LINKS)
		return -1;
	capacity = deferred_link_capacity ? deferred_link_capacity * 2 : 32;
	if (capacity > MAX_ARTICLE_LINKS)
		capacity = MAX_ARTICLE_LINKS;
	links = memory_allocate(capacity * sizeof(*links), "zim-links");
	if (!links)
		return -1;
	if (deferred_link_count)
		memcpy(links, deferred_links,
		       deferred_link_count * sizeof(*links));
	if (deferred_links)
		memory_free(deferred_links, "zim-links");
	deferred_links = links;
	deferred_link_capacity = capacity;
	return 0;
}

static int grow_deferred_anchors(void)
{
	ZIM_DEFERRED_ANCHOR *anchors;
	size_t capacity;

	if (deferred_anchor_count < deferred_anchor_capacity)
		return 0;
	if (deferred_anchor_capacity >= ZIM_MAX_DEFERRED_ANCHORS)
		return -1;
	capacity = deferred_anchor_capacity ? deferred_anchor_capacity * 2 : 64;
	if (capacity > ZIM_MAX_DEFERRED_ANCHORS)
		capacity = ZIM_MAX_DEFERRED_ANCHORS;
	anchors = memory_allocate(capacity * sizeof(*anchors), "zim-anchors");
	if (!anchors)
		return -1;
	if (deferred_anchor_count)
		memcpy(anchors, deferred_anchors,
		       deferred_anchor_count * sizeof(*anchors));
	if (deferred_anchors)
		memory_free(deferred_anchors, "zim-anchors");
	deferred_anchors = anchors;
	deferred_anchor_capacity = capacity;
	return 0;
}

static void article_anchor(void *opaque, const unsigned char *id,
			   size_t id_length, int y)
{
	(void)opaque;
	if (!id_length || grow_deferred_anchors())
		return;
	deferred_anchors[deferred_anchor_count].id = id;
	deferred_anchors[deferred_anchor_count].id_length = id_length;
	deferred_anchors[deferred_anchor_count].y = y;
	deferred_anchor_count++;
}

static int hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Compare a percent-encoded fragment, without its '#', to a raw element id. */
static int fragment_matches(const unsigned char *fragment, size_t length,
			    const unsigned char *id, size_t id_length)
{
	size_t f = 0;
	size_t i = 0;

	while (f < length && i < id_length) {
		unsigned char c = fragment[f];

		if (c == '%' && f + 2 < length && hex_value(fragment[f + 1]) >= 0 &&
		    hex_value(fragment[f + 2]) >= 0) {
			c = (unsigned char)(hex_value(fragment[f + 1]) * 16 +
					    hex_value(fragment[f + 2]));
			f += 3;
		} else {
			f++;
		}
		if (c != id[i++])
			return 0;
	}
	return f == length && i == id_length;
}

static int find_anchor(const unsigned char *fragment, size_t length)
{
	size_t i;

	for (i = 0; i < deferred_anchor_count; i++)
		if (fragment_matches(fragment, length, deferred_anchors[i].id,
				     deferred_anchors[i].id_length))
			return (int)i;
	return -1;
}

/* Scroll the displayed article so the anchor's line is at the top. */
static int scroll_to_fragment(const unsigned char *fragment, size_t length)
{
	int i = find_anchor(fragment, length);

	if (i < 0)
		return 0;
#ifdef ZIM_TRACE_HASH
	debug_printf("fragment -> stream y %d (from %d)\n",
		     deferred_anchors[i].y, lcd_draw_cur_y_pos);
#endif
	display_article_with_pcf(deferred_anchors[i].y + article_start_y_pos -
				 lcd_draw_cur_y_pos);
	return 1;
}

/* Remember the fragment of a link to another article. */
static void note_pending_fragment(const unsigned char *path, size_t length)
{
	size_t i;

	pending_fragment_length = 0;
	for (i = 0; i < length; i++) {
		if (path[i] == '#') {
			size_t n = length - i - 1;

			if (n && n < sizeof(pending_fragment)) {
				memcpy(pending_fragment, path + i + 1, n);
				pending_fragment_length = n;
			}
			return;
		}
	}
}

/* Once the new article's anchors are known, hand its first display the
 * position of the fragment that was followed. */
static void apply_pending_fragment(void)
{
	int i;

	if (!pending_fragment_length)
		return;
	i = find_anchor(pending_fragment, pending_fragment_length);
	pending_fragment_length = 0;
	if (i >= 0) {
#ifdef ZIM_TRACE_HASH
		debug_printf("opening at fragment, stream y %d\n",
			     deferred_anchors[i].y);
#endif
		set_article_initial_y_pos(deferred_anchors[i].y);
	}
}

static uint32_t article_link(void *opaque, const unsigned char *path,
			     size_t path_length)
{
	ZIM_DEFERRED_LINK *deferred;
	size_t i;
	(void)opaque;

	if (!path_length || path_length >= ZIM_DIRENT_TEXT_MAX)
		return 0;
	if (path[0] == '#' && path_length < 2)
		return 0;
	for (i = 0; i < path_length && path[i] != '/' && path[i] != '?' &&
	     path[i] != '#'; i++)
		if (path[i] == ':')
			return 0;
	if (grow_deferred_links())
		return 0;
	deferred = &deferred_links[deferred_link_count];
	deferred->path = path;
	deferred->path_length = path_length;
	deferred_link_count++;
	return ZIM_DEFERRED_LINK_TAG | (uint32_t)deferred_link_count;
}

static long handle_article_link(long article_id, int resolve)
{
	uint32_t id = (uint32_t)article_id;
	uint32_t encoded_index;
	ZIM_DIRENT dirent;
	char path[ZIM_DIRENT_TEXT_MAX];
	int rc;

	if ((id & ZIM_DEFERRED_LINK_MASK) != ZIM_DEFERRED_LINK_TAG)
		return article_id;
	encoded_index = id & ZIM_DEFERRED_LINK_INDEX_MASK;
	if (!encoded_index || encoded_index > deferred_link_count)
		return 0;
	if (!resolve)
		return article_id;
	if (deferred_links[encoded_index - 1].path[0] == '#') {
		/* Same page: scroll instead of loading; 0 tells the caller
		 * there is no article to open. */
		scroll_to_fragment(deferred_links[encoded_index - 1].path + 1,
				   deferred_links[encoded_index - 1].path_length - 1);
		return 0;
	}
	if (zim_link_normalize(current_article_path,
			       deferred_links[encoded_index - 1].path,
			       deferred_links[encoded_index - 1].path_length,
			       path, sizeof(path)))
		return 0;
	note_pending_fragment(deferred_links[encoded_index - 1].path,
			      deferred_links[encoded_index - 1].path_length);
	rc = zim_archive_find_path(&archive, 'C', path, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		return 0;
	return (long)dirent.path_index + 1;
}

/* Stream bytes placeholders may still take.  The wrapper cannot know how much
 * text follows an image, so retrieve_article sizes this from the text before
 * wrapping: what the text will occupy, the link table, and a margin.  Long,
 * picture-heavy articles (New York City: 1.3 MB of HTML, 3,560 links) then
 * lose their last pictures instead of failing to load. */
static size_t image_budget;

static size_t article_image_budget(const unsigned char *text, size_t size)
{
	size_t i = 0;
	size_t kept = 0;
	size_t links = 0;
	size_t need;

	while (i < size) {
		unsigned char c = text[i];
		size_t skip = 1;

		if (c == ZIM_TEXT_LINK_START_MARKER && size - i >= 3) {
			skip = 3 + (text[i + 1] | (size_t)text[i + 2] << 8);
			links++;
		} else if (c == ZIM_TEXT_ANCHOR_MARKER && size - i >= 3) {
			skip = 3 + (text[i + 1] | (size_t)text[i + 2] << 8);
		} else if (c == ZIM_TEXT_IMAGE_MARKER && size - i >= 7) {
			skip = 7 + (text[i + 5] | (size_t)text[i + 6] << 8);
		} else if (c != ZIM_TEXT_LINK_END_MARKER) {
			const unsigned char *run = text + i;
			const unsigned char *end = text + size;

			while (run < end && *run > ZIM_TEXT_IMAGE_MARKER)
				run++;
			skip = run > text + i ? (size_t)(run - text - i) : 1;
			kept += skip;
		}
		i += skip;
	}
	/* Line-break and underline escapes, the link table, and slack. */
	need = kept + kept / 12 + links * (3 + sizeof(ARTICLE_LINK)) + 2048;
	return need < FILE_BUFFER_SIZE ? FILE_BUFFER_SIZE - need : 0;
}

static int article_image(void *opaque, const unsigned char *source_path,
			 size_t source_path_length,
			 unsigned int requested_width,
			 unsigned int requested_height,
			 unsigned char *bitmap, size_t capacity,
			 uint8_t *width, uint16_t *height,
			 size_t *bitmap_size)
{
	ZIM_DEFERRED_IMAGE *deferred;
	const unsigned char *path;
	size_t path_length;
	(void)opaque;

	/* Kiwix thumbnails carry both dimensions. Requiring them lets us reserve
	 * an exact-size blank bitmap without touching or decoding the asset. */
	if (!requested_width || !requested_height ||
	    requested_width < ZIM_MIN_IMAGE_WIDTH ||
	    requested_height < ZIM_MIN_IMAGE_HEIGHT)
		return -1;
	if (normalize_image_path(source_path, source_path_length,
				 &path, &path_length) ||
	    zim_image_fit_dimensions(requested_width, requested_height,
				     requested_width, requested_height,
				     width, height, bitmap_size) ||
	    *bitmap_size > capacity || *bitmap_size + 4 > image_budget ||
	    grow_deferred_images())
		return -1;
	image_budget -= *bitmap_size + 4;
	deferred = &deferred_images[deferred_image_count];
	memset(bitmap, 0, *bitmap_size);
	deferred->stream = bitmap - 4;
	deferred->path = path;
	deferred->path_length = path_length;
	deferred->width = *width;
	deferred->height = *height;
	deferred->bitmap_size = *bitmap_size;
	deferred_image_count++;
	return 0;
}

static int prepare_article_image(unsigned char *stream)
{
	ZIM_DEFERRED_IMAGE *deferred;
	ZIM_DIRENT dirent;
	char path[ZIM_DIRENT_TEXT_MAX];
	size_t webp_size;
	int rc;

	if (deferred_image_next >= deferred_image_count)
		return 0;
	/* Placeholders and metadata are emitted in the same order. Keep a cursor
	 * instead of searching every image for every article-stream token. */
	deferred = &deferred_images[deferred_image_next];
	if (deferred->stream != stream)
		return 0;
	if (deferred_image_decoder) {
		/* Give an already queued gesture ownership of the UI before
		 * beginning another decoder slice.  The decoder state remains live
		 * and resumes after direct and kinetic scrolling have stopped. */
		wikilib_service_pending_touch_events();
		if (finger_touched || finger_move_speed)
			return -1;
		rc = zim_image_decoder_step(deferred_image_decoder);
		if (rc > 0)
			return 1;
		if (rc < 0 || deferred_decode_width != deferred->width ||
		    deferred_decode_height != deferred->height ||
		    deferred_decode_bitmap_size != deferred->bitmap_size)
			memset(stream + 4, 0, deferred->bitmap_size);
		else
			draw_progress_bar(100, ARTICLE_PROGRESS_LIMIT);
		zim_image_decoder_destroy(deferred_image_decoder);
		deferred_image_decoder = NULL;
#ifdef ZIM_TRACE_HASH
		memory_debug("image decoded");
#endif
		goto out;
	}
	deferred_progress_framebuffer = lcd_get_framebuffer();
	memcpy(deferred_saved_bar,
	       deferred_progress_framebuffer + LCD_BUFFER_WIDTH_BYTES,
	       sizeof(deferred_saved_bar));
	draw_progress_bar(0, ARTICLE_PROGRESS_LIMIT);
	draw_progress_bar(1, ARTICLE_PROGRESS_LIMIT);
	memcpy(path, deferred->path, deferred->path_length);
	path[deferred->path_length] = '\0';
	watchdog(WATCHDOG_KEY);
	rc = zim_archive_find_path(&archive, 'C', path, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		goto out;
	draw_progress_bar(IMAGE_PROGRESS_BLOB_START, ARTICLE_PROGRESS_LIMIT);
	rc = zim_archive_read_blob_progress(&archive, &dirent, raw_buffer,
					    ZIM_RAW_BUFFER_SIZE, &webp_size,
					    image_blob_progress, NULL);
	if (rc)
		goto out;
	draw_progress_bar(IMAGE_PROGRESS_BLOB_END, ARTICLE_PROGRESS_LIMIT);
	deferred_image_decoder = zim_image_decoder_create(raw_buffer, webp_size,
		deferred->width, deferred->height, stream + 4,
		deferred->bitmap_size, &deferred_decode_width,
		&deferred_decode_height, &deferred_decode_bitmap_size,
		image_decode_progress, NULL);
	if (!deferred_image_decoder)
		goto out;
	if (finger_touched || finger_move_speed)
		return -1;
	return 1;

out:
	deferred_image_next++;
	draw_progress_bar(0, ARTICLE_PROGRESS_LIMIT);
	if (display_first_page)
		repaint_current_article();
	else
		memcpy(deferred_progress_framebuffer + LCD_BUFFER_WIDTH_BYTES,
		       deferred_saved_bar, sizeof(deferred_saved_bar));
	return 0;
}

bool search_string_changed;
bool search_string_changed_remove;
int more_search_results;
int search_interrupted;
int b_type_a_word_cleared;
unsigned int time_search_last;

extern ARTICLE_LINK articleLink[MAX_ARTICLE_LINKS];
extern int article_link_count;
extern unsigned char *file_buffer;
extern int restricted_article;
extern int current_article_wiki_id;
extern long saved_idx_article;

static size_t align_up(size_t value)
{
	return (value + 3) & ~(size_t)3;
}

static void article_cache_drop(ARTICLE_CACHE_ENTRY *entry)
{
	if (!entry->index)
		return;
	article_cache_bytes -= entry->bytes;
	memory_free(entry->data, "zim-articles");
	memset(entry, 0, sizeof(*entry));
}

static void article_cache_flush(void)
{
	unsigned int i;

	for (i = 0; i < ARTICLE_CACHE_ENTRIES; i++)
		article_cache_drop(&article_cache[i]);
	current_article_valid = 0;
}

static ARTICLE_CACHE_ENTRY *article_cache_oldest(void)
{
	ARTICLE_CACHE_ENTRY *oldest = &article_cache[0];
	unsigned int i;

	for (i = 1; i < ARTICLE_CACHE_ENTRIES; i++)
		if (!article_cache[i].index ||
		    (oldest->index && article_cache[i].age < oldest->age))
			oldest = &article_cache[i];
	return oldest;
}

/* Snapshot the article in file_buffer before another one replaces it. */
static void article_cache_store_current(void)
{
	ARTICLE_CACHE_ENTRY *entry;
	CACHED_IMAGE *images;
	CACHED_LINK *links;
	size_t images_at;
	size_t links_at;
	size_t needed;
	size_t i;

	if (!current_article_valid)
		return;
	current_article_valid = 0;
	images_at = align_up(current_article_size) + align_up(article_text_size);
	links_at = images_at + deferred_image_count * sizeof(CACHED_IMAGE);
	needed = links_at + deferred_link_count * sizeof(CACHED_LINK) +
		deferred_anchor_count * sizeof(CACHED_ANCHOR);
	if (needed > ARTICLE_CACHE_BYTES)
		return;
	for (i = 0; i < ARTICLE_CACHE_ENTRIES; i++)
		if (article_cache[i].index == current_article_index)
			article_cache_drop(&article_cache[i]);
	for (;;) {
		entry = article_cache_oldest();
		if (!entry->index || article_cache_bytes + needed <= ARTICLE_CACHE_BYTES)
			break;
		article_cache_drop(entry);
	}
	if (entry->index)
		article_cache_drop(entry);
	entry->data = memory_allocate(needed, "zim-articles");
	if (!entry->data)
		return;
	memcpy(entry->data, file_buffer, current_article_size);
	memcpy(entry->data, &current_article_header, sizeof(current_article_header));
	memcpy(entry->data + align_up(current_article_size), text_buffer,
	       article_text_size);
	images = (CACHED_IMAGE *)(entry->data + images_at);
	for (i = 0; i < deferred_image_count; i++) {
		images[i].stream_offset = (uint32_t)(deferred_images[i].stream -
						     file_buffer);
		images[i].path_offset = (uint32_t)(deferred_images[i].path -
						   text_buffer);
		images[i].path_length = (uint32_t)deferred_images[i].path_length;
		images[i].bitmap_size = (uint32_t)deferred_images[i].bitmap_size;
		images[i].height = deferred_images[i].height;
		images[i].width = deferred_images[i].width;
	}
	links = (CACHED_LINK *)(entry->data + links_at);
	for (i = 0; i < deferred_link_count; i++) {
		links[i].path_offset = (uint32_t)(deferred_links[i].path -
						  text_buffer);
		links[i].path_length = (uint32_t)deferred_links[i].path_length;
	}
	{
		CACHED_ANCHOR *anchors = (CACHED_ANCHOR *)(links + deferred_link_count);

		for (i = 0; i < deferred_anchor_count; i++) {
			anchors[i].id_offset = (uint32_t)(deferred_anchors[i].id -
							  text_buffer);
			anchors[i].id_length = (uint32_t)deferred_anchors[i].id_length;
			anchors[i].y = deferred_anchors[i].y;
		}
	}
	entry->index = current_article_index;
	entry->age = ++article_cache_clock;
	entry->bytes = needed;
	entry->stream_size = current_article_size;
	entry->text_size = article_text_size;
	entry->image_count = deferred_image_count;
	entry->image_next = deferred_image_next;
	entry->link_count = deferred_link_count;
	entry->anchor_count = deferred_anchor_count;
	entry->height = current_article_height;
	memcpy(entry->path, current_article_path, sizeof(entry->path));
	article_cache_bytes += needed;
}

/* Bring a cached article back into file_buffer; 1 on success. */
static int article_cache_restore(uint32_t index)
{
	ARTICLE_CACHE_ENTRY *entry = NULL;
	const CACHED_IMAGE *images;
	const CACHED_LINK *links;
	size_t i;

	for (i = 0; i < ARTICLE_CACHE_ENTRIES; i++)
		if (article_cache[i].index == index)
			entry = &article_cache[i];
	if (!entry)
		return 0;
	deferred_image_count = 0;
	deferred_image_next = 0;
	deferred_link_count = 0;
	/* The grow helpers only enlarge when the table is full. */
	while (deferred_image_capacity < entry->image_count) {
		deferred_image_count = deferred_image_capacity;
		if (grow_deferred_images())
			return 0;
	}
	while (deferred_link_capacity < entry->link_count) {
		deferred_link_count = deferred_link_capacity;
		if (grow_deferred_links())
			return 0;
	}
	deferred_image_count = 0;
	deferred_link_count = 0;
	while (deferred_anchor_capacity < entry->anchor_count) {
		deferred_anchor_count = deferred_anchor_capacity;
		if (grow_deferred_anchors())
			return 0;
	}
	deferred_anchor_count = 0;
	memcpy(file_buffer, entry->data, entry->stream_size);
	memcpy(text_buffer, entry->data + align_up(entry->stream_size),
	       entry->text_size);
	images = (const CACHED_IMAGE *)(entry->data +
		align_up(entry->stream_size) + align_up(entry->text_size));
	for (i = 0; i < entry->image_count; i++) {
		deferred_images[i].stream = file_buffer + images[i].stream_offset;
		deferred_images[i].path = text_buffer + images[i].path_offset;
		deferred_images[i].path_length = images[i].path_length;
		deferred_images[i].bitmap_size = images[i].bitmap_size;
		deferred_images[i].height = images[i].height;
		deferred_images[i].width = images[i].width;
	}
	links = (const CACHED_LINK *)(images + entry->image_count);
	for (i = 0; i < entry->link_count; i++) {
		deferred_links[i].path = text_buffer + links[i].path_offset;
		deferred_links[i].path_length = links[i].path_length;
	}
	{
		const CACHED_ANCHOR *anchors =
			(const CACHED_ANCHOR *)(links + entry->link_count);

		for (i = 0; i < entry->anchor_count; i++) {
			deferred_anchors[i].id = text_buffer + anchors[i].id_offset;
			deferred_anchors[i].id_length = anchors[i].id_length;
			deferred_anchors[i].y = anchors[i].y;
		}
	}
	deferred_image_count = entry->image_count;
	deferred_image_next = entry->image_next;
	deferred_link_count = entry->link_count;
	deferred_anchor_count = entry->anchor_count;
	article_text_size = entry->text_size;
	memcpy(current_article_path, entry->path, sizeof(current_article_path));
	set_article_stream_height(entry->height);
	entry->age = ++article_cache_clock;
	current_article_valid = 1;
	current_article_index = index;
	current_article_size = entry->stream_size;
	current_article_height = entry->height;
	memcpy(&current_article_header, file_buffer, sizeof(current_article_header));
	return 1;
}

static void print_article_error(void)
{
	unsigned char message[80];
	sprintf((char *)message, "Article %lx failed to load.", saved_idx_article);
	guilib_fb_lock();
	guilib_clear();
	render_string(SEARCH_LIST_FONT_IDX, -1, 94, message, ustrlen(message), 0);
	guilib_fb_unlock();
}

static int device_read_at(void *opaque, uint64_t offset, void *buffer,
			  size_t length)
{
	return zim_file_read_at((ZIM_FILE *)opaque, offset, buffer, length);
}

static void open_archive(int wiki_index)
{
	ZIM_IO io;
	int rc;

	if (archive_file.open && archive_wiki == wiki_index)
		return;
	zim_blob_cache_reset();
	article_cache_flush();
	if (archive_file.open)
		zim_file_close(&archive_file);
	if (zim_catalog_path(wiki_index)) {
		if (zim_file_open(&archive_file, zim_catalog_path(wiki_index)))
			fatal_error("cannot open %s", zim_catalog_path(wiki_index));
	} else if (zim_file_open(&archive_file, "1:/wiki.zim") &&
		   zim_file_open(&archive_file, "zim/wiki.zim"))
		fatal_error("no .zim archive found");
	io.read_at = device_read_at;
	io.opaque = &archive_file;
	io.size = archive_file.size;
	rc = zim_archive_open(&archive, &io);
	if (rc)
		fatal_error("invalid or unsupported ZIM archive");
	archive_wiki = wiki_index;
}

static void search_prefix(unsigned char prefix[MAX_TITLE_SEARCH])
{
	memcpy(prefix, search_string, (size_t)search_length + 1);
	if (prefix[0] >= 'a' && prefix[0] <= 'z')
		prefix[0] = (unsigned char)toupper(prefix[0]);
}

static int title_matches(const ZIM_DIRENT *dirent,
			 const unsigned char *prefix)
{
	return !strncmp(dirent->title, (const char *)prefix, strlen((const char *)prefix));
}

/* The title listing is byte-ordered and case-sensitive while the keyboard
 * only types lower case, so "united states" cannot be one prefix probe:
 * "United States" and the "United states" redirect sit far apart.  A search
 * therefore probes every capitalization of the first few words after the
 * first and reads the matching runs one after another, most capitals first,
 * so the real title heads the list and the redirects follow. */
#define SEARCH_VARIANT_WORDS 4
#define SEARCH_VARIANTS_MAX (1 << SEARCH_VARIANT_WORDS)

typedef struct {
	unsigned char prefix[SEARCH_VARIANTS_MAX][MAX_TITLE_SEARCH];
	uint32_t position[SEARCH_VARIANTS_MAX];
	unsigned int count;
	unsigned int current;
	uint32_t emitted;
} SEARCH_CURSOR;

static SEARCH_CURSOR cursor;

static void search_cursor_open(void)
{
	unsigned char base[MAX_TITLE_SEARCH];
	int boundaries[SEARCH_VARIANT_WORDS];
	unsigned int words = 0;
	unsigned int variant;
	int i;

	cursor.count = 0;
	cursor.current = 0;
	cursor.emitted = 0;
	if (!search_length)
		return;
	search_prefix(base);
	for (i = 1; i < search_length && words < SEARCH_VARIANT_WORDS; i++)
		if (base[i - 1] == ' ' && base[i] >= 'a' && base[i] <= 'z')
			boundaries[words++] = i;
	for (variant = 1u << words; variant-- > 0;) {
		unsigned char *prefix = cursor.prefix[cursor.count];
		unsigned int word;
		uint32_t position;

		memcpy(prefix, base, (size_t)search_length + 1);
		for (word = 0; word < words; word++)
			if (variant & (1u << (words - 1 - word)))
				prefix[boundaries[word]] =
					(unsigned char)toupper(prefix[boundaries[word]]);
		if (zim_archive_find_title_prefix(&archive, (const char *)prefix,
						  &position))
			continue;
		cursor.position[cursor.count++] = position;
	}
}

/* Next matching title across the variants; 0 when exhausted. */
static int search_cursor_next(ZIM_DIRENT *dirent)
{
	while (cursor.current < cursor.count) {
		uint32_t position = cursor.position[cursor.current];
		int rc;

		if (position < archive.title_listing_count) {
			rc = zim_archive_title_at(&archive, position, dirent);
			if ((!rc || rc == ZIM_ERR_TRUNCATED) &&
			    title_matches(dirent, cursor.prefix[cursor.current])) {
				cursor.position[cursor.current] = position + 1;
				cursor.emitted++;
				return 1;
			}
		}
		cursor.current++;
	}
	return 0;
}

/* Whether another result follows, without consuming it. */
static int search_cursor_peek(void)
{
	ZIM_DIRENT dirent;
	SEARCH_CURSOR saved = cursor;
	int more = search_cursor_next(&dirent);

	cursor = saved;
	return more;
}

static void populate_results(void)
{
	ZIM_DIRENT dirent;

	results.count = 0;
	results.selected = -1;
	more_search_results = 0;
	search_cursor_open();
	while (results.count < NUMBER_OF_FIRST_PAGE_RESULTS &&
	       search_cursor_next(&dirent)) {
		results.article[results.count] = dirent.path_index + 1;
		strncpy((char *)results.title[results.count], dirent.title,
			MAX_TITLE_ACTUAL - 1);
		results.title[results.count][MAX_TITLE_ACTUAL - 1] = '\0';
		results.count++;
	}
	more_search_results = search_cursor_peek();
#ifdef ZIM_TRACE_HASH
	debug_printf("search '%s' -> %u variants, %lu results, first '%s' index %lu\n",
		     search_string, cursor.count, (unsigned long)results.count,
		     results.count ? (const char *)results.title[0] : "",
		     results.count ? (unsigned long)results.article[0] : 0UL);
#endif
}

void search_init(void)
{
	set_article_stream_prepare(prepare_article_image);
	set_article_link_handler(handle_article_link);
	if (!archive_file.open) {
		static const unsigned char message[] = "Opening ZIM archive...";

		/* Building FatFs's compact link map is the only long startup step.
		 * Put something on the panel before it begins so a valid boot does
		 * not look like a dead device. */
		guilib_fb_lock();
		guilib_clear();
		render_string(SEARCH_LIST_FONT_IDX, -1, 94, message,
			      sizeof(message) - 1, 0);
		guilib_fb_unlock();
	}
	open_archive(nCurrentWiki);
	zim_bench_startup(archive.io.read_at, archive.io.opaque, archive.io.size);
	results.count = 0;
	results.selected = -1;
}

int search_load_trigram(void)
{
	return 1;
}

void reset_search_info(int wiki_index)
{
	open_archive(wiki_index);
}

unsigned int search_result_count(void) { return results.count; }
int search_result_selected(void) { return results.selected; }
unsigned int search_result_first_item(void) { return 0; }
int get_search_string_len(void) { return search_length; }

void search_set_selection(int selection)
{
	results.selected = selection;
}

void search_select_down(void)
{
	if (results.selected + 1 < (int)results.count)
		results.selected++;
}

void search_select_up(void)
{
	if (results.selected > 0)
		results.selected--;
}

int clear_search_string(void)
{
	if (!search_length)
		return -1;
	search_length = 0;
	search_string[0] = '\0';
	results.count = 0;
	results.selected = -1;
	return 0;
}

int search_add_char(char c, unsigned long event_time)
{
	(void)event_time;
	if ((c == ' ' && (!search_length || search_string[search_length - 1] == ' ')) ||
	    search_length >= MAX_TITLE_SEARCH - 1)
		return -1;
	if (c >= 'A' && c <= 'Z')
		c = (char)tolower(c);
	search_string[search_length++] = (unsigned char)c;
	search_string[search_length] = '\0';
	search_string_changed = true;
	return 0;
}

int search_remove_char(int populate, unsigned long event_time)
{
	(void)event_time;
	if (!search_length)
		return -1;
	search_string[--search_length] = '\0';
	search_string_changed_remove = true;
	if (populate) {
		populate_results();
		search_string_changed = false;
	} else {
		search_string_changed = true;
	}
	return 0;
}

void search_fetch(void)
{
	populate_results();
	search_string_changed = false;
}

int check_search_string_change(void)
{
	if (!search_string_changed)
		return 0;
	populate_results();
	search_string_changed = false;
	search_to_be_reloaded(SEARCH_TO_BE_RELOADED_SET, SEARCH_RELOAD_NORMAL);
	return 1;
}

void search_reload(int flag)
{
	int keyboard_mode = keyboard_get_mode();
	unsigned char *framebuffer = lcd_get_framebuffer();
	unsigned char *render_buffer = framebuffer;
	int buffered = keyboard_mode != KEYBOARD_NONE && search_length;
	unsigned int display_count = keyboard_mode == KEYBOARD_NONE ?
		NUMBER_OF_FIRST_PAGE_RESULTS : NUMBER_OF_RESULTS_KEYBOARD;
	unsigned int y = RESULT_START;
	unsigned int i;
	unsigned char prefix[MAX_TITLE_SEARCH];
	(void)flag;

	guilib_fb_lock();
	if (keyboard_mode == KEYBOARD_NONE)
		guilib_clear();
	else if (buffered) {
		render_buffer = (unsigned char *)search_render_buffer;
		memcpy(render_buffer, framebuffer, LCD_BUFFER_SIZE_BYTES);
		guilib_buffer_clear_area(render_buffer, LCD_WIDTH, LCD_HEIGHT,
					 LCD_BUFFER_WIDTH_BYTES, 0, 0,
					 LCD_BUF_WIDTH_PIXELS - 1,
					 LCD_HEIGHT - KEYBOARD_HEIGHT - 1);
	} else {
		guilib_clear_area(0, 0, LCD_BUF_WIDTH_PIXELS - 1,
				  LCD_HEIGHT - KEYBOARD_HEIGHT - 1);
	}
	if (!search_length) {
		draw_logo_or_type_a_word(0, 35, 239,
					LCD_HEIGHT - KEYBOARD_HEIGHT - 1);
		keyboard_paint();
		guilib_fb_unlock();
		return;
	}
	search_prefix(prefix);
	if (buffered)
		buf_render_string_right(render_buffer,
					LCD_BUF_WIDTH_PIXELS - LCD_LEFT_MARGIN,
					LCD_HEIGHT, LCD_BUFFER_WIDTH_BYTES,
					SEARCH_HEADING_FONT_IDX,
					LCD_LEFT_MARGIN, LCD_TOP_MARGIN + 2,
					prefix, ustrlen(prefix), 0);
	else
		render_string_right(SEARCH_HEADING_FONT_IDX, LCD_LEFT_MARGIN,
				    LCD_TOP_MARGIN + 2, prefix,
				    ustrlen(prefix), 0);
	/* While the keyboard is up, render_search_result_with_pcf sees no
	 * first page to continue from and clears more_search_results; the
	 * full-page list pages on from here, so ask the cursor again. */
	if (keyboard_mode == KEYBOARD_NONE)
		more_search_results = search_cursor_peek();
	article_link_count = 0;
	is_title_in_result_list(0, NULL);
	for (i = 0; i < results.count && i < display_count; i++) {
		unsigned int end_y = y + RESULT_HEIGHT - 1;
		is_title_in_result_list(results.article[i], results.title[i]);
		if (keyboard_get_mode() == KEYBOARD_NONE) {
			articleLink[article_link_count].start_xy = (y - 2) << 8;
			articleLink[article_link_count].end_xy =
				LCD_BUF_WIDTH_PIXELS | ((end_y - 2) << 8);
			articleLink[article_link_count++].article_id = results.article[i];
		}
		if (buffered)
			buf_render_string(render_buffer, LCD_BUF_WIDTH_PIXELS,
					  LCD_BUFFER_WIDTH_BYTES,
					  SEARCH_LIST_FONT_IDX, LCD_LEFT_MARGIN,
					  y, results.title[i],
					  ustrlen(results.title[i]), 0);
		else
			render_string(SEARCH_LIST_FONT_IDX, LCD_LEFT_MARGIN, y,
				      results.title[i],
				      ustrlen(results.title[i]), 0);
		y += RESULT_HEIGHT;
	}
	if (buffered)
		memcpy(framebuffer, render_buffer,
		       (LCD_HEIGHT - KEYBOARD_HEIGHT) * LCD_BUFFER_WIDTH_BYTES);
	guilib_fb_unlock();
}

void search_result_display(void)
{
	search_reload(SEARCH_RELOAD_KEEP_REFRESH);
}

void search_to_be_reloaded(int operation, int reload_flag)
{
	static int pending;
	static int pending_flag;

	switch (operation) {
	case SEARCH_TO_BE_RELOADED_CLEAR:
		pending = 0;
		break;
	case SEARCH_TO_BE_RELOADED_SET:
		/* A key event requests a heading-only repaint before the lookup.
		 * Keep the old complete frame until the result-bearing repaint is
		 * ready instead of showing a transient mixture of both searches. */
		if (reload_flag == SEARCH_RELOAD_NO_POPULATE)
			break;
		if (reload_flag == SEARCH_RELOAD_NORMAL &&
		    keyboard_key_inverted() > 0) {
			pending = 1;
			pending_flag = reload_flag;
		} else {
			search_reload(reload_flag);
			pending = 0;
		}
		break;
	case SEARCH_TO_BE_RELOADED_CHECK:
		if (pending && keyboard_key_inverted() <= 0) {
			search_reload(pending_flag);
			pending = 0;
		}
		break;
	default:
		break;
	}
}

void search_open_article(int selection)
{
	if (selection >= NUMBER_OF_FIRST_PAGE_RESULTS)
		selection -= NUMBER_OF_FIRST_PAGE_RESULTS;
	if (selection >= 0 && selection < (int)results.count)
		display_link_article(results.article[selection]);
}

/* Paging beyond the first screen hands out results by ordinal: the
 * encoded position is one more than the number already shown. */
long result_list_offset_next(void)
{
	return (long)cursor.emitted + 1;
}

long result_list_next_result(long encoded_position, long *article_id,
			     unsigned char *title)
{
	ZIM_DIRENT dirent;
	uint32_t ordinal;

	if (encoded_position <= 0)
		return 0;
	ordinal = (uint32_t)encoded_position - 1;
	if (ordinal != cursor.emitted) {
		search_cursor_open();
		while (cursor.emitted < ordinal)
			if (!search_cursor_next(&dirent))
				return 0;
	}
	if (!search_cursor_next(&dirent))
		return 0;
	*article_id = (long)dirent.path_index + 1;
	strncpy((char *)title, dirent.title, MAX_TITLE_ACTUAL - 1);
	title[MAX_TITLE_ACTUAL - 1] = '\0';
	return (long)ordinal + 2;
}

void get_article_title_from_idx(long index, unsigned char *title)
{
	ZIM_DIRENT dirent;
	int rc;
	title[0] = '\0';
	index &= ARTICLE_INDEX_MASK;
	if (index <= 0 || (uint32_t)index > archive.entry_count)
		return;
	rc = zim_archive_read_dirent(&archive, (uint32_t)index - 1, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		return;
	strncpy((char *)title, dirent.title, MAX_TITLE_ACTUAL - 1);
	title[MAX_TITLE_ACTUAL - 1] = '\0';
}

int retrieve_article(long encoded_index)
{
	ARTICLE_HEADER article_header;
	ZIM_DIRENT dirent;
	uint32_t index = (uint32_t)encoded_index & ARTICLE_INDEX_MASK;
	const unsigned char *raw;
	size_t raw_size;
	size_t text_size;
	size_t article_size;
	int rc = 0;
	int article_height;
	int failed_line = 0;
#define FAIL() do { failed_line = __LINE__; goto error; } while (0)

	(void)failed_line;
	zim_overlay_invalidate();   /* the search screen draws into IVRAM */
	zim_bench_article_begin(index);
	set_article_stream_height(0);
	draw_progress_bar(0, ARTICLE_PROGRESS_LIMIT);
	draw_progress_bar(1, ARTICLE_PROGRESS_LIMIT);
	/* History entries carry the archive they came from in the top byte. */
	if (ARTICLE_WIKI_ID(encoded_index)) {
		int wiki_index = get_wiki_idx_from_id(ARTICLE_WIKI_ID(encoded_index));

		if (wiki_index < 0)
			FAIL();
		if (wiki_index != nCurrentWiki)
			set_wiki(wiki_index);
	}
	if (!index || index > archive.entry_count)
		FAIL();
	if (!raw_buffer)
		raw_buffer = memory_allocate(ZIM_RAW_BUFFER_SIZE, "zim-raw");
	if (!text_buffer)
		/* Not in the decoded cluster's SDRAM bank (zim_blob.c places it
		 * in bank 2): the converter reads the cluster and writes here,
		 * and the two must not alternate rows of one bank. */
		text_buffer = zim_alloc_in_bank(FILE_BUFFER_SIZE, 1);
	if (!raw_buffer || !text_buffer)
		FAIL();
	if (deferred_image_decoder) {
		zim_image_decoder_destroy(deferred_image_decoder);
		deferred_image_decoder = NULL;
	}
	article_cache_store_current();
	if (article_cache_restore(index)) {
		zim_bench_article_cached();
#ifdef ZIM_TRACE_HASH
		debug_printf("article cache hit %lu history y %ld\n",
			     (unsigned long)index, history_get_y_pos());
#endif
		apply_pending_fragment();
		draw_progress_bar(100, ARTICLE_PROGRESS_LIMIT);
		restricted_article = 0;
		current_article_wiki_id = 0;
		return 0;
	}
	draw_progress_bar(2, ARTICLE_PROGRESS_LIMIT);
	rc = zim_archive_read_dirent(&archive, index - 1, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		FAIL();
	if (zim_archive_resolve_redirect(&archive, &dirent))
		FAIL();
	strncpy(current_article_path, dirent.path,
		sizeof(current_article_path) - 1);
	current_article_path[sizeof(current_article_path) - 1] = '\0';
	draw_progress_bar(ARTICLE_PROGRESS_BLOB_START, ARTICLE_PROGRESS_LIMIT);
	/* Convert straight out of the decoded-cluster cache when possible; the
	 * copy into raw_buffer is only needed for uncompressed clusters. */
	rc = zim_archive_view_blob_progress(&archive, &dirent, &raw, &raw_size,
					    article_blob_progress, NULL);
	if (rc) {
		rc = zim_archive_read_blob_progress(&archive, &dirent, raw_buffer,
						    ZIM_RAW_BUFFER_SIZE, &raw_size,
						    article_blob_progress, NULL);
		if (rc)
			FAIL();
		raw = raw_buffer;
	}
	zim_bench_mark(ZIM_BENCH_MARK_BLOB);
	draw_progress_bar(ARTICLE_PROGRESS_BLOB_END, ARTICLE_PROGRESS_LIMIT);
#ifdef ZIM_TRACE_HASH
	/* Build with OPT="-O2 -DZIM_TRACE_HASH" to check decoder changes: the
	 * FNV-1a of the decoded article appears on the serial console. */
	{
		uint32_t hash = 2166136261u;
		size_t k;
		for (k = 0; k < raw_size; k++)
			hash = (hash ^ raw[k]) * 16777619u;
		debug_printf("article fnv %08lx size %lu\n", (unsigned long)hash,
			     (unsigned long)raw_size);
	}
#endif
	rc = zim_html_to_text_images_progress(raw, raw_size, text_buffer,
					      FILE_BUFFER_SIZE, &text_size,
					      article_html_progress, NULL);
	if (rc)
		FAIL();
	zim_bench_mark(ZIM_BENCH_MARK_HTML);
#ifdef ZIM_TRACE_HASH
	/* The converter's output too, so its changes can be checked the same
	 * way as the decoder's. */
	{
		uint32_t hash = 2166136261u;
		size_t k;
		for (k = 0; k < text_size; k++)
			hash = (hash ^ text_buffer[k]) * 16777619u;
		debug_printf("text fnv %08lx size %lu\n", (unsigned long)hash,
			     (unsigned long)text_size);
	}
#endif
	draw_progress_bar(ARTICLE_PROGRESS_HTML_END, ARTICLE_PROGRESS_LIMIT);
	deferred_image_count = 0;
	deferred_image_next = 0;
	deferred_link_count = 0;
	deferred_anchor_count = 0;
	article_text_size = text_size;
	image_budget = article_image_budget(text_buffer, text_size);
	if (zim_text_to_article_images_links_progress(text_buffer, text_size,
					     file_buffer, FILE_BUFFER_SIZE,
					     &article_size, article_image, NULL,
					     article_link, NULL, article_anchor,
					     NULL, &article_height,
					     article_wrap_progress, NULL))
		FAIL();
	zim_bench_mark(ZIM_BENCH_MARK_WRAP);
	zim_bench_article_sizes(raw_size, text_size, article_size);
#ifdef ZIM_TRACE_HASH
	/* And the wrapped article stream with its link table. */
	{
		uint32_t hash = 2166136261u;
		size_t k;
		for (k = 0; k < article_size; k++)
			hash = (hash ^ file_buffer[k]) * 16777619u;
		debug_printf("stream fnv %08lx size %lu\n", (unsigned long)hash,
			     (unsigned long)article_size);
	}
#endif
	draw_progress_bar(ARTICLE_PROGRESS_WRAP_END, ARTICLE_PROGRESS_LIMIT);
	memcpy(&article_header, file_buffer, sizeof(article_header));
	if (article_header.offset_article < sizeof(article_header))
		FAIL();
	for (index = 0; index < deferred_image_count; index++)
		deferred_images[index].stream += article_header.offset_article -
			sizeof(article_header);
	set_article_stream_height(article_height);
	current_article_valid = 1;
	current_article_index = (uint32_t)encoded_index & ARTICLE_INDEX_MASK;
	current_article_size = article_size;
	current_article_height = article_height;
	current_article_header = article_header;
	apply_pending_fragment();
#ifdef ZIM_TRACE_HASH
	debug_printf("article %lu history y %ld\n",
		     (unsigned long)current_article_index,
		     history_get_y_pos());
	memory_debug("article ready");
#endif
	draw_progress_bar(100, ARTICLE_PROGRESS_LIMIT);
	restricted_article = 0;
	current_article_wiki_id = 0;
	return 0;

error:
#ifdef ZIM_TRACE_HASH
	debug_printf("article %lu failed at line %d rc %d\n",
		     (unsigned long)((uint32_t)encoded_index & ARTICLE_INDEX_MASK),
		     failed_line, rc);
#endif
	pending_fragment_length = 0;
	draw_progress_bar(0, ARTICLE_PROGRESS_LIMIT);
	print_article_error();
	return -1;
}

void random_article(void)
{
	ZIM_DIRENT dirent;
	uint32_t position;
	int rc;
	if (!archive.title_listing_count)
		return;
	position = (uint32_t)rand() % archive.title_listing_count;
	rc = zim_archive_title_at(&archive, position, &dirent);
	if (!rc || rc == ZIM_ERR_TRUNCATED)
		display_link_article(dirent.path_index + 1);
}

uint32_t get_article_idx_by_title(unsigned char *search_title,
				  unsigned char *actual_title)
{
	uint32_t position;
	ZIM_DIRENT dirent;
	const char *title = actual_title && *actual_title ?
		(const char *)actual_title : (const char *)search_title;
	if (!title || zim_archive_find_title_prefix(&archive, title, &position))
		return 0;
	while (position < archive.title_listing_count) {
		if (zim_archive_title_at(&archive, position++, &dirent) != ZIM_OK)
			return 0;
		if (strcmp(dirent.title, title))
			return 0;
		return dirent.path_index + 1;
	}
	return 0;
}

bool is_title_in_result_list(long index, unsigned char *title)
{
	static uint32_t seen[MAX_RESULT_LIST];
	static int count;
	int i;
	(void)title;
	if (!index) {
		count = 0;
		return false;
	}
	for (i = 0; i < count; i++)
		if (seen[i] == (uint32_t)index)
			return true;
	if (count < MAX_RESULT_LIST)
		seen[count++] = (uint32_t)index;
	return false;
}

void memrcpy(char *destination, char *source, int length)
{
	while (length-- > 0)
		destination[length] = source[length];
}

int fetch_search_result(long start, long end, int initialize)
{
	(void)start; (void)end; (void)initialize;
	return 0;
}

int search_replace_japanese_sonant(void) { return -1; }
int search_replace_per_language_char(const unsigned char *text)
{
	(void)text; return -1;
}
int search_add_per_language_char(const unsigned char *text)
{
	return text && text[0] && !text[1] ? search_add_char((char)text[0], 0) : -1;
}
int search_replace_hiragana_backward(void) { return -1; }

int is_supported_search_char(unsigned char c)
{
	return c && strchr(SUPPORTED_SEARCH_CHARS, c) != NULL;
}
