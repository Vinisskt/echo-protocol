#ifndef TUN_TAP_H
#define TUN_TAP_H

#include <stdint.h>

int tun_alloc(char *dev, int flags);
int tun_read(int fd, uint8_t *buf, uint16_t len);
int tun_write(int fd, uint8_t *buf, uint16_t len);

#endif // TUN_TAP_H
