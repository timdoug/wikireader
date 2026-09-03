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
vpath %.png ${ROOT_DIR}/wiki
vpath %.xpm ${ROOT_DIR}/wiki
INCLUDES += -I${ROOT_DIR}/wiki -I${ROOT_DIR}/zim
CFLAGS += -DZIM_APP=1 -DNDEBUG

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
SOURCES += zim_file.c
SOURCES += zim_html.c
SOURCES += zstddeclib.c

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
