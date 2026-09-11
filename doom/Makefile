ROOT_DIR := $(abspath ..)
MK_DIR := $(ROOT_DIR)/samo-lib/Mk
TOOLCHAIN_BIN ?= $(ROOT_DIR)/host-tools/toolchain-c33/work/install/bin
include $(MK_DIR)/definitions.mk
include $(GRIFO_APPLICATION_PRE)

PROGRAM = doom
ICON = YES
SIMULATE = NO
SOURCES = doom.c engine.c platform.c video.c controls.c c33_math.c profile.c
CFLAGS += -std=gnu11 -fwrapv -fno-strict-aliasing -DWR_C33
LDFLAGS += -T memory.lds
# Upstream's amalgamation contains legacy warnings; keep our adapter strict.
build/engine.o: CFLAGS += -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-parentheses

include $(GRIFO_APPLICATION_POST)

$(BUILD_OBJECTS): | build
doom.app: memory.lds

build/profile_id.h: $(SOURCES) $(wildcard *.h) vendor/PureDOOM.h memory.lds Makefile build-id.py | build
	python3 build-id.py $@
build/profile.o: build/profile_id.h

.PHONY: test
test: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Itests -I. -fsanitize=address,undefined tests/adapters.c platform.c video.c controls.c -o build/test-adapters
	./build/test-adapters
	$(HOST_CC) -std=gnu11 -w -O1 -fwrapv -fno-strict-aliasing -fsanitize=address,undefined tests/renderer.c c33_math.c -o build/test-renderer
	./build/test-renderer
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Itests -I. -fsanitize=address,undefined tests/profile.c profile.c -o build/test-profile
	./build/test-profile

HOST_CC ?= cc

.PHONY: test-c33
test-c33:
	python3 tests/c33_math_test.py --toolchain $(TOOLCHAIN_BIN)
