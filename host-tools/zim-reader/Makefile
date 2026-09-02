CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Werror -I../../zim
ZSTD_CFLAGS = -O2 -DNDEBUG -std=c99

.PHONY: all clean check

all: zimdump zimfatdump

zimdump: zimdump.c ../../zim/zim_archive.c ../../zim/zim_archive.h \
		../../zim/zim_blob.c ../../zim/zim_blob.h \
		../../zim/zim_html.c ../../zim/zim_html.h zstddeclib.o
	$(CC) $(CFLAGS) -o $@ zimdump.c ../../zim/zim_archive.c \
		../../zim/zim_blob.c ../../zim/zim_html.c zstddeclib.o

zstddeclib.o: ../../zim/zstd/zstddeclib.c
	$(CC) $(ZSTD_CFLAGS) -c -o $@ $<

zimfatdump: zimfatdump.c ../../zim/zim_archive.c ../../zim/zim_archive.h \
		../../zim/zim_fat.c ../../zim/zim_fat.h
	$(CC) $(CFLAGS) -o $@ zimfatdump.c ../../zim/zim_archive.c \
		../../zim/zim_fat.c


fat-cache-test: fat-cache-test.c ../../zim/zim_fat.c ../../zim/zim_fat.h
	$(CC) $(CFLAGS) -o $@ fat-cache-test.c ../../zim/zim_fat.c

check: zimdump fat-cache-test
	./fat-cache-test
	./check.sh

clean:
	rm -f zimdump zimfatdump fat-cache-test zstddeclib.o
