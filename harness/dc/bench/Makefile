# Second-tier memory bandwidth benchmark. Built inside a DC toolchain
# container by build.sh; requires KOS environ.sh to have been sourced.
#
# -O2 is correct here and is NOT a violation of CLAUDE.md §1: that directive
# governs src/, the decomp. This is host-side harness tooling with its own
# link, exactly like harness/dc/selftest/.
#
# -g and -fno-omit-frame-pointer match selftest's, for the same two reasons:
# KOS prints "Stack Trace: frame pointers not enabled!" without the latter,
# and the host crash triage (sh-elf-addr2line) needs the former.

TARGETS = bench_mem.elf

CFLAGS += -O2 -g -fno-omit-frame-pointer -fsigned-char -Wall

all: $(TARGETS)

bench_mem.elf: bench_mem.o
	kos-cc -o $@ $< $(KOS_LIBS)

clean:
	-rm -f $(TARGETS) *.o

include $(KOS_BASE)/Makefile.rules
