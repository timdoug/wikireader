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

/*
 * Give the font file a FatFs fast-seek map.  Glyph misses seek all over a
 * font, and a seek without a map follows the FAT chain from the start of
 * the file, cluster by cluster: on a 3.6 MB font that was most of the time
 * a page of new glyphs took to wrap.  The map is a few words for a
 * contiguous file; a fragmented one gets the size FatFs asks for.  Without
 * a map the font still works, slowly.
 */
static void create_link_map(pcffont_bmf_t *font, int fd)
{
	unsigned long entries = 4;
	file_error_t result;

	font->link_map = (unsigned long *)memory_allocate(entries * sizeof(*font->link_map), "bmfmap");
	if (!font->link_map)
		return;
	result = file_fastseek(fd, font->link_map, entries);
	if (result == FILE_ERROR_NOT_ENOUGH_CORE) {
		entries = font->link_map[0];
		memory_free(font->link_map, "bmfmap");
		font->link_map = NULL;
		if (entries < 4 || entries > 4096)
			return;
		font->link_map = (unsigned long *)memory_allocate(entries * sizeof(*font->link_map), "bmfmap");
		if (!font->link_map)
			return;
		result = file_fastseek(fd, font->link_map, entries);
	}
	if (result != FILE_ERROR_OK) {
		memory_free(font->link_map, "bmfmap");
		font->link_map = NULL;
	}
}

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
	create_link_map(font, fd);

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

	/* A glyph already in memory: the width straight from its record,
	 * without pres_bmfbm() copying the 56-byte record out.  A cached
	 * record is stored after any default-glyph substitution, and a
	 * resident record with a positive width is what would be copied. */
	metric = NULL;
	if (font->glyph_slots) {
		unsigned int slot = val & (font->glyph_slots - 1);

		if (font->glyph_tags[slot] == val + 1)
			metric = (const charmetric_bmf *)(font->glyph_cache +
				slot * sizeof(*metric));
	} else {
		long offset = (long)val * sizeof(*metric) + sizeof(font_bmf_header);

		if (offset <= (long)font->file_size - (long)sizeof(*metric))
			metric = (const charmetric_bmf *)(font->charmetric + offset);
	}
	if (metric && metric->width > 0)
		return metric->widthDevice;
	pres_bmfbm(val, font, &bitmap, &copied);
	return bitmap ? copied.widthDevice : 0;
}

/*
 * Fetch the glyph record for `val` from the card, together with every whole
 * record in the sectors it occupies.  The card delivers whole 512-byte
 * sectors and charges about 1.2 ms per read command before the first byte,
 * so the neighbours cost nothing extra and usually cover the next few
 * misses of a script's run of code points.  In a direct-mapped cache the
 * records go to their own slots; a fully resident font takes them in place.
 * Returns 0 if the record for `val` could not be read.
 */
#define BMF_SECTOR 512

static int load_glyph_window(pcffont_bmf_t *font, ucs4_t val)
{
	const long record_size = (long)sizeof(charmetric_bmf);
	const long header = (long)sizeof(font_bmf_header);
	long offset = (long)val * record_size + header;
	long start = offset - (offset % BMF_SECTOR);
	long end = offset + record_size;
	long length;
	char window[2 * BMF_SECTOR];
	ucs4_t first, count, i;

	end += (BMF_SECTOR - end % BMF_SECTOR) % BMF_SECTOR;
	if (end > (long)font->file_size)
		end = (long)font->file_size;
	if (offset + record_size > end)
		return 0;
	length = end - start;
	file_lseek(font->fd, start);
	if (file_read(font->fd, window, (size_t)length) != (ssize_t)length)
		return 0;
	/* Whole records inside the window; the first may begin before it. */
	first = (ucs4_t)((start - header + record_size - 1) / record_size);
	count = (ucs4_t)((end - header) / record_size) - first;
	for (i = 0; i < count; i++)
	{
		ucs4_t glyph = first + i;
		const char *source = window + (glyph * record_size + header - start);
		char *destination;

		if (glyph < BMF_RESIDENT_RECORDS && font->glyph_slots)
			continue;   /* below the cache's range, resident anyway */
		if (font->glyph_slots)
		{
			unsigned int slot = glyph & (font->glyph_slots - 1);
			destination = font->glyph_cache + slot * record_size;
			memcpy(destination, source, (size_t)record_size);
			font->glyph_tags[slot] = glyph + 1;
		}
		else
		{
			destination = font->charmetric + glyph * record_size + header;
			memcpy(destination, source, (size_t)record_size);
		}
	}
	return 1;
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
			/* glyph_slots is always BMF_GLYPH_CACHE_SLOTS, a power of
			 * two; the C33 has no divide instruction. */
			unsigned int slot = val & (font->glyph_slots - 1);
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
			if (!load_glyph_window(font, val))
				memset(Cmetrics,0,sizeof(charmetric_bmf));
			else
				memcpy(Cmetrics, record, sizeof(charmetric_bmf));
			/* The window put the records in place; a substituted
			 * default glyph below is still remembered. */
			loaded = Cmetrics->width != 0;
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
