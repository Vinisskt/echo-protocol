#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../include/rb_bits.h"

Buffer* rb_init() {
	Buffer *buf = malloc(sizeof(Buffer));
	buf->buf[buf->head] = 0;
	buf->head = 0;
	buf->tail = 0;
	buf->count = 0;
	return buf;
}

uint8_t check_rb(Buffer *buf) {
	if (buf == NULL) {
		printf("[Error] memory alocated");
		return 1;
	}
	return 0;
}

uint8_t put_bits(Buffer *buf, uint8_t *bit) {
	
	if ( ((buf->head + 1) & BUFFER_MASK) == buf->tail) {
		// buffer full
		return 0;
	}
	
	if (buf->count == 8) {
		buf->head++;
		buf->buf[buf->head] = 0;
		buf->count = 0;
	}

	buf->buf[buf->head] = (buf->buf[buf->head] << 1) | *bit;
	buf->count++;

	return 1;
}
