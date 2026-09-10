CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=c99 -Wall -Wextra -Werror
HISTORY_INCLUDES = -I../wiki -I../zim -I../samo-lib/include -I../samo-lib/grifo/include -I../samo-lib/grifo/common
ifeq ($(shell uname -s),Darwin)
HISTORY_LINK = -Wl,-dead_strip
HISTORY_WARN = -Wno-macro-redefined -Wno-asm-operand-widths
else
HISTORY_LINK = -Wl,--gc-sections
endif

.PHONY: all check clean sanitize
all: build/sparrow
build/sparrow: cli.c sparrow.c sparrow.h html.c html.h crc32.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -o $@ cli.c sparrow.c html.c
build/html-test: html-test.c html.c html.h sparrow.c sparrow.h crc32.h ../zim/zim_html.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -o $@ html-test.c html.c sparrow.c ../zim/zim_html.c
build/history-test: history-test.c ../wiki/history.c ../zim/zim_sparrow.c ../zim/zim_sparrow.h
	@mkdir -p build
	$(CC) $(CFLAGS) -std=gnu99 -DZIM_APP -ffunction-sections -fdata-sections $(HISTORY_WARN) $(HISTORY_INCLUDES) $(HISTORY_LINK) -o $@ history-test.c ../wiki/history.c ../zim/zim_sparrow.c
check: build/sparrow build/html-test build/history-test
	./build/html-test
	./build/history-test
	python3 dump-sample-test.py
	python3 tests.py ./build/sparrow
	python3 title-index-test.py
sanitize:
	@mkdir -p build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -o build/sparrow-sanitize cli.c sparrow.c html.c
	python3 tests.py ./build/sparrow-sanitize
	$(CC) -O1 -g -std=gnu99 -DZIM_APP -ffunction-sections -fdata-sections -fsanitize=address,undefined $(HISTORY_WARN) $(HISTORY_INCLUDES) $(HISTORY_LINK) -o build/history-sanitize history-test.c ../wiki/history.c ../zim/zim_sparrow.c
	./build/history-sanitize
clean:
	rm -f build/sparrow build/sparrow-sanitize
