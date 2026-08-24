#include "../include/tun_tap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
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
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    }

    if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
        perror("Erro ao executar ioctl(TUNSETIFF)");
        close(fd);
        return err;
    }

    if (dev) {
        strncpy(dev, ifr.ifr_name, IFNAMSIZ - 1);
        dev[IFNAMSIZ - 1] = '\0';
    }

    return fd;
}

int tun_read(int fd, uint8_t *buf, uint16_t len) {
    return read(fd, buf, len);
}

int tun_write(int fd, uint8_t *buf, uint16_t len) {
    return write(fd, buf, len);
}

/* Open a datagram control socket and prepare ifreq with the device name. */
static int tun_open_control(const char *dev, struct ifreq *ifr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("Erro ao abrir socket de controle");
        return -1;
    }
    memset(ifr, 0, sizeof(*ifr));
    if (dev) {
        strncpy(ifr->ifr_name, dev, IFNAMSIZ - 1);
        ifr->ifr_name[IFNAMSIZ - 1] = '\0';
    }
    return fd;
}

int tun_set_ip(const char *dev, const char *ip) {
    struct ifreq ifr;
    int fd = tun_open_control(dev, &ifr);
    if (fd < 0) return -1;

    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    ifr.ifr_addr.sa_family = AF_INET;
    inet_pton(AF_INET, ip, &addr->sin_addr);

    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        perror("Erro SIOCSIFADDR");
        close(fd);
        return -1;
    }

    /* Network mask 255.255.255.0 */
    inet_pton(AF_INET, "255.255.255.0", &addr->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        perror("Erro SIOCSIFNETMASK");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int tun_set_mtu(const char *dev, int mtu) {
    struct ifreq ifr;
    int fd = tun_open_control(dev, &ifr);
    if (fd < 0) return -1;

    ifr.ifr_mtu = mtu;
    if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) {
        perror("Erro SIOCSIFMTU");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int tun_set_up(const char *dev) {
    struct ifreq ifr;
    int fd = tun_open_control(dev, &ifr);
    if (fd < 0) return -1;

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        perror("Erro SIOCGIFFLAGS");
        close(fd);
        return -1;
    }

    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        perror("Erro SIOCSIFFLAGS");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}
