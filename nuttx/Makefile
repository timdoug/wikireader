# Build NuttX for the WikiReader as a launcher application.
#
# Copyright (c) 2026 Tim Douglas
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This is a wrapper, not a port of the NuttX build: the real work happens in
# work/nuttx, which fetch.sh reconstructs from overlay/, patches/ and the
# upstream revisions in revisions.  See README.md.

ROOT_DIR := $(shell cd .. && pwd)
HERE := $(shell pwd)
WORK := ${HERE}/work/nuttx

# The same toolchain the rest of the tree uses, plus the private virtual
# environment holding kconfiglib, which NuttX's configure and build need on
# the path.
TOOLCHAIN_BIN ?= ${ROOT_DIR}/host-tools/toolchain-c33/work/install/bin
export PATH := ${WORK}/.venv/bin:${TOOLCHAIN_BIN}:${PATH}

CROSS = ${TOOLCHAIN_BIN}/c33-epson-elf-

# NuttX needs GNU make; macOS ships an old BSD-flavoured one as "make".
GMAKE ?= $(shell command -v gmake 2> /dev/null || echo make)

# Toybox's sources are patched for NuttX at unpack time, and nuttx-apps asks
# for GNU patch by name on macOS.  Homebrew's is gpatch; Apple's own is old
# but applies this series, so use it rather than making gpatch a prerequisite.
PATCH ?= $(shell command -v gpatch 2> /dev/null || echo patch)
export PATCH

# wikireader:app is the configuration Grifo loads: the LCD terminal, NSH, vi
# and the native compiler, linked above the resident kernel.  The others
# (lcd, tcc, nsh, ostest) are for a loader that keeps the whole machine.
CONFIG ?= app

XPM2ICON = ${ROOT_DIR}/samo-lib/grifo/scripts/xpm2icon

.PHONY: all
all: nuttx.app nuttx.ico

# The card only needs the loadable sections; the symbols and DWARF are four
# times the size of the program and Grifo's loader reads past neither.  Keep
# the unstripped kernel for debugging.
nuttx.app: ${WORK}/nuttx
	${CROSS}strip -o $@ $<
	@printf '%s: ' $@ ; ls -l $@ | awk '{print $$5 " bytes"}'

%.ico: %.xpm
	${XPM2ICON} --icon=$@ $<

# Configure on demand.  NuttX writes its configuration into the work tree, so
# the stamp is the tree's own .config, and switching CONFIG re-runs this.
${WORK}/.config: ${HERE}/revisions | ${WORK}
	cd ${WORK} && ./tools/configure.sh -E -m -a apps wikireader:${CONFIG} \
		KCONFIG_OLDDEFCONFIG=olddefconfig

${WORK}:
	./fetch.sh

# Always recurse: NuttX's own make decides what is out of date, and this
# wrapper cannot tell from here.
.PHONY: ${WORK}/nuttx
${WORK}/nuttx: ${WORK}/.config
	${GMAKE} -C ${WORK} -j$(shell sysctl -n hw.ncpu 2> /dev/null || echo 4)

.PHONY: fetch
fetch:
	./fetch.sh

.PHONY: update-port
update-port:
	./update-port.sh

# Show what the work tree has that the committed port does not.  The usual
# reason for a difference is an edit that has not been written back yet.
.PHONY: diff
diff:
	@./update-port.sh > /dev/null && git -C ${ROOT_DIR} diff --stat -- nuttx

.PHONY: test
test:
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_grifo_app.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_sdcard.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_lua.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_wrforth.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_bas.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/test_micropython.py \
		--wikireader ${ROOT_DIR} --image ${WORK}/nuttx

# NuttX's benchmarks, run on the emulated device.  Not a test -- it prints a
# table, and the number that matters is how it compares with the same image on
# real hardware, which the script will line up alongside.
#
# It boots grifo off a card holding nuttx.app as init.app, which is what the
# device does; loading the ELF straight in leaves the machine in a state it is
# never in, and that has now cost three bugs.  Needs nuttx.app, so it depends
# on the build.
.PHONY: bench
bench: nuttx.app
	python3 ${WORK}/boards/c33/s1c33e07/wikireader/tools/run_benchmarks.py \
		--wikireader ${ROOT_DIR} --app ${HERE}/nuttx.app \
		--grifo ${ROOT_DIR}/samo-lib/grifo/grifo.elf

# Two builds from clean, compared.  The whole point of a reproducible build is
# that "did this change the output?" has an answer, and the way to keep that
# true is to ask it now and then; it is not part of "test" because it costs
# two builds and answers a question about the build rather than the program.
.PHONY: repro
repro:
	@${GMAKE} clean > /dev/null && ${GMAKE} > /dev/null
	@cp nuttx.app ${HERE}/.repro.app
	@${GMAKE} clean > /dev/null && ${GMAKE} > /dev/null
	@cmp ${HERE}/.repro.app nuttx.app && \
		echo "PASS: two clean builds of nuttx.app are byte-identical"
	@rm -f ${HERE}/.repro.app

# Build products only, like zim-clean and doom-clean: the repository's "make
# clean" reaches this, and it must not cost a NuttX reconfigure.  NuttX's own
# clean keeps .config; it needs Make.defs, which only exists once configured.
.PHONY: clean
clean:
	rm -f nuttx.app nuttx.ico
	[ ! -f ${WORK}/Make.defs ] || ${GMAKE} -C ${WORK} clean

# Drop the configuration too, so the next build picks up a changed CONFIG.
.PHONY: distclean
distclean:
	rm -f nuttx.app nuttx.ico
	[ ! -f ${WORK}/Make.defs ] || ${GMAKE} -C ${WORK} distclean

# ...and the fetched upstream trees with it; the next build re-clones them.
.PHONY: realclean
realclean:
	rm -f nuttx.app nuttx.ico
	rm -rf work
