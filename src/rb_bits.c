#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../include/rb_bits.h"

Buffer* rb_init() {
	Buffer *buf = malloc(sizeof(Buffer));
	if (buf == NULL) {
		return NULL;
	}
	buf->head = 0;
	buf->tail = 0;
	buf->buf[buf->head] = 0;
	buf->count_put = 0;
	buf->count_get = 0;
	return buf;
}

uint8_t put_bits(Buffer *buf, uint8_t *bit) {
	if (((buf->head + 1) & BUFFER_MASK) == buf->tail) {
		return 0;
	}
	
	if (buf->count_put == 8) {
		buf->head = (buf->head + 1) & BUFFER_MASK;
		buf->buf[buf->head] = 0;
		buf->count_put = 0;
	}

	buf->buf[buf->head] |= (*bit << buf->count_put);
	buf->count_put++;

	return 1;
}

uint8_t get_bits(Buffer *buf, uint8_t *bit) {
	if (buf->tail == buf->head && buf->count_get == buf->count_put) {
		return 0;
	}

	if (buf->count_get == 8) {
		buf->tail = (buf->tail + 1) & BUFFER_MASK;
		buf->count_get = 0;
	}

	*bit = (buf->buf[buf->tail] >> buf->count_get) & 1;
	buf->count_get++;
	return 1;
}
