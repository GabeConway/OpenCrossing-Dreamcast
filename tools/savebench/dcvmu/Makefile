# dcvmu guest program. Built inside the DC toolchain container by build.sh,
# which has already sourced KOS environ.sh.
#
# dc_card.c is compiled here from the REAL source, not a copy: whatever this
# harness proves has to be the code the game links, or the proof is worthless.
# -DDC_CARD_STANDALONE swaps dc_platform.h for a ~10-line shim at the top of
# dc_card.c and drops the CARD* API (which needs the decomp's types).
#
# -lz is the kos-ports zlib, the same one dc/Makefile links.

TARGET = dcvmu.elf
OBJS   = main.o dc_card.o

# EXTRA_DEFS is how build.sh passes the kill switches through, e.g.
#   DCVMU_DEFS=-DDC_CARD_NO_COMPRESS=1 bash build.sh
EXTRA_DEFS ?=

CFLAGS += -O2 -g -fno-omit-frame-pointer -fsigned-char -Wall \
          -DDC_CARD_STANDALONE -I. $(EXTRA_DEFS)

all: $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lz $(KOS_LIBS)

clean:
	-rm -f $(TARGET) *.o

include $(KOS_BASE)/Makefile.rules
