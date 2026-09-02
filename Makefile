CC = gcc
CFLAGS = -Wall -Wextra -I./include
LDFLAGS = -lportaudio -llz4 -lm -lpthread
SRC = src/main.c src/rb_bits.c src/mod_fsk.c src/demod_afsk.c src/echo_protocol.c src/tun_tap.c src/audio_io.c src/rohc.c src/log.c src/scrambler.c src/agc.c src/fec.c
OBJ = $(SRC:.c=.o)
TARGET = echo-protocol

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
