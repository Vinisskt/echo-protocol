#ifndef TUN_TAP_H
#define TUN_TAP_H

#include <stdint.h>

int tun_alloc(char *dev, int flags);
int tun_read(int fd, uint8_t *buf, uint16_t len);
int tun_write(int fd, uint8_t *buf, uint16_t len);

int tun_set_ip(const char *dev, const char *ip);
int tun_set_mtu(const char *dev, int mtu);
int tun_set_up(const char *dev);

#endif // TUN_TAP_H
