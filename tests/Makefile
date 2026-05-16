CC = gcc
CFLAGS = -Wall -Wextra -I../include
SRC = ../src/rb_bits.c
TEST_SRCS = $(wildcard test_*.c)
TEST_BINS = $(TEST_SRCS:.c=.bin)

.PHONY: all clean run_tests

all: $(TEST_BINS)

%.bin: %.c $(SRC)
	$(CC) $(CFLAGS) $< $(SRC) -o $@

run_tests: all
	@for test in $(TEST_BINS); do \
		echo "Running $$test..."; \
		./$$test; \
		echo "-----------------------"; \
	done

clean:
	rm -f *.bin
