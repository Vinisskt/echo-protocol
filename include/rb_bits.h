#include <stdint.h>

#define BUFFER_SIZE 1024
#define BUFFER_MASK 1023

typedef struct {
	uint8_t buf[BUFFER_SIZE];
	uint16_t head;
	uint16_t tail;
	uint8_t count_put;
	uint8_t count_get;
} Buffer;

Buffer* rb_init();
uint8_t check_rb(Buffer *buf);
uint8_t put_bits(Buffer *buf, uint8_t *bit);
uint8_t get_bits(Buffer *buf, uint8_t *bit);
