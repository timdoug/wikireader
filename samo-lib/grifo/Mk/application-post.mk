# Makefile
#
# Copyright (c) 2010 Openmoko Inc.
#
# Authors   Christopher Hall <hsw@openmoko.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


ifeq (,$(strip ${PROGRAM}))
# ensure "PROGRAM = prog-name" is set
$(error PROGRAM variable not set)
endif

TARGETS += ${PROGRAM}.app

ifeq (YES,$(strip ${ICON}))
TARGETS += ${PROGRAM}.ico
endif

# list of sources
ifeq (,$(strip ${SOURCES}))
SOURCES += ${PROGRAM}.c
HEADERS +=
endif

# list of object modules (C, or hand-written assembly as .s)
OBJECTS = $(addsuffix .o,$(basename ${SOURCES}))
BUILD_OBJECTS = $(addprefix ${BUILD_PREFIX},${OBJECTS})

# build application library
lib/libapplication.a: lib ${BUILD_OBJECTS}
	${RM} "$@"
	${AR} r "$@" ${BUILD_OBJECTS}


# build application binary
${PROGRAM}.app: build build/${PROGRAM}.o ${GRIFO_APPLICATION_LDS} ${LIBS}
	$(LD) -o $@ ${LDFLAGS} build/${PROGRAM}.o ${LIBS} -T ${GRIFO_APPLICATION_LDS} -Map ${@:.app=.map}
	$(POST_LINK)
	${OBJDUMP} -D "$@" > "${@:.app=.dump}"

# An application may set POST_LINK to a command run on the linked file
# before it is disassembled (the ZIM reader rewrites its overlay sections).
POST_LINK ?= @true


CLEAN_TARGETS += build
build:
	${MKDIR} "$@"

CLEAN_TARGETS += lib
lib:
	${MKDIR} "$@"


# no more assignments to TARGETS or CLEAN_TARGETS  after this point
.PHONY: build-targets
build-targets: ${PREBUILD_TARGETS} ${TARGETS} ${EXTRA_TARGETS}

.PHONY: install
install: all
	@if [ ! -d "${DESTDIR}" ] ; then echo DESTDIR: "'"${DESTDIR}"'" is not a directory ; exit 1; fi
	${COPY} ${TARGETS} "${DESTDIR}"/


.PHONY: clean
clean:
	${RM} -r ${TARGETS} ${CLEAN_TARGETS}
	${RM} -r *.o *.app *.d *.map *.asm33 *.dump *.ico


include ${MK_DIR}/rules.mk
