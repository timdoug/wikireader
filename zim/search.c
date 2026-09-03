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
#include "zim_blob.h"
#include "zim_file.h"
#include "zim_html.h"

#define ZIM_RAW_BUFFER_SIZE FILE_BUFFER_SIZE
typedef struct {
	unsigned char title[NUMBER_OF_FIRST_PAGE_RESULTS][MAX_TITLE_ACTUAL];
	uint32_t article[NUMBER_OF_FIRST_PAGE_RESULTS];
	uint32_t next_title;
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
	if (archive_file.open)
		zim_file_close(&archive_file);
	if (zim_file_open(&archive_file, "zim/wiki.zim"))
		fatal_error("zim/wiki.zim not found");
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

static void populate_results(void)
{
	unsigned char prefix[MAX_TITLE_SEARCH];
	uint32_t position;
	int rc;

	results.count = 0;
	results.selected = -1;
	more_search_results = 0;
	if (!search_length)
		return;
	search_prefix(prefix);
	rc = zim_archive_find_title_prefix(&archive, (const char *)prefix,
					   &position);
	if (rc)
		return;
	while (position < archive.title_listing_count &&
	       results.count < NUMBER_OF_FIRST_PAGE_RESULTS) {
		ZIM_DIRENT dirent;
		rc = zim_archive_title_at(&archive, position, &dirent);
		if (rc && rc != ZIM_ERR_TRUNCATED)
			break;
		if (!title_matches(&dirent, prefix))
			break;
		results.article[results.count] = dirent.path_index + 1;
		strncpy((char *)results.title[results.count], dirent.title,
			MAX_TITLE_ACTUAL - 1);
		results.title[results.count][MAX_TITLE_ACTUAL - 1] = '\0';
		results.count++;
		position++;
	}
	results.next_title = position;
	if (results.count == NUMBER_OF_FIRST_PAGE_RESULTS &&
	    position < archive.title_listing_count)
		more_search_results = 1;
}

void search_init(void)
{
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

long result_list_offset_next(void)
{
	return (long)results.next_title + 1;
}

long result_list_next_result(long encoded_position, long *article_id,
			     unsigned char *title)
{
	unsigned char prefix[MAX_TITLE_SEARCH];
	uint32_t position;
	ZIM_DIRENT dirent;
	int rc;
	if (encoded_position <= 0)
		return 0;
	position = (uint32_t)encoded_position - 1;
	if (position >= archive.title_listing_count)
		return 0;
	search_prefix(prefix);
	rc = zim_archive_title_at(&archive, position, &dirent);
	if ((rc && rc != ZIM_ERR_TRUNCATED) || !title_matches(&dirent, prefix))
		return 0;
	*article_id = (long)dirent.path_index + 1;
	strncpy((char *)title, dirent.title, MAX_TITLE_ACTUAL - 1);
	title[MAX_TITLE_ACTUAL - 1] = '\0';
	return (long)position + 2;
}

void get_article_title_from_idx(long index, unsigned char *title)
{
	ZIM_DIRENT dirent;
	int rc;
	title[0] = '\0';
	index &= 0x00ffffff;
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
	ZIM_DIRENT dirent;
	uint32_t index = (uint32_t)encoded_index & 0x00ffffff;
	size_t raw_size;
	size_t text_size;
	size_t article_size;
	int rc;

	if (!index || index > archive.entry_count)
		goto error;
	if (!raw_buffer)
		raw_buffer = memory_allocate(ZIM_RAW_BUFFER_SIZE, "zim-raw");
	if (!text_buffer)
		text_buffer = memory_allocate(FILE_BUFFER_SIZE, "zim-text");
	if (!raw_buffer || !text_buffer)
		goto error;
	rc = zim_archive_read_dirent(&archive, index - 1, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		goto error;
	rc = zim_archive_read_blob(&archive, &dirent, raw_buffer,
				   ZIM_RAW_BUFFER_SIZE, &raw_size);
	if (rc)
		goto error;
	rc = zim_html_to_text(raw_buffer, raw_size, text_buffer,
			      FILE_BUFFER_SIZE, &text_size);
	if (rc)
		goto error;
	if (zim_text_to_article(text_buffer, text_size, file_buffer,
				FILE_BUFFER_SIZE, &article_size))
		goto error;
	(void)article_size;
	restricted_article = 0;
	current_article_wiki_id = 0;
	return 0;

error:
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
