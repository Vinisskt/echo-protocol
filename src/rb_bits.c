#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../include/rb_bits.h"

Buffer* rb_init() {
	Buffer *buf = malloc(sizeof(Buffer));
	buf->buf[buf->head] = 0;
	buf->head = 0;
	buf->tail = 0;
	buf->count_put = 0;
	buf->count_get = 0;
	return buf;
}

uint8_t check_rb(Buffer *buf) {
	if (buf == NULL) {
		printf("[Error] allocating memory");
		return 1;
	}
	return 0;
}


// insere bits no rb -> inserindo bit a bit em um byte;
uint8_t put_bits(Buffer *buf, uint8_t *bit) {
	
	if ( ((buf->head + 1) & BUFFER_MASK) == buf->tail) {
		// buffer full
		return 0;
	}
	
	if (buf->count_put == 8) {
		buf->head = (buf->head + 1) & BUFFER_MASK;
		buf->buf[buf->head] = 0;
		buf->count_put = 0;
	}

	buf->buf[buf->head] = (buf->buf[buf->head] << 1) | *bit;
	buf->count_put++;

	return 1;
}

// consome bit do rb -> pega os byte do rb e insere bit a bit no parametro bit;
uint8_t get_bits(Buffer *buf, uint8_t *bit) {

	if (buf->count_get == 8) {
		buf->tail = (buf->tail + 1) & BUFFER_MASK;
		buf->count_get = 0;
	}

	if ( buf->tail == buf->head && buf->count_get == buf->count_put ) {
		// buffer empty
		return 0;
	}

	*bit = (buf->buf[buf->tail] >> (7 - buf->count_get)) & 1;
	buf->count_get++;
	return 1;
}
