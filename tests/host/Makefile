# Host-side unit tests for the portable ESP-DJ3000 core (common/).
# Run: make -C tests/host test

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Werror -I../../common/include
LDLIBS   = -lm

COMMON_SRC = ../../common/src/espdj_proto.c \
             ../../common/src/espdj_resampler.c \
             ../../common/src/espdj_ringbuf.c \
             ../../common/src/espdj_mix.c \
             ../../common/src/espdj_sync.c

TESTS = test_proto test_dsp test_sync

all: $(TESTS)

test_%: test_%.c $(COMMON_SRC) test_util.h
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRC) $(LDLIBS)

test: all
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; exit $$fail

clean:
	rm -f $(TESTS)

.PHONY: all test clean
