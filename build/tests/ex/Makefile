# SPDX-License-Identifier: GPL-2.0
export srctree := $(abspath ../../..)
export CC      := gcc
export LD      := ld
export AR      := ar

ex:

include $(srctree)/build/Makefile.include

ex: ex-in.o libex-in.o
	$(CC) -o $@ $^

ex.%: fixdep FORCE
	make -f $(srctree)/build/Makefile.build dir=. $@

ex-in.o: fixdep FORCE
	make $(build)=ex

libex-in.o: fixdep FORCE
	make $(build)=libex

clean:
	find . -name '*.o' -delete -o -name '\.*.cmd' -delete -o -name '\.*.d' -delete
	rm -f ex ex.i ex.s

.PHONY: FORCE
