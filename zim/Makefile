# WikiReader UI with a ZIM storage backend.
define FIND_ROOT_DIR
while : ; do \
  d=$$(pwd) ; \
  [ -d "$${d}/samo-lib" ] && echo $${d} && exit 0 ; \
  [ X"/" = X"$${d}" ] && echo ROOT_DIRECTORY_NOT_FOUND && exit 1 ; \
  cd .. ; \
done
endef
ROOT_DIR := $(shell ${FIND_ROOT_DIR})
MK_DIR := ${ROOT_DIR}/samo-lib/Mk
include ${MK_DIR}/definitions.mk
include ${GRIFO_APPLICATION_PRE}

PROGRAM = zim
ICON = YES
SIMULATE = NO

vpath %.c ${ROOT_DIR}/wiki ${ROOT_DIR}/zim/zstd
vpath %.s ${ROOT_DIR}/zim/zstd
vpath %.c ${ROOT_DIR}/zim/webp/src/dec
vpath %.c ${ROOT_DIR}/zim/webp/src/dsp
vpath %.c ${ROOT_DIR}/zim/webp/src/utils
vpath %.png ${ROOT_DIR}/wiki
vpath %.xpm ${ROOT_DIR}/wiki
INCLUDES += -I${ROOT_DIR}/wiki -I${ROOT_DIR}/zim -I${ROOT_DIR}/zim/webp
CFLAGS += -DZIM_APP=1 -DENABLE_PROGRESS=1 -DNDEBUG -DHAVE_CONFIG_H

SOURCES += zim.c
SOURCES += Alloc.c
SOURCES += bmf.c
SOURCES += glyph.c
SOURCES += guilib.c
SOURCES += history.c
SOURCES += highlight.c
SOURCES += keyboard.c
SOURCES += languages.c
SOURCES += lcd_buf_draw.c
SOURCES += restricted.c
SOURCES += search.c
SOURCES += search_fnd.c
SOURCES += sha1.c
SOURCES += utf8.c
SOURCES += wiki_info.c
SOURCES += wikilib.c
SOURCES += zim_alloc.c
SOURCES += zim_archive.c
SOURCES += zim_article.c
SOURCES += zim_blob.c
SOURCES += zim_catalog.c
SOURCES += zim_file.c
SOURCES += zim_html.c
SOURCES += zim_image.c
SOURCES += zim_link.c
SOURCES += zim_overlay.c
SOURCES += zstddeclib.c
SOURCES += zstd_c33_seq.s   # the sequence loop, in A0 RAM

# ZIM_BENCH=YES times a set of memory and card micro-benchmarks at start-up
# and every article load by phase, appending the lines to bench.txt on the
# boot volume and printing them on the serial console; see zim_bench.h.  The
# Makefile does not track flag changes, so remove build/*.o when switching.
ZIM_BENCH ?= NO
# A/B builds omit the calibration micro-benchmarks and record reader
# initialization itself. Use matching before/after source trees.
ZIM_BENCH_AB ?= NO
ifneq (NO,$(strip ${ZIM_BENCH_AB}))
ifneq ($(filter before after,${ZIM_BENCH_AB}),${ZIM_BENCH_AB})
$(error ZIM_BENCH_AB must be NO, before or after)
endif
ZIM_BENCH = YES
CFLAGS += -DZIM_BENCH_AB=1 -DZIM_BENCH_VARIANT=\"${ZIM_BENCH_AB}\"
endif
ifeq (YES,$(strip ${ZIM_BENCH}))
CFLAGS += -DZIM_BENCH=1
SOURCES += zim_bench.c
TARGETS += zimbench.ico   # inverted kiwi for a second launcher entry
build/zim_bench.o: CFLAGS += -mlong-calls   # its A0 RAM tests are far away
endif

# The Zstandard sequence loop lives in A0 RAM (.fastcode, see
# application.lds), far from the rest of the code, so its calls in and out
# need the long form.
build/zstddeclib.o: CFLAGS += -mlong-calls
# The HTML converter and wrapper run as IVRAM overlays (zim_overlay.h);
# calls between them and the rest of the program span 256 MB.
build/zim_html.o: CFLAGS += -mlong-calls
build/zim_article.o: CFLAGS += -mlong-calls
build/vp8_dec.o: CFLAGS += -mlong-calls
build/zim_image.o: CFLAGS += -mlong-calls
# Target flags follow the shared rules' -fno-builtin. Enable fixed-size
# copies in the WebP bit reader without changing the legacy UI's build.
build/vp8_dec.o build/zim_image.o: CFLAGS += -fbuiltin

# After linking, give each overlay section its load address so the kernel's
# ELF loader stores it in SDRAM; the code inside stays linked for IVRAM.
POST_LINK = for s in ovlhtml ovlwrap ovlhuf ovlwebp ovldither; do \
	lma=$$(${OBJDUMP} -h "$@" | awk -v s=$$s '$$2 == s { print "0x" $$5 }'); \
	${OBJCOPY} --change-section-vma $$s=$$lma "$@" || exit 1; \
done

# Portable libwebp decoder only: no encoder, SIMD, threads, mux, or demux.
SOURCES += alpha_dec.c
SOURCES += buffer_dec.c
SOURCES += frame_dec.c
SOURCES += idec_dec.c
SOURCES += io_dec.c
SOURCES += quant_dec.c
SOURCES += tree_dec.c
SOURCES += vp8_dec.c
SOURCES += vp8l_dec.c
SOURCES += webp_dec.c
SOURCES += alpha_processing.c
SOURCES += cpu.c
SOURCES += dec.c
SOURCES += dec_clip_tables.c
SOURCES += filters.c
SOURCES += lossless.c
SOURCES += rescaler.c
SOURCES += upsampling.c
SOURCES += yuv.c
SOURCES += bit_reader_utils.c
SOURCES += color_cache_utils.c
SOURCES += filters_utils.c
SOURCES += huffman_utils.c
SOURCES += palette.c
SOURCES += quant_levels_dec_utils.c
SOURCES += random_utils.c
SOURCES += rescaler_utils.c
SOURCES += thread_utils.c
SOURCES += utils.c

IMAGES += keyboard_123.png
IMAGES += keyboard_abc2_jp.png
IMAGES += keyboard_abc2_ko.png
IMAGES += keyboard_abc_da.png
IMAGES += keyboard_abc_ko.png
IMAGES += keyboard_abc.png
IMAGES += keyboard_phone_jp_123.png
IMAGES += keyboard_phone_jp_abc.png
IMAGES += keyboard_phone_jp.png
IMAGES += keyboard_phone_tw_123.png
IMAGES += keyboard_phone_tw_abc.png
IMAGES += keyboard_phone_tw.png
IMAGES += nls.png

ALL_IMAGES = $(addprefix ${BUILD_PREFIX},all_images.h)
PREBUILD_TARGETS += ${ALL_IMAGES}
IMAGE_HEADERS = $(addsuffix .h,$(addprefix ${BUILD_PREFIX},$(basename ${IMAGES})))
CLEAN_TARGETS += ${IMAGE_HEADERS}

${ALL_IMAGES}: build ${IMAGE_HEADERS}
	@${RM} "$@"
	@for f in ${IMAGE_HEADERS}; do \
	  echo '#include "'$$(basename "$${f}")'"' >> "$@"; \
	done

zim.xpm: ${ROOT_DIR}/wiki/wiki.xpm
	ln -sf "$<" "$@"

include ${GRIFO_APPLICATION_POST}
