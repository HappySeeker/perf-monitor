# SPDX-License-Identifier: GPL-2.0
# # Some of the tools (perf) use same make variables
# # as in kernel build.
export srctree=$(CURDIR)
export objtree=$(CURDIR)

ifeq ("$(origin V)", "command line")
  VERBOSE = $(V)
endif
ifndef VERBOSE
  VERBOSE = 0
endif

ifeq ($(VERBOSE),1)
  Q =
else
  Q = @
endif

MAKEFLAGS += --no-print-directory

all: __build
include $(srctree)/scripts/Makefile.include
include $(srctree)/build/Makefile.include

INCLUDES = \
-I$(srctree)/lib/perf/include \
-I$(srctree)/lib/traceevent \
-I$(srctree)/lib/ \
-I$(srctree)/include \
-I$(srctree)/

# Append required CFLAGS
override CFLAGS += $(EXTRA_WARNINGS)
override CFLAGS += -Werror -Wall
override CFLAGS += $(INCLUDES)
override CFLAGS += -fvisibility=hidden

export srctree OUTPUT CFLAGS V

__build clean:
	$(Q)$(MAKE) -f $(srctree)/build/Makefile.bin dir=. $@

