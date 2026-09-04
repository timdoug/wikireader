/*
 * Copyright (c) 2009 Openmoko Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <grifo.h>

#include "ustring.h"
#include "bmf.h"
#include "wikilib.h"

/*
 * The three CJK "all" fonts are 3.6 MB each, and production boards have 16 MB
 * of SDRAM.  Rather than reserve every font's full file size, fonts above
 * BMF_RESIDENT_LIMIT keep the header plus the first 256 glyph records in
 * memory and serve everything else through a direct-mapped cache of
 * BMF_GLYPH_CACHE_SLOTS records, refilled from the card on a miss.
 */
#define BMF_RESIDENT_LIMIT (256UL * 1024)
#define BMF_RESIDENT_RECORDS 256
#define BMF_GLYPH_CACHE_SLOTS 2048

int load_bmf(pcffont_bmf_t *font)
{
	int fd;
	font_bmf_header header;
	unsigned long resident = sizeof(font_bmf_header) +
		BMF_RESIDENT_RECORDS * sizeof(charmetric_bmf);

	if (NULL == font || NULL == font->file) {
		fatal_error("font is NULL");
	}

	fd = file_open(font->file, FILE_OPEN_READ);
	if(fd < 0) {
		panic("failed to open font: %s", font->file);
		//debug_printf("failed to open font: %s\n", font->file);
		return -1;
	}

	file_size(font->file, &(font->file_size));
	if (0 == font->file_size) {
		fatal_error("zero size font: %s", font->file);
	}
	if (font->file_size > BMF_RESIDENT_LIMIT) {
		size_t records = BMF_GLYPH_CACHE_SLOTS * sizeof(charmetric_bmf);

		font->charmetric = (char*)memory_allocate(resident, "bmf");
		font->glyph_cache = (char*)memory_allocate(records +
			BMF_GLYPH_CACHE_SLOTS * sizeof(uint32_t), "bmfcache");
		if (!font->charmetric || !font->glyph_cache) {
			fatal_error("load_bmf malloc error on: %s", font->file);
		}
		font->glyph_tags = (uint32_t *)(font->glyph_cache + records);
		font->glyph_slots = BMF_GLYPH_CACHE_SLOTS;
		memset(font->glyph_tags, 0, BMF_GLYPH_CACHE_SLOTS * sizeof(uint32_t));
		memset(font->charmetric, 0, resident);
	} else {
		if (resident > font->file_size)
			resident = font->file_size;
		font->charmetric = (char*)memory_allocate(font->file_size, "bmf");
		if (!font->charmetric) {
			fatal_error("load_bmf malloc error on: %s", font->file);
		}
		font->glyph_cache = NULL;
		font->glyph_tags = NULL;
		font->glyph_slots = 0;
		memset(font->charmetric, 0, font->file_size);
	}

	file_read(fd, font->charmetric, resident);

	memcpy(&header,font->charmetric,sizeof(font_bmf_header));

	font->Fmetrics.linespace = header.linespace;
	font->Fmetrics.ascent    = header.ascent;
	font->Fmetrics.descent   = header.descent;
	font->Fmetrics.default_char = header.default_char;

	return fd;
}

int bmf_char_width(ucs4_t val, pcffont_bmf_t *font)
{
	const charmetric_bmf *metric;
	bmf_bm_t *bitmap = NULL;
	charmetric_bmf copied;

	if (!font || font->fd < 0)
		return 0;
	if (font->fd == FONT_FD_NOT_INITED) {
		font->fd = load_bmf(font);
		if (font->fd < 0)
			return 0;
	}

	/* load_bmf() brings the first 256 fixed-size records into memory.  Width
	 * measurement needs only two signed bytes, not the record's 48-byte
	 * bitmap, so avoid copying the whole record for every measured glyph. */
	if (val < 256) {
		metric = (const charmetric_bmf *)(font->charmetric +
			val * sizeof(*metric) + sizeof(font_bmf_header));
		return (val == 32 || metric->width > 0) ?
			metric->widthDevice : 0;
	}

	pres_bmfbm(val, font, &bitmap, &copied);
	return bitmap ? copied.widthDevice : 0;
}

int
pres_bmfbm(ucs4_t val, pcffont_bmf_t *font, bmf_bm_t **bitmap,charmetric_bmf *Cmetrics)
{
	long offset;
	int font_header = sizeof(font_bmf_header);
	char *record = NULL;      /* where this glyph's record is kept */
	uint32_t *tag = NULL;     /* cache tag to set once the record is final */
	int loaded = 1;

	// callers use Cmetrics for the space character even when this
	// function fails, so never leave it holding stack garbage
	if(font==NULL || font->fd < 0)
	{
		memset(Cmetrics,0,sizeof(charmetric_bmf));
		return -1;
	}

	if (font->fd == FONT_FD_NOT_INITED)
	{
		font->fd = load_bmf(font);
		if(font->fd < 0)
		{
			memset(Cmetrics,0,sizeof(charmetric_bmf));
			return -1;
		}
	}

	if(val < 256)
	{
		memcpy(Cmetrics,font->charmetric+val*sizeof(charmetric_bmf)+font_header,sizeof(charmetric_bmf));
	}
	else
	{
		offset = (long)val*sizeof(charmetric_bmf)+font_header;
		if (offset > (long)font->file_size - (long)sizeof(charmetric_bmf))
		{
			if (font->bPartialFont)
			{ // character not defined in the current font file (and it is intended to include partial characters)
				font = font->supplement_font;
				return pres_bmfbm(val, font, bitmap, Cmetrics);
			}
			memset(Cmetrics,0,sizeof(charmetric_bmf));
			return -1;
		}
		if (font->glyph_slots)
		{
			unsigned int slot = val % font->glyph_slots;
			record = font->glyph_cache + slot * sizeof(charmetric_bmf);
			tag = &font->glyph_tags[slot];
			loaded = (*tag == val + 1);
			if (loaded)
				memcpy(Cmetrics, record, sizeof(charmetric_bmf));
		}
		else
		{
			record = font->charmetric + offset;
			memcpy(Cmetrics, record, sizeof(charmetric_bmf));
			loaded = Cmetrics->width != 0;
		}
		if (!loaded)
		{
			file_lseek(font->fd,offset);
			// A truncated font file gives a short read.  The metrics must be
			// zeroed in that case, or the caller renders a glyph using
			// whatever happened to be on the stack.
			if (file_read(font->fd,Cmetrics,sizeof(charmetric_bmf)) != (ssize_t)sizeof(charmetric_bmf))
				memset(Cmetrics,0,sizeof(charmetric_bmf));
		}
	}

	if(Cmetrics->width>0)
		*bitmap = (bmf_bm_t*)Cmetrics+8;
	else if (val > 256 && record)
	{
		if (font->Fmetrics.default_char && val != (ucs4_t)font->Fmetrics.default_char)
		{
			pres_bmfbm(font->Fmetrics.default_char, font, bitmap, Cmetrics);
		}
		if (!Cmetrics->width)
		{
			Cmetrics->width = 1;
			Cmetrics->height = 0;
		}
		loaded = 0;   /* the substituted record is what gets remembered */
	}
	if (record && !loaded)
	{
		/* Store last: the default-glyph lookup above may have used the
		 * same cache slot. */
		memcpy(record, Cmetrics, sizeof(charmetric_bmf));
		if (tag)
			*tag = val + 1;
	}

	return 1;
}
