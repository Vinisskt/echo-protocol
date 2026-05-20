#include "../include/tun_tap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/if.h>
#include <linux/if_tun.h>

int tun_alloc(char *dev, int flags) {
    struct ifreq ifr;
    int fd, err;
    const char *clonedev = "/dev/net/tun";

    if ((fd = open(clonedev, O_RDWR)) < 0) {
        perror("Erro ao abrir /dev/net/tun");
        return fd;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = flags;

    if (dev && *dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
        perror("Erro ao executar ioctl(TUNSETIFF)");
        close(fd);
        return err;
    }

    if (dev) {
        strcpy(dev, ifr.ifr_name);
    }

    return fd;
}

int tun_read(int fd, uint8_t *buf, uint16_t len) {
    return read(fd, buf, len);
}

int tun_write(int fd, uint8_t *buf, uint16_t len) {
    return write(fd, buf, len);
}
