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
#define LINE_BUF      8192
#define TIMEOUT_S     120

static int open_serial(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);

    /* Switch to blocking I/O — O_NONBLOCK was only needed to avoid hanging on open. */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Discard anything the Teensy sent before we connected (e.g. the ready prompt). */
    usleep(100000);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int send_data(int fd, const unsigned char *data, size_t size)
{
    /* 4-byte little-endian length prefix so the firmware reads exactly this
       many bytes and never relies on a timeout to detect end-of-stream. */
    unsigned char hdr[4] = {
        (unsigned char)(size >>  0),
        (unsigned char)(size >>  8),
        (unsigned char)(size >> 16),
        (unsigned char)(size >> 24),
    };
    if (write(fd, hdr, 4) != 4) {
        fprintf(stderr, "write (header): %s\n", strerror(errno));
        return -1;
    }

    size_t sent = 0;
    while (sent < size) {
        size_t n = size - sent;
        if (n > CHUNK_SIZE) n = CHUNK_SIZE;
        ssize_t w = write(fd, data + sent, n);
        if (w < 0) {
            fprintf(stderr, "write: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/*
 * Read lines from fd, printing each to stdout.  Returns 1 when a line
 * containing any keyword in the NULL-terminated `stop` array is seen,
 * 0 on timeout, -1 on error.  Sets *fail if a "FAIL" line is seen.
 * Lines containing "MISMATCH" are also written to stderr.
 */
static int read_lines(int fd, const char **stop, int timeout_s, int *fail)
{
    char   buf[LINE_BUF];
    size_t len = 0;
    time_t deadline = time(NULL) + timeout_s;
    int    found = 0;

    if (fail) *fail = 0;

    while (!found && time(NULL) < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "select: %s\n", strerror(errno));
            return -1;
        }
        if (r == 0) continue;

        ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "read: %s\n", strerror(errno));
            return -1;
        }
        len += (size_t)n;
        buf[len] = '\0';

        /* Process all complete lines. */
        char *p = buf;
        for (;;) {
            char *nl = memchr(p, '\n', buf + len - p);
            if (!nl) break;

            size_t llen = (size_t)(nl - p);
            if (llen > 0 && p[llen - 1] == '\r') llen--;
            char saved = p[llen];
            p[llen] = '\0';

            if (llen > 0) {
                if (strstr(p, "MISMATCH"))
                    fprintf(stderr, "%s\n", p);
                else {
                    puts(p);
                    fflush(stdout);
                }
                if (fail && strstr(p, "FAIL"))
                    *fail = 1;
                if (stop) {
                    for (const char **kw = stop; *kw; kw++) {
                        if (strstr(p, *kw)) { found = 1; break; }
                    }
                }
            }

            p[llen] = saved;
            p = nl + 1;
        }

        /* Compact the buffer. */
        len = (size_t)((buf + len) - p);
        memmove(buf, p, len);
    }

    return found;
}

int main(int argc, char *argv[])
{
    const char *port    = DEFAULT_PORT;
    const char *binpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if (argv[i][0] != '-') {
            binpath = argv[i];
        } else {
            fprintf(stderr, "usage: nvram_write [--port DEV] <binary>\n");
            return 1;
        }
    }
    if (!binpath) {
        fprintf(stderr, "usage: nvram_write [--port DEV] <binary>\n");
        return 1;
    }

    FILE *f = fopen(binpath, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", binpath, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    unsigned char *data = malloc((size_t)fsize);
    if (!data) { fprintf(stderr, "out of memory\n"); fclose(f); return 1; }
    if ((long)fread(data, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "short read from %s\n", binpath);
        free(data); fclose(f); return 1;
    }
    fclose(f);

    printf("Image: %s (%ld bytes)\n", binpath, fsize);

    int fd = open_serial(port);
    if (fd < 0) { free(data); return 1; }
    printf("Opened %s at 115200 baud.\n", port);

    int rc = 0;

    /* Send the binary once; the firmware writes and reads back each byte inline. */
    printf("Writing %ld bytes...\n", fsize);
    fflush(stdout);
    if (send_data(fd, data, (size_t)fsize) < 0) { rc = 1; goto done; }

    {
        const char *stop[] = { "PASS", "FAIL", NULL };
        int fail = 0;
        if (!read_lines(fd, stop, TIMEOUT_S, &fail)) {
            fprintf(stderr, "timed out — no PASS/FAIL received\n");
            rc = 1;
            goto done;
        }
        if (fail) rc = 1;
    }

done:
    close(fd);
    free(data);
    return rc;
}
