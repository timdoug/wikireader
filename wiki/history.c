/*
 * Copyright (c) 2009 Openmoko Inc.
 *
 * Authors   Holger Hans Peter Freyther <zecke@openmoko.org>
 *           Matt Hsu <matt_hsu@openmoko.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <guilib.h>

#include <grifo.h>

#include "ustring.h"
#include "history.h"
#include "wikilib.h"
#include "guilib.h"
#include "glyph.h"
#include "search.h"
#include "lcd_buf_draw.h"
#include "wiki_info.h"
#ifdef ZIM_APP
#include "zim_sparrow.h"
#endif

#define DBG_HISTORY 0

#define HISTORY_MAX_ITEM	19
#define HISTORY_MAX_DISPLAY_ITEM	18U
#define MAX_VIEWING_LIST 30

HISTORY history_list[MAX_HISTORY];
struct _viewing_list {
	long idx_article;
	long last_y_pos;
} viewing_list[MAX_VIEWING_LIST];
long history_y_pos = 0;
int viewing_count = 0;
int history_count = 0;
int rendered_history_count = -1;
int history_changed = HISTORY_SAVE_NONE;
extern int display_mode;

static inline unsigned int history_modulus(int modulus) {
	return modulus % HISTORY_MAX_DISPLAY_ITEM;
}

long history_get_previous_idx(long current_idx_article, int b_drop_from_list)
{
	long previous_idx_article;

	if (viewing_count > 0)
	{
		if (viewing_count > 1)
		{
			previous_idx_article = viewing_list[viewing_count - 2].idx_article;
			history_y_pos = viewing_list[viewing_count - 2].last_y_pos;
		}
		else if (viewing_list[0].idx_article != current_idx_article)
		{
			previous_idx_article = viewing_list[0].idx_article;
			history_y_pos = viewing_list[0].last_y_pos;
		}
		else
			previous_idx_article = 0;
		if (b_drop_from_list)
			viewing_count--;
	}
	else
		previous_idx_article = 0;

	return previous_idx_article;
}

void history_reload()
{
	rendered_history_count = 0;
	render_history_with_pcf();
}

void history_add(long idx_article, const unsigned char *title, int b_keep_pos)
{
	int i = 0;
	int bFound = 0;
#ifdef ZIM_APP
	unsigned char question_title[MAX_TITLE_ACTUAL];
#endif

	if (!ARTICLE_WIKI_ID(idx_article)) // idx_article for current wiki
	{
		idx_article |= ARTICLE_WIKI_BITS(get_wiki_id_from_idx(nCurrentWiki));
	}

	if (!viewing_count || viewing_list[viewing_count - 1].idx_article != idx_article)
	{
		if (viewing_count >= MAX_VIEWING_LIST)
		{
			for (i=0; i < MAX_VIEWING_LIST - 1; i++)
				viewing_list[i] = viewing_list[i + 1];
			viewing_count--;
		}
		viewing_list[viewing_count].idx_article = idx_article;
		viewing_list[viewing_count++].last_y_pos = 0;
	}

#ifdef ZIM_APP
	/* Persist the question rather than the rendered answer heading. This
	 * lets an evicted page, or a page from a previous boot, be regenerated. */
	if (ZIM_SPARROW_IS_PAGE(idx_article)) {
		const char *q = zim_sparrow_question((uint32_t)idx_article);
		if (!q) return;
		snprintf((char *)question_title, sizeof(question_title), "ask %s", q);
		title = question_title;
	}
#endif
	history_changed = HISTORY_SAVE_NORMAL;
	i = 0; // the viewing-list shift above reuses i; restart the duplicate scan
	while (!bFound && i < history_count)
	{
		if (idx_article == history_list[i].idx_article)
		{
			HISTORY history_tmp;
			history_tmp = history_list[i];
			if (!b_keep_pos)
				history_tmp.last_y_pos = 0;
			/* Refresh the stored title: an entry recorded when the
			 * article rendered wrongly kept its bad title for ever,
			 * because revisiting only moved the entry to the top. */
			if (title && *title)
				ustrcpy(history_tmp.title, title);
			memrcpy((void*)&history_list[1],(void*)&history_list[0],sizeof(HISTORY)*i);
			history_list[0]=history_tmp;
			bFound = 1;
		}
		else
			i++;
	}

	if(bFound)
		return;

	if (history_count >= MAX_HISTORY)
		history_count = MAX_HISTORY - 1;
	memrcpy((void*)&history_list[1],(void*)&history_list[0],sizeof(HISTORY)*history_count);
	history_list[0].idx_article = idx_article;
	ustrcpy(history_list[0].title, title);
	history_list[0].last_y_pos = 0;
	history_count++;
}

void history_log_y_pos(const long y_pos)
{
	/* Repainting the same viewport is not a history change. In particular,
	 * tapping a stationary page must not start another five-second save
	 * delay and keep the CPU awake. */
	if (history_count > 0 && history_list[0].last_y_pos != y_pos &&
	    (!viewing_count || history_list[0].idx_article == viewing_list[viewing_count - 1].idx_article))
	{
		if (history_changed != HISTORY_SAVE_NORMAL)
			history_changed = HISTORY_SAVE_POWER_OFF;
		history_list[0].last_y_pos = y_pos;
	}
	if (viewing_count > 0)
		viewing_list[viewing_count - 1].last_y_pos = y_pos;
}

void history_set_y_pos(const long idx_article)
{
	int i = 0;
	int bFound = 0;

	history_y_pos = 0;
	while (!bFound && i < history_count)
	{
		if (idx_article == history_list[i].idx_article)
		{
			history_y_pos = history_list[i].last_y_pos;
			bFound = 1;
		}
		else
			i++;
	}
}

long history_get_y_pos()
{
	return history_y_pos;
}

unsigned int history_get_count()
{
	return history_count;
}

void history_clear()
{
	history_count = 0;
	memset((void *)history_list, 0, sizeof(history_list));
	history_changed = HISTORY_SAVE_NORMAL;
}

void history_list_init(void)
{
	int fd_hst;
	HISTORY item;

	memset((void *)history_list, 0, sizeof(history_list));
	history_count = 0;
	fd_hst = file_open(APP_HISTORY_FILE, FILE_OPEN_READ);
	if (fd_hst >= 0)
	{
		while (history_count < MAX_HISTORY &&
		       file_read(fd_hst, &item, sizeof(item)) == sizeof(item))
		{
			if (!item.idx_article) break;
			if (memchr(item.title, 0, sizeof(item.title)))
				history_list[history_count++] = item;
		}
		file_close(fd_hst);
	}
#ifdef ZIM_APP
	zim_sparrow_history_loaded();
#endif
}

int history_needs_save(void)
{
	return history_changed != HISTORY_SAVE_NONE;
}

int history_list_save(int level)
{
	int fd_hst;
	int rc = 0;

	if (history_changed != HISTORY_SAVE_NONE)
	{
		if (level == HISTORY_SAVE_POWER_OFF || history_changed == HISTORY_SAVE_NORMAL)
		{
			fd_hst = file_open(APP_HISTORY_FILE, FILE_OPEN_WRITE);
			if (fd_hst < 0)
				fd_hst = file_create(APP_HISTORY_FILE, FILE_OPEN_WRITE);
			if (fd_hst >= 0)
			{
				int written = file_write(fd_hst, (void *)history_list, sizeof(HISTORY) * MAX_HISTORY);
				file_close(fd_hst);
				if (written != sizeof(HISTORY) * MAX_HISTORY) return -1;
			}
			else return -1;
			history_changed = HISTORY_SAVE_NONE;
			rc = 1;
		}
		else
			rc = -1;
	}
	return rc;
}

void draw_clear_history(int bClear)
{
	int i;
	static unsigned char localBuffer[27 * LCD_BUFFER_WIDTH / 8];
	const unsigned char *pText;
	uint8_t *framebuffer = lcd_get_framebuffer();

	if (bClear)
	{
		memcpy(&framebuffer[181 * LCD_BUFFER_WIDTH / 8], localBuffer, 27 * LCD_BUFFER_WIDTH / 8);
	}
	else
	{
		memcpy(localBuffer, &framebuffer[181 * LCD_BUFFER_WIDTH / 8], 27 * LCD_BUFFER_WIDTH / 8);
		memset(&framebuffer[181 * LCD_BUFFER_WIDTH / 8], 0xFF,  27 * LCD_BUFFER_WIDTH / 8);

		framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 16] = 0xFE;
		memset(&framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 17], 0, 5);
		framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 22] = 0x07;

		framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 23] = 0xF8;
		memset(&framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 24], 0, 5);
		framebuffer[184 * LCD_BUFFER_WIDTH / 8 + 29] = 0x1F;

		for (i = 185; i <= 203; i++)
		{
			framebuffer[i * LCD_BUFFER_WIDTH / 8 + 16] = 0xFC;
			memset(&framebuffer[i * LCD_BUFFER_WIDTH / 8 + 17], 0, 5);
			framebuffer[i * LCD_BUFFER_WIDTH / 8 + 22] = 0x03;

			framebuffer[i * LCD_BUFFER_WIDTH / 8 + 23] = 0xF0;
			memset(&framebuffer[i * LCD_BUFFER_WIDTH / 8 + 24], 0, 5);
			framebuffer[i * LCD_BUFFER_WIDTH / 8 + 29] = 0x0F;
		}

		framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 16] = 0xFE;
		memset(&framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 17], 0, 5);
		framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 22] = 0x07;

		framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 23] = 0xF8;
		memset(&framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 24], 0, 5);
		framebuffer[204 * LCD_BUFFER_WIDTH / 8 + 29] = 0x1F;

		pText=get_nls_text("clear_history");
		render_string(SUBTITLE_FONT_IDX, LCD_LEFT_MARGIN, 185, pText, ustrlen(pText), 1);
		pText=get_nls_text("yes");
		render_string(SUBTITLE_FONT_IDX, 131 + (184 - 131 - (get_external_str_pixel_width(pText, SUBTITLE_FONT_IDX)) + 1) / 2,
			      185, pText, ustrlen(pText), 0);
		pText=get_nls_text("no");
		render_string(SUBTITLE_FONT_IDX, 185 + (238 - 185 - (get_external_str_pixel_width(pText, SUBTITLE_FONT_IDX)) + 1) / 2,
			      185, pText, ustrlen(pText), 0);
	}
}
