#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>

#define DEFAULT_PORT   "/dev/ttyACM0"
#define DEFAULT_LENGTH (512UL * 1024UL)
#define TIMEOUT_S      600

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

    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    usleep(100000);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int main(int argc, char *argv[])
{
    const char    *port    = DEFAULT_PORT;
    const char    *outpath = NULL;
    unsigned long  length  = DEFAULT_LENGTH;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
            length = strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] != '-') {
            outpath = argv[i];
        } else {
            fprintf(stderr, "usage: nvram_read [--port DEV] [--length N] <outfile>\n");
            return 1;
        }
    }
    if (!outpath) {
        fprintf(stderr, "usage: nvram_read [--port DEV] [--length N] <outfile>\n");
        return 1;
    }

    unsigned char *buf = malloc(length);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }

    int fd = open_serial(port);
    if (fd < 0) { free(buf); return 1; }
    printf("Opened %s at 115200 baud.\n", port);
    printf("Reading %lu bytes...\n", length);
    fflush(stdout);

    /* Send 'R' + 4-byte little-endian length. */
    unsigned char hdr[5] = {
        'R',
        (unsigned char)(length >>  0),
        (unsigned char)(length >>  8),
        (unsigned char)(length >> 16),
        (unsigned char)(length >> 24),
    };
    if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        fprintf(stderr, "write (cmd): %s\n", strerror(errno));
        free(buf); close(fd); return 1;
    }

    /* Read raw bytes back from the firmware. */
    size_t received = 0;
    time_t deadline = time(NULL) + TIMEOUT_S;

    while (received < length && time(NULL) < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "select: %s\n", strerror(errno));
            break;
        }
        if (r == 0) continue;

        ssize_t n = read(fd, buf + received, length - received);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "read: %s\n", strerror(errno));
            break;
        }

        size_t prev = received;
        received += (size_t)n;

        /* Progress: dot per 4 KB. */
        if (received / (4 * 1024) != prev / (4 * 1024)) {
            putchar('.');
            if (received / (64 * 1024) != prev / (64 * 1024))
                putchar('\n');
            fflush(stdout);
        }
    }
    putchar('\n');

    close(fd);

    if (received != length) {
        fprintf(stderr, "Timed out: received %zu of %lu bytes.\n", received, length);
        free(buf); return 1;
    }

    FILE *f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "%s: %s\n", outpath, strerror(errno)); free(buf); return 1; }
    if (fwrite(buf, 1, received, f) != received) {
        fprintf(stderr, "%s: write failed\n", outpath);
        fclose(f); free(buf); return 1;
    }
    fclose(f);
    free(buf);

    printf("Wrote %zu bytes to %s\n", received, outpath);
    return 0;
}
