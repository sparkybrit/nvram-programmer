#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>

#define DEFAULT_PORT  "/dev/ttyACM0"
#define CHUNK_SIZE    4096
#define TIMEOUT_S     60

static int open_serial(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    usleep(100000);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int send_bytes(int fd, const unsigned char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, buf + sent, n - sent > CHUNK_SIZE ? CHUNK_SIZE : n - sent);
        if (w < 0) { fprintf(stderr, "write: %s\n", strerror(errno)); return -1; }
        sent += (size_t)w;
    }
    return 0;
}

/* Drain text lines from fd until one contains kw, printing each. */
static int drain_until(int fd, const char *kw, int timeout_s)
{
    char buf[4096];
    size_t len = 0;
    time_t deadline = time(NULL) + timeout_s;
    while (time(NULL) < deadline) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv = { 1, 0 };
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
        if (n <= 0) continue;
        len += (size_t)n; buf[len] = '\0';
        char *p = buf;
        for (;;) {
            char *nl = memchr(p, '\n', buf + len - p);
            if (!nl) break;
            *nl = '\0';
            if (nl > p && *(nl - 1) == '\r') *(nl - 1) = '\0';
            if (*p) { puts(p); fflush(stdout); }
            if (strstr(p, kw)) return 1;
            p = nl + 1;
        }
        len = (size_t)((buf + len) - p);
        memmove(buf, p, len);
    }
    return 0;
}

/* Receive exactly n raw bytes from fd into buf. */
static int recv_bytes(int fd, unsigned char *buf, size_t n, int timeout_s)
{
    size_t got = 0;
    time_t deadline = time(NULL) + timeout_s;
    while (got < n && time(NULL) < deadline) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv = { 1, 0 };
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        ssize_t r = read(fd, buf + got, n - got);
        if (r > 0) got += (size_t)r;
    }
    if (got != n) {
        fprintf(stderr, "read timeout: received %zu of %zu bytes\n", got, n);
        return -1;
    }
    return 0;
}

static void send_cmd(int fd, char cmd, size_t length)
{
    unsigned char hdr[5] = {
        (unsigned char)cmd,
        (unsigned char)(length >>  0), (unsigned char)(length >>  8),
        (unsigned char)(length >> 16), (unsigned char)(length >> 24),
    };
    if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
        fprintf(stderr, "write (cmd): %s\n", strerror(errno));
}

int main(int argc, char *argv[])
{
    const char *port    = DEFAULT_PORT;
    const char *binpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = argv[++i];
        else if (argv[i][0] != '-') binpath = argv[i];
        else { fprintf(stderr, "usage: nvram_write [--port DEV] <binary>\n"); return 1; }
    }
    if (!binpath) { fprintf(stderr, "usage: nvram_write [--port DEV] <binary>\n"); return 1; }

    FILE *f = fopen(binpath, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", binpath, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    unsigned char *data = malloc((size_t)fsize);
    if (!data) { fprintf(stderr, "out of memory\n"); fclose(f); return 1; }
    if ((long)fread(data, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "short read\n"); free(data); fclose(f); return 1;
    }
    fclose(f);

    printf("Image: %s (%ld bytes)\n", binpath, fsize);

    int fd = open_serial(port);
    if (fd < 0) { free(data); return 1; }

    /* Write */
    send_cmd(fd, 'W', (size_t)fsize);
    if (send_bytes(fd, data, (size_t)fsize) < 0) { free(data); close(fd); return 1; }
    if (!drain_until(fd, "Written.", TIMEOUT_S) && !drain_until(fd, "Timeout.", 1)) {
        fprintf(stderr, "timed out waiting for write to complete\n");
        free(data); close(fd); return 1;
    }

    /* Read back */
    unsigned char *rbuf = malloc((size_t)fsize);
    if (!rbuf) { fprintf(stderr, "out of memory\n"); free(data); close(fd); return 1; }
    send_cmd(fd, 'R', (size_t)fsize);
    if (recv_bytes(fd, rbuf, (size_t)fsize, TIMEOUT_S) < 0) {
        free(rbuf); free(data); close(fd); return 1;
    }

    /* Compare */
    int rc = 0;
    if (memcmp(data, rbuf, (size_t)fsize) == 0) {
        printf("Verified.\n");
    } else {
        for (long i = 0; i < fsize; i++) {
            if (data[i] != rbuf[i]) {
                fprintf(stderr, "FAIL: first mismatch at 0x%05lx: wrote %02x read %02x\n",
                        i, data[i], rbuf[i]);
                break;
            }
        }
        rc = 1;
    }

    free(rbuf);
    free(data);
    close(fd);
    return rc;
}
