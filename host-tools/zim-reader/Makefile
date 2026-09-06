CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Werror -I../../zim -I../../zim/webp \
	-DHAVE_CONFIG_H
ZSTD_CFLAGS = -O2 -DNDEBUG -std=c99 -I../../zim

WEBP_DECODER_SOURCES = $(addprefix ../../zim/webp/src/dec/, \
	alpha_dec.c buffer_dec.c frame_dec.c idec_dec.c io_dec.c quant_dec.c \
	tree_dec.c vp8_dec.c vp8l_dec.c webp_dec.c)
WEBP_DSP_SOURCES = $(addprefix ../../zim/webp/src/dsp/, \
	alpha_processing.c cpu.c dec.c dec_clip_tables.c filters.c lossless.c \
	rescaler.c upsampling.c yuv.c)
WEBP_UTILS_SOURCES = $(addprefix ../../zim/webp/src/utils/, \
	bit_reader_utils.c color_cache_utils.c filters_utils.c huffman_utils.c \
	palette.c quant_levels_dec_utils.c random_utils.c rescaler_utils.c \
	thread_utils.c utils.c)
WEBP_SOURCES = $(WEBP_DECODER_SOURCES) $(WEBP_DSP_SOURCES) \
	$(WEBP_UTILS_SOURCES)

.PHONY: all clean check

all: zimdump zimfatdump blob-cache-test

zimdump: zimdump.c ../../zim/zim_archive.c ../../zim/zim_archive.h \
		../../zim/zim_blob.c ../../zim/zim_blob.h \
		../../zim/zim_html.c ../../zim/zim_html.h \
		../../zim/zim_image.c ../../zim/zim_image.h $(WEBP_SOURCES) \
		zstddeclib.o
	$(CC) $(CFLAGS) -o $@ zimdump.c ../../zim/zim_archive.c \
		../../zim/zim_blob.c ../../zim/zim_html.c ../../zim/zim_image.c \
		$(WEBP_SOURCES) zstddeclib.o

zstddeclib.o: ../../zim/zstd/zstddeclib.c
	$(CC) $(ZSTD_CFLAGS) -c -o $@ $<

zimfatdump: zimfatdump.c ../../zim/zim_archive.c ../../zim/zim_archive.h \
		../../zim/zim_fat.c ../../zim/zim_fat.h
	$(CC) $(CFLAGS) -o $@ zimfatdump.c ../../zim/zim_archive.c \
		../../zim/zim_fat.c


blob-cache-test: blob-cache-test.c ../../zim/zim_archive.c \
		../../zim/zim_archive.h ../../zim/zim_blob.c ../../zim/zim_blob.h \
		zstddeclib.o
	$(CC) $(CFLAGS) -o $@ blob-cache-test.c ../../zim/zim_archive.c \
		../../zim/zim_blob.c zstddeclib.o

fat-cache-test: fat-cache-test.c ../../zim/zim_fat.c ../../zim/zim_fat.h
	$(CC) $(CFLAGS) -o $@ fat-cache-test.c ../../zim/zim_fat.c

article-link-test: article-link-test.c ../../zim/zim_article.c \
		../../zim/zim_article.h ../../zim/zim_html.c ../../zim/zim_html.h \
		../../zim/zim_link.c ../../zim/zim_link.h
	$(CC) $(CFLAGS) -DGRIFO_SIMULATOR=1 -Wno-macro-redefined \
		-I../../wiki -I../../samo-lib/include \
		-I../../samo-lib/grifo/include -I../../samo-lib/grifo/common \
		-o $@ article-link-test.c \
		../../zim/zim_article.c ../../zim/zim_html.c ../../zim/zim_link.c

check: zimdump blob-cache-test fat-cache-test article-link-test
	./fat-cache-test
	./article-link-test
	./check.sh

clean:
	rm -f zimdump zimfatdump blob-cache-test fat-cache-test article-link-test \
		zstddeclib.o
