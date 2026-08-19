CC = gcc
CFLAGS = -Wall -Wextra -I../include
LDFLAGS = -lm -llz4 -lpthread
SRC = ../src/rb_bits.c ../src/mod_fsk.c ../src/mod_afsk.c ../src/demod_afsk.c ../src/echo_protocol.c ../src/tun_tap.c ../src/rohc.c ../src/log.c ../src/scrambler.c ../src/fec.c
TEST_SRCS = $(wildcard test_*.c)
TEST_BINS = $(TEST_SRCS:.c=.bin)

.PHONY: all clean run_tests

all: $(TEST_BINS)

%.bin: %.c $(SRC)
	$(CC) $(CFLAGS) $< $(SRC) -o $@ $(LDFLAGS)

run_tests: all
	@for test in $(TEST_BINS); do \
		echo "Running $$test..."; \
		./$$test; \
		echo "-----------------------"; \
	done

clean:
	rm -f *.bin
