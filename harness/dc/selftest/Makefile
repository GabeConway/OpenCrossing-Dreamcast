# Harness self-test guest programs. Built inside a DC toolchain container by
# build.sh; requires KOS environ.sh to have been sourced.
#
# -g and -fno-omit-frame-pointer are deliberate: KOS prints
# "Stack Trace: frame pointers not enabled!" without the latter, and the host
# crash triage (sh-elf-addr2line) needs the former.

TARGETS = selftest.elf crashtest.elf

CFLAGS += -O2 -g -fno-omit-frame-pointer -fsigned-char -Wall

all: $(TARGETS)

selftest.elf: selftest.o
	kos-cc -o $@ $< $(KOS_LIBS)

crashtest.elf: crashtest.o
	kos-cc -o $@ $< $(KOS_LIBS)

clean:
	-rm -f $(TARGETS) *.o

include $(KOS_BASE)/Makefile.rules
