#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../include/tun_tap.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { printf("  - %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_tun_read_from_pipe() {
    TEST("tun_read reads data from a pipe fd");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    uint8_t write_buf[] = {0x45, 0x00, 0x00, 0x1C, 0x12, 0x34};
    write(pipefd[1], write_buf, sizeof(write_buf));
    uint8_t read_buf[64];
    int n = tun_read(pipefd[0], read_buf, sizeof(read_buf));
    if (n != (int)sizeof(write_buf)) { FAIL("read size wrong"); close(pipefd[0]); close(pipefd[1]); return; }
    if (memcmp(read_buf, write_buf, sizeof(write_buf)) != 0) { FAIL("read data wrong"); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]);
}

void test_tun_read_invalid_fd_returns_negative() {
    TEST("tun_read returns -1 on invalid fd");
    uint8_t buf[16];
    int n = tun_read(-1, buf, sizeof(buf));
    if (n >= 0) { FAIL("should return negative"); return; }
    PASS();
}

void test_tun_read_zero_size() {
    TEST("tun_read with size 0 returns 0 on valid pipe");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    uint8_t buf[16];
    int n = tun_read(pipefd[0], buf, 0);
    if (n != 0) { FAIL("read of 0 bytes should return 0"); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]);
}

void test_tun_write_to_pipe() {
    TEST("tun_write writes data to a pipe fd");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    uint8_t write_buf[] = {0x45, 0x00, 0x00, 0x1C, 0xAA, 0xBB};
    int n = tun_write(pipefd[1], write_buf, sizeof(write_buf));
    if (n != (int)sizeof(write_buf)) { FAIL("write size wrong"); close(pipefd[0]); close(pipefd[1]); return; }
    uint8_t read_buf[64];
    n = read(pipefd[0], read_buf, sizeof(read_buf));
    if (n != (int)sizeof(write_buf)) { FAIL("read from pipe size wrong"); close(pipefd[0]); close(pipefd[1]); return; }
    if (memcmp(read_buf, write_buf, sizeof(write_buf)) != 0) { FAIL("written data differs"); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]);
}

void test_tun_write_invalid_fd_returns_negative() {
    TEST("tun_write returns -1 on invalid fd");
    uint8_t buf[] = {0x01, 0x02};
    int n = tun_write(-1, buf, sizeof(buf));
    if (n >= 0) { FAIL("should return negative"); return; }
    PASS();
}

void test_tun_write_zero_size() {
    TEST("tun_write with size 0 returns 0 on valid pipe");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    uint8_t buf[] = {0x00};
    int n = tun_write(pipefd[1], buf, 0);
    if (n != 0) { FAIL("write of 0 bytes should return 0"); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    close(pipefd[0]); close(pipefd[1]);
}

void test_tun_read_write_large_block() {
    TEST("tun_read/tun_write transfer 4096 bytes correctly");
    int pipefd[2];
    if (pipe(pipefd) == -1) { FAIL("pipe failed"); return; }
    uint8_t *write_buf = malloc(4096);
    uint8_t *read_buf = malloc(4096);
    for (int i = 0; i < 4096; i++) write_buf[i] = (uint8_t)(i & 0xFF);
    int n = tun_write(pipefd[1], write_buf, 4096);
    if (n != 4096) { FAIL("write of 4096 bytes failed"); free(write_buf); free(read_buf); close(pipefd[0]); close(pipefd[1]); return; }
    n = tun_read(pipefd[0], read_buf, 4096);
    if (n != 4096) { FAIL("read of 4096 bytes failed"); free(write_buf); free(read_buf); close(pipefd[0]); close(pipefd[1]); return; }
    if (memcmp(read_buf, write_buf, 4096) != 0) { FAIL("4096 bytes data differs"); free(write_buf); free(read_buf); close(pipefd[0]); close(pipefd[1]); return; }
    PASS();
    free(write_buf); free(read_buf);
    close(pipefd[0]); close(pipefd[1]);
}

int main() {
    printf("=== Complete Tests: TUN/TAP ===\n\n");

    printf("[tun_read]\n");
    test_tun_read_from_pipe();
    test_tun_read_invalid_fd_returns_negative();
    test_tun_read_zero_size();

    printf("\n[tun_write]\n");
    test_tun_write_to_pipe();
    test_tun_write_invalid_fd_returns_negative();
    test_tun_write_zero_size();

    printf("\n[integrity]\n");
    test_tun_read_write_large_block();

    printf("\n=== Summary: %d PASS, %d FAIL ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
