############################################################################
# apps/interpreters/wrforth/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = wrforth
PRIORITY  = $(CONFIG_INTERPRETERS_WRFORTH_PRIORITY)
STACKSIZE = $(CONFIG_INTERPRETERS_WRFORTH_STACKSIZE)
MODULE    = $(CONFIG_INTERPRETERS_WRFORTH)

MAINSRC = wrforth.c
ASRCS   = forth.s

WRFORTH_SRC = $(patsubst "%",%,$(strip $(CONFIG_INTERPRETERS_WRFORTH_SRCDIR)))

# The interpreter is one assembly file that gforth writes, bracketed by two
# hand-written ones.  NUTTX_FORTH picks the task entry over the reset entry
# and the allocated workspace over the reserved one; the include path is for
# the register definitions the trailer pulls in.

AFLAGS += -Wa,--defsym,NUTTX_FORTH=1
AFLAGS += -Wa,-I,$(WRFORTH_SRC)

# The metacompiler is gforth's; everything else here is POSIX awk, so any
# awk will do.  Both are overridable for a host that keeps them elsewhere.

AWK    ?= awk
GFORTH ?= gforth

# sym.awk turns every "word :: label" line into the metacompiler's symbol
# table, and has to see the extra words here as well as the interpreter.

forth-symbols.fi: $(WRFORTH_SRC)/sym.awk $(WRFORTH_SRC)/ansi-forth.fs \
                  nuttx-forth.fi
	$(Q) $(AWK) -f $(WRFORTH_SRC)/sym.awk $(WRFORTH_SRC)/ansi-forth.fs \
		nuttx-forth.fi > $@

forth.s: $(WRFORTH_SRC)/header.s $(WRFORTH_SRC)/meta.fs forth-symbols.fi \
         $(WRFORTH_SRC)/ansi-forth.fs nuttx-forth.fi \
         $(WRFORTH_SRC)/trailer.s $(WRFORTH_SRC)/hoist-flags.awk
	$(Q) command -v $(GFORTH) > /dev/null || \
		{ echo "$(GFORTH) is needed to metacompile the Forth" >&2; exit 1; }
	$(Q) $(GFORTH) $(WRFORTH_SRC)/meta.fs forth-symbols.fi \
		$(WRFORTH_SRC)/ansi-forth.fs nuttx-forth.fi -e "bye" > forth-body.s
	$(Q) cat $(WRFORTH_SRC)/header.s > $@
	$(Q) $(AWK) -f $(WRFORTH_SRC)/hoist-flags.awk forth-body.s >> $@
	$(Q) cat $(WRFORTH_SRC)/trailer.s >> $@

# Application.mk builds the dependency list before anything else, and it
# cannot scan a file that has not been written yet.

depend:: forth.s

clean::
	$(call DELFILE, forth.s)
	$(call DELFILE, forth-body.s)
	$(call DELFILE, forth-symbols.fi)

include $(APPDIR)/Application.mk
